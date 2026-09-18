// Android host for the engine: one EGL context (GLES 3.0, RGBA8 + depth24) on the ANativeWindow that
// Flutter's SurfaceTexture gives us, and one render thread that is the ONLY thread ever calling eng_*.
// Everything else (Kotlin through JNI, Dart through FFI) posts to engine/api/host/host_queue.h.
//
//   Kotlin  AyanaPlugin  -> nativeAttach(Surface) / nativeResize / nativeDetach      (JNI)
//   Dart    package:ayana -> ayana_start / ayana_post_* / ayana_call_* / ayana_poll_request (FFI)
//
// The render thread owns the whole GL lifetime: attach creates the surface, detach destroys it, and
// the loop draws only while a surface exists (it parks on the queue otherwise, so posted work still runs).
#include "engine/api/engine_api.h"
#include "engine/api/eng_export.h"
#include "engine/api/host/host_queue.h"
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <atomic>
#include <cstring>
#include <jni.h>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "ayana", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "ayana", __VA_ARGS__)

namespace {

using eng::host::queue;

struct Gl {
  EGLDisplay dpy = EGL_NO_DISPLAY;
  EGLContext ctx = EGL_NO_CONTEXT;
  EGLSurface surf = EGL_NO_SURFACE;
  EGLConfig cfg = nullptr;
  ANativeWindow* win = nullptr;
  int w = 0, h = 0;

  bool initDisplay() {
    if (dpy != EGL_NO_DISPLAY) return true;
    dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, nullptr, nullptr)) { LOGE("eglInitialize failed"); return false; }
    const EGLint attrs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                            EGL_DEPTH_SIZE, 24, EGL_NONE};
    EGLint n = 0;
    if (!eglChooseConfig(dpy, attrs, &cfg, 1, &n) || n < 1) { LOGE("eglChooseConfig failed"); return false; }
    const EGLint ctxAttrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxAttrs);
    if (ctx == EGL_NO_CONTEXT) { LOGE("eglCreateContext (GLES3) failed"); return false; }
    return true;
  }

  // Binds `window` (owned reference: released in dropSurface).
  bool bind(ANativeWindow* window, int width, int height) {
    if (!initDisplay()) return false;
    dropSurface();
    win = window;
    surf = eglCreateWindowSurface(dpy, cfg, win, nullptr);
    if (surf == EGL_NO_SURFACE) { LOGE("eglCreateWindowSurface failed"); dropSurface(); return false; }
    if (!eglMakeCurrent(dpy, surf, surf, ctx)) { LOGE("eglMakeCurrent failed"); dropSurface(); return false; }
    eglSwapInterval(dpy, 1);
    w = width; h = height;
    return true;
  }

  void dropSurface() {
    if (dpy != EGL_NO_DISPLAY) eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (surf != EGL_NO_SURFACE) { eglDestroySurface(dpy, surf); surf = EGL_NO_SURFACE; }
    if (win) { ANativeWindow_release(win); win = nullptr; }
  }

  void destroy() {
    dropSurface();
    if (ctx != EGL_NO_CONTEXT) { eglDestroyContext(dpy, ctx); ctx = EGL_NO_CONTEXT; }
    if (dpy != EGL_NO_DISPLAY) { eglTerminate(dpy); dpy = EGL_NO_DISPLAY; }
  }

  bool live() const { return surf != EGL_NO_SURFACE; }
  void swap() { eglSwapBuffers(dpy, surf); }
};

struct Host {
  std::thread thread;
  std::atomic<bool> running{false};
  std::atomic<bool> engineUp{false};
  std::atomic<float> fps{0};
  Gl gl;                                  // render thread only
  eng::host::FramePacer pacer;
  float dpr = 1;
  std::vector<uint8_t> font;              // handed over before eng_init (eng_set_font)
  std::mutex fontMutex;
  std::string stats;                      // render thread writes, ayana_stats copies under the queue
};
Host& host() { static Host h; return h; }

// The render thread: pump the queue, draw when there is a surface, repeat.
void renderLoop() {
  Host& H = host();
  while (H.running.load(std::memory_order_relaxed)) {
    if (!H.gl.live()) { queue().waitPump(50); continue; }   // no surface: still service posted work
    queue().pump();
    if (!H.running.load(std::memory_order_relaxed)) break;
    float dt = H.pacer.tick();
    if (H.engineUp.load(std::memory_order_relaxed)) {
      eng_frame(dt);
      H.gl.swap();
      float inst = dt > 0 ? 1.f / dt : 0.f;
      H.fps.store(H.fps.load() * 0.9f + inst * 0.1f, std::memory_order_relaxed);
    } else {
      glClearColor(0.05f, 0.07f, 0.10f, 1.f);
      glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
      H.gl.swap();
    }
    int ms = H.pacer.sleepMs();
    if (ms > 0) queue().waitPump(ms);
  }
  if (H.engineUp.exchange(false)) eng_shutdown();
  H.gl.destroy();
  queue().shutdown();   // releases anyone blocked on a call; later calls return their fallback
}

void startThread() {
  Host& H = host();
  if (H.running.exchange(true)) return;
  H.thread = std::thread(renderLoop);
}

void stopThread() {
  Host& H = host();
  if (!H.running.exchange(false)) return;
  queue().post([] {});      // wake the loop out of waitPump
  if (H.thread.joinable()) H.thread.join();
}

// Attach/resize/detach all run ON the render thread (EGL is bound there), so they go through the queue.
void attachWindow(ANativeWindow* win, int w, int h, float dpr) {
  Host& H = host();
  H.dpr = dpr > 0 ? dpr : 1;
  startThread();
  queue().run([win, w, h] {
    Host& h2 = host();
    if (!h2.gl.bind(win, w, h)) { LOGE("attach: no GL"); return; }
    if (!h2.engineUp.load()) {
      {
        std::lock_guard<std::mutex> lk(h2.fontMutex);
        if (!h2.font.empty()) eng_set_font(h2.font.data(), (int)h2.font.size());
      }
      if (eng_init(w, h, h2.dpr)) h2.engineUp.store(true);
      else LOGE("eng_init failed");
    } else {
      eng_resize(w, h, h2.dpr);     // context survived: the GL objects are still ours
    }
    LOGI("attached %dx%d dpr=%.2f engine=%d", w, h, (double)h2.dpr, (int)h2.engineUp.load());
  });
}

} // namespace

// ---------------- JNI (Kotlin plugin) ----------------
extern "C" {

JNIEXPORT void JNICALL Java_com_ayana_engine_AyanaPlugin_nativeAttach(JNIEnv* env, jclass, jobject surface, jint w, jint h, jfloat dpr) {
  ANativeWindow* win = ANativeWindow_fromSurface(env, surface);
  if (!win) { LOGE("ANativeWindow_fromSurface = null"); return; }
  attachWindow(win, w, h, dpr);
}

JNIEXPORT void JNICALL Java_com_ayana_engine_AyanaPlugin_nativeResize(JNIEnv*, jclass, jint w, jint h, jfloat dpr) {
  queue().post([w, h, dpr] {
    Host& H = host();
    H.dpr = dpr > 0 ? dpr : 1;
    H.gl.w = w; H.gl.h = h;
    if (H.engineUp.load()) eng_resize(w, h, H.dpr);
  });
}

JNIEXPORT void JNICALL Java_com_ayana_engine_AyanaPlugin_nativeDetach(JNIEnv*, jclass) {
  queue().run([] { host().gl.dropSurface(); });
}

JNIEXPORT void JNICALL Java_com_ayana_engine_AyanaPlugin_nativeStop(JNIEnv*, jclass) { stopThread(); }

// ---------------- Plain C (Dart FFI) ----------------
// M1 set. Every one of these is a wrapper over the queue: Dart never calls an eng_* symbol itself.

ENG_EXPORT void ayana_set_font(const uint8_t* bytes, int len) {
  Host& H = host();
  std::lock_guard<std::mutex> lk(H.fontMutex);
  H.font.assign(bytes, bytes + (len > 0 ? len : 0));
}

ENG_EXPORT void ayana_set_target_fps(float fps) { queue().post([fps] { host().pacer.setTargetFps(fps); }); }

ENG_EXPORT void ayana_shutdown(void) { stopThread(); }

ENG_EXPORT int ayana_ready(void) {
  if (!host().engineUp.load()) return 0;
  return queue().callInt([] { return eng_ready(); });
}

ENG_EXPORT float ayana_fps(void) { return host().fps.load(std::memory_order_relaxed); }

ENG_EXPORT void ayana_orbit(float dx, float dy) { queue().post([dx, dy] { eng_orbit(dx, dy); }); }
ENG_EXPORT void ayana_zoom(float steps) { queue().post([steps] { eng_zoom(steps); }); }
ENG_EXPORT void ayana_set_view(float distance, float yaw, float pitch) {
  queue().post([distance, yaw, pitch] { eng_set_view(distance, yaw, pitch); });
}
ENG_EXPORT void ayana_set_theme(int dark) { queue().post([dark] { eng_set_theme(dark); }); }

// `bytes` is copied: Dart frees its buffer as soon as this returns.
ENG_EXPORT int ayana_model_begin(const char* slot, const uint8_t* bytes, int len) {
  std::string s = slot ? slot : "";
  std::vector<uint8_t> data(bytes, bytes + (len > 0 ? len : 0));
  return queue().callInt([s, data] { return eng_model_begin(s.c_str(), data.data(), (int)data.size()); });
}

// The stats JSON into a caller-owned buffer (no ownership games across the FFI boundary).
ENG_EXPORT int ayana_stats(char* out, int cap) {
  std::string s = queue().callString([] { const char* p = eng_stats(); return std::string(p ? p : "{}"); });
  if (!out || cap <= 0) return (int)s.size();
  int n = (int)s.size() < cap - 1 ? (int)s.size() : cap - 1;
  std::memcpy(out, s.data(), (size_t)n); out[n] = 0;
  return n;
}

// One pending asset request, or 0. Same contract as eng_request_poll but off the render thread.
ENG_EXPORT int ayana_poll_request(char* kind, int kindCap, char* path, int pathCap) {
  eng::host::AssetRequest r;
  if (!queue().popRequest(r)) return 0;
  auto copy = [](char* dst, int cap, const std::string& src) {
    if (!dst || cap <= 0) return;
    int n = (int)src.size() < cap - 1 ? (int)src.size() : cap - 1;
    std::memcpy(dst, src.data(), (size_t)n); dst[n] = 0;
  };
  copy(kind, kindCap, r.kind); copy(path, pathCap, r.path);
  return 1;
}

} // extern "C"

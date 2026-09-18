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
#include <cstdio>
#include <cstring>
#include <jni.h>
#include <unistd.h>
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
  // 60 by default: a SurfaceTexture producer is NOT vsync-throttled (eglSwapInterval is ignored), so a free
  // running loop drew an empty world at 140 fps on a Helio G99 and starved the UI thread once the world was in.
  eng::host::FramePacer pacer{60};
  float dpr = 1;
  std::vector<uint8_t> font;              // handed over before eng_init (eng_set_font)
  std::mutex fontMutex;
  // Snapshots the render thread refreshes every frame so Dart NEVER waits on it for a read: the
  // loader polled ready/lastError at 10 Hz through blocking calls, and while the decor build held the
  // render thread for 9 s the UI isolate hung with it -> "168 Railway isn't responding".
  std::atomic<bool> ready{false};
  std::mutex infoMutex;
  std::string lastError, stats;
  int frame = 0;
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
      H.ready.store(eng_ready() != 0, std::memory_order_relaxed);
      if ((H.frame++ & 15) == 0) {
        const char* e = eng_last_error(); const char* st = eng_stats();
        std::lock_guard<std::mutex> lk(H.infoMutex);
        H.lastError = e ? e : ""; H.stats = st ? st : "{}";
      }
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
  H.ready.store(false);
  H.gl.destroy();
  queue().shutdown();   // releases anyone blocked on a call; later calls return their fallback
}

// The engine reports with printf/fprintf ("[ayana] world built in ...", "terrain index: bad sat layer", GL
// failures). Android drops a process's stdout, so the first start pipes both streams into logcat (tag
// ayana) through a small reader thread. Once per process; the fds stay redirected for good.
void pipeStdioToLogcat() {
  static bool done = false;
  if (done) return;
  done = true;
  int fds[2];
  if (pipe(fds) != 0) return;
  setvbuf(stdout, nullptr, _IOLBF, 0);
  setvbuf(stderr, nullptr, _IONBF, 0);
  dup2(fds[1], 1);
  dup2(fds[1], 2);
  int rd = fds[0];
  std::thread([rd] {
    std::string line; char buf[512];
    for (;;) {
      ssize_t n = read(rd, buf, sizeof buf);
      if (n <= 0) break;
      for (ssize_t i = 0; i < n; ++i) {
        if (buf[i] == '\n') { if (!line.empty()) LOGI("%s", line.c_str()); line.clear(); }
        else line.push_back(buf[i]);
      }
    }
  }).detach();
}

void startThread() {
  Host& H = host();
  if (H.running.exchange(true)) return;
  pipeStdioToLogcat();
  if (H.thread.joinable()) H.thread.join();   // the previous loop (see stopThread) — normally long gone
  queue().restart();        // the previous loop shut the queue down on its way out (second visit to the page)
  H.thread = std::thread(renderLoop);
}

// Asks the loop to end and returns AT ONCE: stop runs on the platform thread when the page is left, and
// joining there while the render thread is inside a long build (decor: seconds) was an ANR. The join
// moves to the next startThread, by which time the loop has finished.
void stopThread() {
  Host& H = host();
  if (!H.running.exchange(false)) return;
  queue().post([] {});      // wake the loop out of waitPump
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

// Posted, not awaited (platform thread, same ANR reasoning as stopThread): the ANativeWindow reference
// we hold keeps the buffer queue alive until dropSurface runs, so Kotlin may release its Surface first.
JNIEXPORT void JNICALL Java_com_ayana_engine_AyanaPlugin_nativeDetach(JNIEnv*, jclass) {
  queue().post([] { host().gl.dropSurface(); });
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

ENG_EXPORT int ayana_ready(void) { return host().ready.load(std::memory_order_relaxed) ? 1 : 0; }

ENG_EXPORT float ayana_fps(void) { return host().fps.load(std::memory_order_relaxed); }

ENG_EXPORT void ayana_orbit(float dx, float dy) { queue().post([dx, dy] { eng_orbit(dx, dy); }); }
ENG_EXPORT void ayana_zoom(float steps) { queue().post([steps] { eng_zoom(steps); }); }
ENG_EXPORT void ayana_set_view(float distance, float yaw, float pitch) {
  queue().post([distance, yaw, pitch] { eng_set_view(distance, yaw, pitch); });
}
ENG_EXPORT void ayana_set_theme(int dark) { queue().post([dark] { eng_set_theme(dark); }); }

// `bytes` is copied: Dart frees its buffer as soon as this returns. Fire and forget, like every asset
// answer below: the render thread parses / uploads when it gets to it (a station model plus its
// textures took seconds on the phone), and a rejected file shows up in ayana_last_error, not here.
ENG_EXPORT int ayana_model_begin(const char* slot, const uint8_t* bytes, int len) {
  std::string s = slot ? slot : "";
  std::vector<uint8_t> data(bytes, bytes + (len > 0 ? len : 0));
  queue().post([s, data] { eng_model_begin(s.c_str(), data.data(), (int)data.size()); });
  return 1;
}

// ---- M2: the world and its assets ----
// The four strings are big (the save ~60 KB, model.json ~600 KB): they are copied into std::string on
// the heap and read on the render thread. Nothing ever goes near a stack buffer. This is the ONE call
// that still blocks the caller (about 1.3 s on a Helio G99): the loader wants the verdict before it
// starts answering asset requests.
ENG_EXPORT int ayana_load_world(const char* worldJson, const char* summaryJson, const char* mapSlug, const char* catalogJson) {
  std::string w = worldJson ? worldJson : "", s = summaryJson ? summaryJson : "{}";
  std::string m = mapSlug ? mapSlug : "", c = catalogJson ? catalogJson : "{}";
  return queue().callInt([w, s, m, c] { return eng_load_world(w.c_str(), s.c_str(), m.c_str(), c.c_str()); });
}

ENG_EXPORT int ayana_terrain_index(const uint8_t* bytes, int len) {
  std::vector<uint8_t> b(bytes, bytes + (len > 0 ? len : 0));
  queue().post([b] { eng_terrain_index(b.empty() ? nullptr : b.data(), (int)b.size()); });
  return 1;
}

// RGBA8, tightly packed, row 0 = north (the same buffer the web adapter hands over from a canvas).
ENG_EXPORT int ayana_terrain_tile_rgba(const char* dir, int z, int x, int y, int w, int h, const uint8_t* rgba) {
  std::string d = dir ? dir : "";
  size_t n = (size_t)(w > 0 ? w : 0) * (size_t)(h > 0 ? h : 0) * 4;
  std::vector<uint8_t> b(rgba, rgba + n);
  queue().post([d, z, x, y, w, h, b] { eng_terrain_tile_rgba(d.c_str(), z, x, y, w, h, b.data()); });
  return 1;
}

// The fetch_tiles binary form (dem: f32 grid, sat: EIMG), for a host that serves the baked tiles.
ENG_EXPORT int ayana_terrain_tile(const char* dir, int z, int x, int y, const uint8_t* bytes, int len) {
  std::string d = dir ? dir : "";
  std::vector<uint8_t> b(bytes, bytes + (len > 0 ? len : 0));
  queue().post([d, z, x, y, b] { eng_terrain_tile(d.c_str(), z, x, y, b.data(), (int)b.size()); });
  return 1;
}

ENG_EXPORT void ayana_terrain_tile_fail(const char* dir, int z, int x, int y) {
  std::string d = dir ? dir : "";
  queue().post([d, z, x, y] { eng_terrain_tile_fail(d.c_str(), z, x, y); });
}

ENG_EXPORT int ayana_city_json(const uint8_t* bytes, int len) {
  std::vector<uint8_t> b(bytes, bytes + (len > 0 ? len : 0));
  queue().post([b] { eng_city_json(b.empty() ? nullptr : b.data(), (int)b.size()); });
  return 1;
}

ENG_EXPORT void ayana_model_fail(const char* slot) {
  std::string s = slot ? slot : "";
  queue().post([s] { eng_model_fail(s.c_str()); });
}

// Android .emod files carry their textures (ETC2) inside, so a successful model needs nothing more;
// this is here for the geometry-only web files and for a model whose textures could not be fetched.
ENG_EXPORT void ayana_model_textures_unavailable(const char* slot) {
  std::string s = slot ? slot : "";
  queue().post([s] { eng_model_textures_unavailable(s.c_str()); });
}

// The per-frame simulation state (tens of KB of JSON from the WebView). Fire and forget: the render
// thread picks up the newest one it has when it draws.
ENG_EXPORT void ayana_set_state(const char* stepJson) {
  std::string s = stepJson ? stepJson : "{}";
  queue().post([s] { eng_set_state(s.c_str()); });
}

ENG_EXPORT void ayana_pointer(float x, float y, int button, int phase) {
  queue().post([x, y, button, phase] { eng_pointer(x, y, button, phase); });
}
ENG_EXPORT void ayana_compass_click(float x, float y) { queue().post([x, y] { eng_compass_click(x, y); }); }
// Both answer with the value they asked for (clamped as the engine clamps); the engine applies it at its
// next pump. A train mode without a train stays put in the engine — eng_camera_json tells, not this.
ENG_EXPORT int ayana_camera_mode(int mode) { queue().post([mode] { eng_camera_mode(mode); }); return mode < 0 ? 0 : mode > 6 ? 6 : mode; }
ENG_EXPORT int ayana_set_quality(int tier) { queue().post([tier] { eng_set_quality(tier); }); return tier < 0 ? 0 : tier > 4 ? 4 : tier; }
ENG_EXPORT void ayana_set_sky_time(double sec) { queue().post([sec] { eng_set_sky_time(sec); }); }

// The last failure message into a caller-owned buffer ("" when none). Snapshot, at most 16 frames old.
ENG_EXPORT int ayana_last_error(char* out, int cap) {
  std::string s; { Host& H = host(); std::lock_guard<std::mutex> lk(H.infoMutex); s = H.lastError; }
  if (!out || cap <= 0) return (int)s.size();
  int n = (int)s.size() < cap - 1 ? (int)s.size() : cap - 1;
  std::memcpy(out, s.data(), (size_t)n); out[n] = 0;
  return n;
}

// The stats JSON into a caller-owned buffer (no ownership games across the FFI boundary). Snapshot too.
ENG_EXPORT int ayana_stats(char* out, int cap) {
  std::string s; { Host& H = host(); std::lock_guard<std::mutex> lk(H.infoMutex); s = H.stats; if (s.empty()) s = "{}"; }
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

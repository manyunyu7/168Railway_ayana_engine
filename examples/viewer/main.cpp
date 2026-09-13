// Example 2: model viewer. `viewer model.emod` — drag to orbit, scroll to zoom.
// Models are loaded through fetchFile: synchronously on desktop, streamed with emscripten_fetch on the web,
// where the page (web/index.html) calls the exported viewer_load(url) to switch models and polls
// viewer_fps() / viewer_bytes() for the readouts.
#include "engine/asset/emod.h"
#include "engine/core/fetch.h"
#include "engine/core/orbit_camera.h"
#include "engine/core/window.h"
#include "engine/render/model_renderer.h"
#include "engine/render/sky.h"
#include "engine/render/text.h"
#include <GLFW/glfw3.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

using namespace eng;

struct App {
  Window win; GpuModel gpu; ModelRenderer renderer; TextRenderer text; Lighting light; Sky sky; OrbitCamera cam;
  std::string path, status; double mxPrev = 0, myPrev = 0; bool dragging = false; int frame = 0;
  bool loading = false, ready = false; size_t bytes = 0; double loadMs = 0;
  double fps = 0, fpsAccum = 0; int fpsFrames = 0; double lastTime = 0;
  void load(const std::string& url);
  void step();
};
static App* g_app = nullptr;
[[maybe_unused]] static void stepThunk() { g_app->step(); }

void App::load(const std::string& url) {
  path = url; loading = true; status = "loading " + url + " ...";
  auto t0 = std::chrono::steady_clock::now();
  fetchFile(url, [this, t0](FetchResult& r) {
    loading = false;
    if (!r.ok) { status = r.error; std::fprintf(stderr, "%s\n", r.error.c_str()); return; }
    Model model; std::string err;
    if (!loadEmod(r.bytes, model, err)) { status = err; std::fprintf(stderr, "%s\n", err.c_str()); return; }
    if (ready) gpu.destroy();
    gpu.upload(model);
    bytes = r.bytes.size(); ready = true; status.clear();
    loadMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("loaded %s (%.1f MB) in %.0f ms: %zu nodes, %zu textures\n", path.c_str(), bytes / 1e6, loadMs, gpu.nodes.size(), gpu.textures.size());
    cam.target = gpu.bounds.center();
    cam.distance = length(gpu.bounds.extent()) * 1.8f;
    cam.far = cam.distance * 20; cam.near = cam.distance * 0.002f;
  });
}

extern "C" {
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void viewer_load(const char* url) { if (g_app) g_app->load(url); }
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
double viewer_fps() { return g_app ? g_app->fps : 0; }
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int viewer_bytes() { return g_app ? (int)g_app->bytes : 0; }   // size of the model file on screen (0 while loading)
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int viewer_ready() { return g_app && g_app->ready && !g_app->loading; }
}

int main(int argc, char** argv) {
#ifdef __EMSCRIPTEN__
  const char* path = argc >= 2 ? argv[1] : "";   // the page decides what to load (viewer_load)
#else
  if (argc < 2) { std::fprintf(stderr, "usage: viewer model.emod\n"); return 2; }
  const char* path = argv[1];
#endif
  static App app; g_app = &app;
  Window& win = app.win;
  if (!win.open(1280, 800, "engine — viewer")) return 1;
  rhi::init();
  app.renderer.init();
  std::string err;
  if (!app.text.load("assets/font.efnt", err)) std::fprintf(stderr, "font: %s\n", err.c_str());
  app.sky.init(); app.sky.sunDir = app.light.sunDir;
  app.cam.distance = 10; app.cam.far = 200; app.cam.near = 0.02f;
  app.lastTime = win.time();
  if (*path) app.load(path);
#ifndef __EMSCRIPTEN__
  if (!app.ready) return 1;   // native loads are synchronous
#endif

#ifdef __EMSCRIPTEN__
  emscripten_set_main_loop(stepThunk, 0, 1);
#else
  while (win.isOpen()) app.step();
#endif
  app.gpu.destroy(); app.renderer.shutdown(); app.text.shutdown(); app.sky.shutdown(); win.close();
  return 0;
}

void App::step() {
  win.pollEvents();
  double now = win.time();
  fpsAccum += now - lastTime; lastTime = now;
  if (++fpsFrames >= 30) { fps = fpsFrames / fpsAccum; fpsAccum = 0; fpsFrames = 0; }
  double mx, my; win.mousePos(mx, my);
  if (win.mouseButton(0)) { if (dragging) cam.rotate((float)(mx - mxPrev), (float)(my - myPrev)); dragging = true; }
  else dragging = false;
  mxPrev = mx; myPrev = my;
  if (win.scroll != 0) { cam.zoom((float)win.scroll); win.scroll = 0; }

  int w, h; win.framebufferSize(w, h);
  rhi::setViewport(w, h);
  rhi::clear(0, 0, 0, 1);
  mat4 vp = cam.projection((float)w / (float)h) * cam.view();
  sky.draw(vp.inverse(), cam.position());
  renderer.beginFrame(vp, cam.position(), light);
  if (ready) renderer.draw(gpu);
  renderer.flushTransparent();
  char hud[256];
  if (!status.empty()) std::snprintf(hud, sizeof hud, "%s", status.c_str());
  else std::snprintf(hud, sizeof hud, "%s  %.1f MB  %.0f ms  draws %u  culled %u  %.0f fps", path.c_str(), bytes / 1e6, loadMs, renderer.drawCalls, renderer.culled, fps);
  text.rect(8, 8, text.measure(hud) + 16, text.lineHeight() + 8, {0, 0, 0, 0.5f});
  text.draw(hud, 16, 12);
  text.flush(w, h);

  if (frame++ == 0) rhi::checkErrors("first frame");
  if (const char* cap = std::getenv("ENG_CAPTURE"); cap && frame == 30) {
    rhi::captureFramebuffer(cap, w, h); std::printf("captured -> %s\n", cap); glfwSetWindowShouldClose((GLFWwindow*)win.handle, 1); return;
  }
  win.swapBuffers();
}

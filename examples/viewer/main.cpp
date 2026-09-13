// Example 2: model viewer. `viewer model.emod` — drag to orbit, scroll to zoom.
#include "engine/asset/emod.h"
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
  std::string path; double mxPrev = 0, myPrev = 0; bool dragging = false; int frame = 0;
  void step();
};
static App* g_app = nullptr;
[[maybe_unused]] static void stepThunk() { g_app->step(); }

int main(int argc, char** argv) {
#ifdef __EMSCRIPTEN__
  const char* path = argc >= 2 ? argv[1] : "/models/cc203.emod";
#else
  if (argc < 2) { std::fprintf(stderr, "usage: viewer model.emod\n"); return 2; }
  const char* path = argv[1];
#endif
  static App app; g_app = &app; app.path = path;
  Window& win = app.win;
  if (!win.open(1280, 800, "engine — viewer")) return 1;
  rhi::init();

  auto t0 = std::chrono::steady_clock::now();
  Model model; std::string err;
  if (!loadEmod(path, model, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
  GpuModel& gpu = app.gpu; gpu.upload(model);
  double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  std::printf("loaded %s in %.0f ms: %zu nodes, %zu textures\n", path, ms, gpu.nodes.size(), gpu.textures.size());
  model = {};

  app.renderer.init();
  if (!app.text.load("assets/font.efnt", err)) std::fprintf(stderr, "font: %s\n", err.c_str());
  app.sky.init(); app.sky.sunDir = app.light.sunDir;
  OrbitCamera& cam = app.cam;
  cam.target = gpu.bounds.center();
  cam.distance = length(gpu.bounds.extent()) * 1.8f;
  cam.far = cam.distance * 20; cam.near = cam.distance * 0.002f;

#ifdef __EMSCRIPTEN__
  emscripten_set_main_loop(stepThunk, 0, 1);
#else
  while (win.isOpen()) app.step();
#endif
  gpu.destroy(); app.renderer.shutdown(); app.text.shutdown(); app.sky.shutdown(); win.close();
  return 0;
}

void App::step() {
  {
    win.pollEvents();
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
    renderer.draw(gpu);
    renderer.flushTransparent();
    char hud[128]; std::snprintf(hud, sizeof hud, "%s  draws %u  culled %u", path.c_str(), renderer.drawCalls, renderer.culled);
    text.rect(8, 8, text.measure(hud) + 16, text.lineHeight() + 8, {0, 0, 0, 0.5f});
    text.draw(hud, 16, 12);
    text.flush(w, h);

    if (frame++ == 0) rhi::checkErrors("first frame");
    if (const char* cap = std::getenv("ENG_CAPTURE"); cap && frame == 30) {
      rhi::captureFramebuffer(cap, w, h); std::printf("captured -> %s\n", cap); glfwSetWindowShouldClose((GLFWwindow*)win.handle, 1); return;
    }
    win.swapBuffers();
  }
}

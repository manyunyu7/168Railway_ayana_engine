// Example 2: model viewer. `viewer model.emod` — drag to orbit, scroll to zoom.
#include "engine/asset/emod.h"
#include "engine/core/orbit_camera.h"
#include "engine/core/window.h"
#include "engine/render/model_renderer.h"
#include "engine/render/text.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>

using namespace eng;

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: viewer model.emod\n"); return 2; }
  Window win;
  if (!win.open(1280, 800, "engine — viewer")) return 1;
  rhi::init();

  auto t0 = std::chrono::steady_clock::now();
  Model model; std::string err;
  if (!loadEmod(argv[1], model, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
  GpuModel gpu; gpu.upload(model);
  double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  std::printf("loaded %s in %.0f ms: %zu nodes, %zu textures\n", argv[1], ms, gpu.nodes.size(), gpu.textures.size());
  model = {};

  ModelRenderer renderer; renderer.init();
  TextRenderer text; if (!text.load("assets/font.efnt", err)) std::fprintf(stderr, "font: %s\n", err.c_str());
  Lighting light;
  OrbitCamera cam;
  cam.target = gpu.bounds.center();
  cam.distance = length(gpu.bounds.extent()) * 1.8f;
  cam.far = cam.distance * 20; cam.near = cam.distance * 0.002f;

  double mxPrev = 0, myPrev = 0; bool dragging = false; int frame = 0;
  while (win.isOpen()) {
    win.pollEvents();
    double mx, my; win.mousePos(mx, my);
    if (win.mouseButton(0)) { if (dragging) cam.rotate((float)(mx - mxPrev), (float)(my - myPrev)); dragging = true; }
    else dragging = false;
    mxPrev = mx; myPrev = my;
    if (win.scroll != 0) { cam.zoom((float)win.scroll); win.scroll = 0; }

    int w, h; win.framebufferSize(w, h);
    rhi::setViewport(w, h);
    rhi::clear(0.55f, 0.65f, 0.8f, 1);
    renderer.beginFrame(cam.projection((float)w / (float)h) * cam.view(), cam.position(), light);
    renderer.draw(gpu);
    renderer.flushTransparent();
    char hud[128]; std::snprintf(hud, sizeof hud, "%s  draws %u  culled %u", argv[1], renderer.drawCalls, renderer.culled);
    text.rect(8, 8, text.measure(hud) + 16, text.lineHeight() + 8, {0, 0, 0, 0.5f});
    text.draw(hud, 16, 12);
    text.flush(w, h);

    if (frame++ == 0) rhi::checkErrors("first frame");
    if (const char* cap = std::getenv("ENG_CAPTURE"); cap && frame == 30) {
      rhi::captureFramebuffer(cap, w, h); std::printf("captured -> %s\n", cap); break;
    }
    win.swapBuffers();
  }
  gpu.destroy(); renderer.shutdown(); text.shutdown(); win.close();
  return 0;
}

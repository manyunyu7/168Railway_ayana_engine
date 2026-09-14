// Example: rolling stock test. `traintest [map] [clock]` — runs the PPKA sim (AI PPKA on) at
// 5x real time and renders every train with catalog GLBs (box fallback) on a flat grey ground.
// Orbit camera follows the first train (drag to orbit, scroll to zoom). ENG_CAPTURE=/path.ppm
// captures frame ENG_CAPTURE_FRAME (default 60) and exits; ENG_VIEW=dist,yaw,pitch (radians).
#include "engine/core/orbit_camera.h"
#include "engine/core/window.h"
#include "engine/render/mesh_builder.h"
#include "engine/render/model_renderer.h"
#include "engine/render/sky.h"
#include "engine/render/text.h"
#include "engine/sim/sim_process.h"
#include "engine/world/asset_catalog.h"
#include "engine/world/coords.h"
#include "engine/world/rolling_stock.h"
#include "engine/world/train_visual.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace eng;

int main(int argc, char** argv) {
  std::string map = argc > 1 ? argv[1] : "mojokerto";
  std::string clock = argc > 2 ? argv[2] : "05:10";
  const char* capture = std::getenv("ENG_CAPTURE");
  int captureFrame = std::getenv("ENG_CAPTURE_FRAME") ? std::atoi(std::getenv("ENG_CAPTURE_FRAME")) : 60;

  SimProcess sim;
  if (!sim.start()) { std::fprintf(stderr, "sim start failed: %s\n", sim.error().c_str()); return 1; }
  if (!sim.load(map)["ok"].boolOr(false)) { std::fprintf(stderr, "load failed: %s\n", sim.lastResponse()["error"].stringOr(sim.error()).c_str()); return 1; }
  if (!sim.startSession(true, clock)["ok"].boolOr(false)) { std::fprintf(stderr, "session start failed\n"); return 1; }
  std::printf("session: %d trains on line, %d pending\n", sim.lastResponse()["trains"].intOr(0), sim.lastResponse()["pending"].intOr(0));

  // Scene origin = centre of the track-node bbox (§2.2).
  WorldOrigin origin;
  {
    double minX = 1e30, minY = 1e30, maxX = -1e30, maxY = -1e30;
    for (const Json& n : sim.summary()["nodes"].arr) {
      double x = n["x"].numberOr(0), y = n["y"].numberOr(0);
      minX = std::min(minX, x); maxX = std::max(maxX, x); minY = std::min(minY, y); maxY = std::max(maxY, y);
    }
    origin.ox = (minX + maxX) / 2; origin.oz = (minY + maxY) / 2;
  }

  Window win;
  if (!win.open(1280, 800, "engine — traintest")) return 1;
  rhi::init();
  AssetCatalog catalog;
  if (!catalog.load()) std::fprintf(stderr, "catalog: %s (boxes only)\n", catalog.error().c_str());
  RollingStock stock; stock.init(catalog);
  TrainVisuals trains; trains.init(stock);

  ModelRenderer renderer; renderer.init();
  TextRenderer text; std::string err;
  if (!text.load("assets/font.efnt", err)) std::fprintf(stderr, "font: %s\n", err.c_str());
  Lighting light; light.fogDensity = 1.f / 4000; Sky sky; sky.init(); sky.sunDir = light.sunDir;
  MeshBuilder gb; gb.quad({-4000, 0, 4000}, {4000, 0, 4000}, {4000, 0, -4000}, {-4000, 0, -4000});
  rhi::Mesh ground = gb.upload();
  Material groundMat; groundMat.baseColor = {0.42f, 0.42f, 0.40f, 1}; groundMat.metallic = 0; groundMat.roughness = 1;

  OrbitCamera cam; cam.distance = 45; cam.yaw = radians(150); cam.pitch = radians(12); cam.near = 0.5f; cam.far = 6000;
  if (const char* v = std::getenv("ENG_VIEW")) std::sscanf(v, "%f,%f,%f", &cam.distance, &cam.yaw, &cam.pitch);   // debug: dist,yaw(rad),pitch(rad)
  double mxPrev = 0, myPrev = 0; bool dragging = false; int frame = 0;
  auto tPrev = std::chrono::steady_clock::now();
  while (win.isOpen()) {
    win.pollEvents();
    double mx, my; win.mousePos(mx, my);
    if (win.mouseButton(0)) { if (dragging) cam.rotate((float)(mx - mxPrev), (float)(my - myPrev)); dragging = true; }
    else dragging = false;
    mxPrev = mx; myPrev = my;
    if (win.scroll != 0) { cam.zoom((float)win.scroll); win.scroll = 0; }

    auto tNow = std::chrono::steady_clock::now();
    double dt = std::chrono::duration<double>(tNow - tPrev).count(); tPrev = tNow;
    if (capture) dt = 1.0 / 30;                                     // deterministic captures
    if (!sim.step(std::min(0.1, dt) * 5)["ok"].boolOr(false)) { std::fprintf(stderr, "step failed: %s\n", sim.error().c_str()); break; }
    SimState st = sim.state();
    if (std::getenv("ENG_DOORS")) for (SimTrain& t : st.trains) { t.state = "dwell"; t.istirahat = false; }   // debug: every consist dwelling (doors open)
    trains.update(st, origin, nullptr, 5);                          // no rail profile yet: y = 0
    // ENG_TRAIN=<train no> follows that train (default: the first); ENG_NIGHT=1 forces the night switch
    size_t follow = 0;
    if (const char* want = std::getenv("ENG_TRAIN")) for (size_t i = 0; i < trains.labels().size(); ++i) if (trains.labels()[i].no == want) follow = i;
    if (!trains.labels().empty()) cam.target = trains.labels()[follow].anchor - vec3{0, 3.0f, 0};
    { double hh = std::fmod(st.clock / 3600.0, 24.0); int w0, h0; win.framebufferSize(w0, h0);
      trains.setView(cam.fovY, h0, std::getenv("ENG_NIGHT") ? true : (hh < 6 || hh >= 18)); }

    int w, h; win.framebufferSize(w, h);
    rhi::setViewport(w, h);
    rhi::clear(light.fogColor.x, light.fogColor.y, light.fogColor.z, 1);
    mat4 vp = cam.projection((float)w / (float)h) * cam.view();
    Frustum frustum(vp);
    sky.draw(vp.inverse(), cam.position());
    renderer.beginFrame(vp, cam.position(), light);
    renderer.drawMesh(ground, groundMat, {}, mat4::identity());
    trains.draw(renderer, &frustum);
    renderer.flushTransparent();

    int hh = (int)st.clock / 3600 % 24, mm = (int)st.clock / 60 % 60, ss = (int)st.clock % 60;
    char hud[256];
    std::snprintf(hud, sizeof hud, "%02d:%02d:%02d  trains %zu  vehicles %zu  draws %u", hh, mm, ss, st.trains.size(), trains.vehicles().size(), renderer.drawCalls);
    text.rect(8, 8, text.measure(hud) + 16, text.lineHeight() * 2 + 12, {0, 0, 0, 0.5f});
    text.draw(hud, 16, 12);
    if (!trains.labels().empty()) {
      const TrainLabel& l = trains.labels()[follow];
      std::snprintf(hud, sizeof hud, "KA %s %s  %s  %.0f km/h  doors %.2f s  lights %zu", l.no.c_str(), l.name.c_str(), l.state.c_str(), l.speed * 3.6f, trains.doorTime(l.id), trains.lights().size());
      text.draw(hud, 16, 12 + text.lineHeight());
    }
    text.flush(w, h);

    if (frame++ == 0) rhi::checkErrors("first frame");
    if (capture && frame == captureFrame) {
      rhi::captureFramebuffer(capture, w, h); std::printf("captured -> %s\n", capture); break;
    }
    win.swapBuffers();
  }
  rhi::destroyMesh(ground);
  trains.shutdown(); catalog.destroy(); renderer.shutdown(); text.shutdown(); sky.shutdown(); win.close();
  sim.stop();
  return 0;
}

// Example: terrain test. `terraintest [map]` — loads assets/terrain/<map>.dem/.sat, carves the rail
// corridor from a synthetic rail sample list (node chords of world.graph polylined at 12 m, rail height =
// DEM smoothed along the chord) and orbits the station. Drag to orbit, scroll to zoom, WASD to pan.
// Trees are scattered from the satellite green mask (Vegetation) with the catalog's `vegetasi` models.
// Env: ENG_DIST/ENG_PITCH/ENG_YAW camera, ENG_AT_CARVE=1 targets the strongest carve, ENG_CAPTURE=file.ppm,
// ENG_NO_TREES=1 skips vegetation, ENG_CLOCK=hh.h sets the sun/cloud tint (default 10:00), ENG_NO_CLOUDS=1 skips clouds.
// Brush deltas (world.tanah) are applied when the save has them (bks).
#include "engine/core/json.h"
#include "engine/core/orbit_camera.h"
#include "engine/core/window.h"
#include "engine/render/model_renderer.h"
#include "engine/render/sky.h"
#include "engine/render/text.h"
#include "engine/world/asset_catalog.h"
#include "engine/world/cloud_visual.h"
#include "engine/world/sun.h"
#include "engine/world/terrain.h"
#include "engine/world/vegetation.h"
#include <GLFW/glfw3.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace eng;

int main(int argc, char** argv) {
  std::string map = argc > 1 ? argv[1] : "mojokerto";
  std::string root = ENG_SOURCE_DIR;
  Window win;
  if (!win.open(1280, 800, "engine — terrain")) return 1;
  rhi::init();
  rhi::setAnisotropy(8);

  auto t0 = std::chrono::steady_clock::now();
  Terrain terrain; std::string err;
  if (!terrain.load(root + "/assets/terrain/" + map + ".dem", root + "/assets/terrain/" + map + ".sat", err)) {
    std::fprintf(stderr, "%s (run: fetch_tiles %s)\n", err.c_str(), map.c_str()); return 1;
  }
  const WorldOrigin& org = terrain.origin();
  std::printf("terrain %s: origin (%.1f, %.1f), demBase %.1f m, rawMin %.1f m, %zu DEM layers\n", map.c_str(), org.ox, org.oz,
              terrain.dem().demBase, terrain.dem().rawMin, terrain.dem().layers.size());

  // ---- synthetic rail samples from the map graph (node chords, 12 m steps) ----
  std::vector<RailSample> rails;
  double stationX = org.ox, stationY = org.oz;
  {
    std::ifstream in(root + "/../ppka-wannabe-2/src/data/" + map + ".json");
    std::stringstream ss; ss << in.rdbuf();
    Json doc = Json::parse(ss.str(), &err);
    if (!err.empty()) { std::fprintf(stderr, "map json: %s\n", err.c_str()); return 1; }
    std::map<std::string, std::pair<double, double>> nodes;
    for (const Json& n : doc["world"]["graph"]["nodes"].arr) nodes[n["id"].str] = {n["x"].numberOr(0), n["y"].numberOr(0)};
    for (const Json& s : doc["world"]["graph"]["segments"].arr) {
      auto a = nodes.find(s["a"].str), b = nodes.find(s["b"].str);
      if (a == nodes.end() || b == nodes.end()) continue;
      std::string kind = s["jenisRel"].stringOr("tanah");
      bool atGrade = kind == "tanah";
      double ax = a->second.first, ay = a->second.second, bx = b->second.first, by = b->second.second;
      double L = std::hypot(bx - ax, by - ay); int n = std::max(1, (int)std::ceil(L / 12));
      double tx = (bx - ax) / L, ty = (by - ay) / L;
      for (int i = 0; i <= n; ++i) {
        double x = ax + (bx - ax) * i / n, y = ay + (by - ay) * i / n, h = 0;
        for (double d : {-90.0, -45.0, 0.0, 45.0, 90.0}) h += terrain.dem().heightScene(x + tx * d, y + ty * d);
        rails.push_back({x, y, (float)(h / 5), atGrade, 1.f});
      }
      rails.push_back({0, 0, 0, false, 0});   // chain break between segments
    }
    terrain.setBrushDeltas(doc["world"]["tanah"]);
    const Json& st = doc["world"]["hiasan"]["objek"][0];
    if (st.isObject()) { stationX = st["x"].numberOr(org.ox); stationY = st["y"].numberOr(org.oz); }
  }
  terrain.setRails(rails);
  terrain.build();
  double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  const Terrain::Stats& s = terrain.stats;
  std::printf("built in %.0f ms (load %.0f, build %.0f): %d near + %d far tiles, %d patches (%d detail), %zu vertices, %zu triangles, %zu rail samples\n",
              ms, s.loadMs, s.buildMs, s.nearTiles, s.farTiles, s.patches, s.detailPatches, s.vertices, s.triangles, rails.size());

  {  // carving check: carved ground must sit 0.62 m under the rail head, and differ from the raw DEM somewhere
    float maxDelta = 0, worst = 0; size_t k = 0; double wxBest = 0, wyBest = 0;
    for (const RailSample& r : rails) {
      if (!r.atGrade) continue;
      float g = terrain.groundHeight(r.wx, r.wy), raw = terrain.dem().heightScene(r.wx, r.wy);
      worst = std::max(worst, std::fabs(g - (r.railY + terrain::PLATEAU_OFFSET)));
      if (std::fabs(g - raw) > maxDelta) { maxDelta = std::fabs(g - raw); wxBest = r.wx; wyBest = r.wy; }
      ++k;
    }
    std::printf("carve check over %zu at-grade samples: max |carved - raw| %.2f m at (%.0f, %.0f), worst plateau error %.3f m\n",
                k, maxDelta, wxBest, wyBest, worst);
    if (std::getenv("ENG_AT_CARVE")) { stationX = wxBest; stationY = wyBest; }
  }

  ModelRenderer renderer; renderer.init();
  AssetCatalog catalog; Vegetation trees;
  if (!std::getenv("ENG_NO_TREES")) {
    if (!catalog.load()) std::fprintf(stderr, "catalog: %s\n", catalog.error().c_str());
    trees.build(terrain, catalog);
    std::printf("vegetation: %zu trees in %d cells, %d models, %.0f ms\n", trees.stats.trees, trees.stats.cells, trees.stats.models, trees.stats.buildMs);
  }
  TextRenderer text; if (!text.load(root + "/assets/font.efnt", err)) std::fprintf(stderr, "font: %s\n", err.c_str());
  auto envf = [](const char* k, float d) { const char* v = std::getenv(k); return v ? (float)std::atof(v) : d; };
  Lighting light; light.fogDensity = 1.f / 9000; Sky sky; sky.init(); sky.sunDir = light.sunDir;
  light.fogColor = {0.79f, 0.86f, 0.93f};
  { double lon, lat; worldToLonLat(org.ox, org.oz, lon, lat); applySun(envf("ENG_CLOCK", 10) * 3600.0, lon, lat, light, sky); }
  CloudVisual clouds;
  if (!std::getenv("ENG_NO_CLOUDS")) {
    const double* bb = terrain.dem().bbox;
    clouds.build((float)(bb[0] - org.ox), (float)(bb[1] - org.oz), (float)(bb[2] - org.ox), (float)(bb[3] - org.oz));
    std::printf("clouds: %d sprites\n", clouds.count());
  }
  OrbitCamera cam;
  cam.target = org.toScene(stationX, stationY, terrain.groundHeight(stationX, stationY));
  cam.distance = envf("ENG_DIST", 500); cam.pitch = radians(envf("ENG_PITCH", 28)); cam.yaw = radians(envf("ENG_YAW", -30));
  cam.near = 1; cam.far = 40000;

  double mxPrev = 0, myPrev = 0, tPrev = win.time(); bool dragging = false; int frame = 0;
  while (win.isOpen()) {
    win.pollEvents();
    double now = win.time(); float dt = (float)(now - tPrev); tPrev = now;
    double mx, my; win.mousePos(mx, my);
    if (win.mouseButton(0)) { if (dragging) cam.rotate((float)(mx - mxPrev), (float)(my - myPrev)); dragging = true; }
    else dragging = false;
    mxPrev = mx; myPrev = my;
    if (win.scroll != 0) { cam.zoom((float)win.scroll); win.scroll = 0; }
    vec3 fwd{-std::sin(cam.yaw), 0, -std::cos(cam.yaw)}, right{std::cos(cam.yaw), 0, -std::sin(cam.yaw)};
    float sp = cam.distance * dt;
    if (win.key(GLFW_KEY_W)) cam.target += fwd * sp;
    if (win.key(GLFW_KEY_S)) cam.target -= fwd * sp;
    if (win.key(GLFW_KEY_D)) cam.target += right * sp;
    if (win.key(GLFW_KEY_A)) cam.target -= right * sp;
    double wx, wy; org.toWorld(cam.target, wx, wy);
    cam.target.y = terrain.groundHeight(wx, wy);

    int w, h; win.framebufferSize(w, h);
    rhi::setViewport(w, h);
    rhi::clear(0, 0, 0, 1);
    mat4 vp = cam.projection((float)w / (float)h) * cam.view();
    Frustum fr(vp);
    sky.draw(vp.inverse(), cam.position());
    renderer.beginFrame(vp, cam.position(), light);
    terrain.draw(renderer, &fr);
    trees.draw(renderer, cam.position(), &fr);
    renderer.flushTransparent();
    clouds.draw(vp, cam.view(), cam.position(), sky, light, dt);
    char hud[256];
    std::snprintf(hud, sizeof hud, "%s  tiles %d+%d  patches %d  verts %zu  tris %zu  draws %u culled %u  trees %u/%zu  target (%.0f, %.0f) h %.1f", map.c_str(),
                  s.nearTiles, s.farTiles, s.patches, s.vertices, s.triangles, renderer.drawCalls, renderer.culled, trees.stats.drawn, trees.stats.trees, wx, wy, cam.target.y);
    text.rect(8, 8, text.measure(hud) + 16, text.lineHeight() + 8, {0, 0, 0, 0.5f});
    text.draw(hud, 16, 12);
    text.flush(w, h);

    if (frame++ == 0) rhi::checkErrors("first frame");
    if (const char* cap = std::getenv("ENG_CAPTURE"); cap && frame == 30) {
      rhi::captureFramebuffer(cap, w, h); std::printf("captured -> %s\n", cap); break;
    }
    win.swapBuffers();
  }
  clouds.destroy(); trees.destroy(); catalog.destroy(); terrain.destroy(); renderer.shutdown(); text.shutdown(); sky.shutdown(); win.close();
  return 0;
}

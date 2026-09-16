// Example: rail test. `railtest [map.json] [--sim]` — loads a PPKA save, builds the track graph,
// vertical profile (synthetic terrain), rail/bridge meshes, signals and points arrows, and shows
// them with an orbit camera centred on the station. With --sim the map is loaded through the
// TypeScript bridge and the session is stepped so signal aspects and point settings are real.
// When assets/terrain/<map>.dem/.sat exist (tools/fetch_tiles) the real DEM drives the profile and the
// carved ground is drawn; otherwise a synthetic sine terrain is used.
// Env: ENG_CAPTURE=file.ppm (frame 30, exit), ENG_VIEW=dist,yaw,pitch,
// ENG_TARGET=signal:<name>|point:<nodeId>|seg:<id>|bridge:<n>|tunnel:<n>|krl|x,z (bridge/tunnel: n-th such segment),
// ENG_TEST_KRL=<tracks> plants a procedural KRL station (KrlStation) at the first station, aligned with the rails
#include "engine/core/json.h"
#include "engine/core/orbit_camera.h"
#include "engine/core/window.h"
#include "engine/render/model_renderer.h"
#include "engine/render/sky.h"
#include "engine/render/text.h"
#include "engine/sim/sim_process.h"
#include "engine/world/point_visual.h"
#include "engine/world/rail_builder.h"
#include "engine/world/rail_profile.h"
#include "engine/world/signal_visual.h"
#include "engine/world/krl_station.h"
#include "engine/world/terrain.h"
#include "engine/world/track_graph.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

using namespace eng;

// Rolling synthetic terrain (metres, raw DEM datum) until real tiles are wired in.
struct SineHeight : HeightSource {
  float rawHeight(double wx, double wy) const override {
    return 20.f + 5.f * (float)std::sin(wx / 700.0) * (float)std::cos(wy / 900.0) + 2.f * (float)std::sin((wx + wy) / 260.0);
  }
};

int main(int argc, char** argv) {
  std::string path = std::string(ENG_SOURCE_DIR) + "/../ppka-wannabe-2/src/data/mojokerto.json";
  bool useSim = false;
  for (int i = 1; i < argc; ++i) { std::string a = argv[i]; if (a == "--sim") useSim = true; else path = a; }

  Json save; std::string err;
  SimProcess sim;
  if (useSim) {
    if (!sim.start()) { std::fprintf(stderr, "sim start failed: %s\n", sim.error().c_str()); return 1; }
    std::string map = path.substr(path.find_last_of('/') + 1); map = map.substr(0, map.find('.'));
    if (!sim.load(map)["ok"].boolOr(false)) { std::fprintf(stderr, "sim load failed\n"); return 1; }
    save.type = Json::Type::Object; save.obj["world"] = sim.world();
    sim.startSession(true, "");
    for (int i = 0; i < 120; ++i) sim.step(0.5);   // one simulated minute
  } else {
    std::ifstream f(path);
    if (!f) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); return 1; }
    std::stringstream ss; ss << f.rdbuf();
    save = Json::parse(ss.str(), &err);
    if (!err.empty()) { std::fprintf(stderr, "json: %s\n", err.c_str()); return 1; }
  }
  const Json& world = save["world"];

  Window win;
  if (!win.open(1280, 800, "engine — railtest")) return 1;
  rhi::init();

  auto t0 = std::chrono::steady_clock::now();
  TrackGraph graph;
  if (!graph.fromJson(world, &err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
  std::vector<StationZone> stations; double stX = graph.origin().ox, stY = graph.origin().oz;
  for (const Json& sc : world["scenery"].arr)
    if (sc["kind"].stringOr("") == "station") {
      stations.push_back({sc["pos"]["x"].numberOr(0), sc["pos"]["y"].numberOr(0), 160});
      if (stations.size() == 1) { stX = stations[0].wx; stY = stations[0].wy; }
    }
  SineHeight sine;
  Terrain terrain; std::string mapName = path.substr(path.find_last_of('/') + 1); mapName = mapName.substr(0, mapName.find('.'));
  bool haveTerrain = terrain.load(std::string(ENG_SOURCE_DIR) + "/assets/terrain/" + mapName + ".dem", std::string(ENG_SOURCE_DIR) + "/assets/terrain/" + mapName + ".sat", err);
  if (!haveTerrain) std::printf("no terrain tiles (%s) — synthetic ground\n", err.c_str());
  const HeightSource& dem = haveTerrain ? (const HeightSource&)terrain.dem() : sine;
  float demBase = haveTerrain ? terrain.dem().demBase : sine.rawHeight(graph.origin().ox, graph.origin().oz);
  VerticalProfile profile; profile.build(graph, dem, stations, demBase);
  RailBuilder rails; rails.build(graph, profile, &dem, demBase);
  if (haveTerrain) { terrain.setBrushDeltas(world["tanah"]); terrain.setRails(rails.samples()); terrain.build(); }
  SignalVisuals signals; signals.build(graph, profile, world["trackside"]);
  PointVisuals points; points.build(graph, profile);
  double buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  std::printf("graph %zu nodes %zu segs %d points %.3f km | profile %d chains gmax %.1f permille | rails %d chunks %u tris %d piers, %d bridge segs (%d truss, %d viaduct), %d tunnel segs, %d bed rings / %d strip rings (%.1f ms) | %zu signals | total %.1f ms\n",
              graph.nodes.size(), graph.segments.size(), graph.pointCount(), graph.totalLength() / 1000, profile.chainCount(),
              profile.gmax() * 1000, rails.stats().chunks, rails.stats().tris, rails.stats().piers, rails.stats().bridgeSegs, rails.stats().trussSegs, rails.stats().viaductSegs, rails.stats().tunnelSegs, rails.stats().bedRings, rails.stats().stripRings, rails.stats().buildMs,
              signals.signals().size(), buildMs);

  for (const RailBuilder::BridgeInfo& b : rails.stats().bridges)
    if (b.shape != BridgeShape::Deck) std::printf("  bridge %s: %s, chain span %.0f m, deck %.1f m above ground\n", b.seg.c_str(), b.shape == BridgeShape::Truss ? "truss" : "viaduct", b.span, b.height);
  auto applySim = [&]() {
    for (const SimSignal& s : sim.state().signals) signals.setAspect(s.id, s.aspect);
    for (const SimPoint& p : sim.state().points) points.setState(p.id, p.setting, !p.lockedBy.empty());
  };
  if (useSim) applySim();
  else {   // demo aspects: cycle so every colour shows up
    int k = 0;
    for (const SignalInstance& s : signals.signals()) signals.setAspect(s.id, (Aspect)(k++ % 3));
    k = 0;
    for (const PointInstance& p : points.points()) points.setState(p.nodeId, k % 2, (k / 2) % 3 == 0), ++k;
  }

  ModelRenderer renderer; renderer.init();
  KrlStation krl; mat4 krlXf = mat4::identity();
  if (const char* k = std::getenv("ENG_TEST_KRL")) {
    KrlOptions ko; ko.tracks = std::max(1, std::atoi(k));
    krl.build(ko);
    // nearest segment to the station: yaw from its tangent, y from the rail head
    double best = 1e30; int bs = 0; double bsS = 0;
    for (size_t si = 0; si < graph.segments.size(); ++si)
      for (double sv = 0; sv <= graph.length(si); sv += 10) {
        TrackSample sm = graph.sampleAt((int)si, sv); double d = std::hypot(sm.wx - stX, sm.wy - stY);
        if (d < best) { best = d; bs = (int)si; bsS = sv; }
      }
    TrackSample sm = graph.sampleAt(bs, bsS);
    vec3 pos = graph.origin().toScene(sm.wx, sm.wy, profile.railHeight(bs, bsS));
    // local +X along the rails: scene tangent (tx, ty) -> yaw about Y (scene z = world y)
    float yaw = std::atan2(-(float)sm.ty, (float)sm.tx);
    krlXf = mat4::translation(pos) * mat4::rotationY(yaw);
    std::printf("krl station: %d tracks, %d parts, %u tris (%.1f ms) at seg %s s=%.0f, %zu canopy openings\n", ko.tracks, krl.stats.parts, krl.stats.tris, krl.stats.buildMs, graph.segments[(size_t)bs].id.c_str(), bsS, krl.openings().size());
  }
  TextRenderer text; if (!text.load(std::string(ENG_SOURCE_DIR) + "/assets/font.efnt", err)) std::fprintf(stderr, "font: %s\n", err.c_str());
  Lighting light; light.fogDensity = 1.f / 6000; Sky sky; sky.init(); sky.sunDir = light.sunDir;
  OrbitCamera cam;
  cam.target = graph.origin().toScene(stX, stY, profile.railHeight(graph.segments[0].id.c_str(), 0));
  {   // rail height near the station: nearest node
    double best = 1e30; int bi = 0;
    for (size_t i = 0; i < graph.nodes.size(); ++i) { double d = std::hypot(graph.nodes[i].wx - stX, graph.nodes[i].wy - stY); if (d < best) { best = d; bi = (int)i; } }
    const TrackSegment& s = graph.segments[(size_t)graph.nodes[(size_t)bi].segs[0]];
    cam.target.y = profile.railHeight(s.id.c_str(), s.a == bi ? 0.0 : s.length);
  }
  cam.distance = 260; cam.yaw = radians(35); cam.pitch = radians(22); cam.fovY = radians(52); cam.near = 1; cam.far = 40000;
  if (const char* v = std::getenv("ENG_VIEW")) std::sscanf(v, "%f,%f,%f", &cam.distance, &cam.yaw, &cam.pitch);
  if (const char* tg = std::getenv("ENG_TARGET")) {
    std::string t = tg;
    if (t.rfind("face:", 0) == 0) {   // camera in front of the signal head, looking at the lenses
      for (const SignalInstance& s : signals.signals()) if (s.name == t.substr(5)) {
        cam.target = s.lensWorld[s.lamps - 1]; vec3 f = s.world.transformDir({-1, 0, 0});
        cam.yaw = std::atan2(f.x, f.z); cam.pitch = 0.05f; cam.distance = 4;
      }
    } else if (t.rfind("signal:", 0) == 0) { for (const SignalInstance& s : signals.signals()) if (s.name == t.substr(7)) cam.target = s.railPos; }
    else if (t.rfind("seg:", 0) == 0) {
      int si = graph.segIndex(t.substr(4));
      if (si >= 0) { TrackSample sm = graph.sampleAt(si, graph.length(si) / 2); cam.target = graph.origin().toScene(sm.wx, sm.wy, profile.railHeight(si, graph.length(si) / 2)); }
    }
    else if (t.rfind("bridge:", 0) == 0 || t.rfind("tunnel:", 0) == 0) {   // n-th bridge/tunnel segment, camera at its middle
      RailKind want = t[0] == 'b' ? RailKind::Bridge : RailKind::Tunnel; int n = std::atoi(t.c_str() + 7), k = 0;
      for (size_t si = 0; si < graph.segments.size(); ++si) if (graph.segments[si].kind == want && k++ == n) {
        TrackSample sm = graph.sampleAt((int)si, graph.length(si) / 2); cam.target = graph.origin().toScene(sm.wx, sm.wy, profile.railHeight((int)si, graph.length(si) / 2));
        std::printf("target %s %d = seg %s (%.0f m)\n", t.c_str(), n, graph.segments[si].id.c_str(), graph.length(si));
      }
    }
    else if (t.rfind("mouth:", 0) == 0) {   // n-th tunnel segment: camera outside its `a` mouth looking in
      int n = std::atoi(t.c_str() + 6), k = 0;
      for (size_t si = 0; si < graph.segments.size(); ++si) if (graph.segments[si].kind == RailKind::Tunnel && k++ == n) {
        TrackSample sm = graph.sampleAt((int)si, 0.0); vec3 m = graph.origin().toScene(sm.wx, sm.wy, profile.railHeight((int)si, 0.0));
        vec3 in = normalize(vec3{(float)sm.tx, 0, (float)sm.ty});   // scene z = world y
        cam.target = m + vec3{0, 2, 0}; cam.yaw = std::atan2(-in.x, -in.z); cam.pitch = 0.12f; cam.distance = 40;
        if (const char* c = std::strchr(t.c_str() + 6, ':')) {   // mouth:n:dist — negative: the camera stands that far inside the tunnel
          float d = (float)std::atof(c + 1);
          if (d >= 0) cam.distance = d;
          else { cam.distance = 12; cam.target = m + in * (-d + 12) + vec3{0, 2, 0}; }
          if (const char* v = std::strchr(c + 1, ':')) {   // :out = look back toward the mouth, :top = plan view over the mouth
            if (!std::strcmp(v + 1, "out")) { cam.target = m + in * (-d - 12) + vec3{0, 2, 0}; cam.yaw += PI; }
            if (!std::strcmp(v + 1, "top")) { cam.target = m + in * 4; cam.pitch = 1.45f; cam.distance = std::max(20.f, std::fabs(d)); }
          }
        }
        std::printf("target %s = seg %s mouth\n", t.c_str(), graph.segments[si].id.c_str());
        if (haveTerrain) for (float d : {-6.f, -12.f}) {   // cross-section inside the hill: ground vs rail at lateral offsets
          std::printf("  d %+3.0f m lateral:", d);
          for (float l = -9; l <= 9; l += 3) { vec3 q = m - in * d + vec3{-in.z, 0, in.x} * l; double wx, wy; graph.origin().toWorld(q, wx, wy); std::printf(" %+.0f:%.1f", l, terrain.groundHeight(wx, wy) - m.y); }
          std::printf("  (rel. rail)\n");
        }
        if (haveTerrain) for (float d = -20; d <= 80; d += 10) {   // ground vs rail head along the approach (negative = inside)
          vec3 q = m - in * d; double wx, wy; graph.origin().toWorld(q, wx, wy);
          std::printf("  d %+4.0f m: ground %.1f raw %.1f rail %.1f\n", d, terrain.groundHeight(wx, wy), terrain.dem().heightScene(wx, wy), m.y);
        }
      }
    }
    else if (t == "krl") { cam.target = krlXf.transformPoint({0, 3, 0}); }
    else if (t.rfind("point:", 0) == 0) { for (const PointInstance& p : points.points()) if (p.nodeId == t.substr(6)) cam.target = p.pos; }
    else { float x, z; if (std::sscanf(tg, "%f,%f", &x, &z) == 2) { cam.target.x = x; cam.target.z = z; } }
  }

  if (haveTerrain) terrain.prime(cam.target);
  double mxPrev = 0, myPrev = 0; bool dragging = false; int frame = 0; double simAcc = 0, last = win.time();
  double fpsAcc = 0; int fpsN = 0; float fps = 0;
  while (win.isOpen()) {
    win.pollEvents();
    double now = win.time(), dt = now - last; last = now;
    fpsAcc += dt; if (++fpsN == 30) { fps = (float)(fpsN / fpsAcc); fpsAcc = 0; fpsN = 0; }
    double mx, my; win.mousePos(mx, my);
    if (win.mouseButton(0)) { if (dragging) cam.rotate((float)(mx - mxPrev), (float)(my - myPrev)); dragging = true; }
    else dragging = false;
    mxPrev = mx; myPrev = my;
    if (win.scroll != 0) { cam.zoom((float)win.scroll); win.scroll = 0; }
    if (useSim) { simAcc += dt; if (simAcc >= 0.5) { sim.step(simAcc); simAcc = 0; applySim(); } }

    int w, h; win.framebufferSize(w, h);
    rhi::setViewport(w, h);
    rhi::clear(light.fogColor.x, light.fogColor.y, light.fogColor.z, 1);
    mat4 vp = cam.projection((float)w / (float)h) * cam.view();
    Frustum fr(vp);
    sky.draw(vp.inverse(), cam.position());
    renderer.beginFrame(vp, cam.position(), light);
    if (haveTerrain) { terrain.update(cam.target, (float)dt); terrain.draw(renderer, &fr); }
    rails.draw(renderer, &fr);
    if (krl.stats.parts) krl.draw(renderer, krlXf, &fr);
    signals.draw(renderer, cam.position(), &fr);
    points.draw(renderer, &fr);
    renderer.flushTransparent();

    // HUD: counts + signal name labels at the top lens.
    for (const ScreenPoint& sp : signals.screenPositions(vp, w, h, cam.position())) {
      if (!sp.visible) continue;
      const SignalInstance& s = signals.signals()[(size_t)signals.indexOf(sp.id)];
      float tw = text.measure(s.name, 0.8f);
      text.rect(sp.x - tw / 2 - 3, sp.y - 26, tw + 6, text.lineHeight(0.8f) + 2, {0, 0, 0, 0.55f});
      text.draw(s.name, sp.x - tw / 2, sp.y - 25, {1, 1, 1, 1}, 0.8f);
    }
    char hud[256];
    std::snprintf(hud, sizeof hud, "%zu nodes  %zu segs  %d points  %.1f km  |  %d chunks  %u tris  |  %zu signals  |  draws %u culled %u  %.0f fps",
                  graph.nodes.size(), graph.segments.size(), graph.pointCount(), graph.totalLength() / 1000,
                  rails.stats().chunks, rails.stats().tris, signals.signals().size(), renderer.drawCalls, renderer.culled, fps);
    text.rect(8, 8, text.measure(hud) + 16, text.lineHeight() + 8, {0, 0, 0, 0.5f});
    text.draw(hud, 16, 12);
    text.flush(w, h);

    if (frame++ == 0) rhi::checkErrors("first frame");
    static const int capFrame = std::getenv("ENG_CAPTURE_FRAME") ? std::atoi(std::getenv("ENG_CAPTURE_FRAME")) : 30;
    if (const char* cap = std::getenv("ENG_CAPTURE"); cap && frame == capFrame) {
      rhi::captureFramebuffer(cap, w, h); std::printf("captured -> %s\n", cap); break;
    }
    win.swapBuffers();
  }
  rails.destroy(); signals.destroy(); points.destroy(); terrain.destroy(); krl.destroy();
  renderer.shutdown(); text.shutdown(); sky.shutdown(); win.close();
  if (useSim) sim.stop();
  return 0;
}

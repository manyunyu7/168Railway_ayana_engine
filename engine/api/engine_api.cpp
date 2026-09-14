// See engine_api.h. Everything lives in one static `Api` object; the host is single-threaded (browser main
// thread), so no locking. Native builds compile this too (the GL context then has to exist already), which
// keeps the ABI honest and lets it be unit-tested without a browser.
#include "engine/api/engine_api.h"
#include "engine/app/camera_rig.h"
#include "engine/app/compass.h"
#include "engine/app/world_scene.h"
#include "engine/core/orbit_camera.h"
#include "engine/sim/sim_state.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5.h>
// Module.heap() = current HEAPU8 view (reassigned on memory growth); Module.onAssetRequest(kind, path) is the
// host's fetch hook. Both are looked up at call time so the page can install them after instantiation.
EM_JS(void, api_installHeap, (), { Module['heap'] = () => HEAPU8; });
EM_JS(void, api_request, (const char* kind, const char* path), {
  const k = UTF8ToString(kind), p = UTF8ToString(path);
  if (Module['onAssetRequest']) Module['onAssetRequest'](k, p); else console.warn('ayana: no onAssetRequest for', k, p);
});
#endif

using namespace eng;

static std::string SimProcessEscapeShim(const std::string& s);

namespace {

static vec3 horizontalXZ(vec3 v) { v.y = 0; return v; }

struct Api {
  bool gl = false, ready = false, decorDone = false;
  int w = 1, h = 1; float dpr = 1;
  WorldScene scene;
  Json world, summary, catalogJson; std::string map;
  SimState state; double timeScale = 1;
  // camera
  OrbitCamera orbit; CameraRig rig; Compass compass; std::string subjectId;
  mat4 viewProj; bool viewValid = false;
  // input
  double mx = 0, my = 0, mxPrev = 0, myPrev = 0; bool left = false, right = false, ctrl = false;
  bool keyW = false, keyA = false, keyS = false, keyD = false, shift = false, up = false, down = false, kl = false, kr = false;
  int compassClickFrames = 0; float compassX = 0, compassY = 0; bool compassGlide = false;
  // hover
  std::string hoverId; bool hoverSignal = false; vec3 hoverPos;
  // assets
  std::set<std::string> decorPending; int tilesPending = 0; bool terrainDone = false;
  int modelsPending = 0;
  // texture stream
  std::string incomingSlot; Image incoming; ImageVariant incomingVar; int incomingIndex = -1;
  // stats
  double fps = 0, fpsAccum = 0; int fpsFrames = 0; int frame = 0;
  std::string strBuf;
};
Api* g = nullptr;

void request(const char* kind, const std::string& path) {
#ifdef __EMSCRIPTEN__
  api_request(kind, path.c_str());
#else
  std::printf("[ayana] request %s %s\n", kind, path.c_str());
#endif
}

vec3 camEye() { return g->rig.mode != CamMode::Bebas ? g->rig.eye() : g->orbit.position(); }
mat4 camView() { return g->rig.mode != CamMode::Bebas ? g->rig.view() : g->orbit.view(); }
mat4 camProj(float aspect) { return g->rig.mode != CamMode::Bebas ? g->rig.projection(aspect) : g->orbit.projection(aspect); }
float camFovY() { return g->rig.mode != CamMode::Bebas ? radians(g->rig.fovDeg()) : g->orbit.fovY; }

// Subject train polyline (nose first, coupler points on the rail head): selected/followed train, else the
// nearest to the camera (dunia3d.ts subjek). Same as Game::subjectPath.
bool subjectPath(TrainPath& out, std::string* idOut = nullptr) {
  const SimState& st = g->state;
  if (st.trains.empty()) return false;
  const SimTrain* t = nullptr;
  for (const SimTrain& x : st.trains) if (!g->subjectId.empty() && x.id == g->subjectId) t = &x;
  if (!t) {
    vec3 eye = camEye(); float bd = 1e30f;
    for (const SimTrain& x : st.trains) { vec3 p = g->scene.origin().toScene(x.x, x.y, 0); float d = (p.x - eye.x) * (p.x - eye.x) + (p.z - eye.z) * (p.z - eye.z); if (d < bd) { bd = d; t = &x; } }
  }
  if (!t || t->vehicles.empty()) return false;
  out = TrainPath{};
  out.sarana = t->vehicles[0].sarana;
  float acc = 0;
  auto push = [&](double wx, double wy, const std::string& seg, float s) {
    vec3 p = g->scene.origin().toScene(wx, wy, g->scene.profile().railHeight(seg.c_str(), s));
    if (!out.pts.empty()) acc += length(horizontalXZ(p - out.pts.back()));
    out.pts.push_back(p); out.cum.push_back(acc);
  };
  for (const SimVehicle& v : t->vehicles) { push(v.x1, v.y1, v.seg, v.s); push(v.x2, v.y2, v.seg2, v.s2); }
  out.length = t->length > 0 ? t->length : acc;
  if (idOut) *idOut = t->id;
  return out.valid();
}

void updateCamera(float dt) {
  if (g->rig.mode == CamMode::Bebas) { g->rig.step(dt, nullptr, {}, {}); return; }
  auto ground = [](float x, float z) { return g->scene.groundScene(x, z); };
  WalkInput in;
  if (g->rig.mode == CamMode::Jalan) {
    in.forward = (g->keyW || g->up ? 1.f : 0.f) - (g->keyS || g->down ? 1.f : 0.f);
    in.side = (g->keyD || g->kr ? 1.f : 0.f) - (g->keyA || g->kl ? 1.f : 0.f);
    in.run = g->shift;
  }
  TrainPath tp; bool has = subjectPath(tp);
  if (!g->rig.step(dt, has ? &tp : nullptr, ground, in)) eng_camera_mode(0);
}

// The static world: terrain data complete -> rails, signals, ... ; then the decor models are requested.
void buildStaticWorld() {
  if (g->ready) return;
  g->scene.terrain().finishTiles();
  std::string city;   // the baked city arrives through eng_city_json as a MEMFS file (may come later: built then)
  FILE* f = std::fopen("/ayana-city.json", "rb");
  if (f) { std::fclose(f); city = "/ayana-city.json"; }
  if (!g->scene.buildStatic(g->world, g->map, "/assets/font.efnt", city, [](const std::string& s) { std::printf("[ayana] %s\n", s.c_str()); })) {
    std::fprintf(stderr, "[ayana] world build failed\n"); return;
  }
  vec3 st = g->scene.stationScene();
  g->orbit.target = st; g->orbit.distance = 160; g->orbit.pitch = radians(18); g->orbit.yaw = radians(35);
  g->orbit.near = 1; g->orbit.far = 40000; g->orbit.fovY = radians(52);
  g->compass.init([](float x, float z) { return g->scene.groundScene(x, z); });
  g->ready = true;
  // decor models: hiasan objects, garis classes, trees. catalog.model() files the requests; ids still missing
  // are awaited before buildDecor (a failed one just drops out).
  AssetCatalog& cat = g->scene.catalog();
  auto want = [&](const std::string& id) { if (id.empty()) return; cat.model(id); if (cat.pending(id)) g->decorPending.insert(id); };
  for (const Json& o : g->world["hiasan"]["objek"].arr) want(o["model"].stringOr(""));
  for (const Json& gsp : g->world["hiasan"]["garis"].arr) {
    const GarisEntry* k = cat.findGaris(gsp["kelas"].stringOr(""));
    if (!k) continue;
    if (!k->berkas.empty()) want("garis:" + k->id);
    if (!k->tiang.empty()) want("garis:" + k->id + ":tiang");
    for (size_t i = 0; i < k->slot.size(); ++i) if (!k->slot[i].berkas.empty()) want("garis:" + k->id + ":slot" + std::to_string(i));
  }
  for (const std::string& id : cat.idsByCategory("vegetasi")) want(id);
  if (g->decorPending.empty()) { g->scene.buildDecor(g->world, false, &g->summary); g->decorDone = true; }
  std::printf("[ayana] static world ready, %zu decor models pending\n", g->decorPending.size());
}

void afterModel(const std::string& slot) {
  g->scene.stock().forget();
  if (g->decorPending.erase(slot) && g->decorPending.empty() && g->ready && !g->decorDone) {
    g->scene.buildDecor(g->world, false, &g->summary); g->decorDone = true;
    std::printf("[ayana] %s\n", g->scene.stats().summary.c_str());
  } else if (g->decorDone && g->ready) {
    // a hiasan/garis/tree model that arrived late (or a re-request): rebuild the decor so it shows
    bool decor = false;
    for (const Json& o : g->world["hiasan"]["objek"].arr) if (o["model"].stringOr("") == slot) decor = true;
    if (slot.rfind("garis:", 0) == 0) decor = true;
    const CatalogEntry* e = g->scene.catalog().find(slot); if (e && e->kategori == "vegetasi") decor = true;
    if (decor) g->scene.buildDecor(g->world, false, &g->summary);
  }
}

const char* ret(std::string s) { g->strBuf = std::move(s); return g->strBuf.c_str(); }

} // namespace

// JSON string escaping (SimProcess::escape is native-only).
static std::string SimProcessEscapeShim(const std::string& s) {
  std::string o; o.reserve(s.size() + 2);
  for (char c : s) {
    switch (c) {
      case '"': o += "\\\""; break; case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break; case '\r': o += "\\r"; break; case '\t': o += "\\t"; break;
      default: if ((unsigned char)c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; } else o += c;
    }
  }
  return o;
}

extern "C" {

#ifdef __EMSCRIPTEN__
#define KEEP EMSCRIPTEN_KEEPALIVE
#else
#define KEEP
#endif

KEEP int eng_init(int width, int height, float dpr) {
  if (!g) g = new Api();
  if (g->gl) { eng_resize(width, height, dpr); return 1; }
#ifdef __EMSCRIPTEN__
  api_installHeap();
  EmscriptenWebGLContextAttributes attr; emscripten_webgl_init_context_attributes(&attr);
  attr.majorVersion = 2; attr.minorVersion = 0; attr.antialias = true; attr.alpha = false; attr.depth = true; attr.stencil = false;
  attr.powerPreference = EM_WEBGL_POWER_PREFERENCE_HIGH_PERFORMANCE; attr.enableExtensionsByDefault = true;
  EMSCRIPTEN_WEBGL_CONTEXT_HANDLE ctx = emscripten_webgl_create_context("#ayana-canvas", &attr);
  if (ctx <= 0) { std::fprintf(stderr, "[ayana] WebGL2 context failed (%d)\n", (int)ctx); return 0; }
  emscripten_webgl_make_context_current(ctx);
#endif
  rhi::init();
  rhi::setAnisotropy(8);
  g->scene.initGpu();
  g->gl = true;
  eng_resize(width, height, dpr);
  return 1;
}

KEEP void eng_resize(int width, int height, float dpr) {
  if (!g) return;
  g->w = std::max(1, width); g->h = std::max(1, height); g->dpr = dpr > 0 ? dpr : 1;
#ifdef __EMSCRIPTEN__
  emscripten_set_canvas_element_size("#ayana-canvas", (int)(g->w * g->dpr), (int)(g->h * g->dpr));
#endif
}

KEEP void eng_shutdown(void) {
  if (!g) return;
  g->compass.shutdown(); g->scene.destroy();
  delete g; g = nullptr;
}

KEEP int eng_load_world(const char* worldJson, const char* summaryJson, const char* mapSlug, const char* catalogJson) {
  if (!g || !g->gl) return 0;
  std::string err;
  g->world = Json::parse(worldJson ? worldJson : "", &err);
  if (!err.empty()) { std::fprintf(stderr, "[ayana] world: %s\n", err.c_str()); return 0; }
  g->summary = Json::parse(summaryJson ? summaryJson : "{}", &err);
  if (!err.empty()) { std::fprintf(stderr, "[ayana] summary: %s\n", err.c_str()); g->summary = Json{}; }
  g->map = mapSlug ? mapSlug : "";
  g->ready = g->decorDone = g->terrainDone = false; g->decorPending.clear(); g->tilesPending = 0;
  g->scene.setWorld(g->world);
  if (!g->scene.catalog().loadFromText(catalogJson ? catalogJson : "{}", [](const std::string& id, const std::string&) { ++g->modelsPending; request("model", id); }))
    std::fprintf(stderr, "[ayana] catalog: %s\n", g->scene.catalog().error().c_str());
  std::remove("/ayana-city.json");
  request("city", (g->map == "bks" ? "bekasi" : g->map) + ".json");
  request("terrain", g->map + "/index.json");
  return 1;
}

KEEP int eng_terrain_index(const uint8_t* bytes, int len) {
  if (!g) return 0;
  std::string err;
  Json idx = Json::parse(std::string_view((const char*)bytes, (size_t)std::max(0, len)), &err);
  if (!err.empty() || !g->scene.terrain().loadIndex(idx, err)) { std::fprintf(stderr, "[ayana] terrain index: %s\n", err.c_str()); return 0; }
  std::vector<Terrain::TileRef> tiles = g->scene.terrain().tilesWanted();
  g->tilesPending = (int)tiles.size();
  for (const Terrain::TileRef& t : tiles) request("terrain", g->map + "/" + t.path());
  if (g->tilesPending == 0) buildStaticWorld();
  return g->tilesPending;
}

KEEP int eng_terrain_tile(const char* dir, int z, int x, int y, const uint8_t* bytes, int len) {
  if (!g || !dir) return 0;
  std::span<const uint8_t> b(bytes, (size_t)std::max(0, len));
  bool ok = false; std::string err;
  if (!std::strcmp(dir, "dem")) ok = g->scene.terrain().addDemTile(z, x, y, b);
  else if (!std::strncmp(dir, "sat/", 4)) ok = g->scene.terrain().addSatTile(std::atoi(dir + 4), z, x, y, b, err);
  if (!ok && len > 0) std::fprintf(stderr, "[ayana] tile %s %d/%d/%d rejected: %s\n", dir, z, x, y, err.c_str());
  if (--g->tilesPending <= 0 && !g->ready) { g->tilesPending = 0; buildStaticWorld(); }
  return ok;
}

KEEP int eng_city_json(const uint8_t* bytes, int len) {
  if (!g || len <= 0) return 0;
  FILE* f = std::fopen("/ayana-city.json", "wb");
  if (!f) return 0;
  std::fwrite(bytes, 1, (size_t)len, f); std::fclose(f);
  if (g->ready) { auto ground = [](double wx, double wy) { return g->scene.groundHeight(wx, wy); }; g->scene.city().build("/ayana-city.json", g->scene.origin(), g->scene.graph(), ground); }
  return 1;
}

KEEP int eng_set_state(const char* stepJson) {
  if (!g) return 0;
  std::string err;
  Json j = Json::parse(stepJson ? stepJson : "{}", &err);
  if (!err.empty()) { std::fprintf(stderr, "[ayana] state: %s\n", err.c_str()); return 0; }
  g->timeScale = j["timeScale"].numberOr(g->timeScale);
  g->state = parseSimState(j);
  if (g->ready) g->scene.applyState(g->state, g->timeScale);
  return 1;
}

KEEP void eng_frame(float dt) {
  if (!g || !g->gl) return;
  dt = std::clamp(dt, 0.f, 0.1f);
  g->fpsAccum += dt; if (++g->fpsFrames >= 30) { g->fps = g->fpsAccum > 0 ? g->fpsFrames / g->fpsAccum : 0; g->fpsAccum = 0; g->fpsFrames = 0; }
  int w = (int)(g->w * g->dpr), h = (int)(g->h * g->dpr);
  rhi::setViewport(w, h);
  rhi::clear(0, 0, 0, 1);
  if (!g->ready) { ++g->frame; return; }
  // input: orbit drag with the left button (free camera), rig drag in the train modes; compass on the right button
  float dx = (float)(g->mx - g->mxPrev) * g->dpr, dy = (float)(g->my - g->myPrev) * g->dpr;
  if (g->left) { if (g->rig.mode != CamMode::Bebas) g->rig.drag(dx, dy); else g->orbit.rotate(dx, dy); }
  g->mxPrev = g->mx; g->myPrev = g->my;
  if (g->rig.mode == CamMode::Bebas && g->viewValid) {
    bool rmb = g->right;
    double cx = g->mx * g->dpr, cy = g->my * g->dpr;
    if (g->compassClickFrames > 0) { rmb = g->compassClickFrames > 1; cx = g->compassX * g->dpr; cy = g->compassY * g->dpr; --g->compassClickFrames; }
    else if (g->compassGlide) { rmb = true; cx = g->compassX * g->dpr; cy = g->compassY * g->dpr; }
    g->compass.update(g->orbit, cx, cy, w, h, rmb, g->ctrl, g->kl, g->kr, g->up, g->down, dt, g->viewProj.inverse());
  }
  updateCamera(dt);
  float aspect = (float)w / (float)h;
  mat4 view = camView(), proj = camProj(aspect);
  vec3 eye = camEye();
  g->viewProj = proj * view; g->viewValid = true;
  g->scene.draw(g->viewProj, view, eye, camFovY(), h, g->state.clock, dt, g->timeScale, g->rig.fogDensity(g->scene.worldWidth()), false);
  if (!g->hoverId.empty() && g->scene.objectPos(g->hoverId, g->hoverSignal, g->hoverPos)) g->scene.drawHoverRing(g->hoverPos, eye);
  { Frustum frustum(g->viewProj); g->scene.trains().draw(g->scene.renderer(), &frustum); }
  if (g->rig.mode == CamMode::Bebas) g->compass.draw(g->scene.renderer(), g->orbit);
  g->scene.renderer().flushTransparent();
  if (g->frame == 0) rhi::checkErrors("first frame");
  ++g->frame;
}

KEEP int eng_ready(void) { return g && g->ready; }

KEEP const char* eng_stats(void) {
  if (!g) return "{}";
  char b[512];
  std::snprintf(b, sizeof b, "{\"fps\":%.1f,\"drawCalls\":%u,\"culled\":%u,\"buildMs\":%.0f,\"ready\":%s,\"decor\":%s,\"tilesPending\":%d,\"decorPending\":%zu,\"modelsRequested\":%d,\"frame\":%d,\"summary\":\"%s\"}",
                g->fps, g->scene.renderer().drawCalls, g->scene.renderer().culled, g->scene.stats().buildMs, g->ready ? "true" : "false", g->decorDone ? "true" : "false",
                g->tilesPending, g->decorPending.size(), g->modelsPending, g->frame, SimProcessEscapeShim(g->scene.stats().summary).c_str());
  return ret(b);
}

// ---- camera ----
KEEP int eng_camera_mode(int mode) {
  if (!g || !g->ready) return 0;
  CamMode m = (CamMode)std::clamp(mode, 0, 5);
  if (m == g->rig.mode) return mode;
  TrainPath tp;
  if (m != CamMode::Bebas && m != CamMode::Jalan && !subjectPath(tp)) return (int)g->rig.mode;
  vec3 eye = camEye(), look = g->rig.mode != CamMode::Bebas ? g->rig.look() : g->orbit.target;
  CamMode old = g->rig.mode;
  if (m == CamMode::Bebas) {
    if (old == CamMode::Jalan) look = eye + normalize(look - eye) * 60;
    vec3 o = eye - look; float d = std::fmax(length(o), 2.f);
    g->orbit.target = look; g->orbit.distance = d; g->orbit.pitch = std::asin(std::clamp(o.y / d, -0.999f, 0.999f)); g->orbit.yaw = std::atan2(o.x, o.z);
  }
  g->rig.setMode(m, eye, look);
  if (m == CamMode::Jalan) g->rig.enterWalk(eye, look, [](float x, float z) { return g->scene.groundScene(x, z); });
  return (int)m;
}
KEEP int eng_get_camera_mode(void) { return g ? (int)g->rig.mode : 0; }
KEEP void eng_orbit(float dx, float dy) { if (!g) return; if (g->rig.mode != CamMode::Bebas) g->rig.drag(dx, dy); else g->orbit.rotate(dx, dy); }
KEEP void eng_zoom(float steps) { if (!g) return; if (g->rig.mode != CamMode::Bebas) g->rig.scroll(steps); else g->orbit.zoom(steps); }
KEEP void eng_set_view(float distance, float yaw, float pitch) { if (!g) return; g->orbit.distance = distance; g->orbit.yaw = yaw; g->orbit.pitch = pitch; }
KEEP void eng_look_at(double wx, double wy) {
  if (!g || !g->ready) return;
  vec3 p = g->scene.origin().toScene(wx, wy, 0); p.y = g->scene.groundScene(p.x, p.z);
  g->compass.jumpTo(g->orbit, p);
}
KEEP void eng_compass_click(float x, float y) { if (!g) return; g->compassX = x; g->compassY = y; g->compassClickFrames = 3; }
KEEP void eng_compass_glide(float x, float y, int on) { if (!g) return; g->compassX = x; g->compassY = y; g->compassGlide = on != 0; }
KEEP void eng_follow_train(const char* id) {
  if (!g) return;
  g->subjectId = id ? id : "";
  if (g->subjectId.empty() || !g->ready) return;
  for (const SimTrain& t : g->state.trains) if (t.id == g->subjectId || t.no == g->subjectId) {
    g->subjectId = t.id;
    if (g->rig.mode == CamMode::Bebas) eng_look_at(t.x, t.y);
  }
}
KEEP void eng_cycle_subject(int dir) {
  if (!g || g->state.trains.empty()) return;
  std::string cur; TrainPath tp; subjectPath(tp, &cur);
  int i = -1; for (size_t k = 0; k < g->state.trains.size(); ++k) if (g->state.trains[k].id == cur) i = (int)k;
  g->subjectId = g->state.trains[(size_t)((i + dir + (int)g->state.trains.size()) % (int)g->state.trains.size())].id;
}
KEEP void eng_telescope(int held) { if (g) g->rig.teropongTahan = held != 0; }
KEEP void eng_key(const char* code, int down) {
  if (!g || !code) return;
  std::string c = code; bool d = down != 0;
  if (c == "KeyW") g->keyW = d; else if (c == "KeyA") g->keyA = d; else if (c == "KeyS") g->keyS = d; else if (c == "KeyD") g->keyD = d;
  else if (c == "ShiftLeft" || c == "ShiftRight") g->shift = d;
  else if (c == "ArrowUp") g->up = d; else if (c == "ArrowDown") g->down = d; else if (c == "ArrowLeft") g->kl = d; else if (c == "ArrowRight") g->kr = d;
  else if (c == "ControlLeft" || c == "ControlRight" || c == "MetaLeft") g->ctrl = d;
}
KEEP void eng_pointer(float x, float y, int button, int phase) {
  if (!g) return;
  g->mx = x; g->my = y;
  if (phase == 0) { if (button == 0) { g->left = true; g->mxPrev = x; g->myPrev = y; } else if (button == 1) g->right = true; }
  else if (phase == 2) { if (button == 0) g->left = false; else if (button == 1) g->right = false; }
}
KEEP const char* eng_camera_json(void) {
  if (!g) return "{}";
  vec3 e = camEye(); vec3 l = g->rig.mode != CamMode::Bebas ? g->rig.look() : g->orbit.target;
  char b[300];
  std::snprintf(b, sizeof b, "{\"mode\":%d,\"eye\":[%.2f,%.2f,%.2f],\"look\":[%.2f,%.2f,%.2f],\"distance\":%.2f,\"yaw\":%.4f,\"pitch\":%.4f,\"fov\":%.2f,\"azimuth\":%d}",
                (int)g->rig.mode, e.x, e.y, e.z, l.x, l.y, l.z, g->orbit.distance, g->orbit.yaw, g->orbit.pitch, degrees(camFovY()), g->ready ? g->compass.azimuth(g->orbit) : 0);
  return ret(b);
}

// ---- picking ----
KEEP const char* eng_pick(float x, float y) {
  if (!g || !g->ready || !g->viewValid) return "";
  std::string sig, pt;
  g->scene.pickAt(x * g->dpr, y * g->dpr, (int)(g->w * g->dpr), (int)(g->h * g->dpr), g->viewProj, camEye(), sig, pt);
  if (!sig.empty()) return ret("signal:" + sig);
  if (!pt.empty()) return ret("point:" + pt);
  return "";
}
KEEP void eng_hover(const char* id) {
  if (!g) return;
  std::string s = id ? id : "";
  if (s.rfind("signal:", 0) == 0) { g->hoverId = s.substr(7); g->hoverSignal = true; }
  else if (s.rfind("point:", 0) == 0) { g->hoverId = s.substr(6); g->hoverSignal = false; }
  else { g->hoverId = s; g->hoverSignal = g->ready && g->scene.signals().indexOf(s) >= 0; }
  if (g->hoverId.empty()) g->scene.routes().clearPreview();
}
KEEP const char* eng_object_screen(const char* id) {
  if (!g || !g->ready || !g->viewValid || !id) return "";
  std::string s = id; bool signal = true;
  if (s.rfind("signal:", 0) == 0) s = s.substr(7); else if (s.rfind("point:", 0) == 0) { s = s.substr(6); signal = false; } else signal = g->scene.signals().indexOf(s) >= 0;
  vec3 p; if (!g->scene.objectPos(s, signal, p)) return "";
  vec4 c = g->viewProj * vec4(p + vec3{0, signal ? 4.5f : 3.f, 0}, 1);
  if (c.w <= 0) return "";
  char b[64]; std::snprintf(b, sizeof b, "%.1f,%.1f", (c.x / c.w * 0.5f + 0.5f) * g->w, (1 - (c.y / c.w * 0.5f + 0.5f)) * g->h);
  return ret(b);
}
KEEP int eng_set_preview(const char* json) {
  if (!g || !g->ready) return 0;
  std::string err; Json j = Json::parse(json && *json ? json : "null", &err);
  if (!j.isObject()) { g->scene.routes().clearPreview(); return 1; }
  std::vector<std::string> path; for (const Json& sg : j["path"].arr) path.push_back(sg.stringOr(""));
  if (path.empty()) g->scene.routes().clearPreview(); else g->scene.routes().setPreview(path, j["dead"].boolOr(false));
  return 1;
}
KEEP const char* eng_train_screen(void) {
  if (!g || !g->ready || !g->viewValid) return "[]";
  std::string out = "["; vec3 eye = camEye();
  for (const TrainLabel& l : g->scene.trains().labels()) {
    vec4 c = g->viewProj * vec4(l.anchor, 1);
    if (c.w <= 0) continue;
    float x = (c.x / c.w * 0.5f + 0.5f) * g->w, y = (1 - (c.y / c.w * 0.5f + 0.5f)) * g->h;
    if (x < -50 || y < -50 || x > g->w + 50 || y > g->h + 50) continue;
    char b[320];
    std::snprintf(b, sizeof b, "%s{\"id\":\"%s\",\"no\":\"%s\",\"name\":\"%s\",\"state\":\"%s\",\"speed\":%.1f,\"x\":%.1f,\"y\":%.1f,\"d\":%.0f}", out.size() > 1 ? "," : "",
                  SimProcessEscapeShim(l.id).c_str(), SimProcessEscapeShim(l.no).c_str(), SimProcessEscapeShim(l.name).c_str(), SimProcessEscapeShim(l.state).c_str(), l.speed, x, y, length(l.anchor - eye));
    out += b;
  }
  return ret(out + "]");
}

// ---- assets ----
KEEP void* eng_alloc(int bytes) { return bytes > 0 ? std::malloc((size_t)bytes) : nullptr; }
KEEP void eng_free(void* p) { std::free(p); }
KEEP int eng_model_begin(const char* slot, const uint8_t* bytes, int len) {
  if (!g || !slot) return 0;
  std::string err;
  bool ok = g->scene.catalog().provide(slot, std::span<const uint8_t>(bytes, (size_t)std::max(0, len)), err);
  if (!ok) std::fprintf(stderr, "[ayana] model %s: %s\n", slot, err.c_str());
  afterModel(slot);
  return ok;
}
KEEP void eng_model_fail(const char* slot) { if (!g || !slot) return; g->scene.catalog().fail(slot); afterModel(slot); }

static const rhi::Format kFormatMap[] = {rhi::Format::RGBA8, rhi::Format::ETC2_RGB, rhi::Format::ETC2_RGBA, rhi::Format::BC1, rhi::Format::BC3, rhi::Format::BC7};
KEEP int eng_supports(int format) { return g && g->gl && format >= 0 && format <= 5 && rhi::supports(kFormatMap[format]); }
KEEP int eng_image_count(const char* slot) { const auto* h = g && slot ? g->scene.catalog().imageHints(slot) : nullptr; return h ? (int)h->size() : 0; }
KEEP int eng_image_source(const char* slot, int i) { const auto* h = g && slot ? g->scene.catalog().imageHints(slot) : nullptr; return h && i >= 0 && i < (int)h->size() ? (*h)[(size_t)i].source : -1; }
KEEP int eng_image_flags(const char* slot, int i) {
  const auto* h = g && slot ? g->scene.catalog().imageHints(slot) : nullptr;
  if (!h || i < 0 || i >= (int)h->size()) return 0;
  const AssetCatalog::ImageHint& im = (*h)[(size_t)i];
  return im.wrapS | im.wrapT << 8 | (im.linear ? 1 : 0) << 16 | (im.placeholder ? 1 : 0) << 17;
}
KEEP int eng_texture_begin(const char* slot, int image, int width, int height, int format, int mipCount, int srgb, int wrapS, int wrapT) {
  if (!g || !slot) return 0;
  GpuModel* m = g->scene.catalog().streamedModel(slot);
  if (!m || image < 0 || image >= (int)m->textures.size() || format < 0 || format > 5 || mipCount < 1 || width < 1 || height < 1) return 0;
  g->incomingSlot = slot; g->incomingIndex = image; g->incoming = {}; g->incomingVar = {};
  g->incoming.width = width; g->incoming.height = height; g->incoming.channels = 4;
  g->incoming.wrapS = (uint8_t)wrapS; g->incoming.wrapT = (uint8_t)wrapT; g->incoming.linear = !srgb;
  g->incomingVar.format = (TexFormat)format; g->incomingVar.mips.resize((size_t)mipCount);
  int w = width, h = height;
  for (MipLevel& l : g->incomingVar.mips) { l.width = w; l.height = h; w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1; }
  return 1;
}
KEEP int eng_texture_mip(int level, const uint8_t* data, int bytes) {
  if (!g || g->incomingIndex < 0 || level < 0 || level >= (int)g->incomingVar.mips.size() || !data || bytes < 0) return 0;
  MipLevel& l = g->incomingVar.mips[(size_t)level];
  if ((size_t)bytes != texLevelBytes(g->incomingVar.format, l.width, l.height)) { std::fprintf(stderr, "[ayana] texture mip %d: %d bytes, expected %zu\n", level, bytes, texLevelBytes(g->incomingVar.format, l.width, l.height)); return 0; }
  l.data.assign(data, data + bytes);
  return 1;
}
KEEP int eng_texture_end(void) {
  if (!g || g->incomingIndex < 0) return 0;
  int i = g->incomingIndex; g->incomingIndex = -1;
  GpuModel* m = g->scene.catalog().streamedModel(g->incomingSlot);
  if (!m || i >= (int)m->textures.size()) return 0;
  for (const MipLevel& l : g->incomingVar.mips) if (l.data.empty()) { std::fprintf(stderr, "[ayana] texture %d: missing mip level\n", i); return 0; }
  g->incoming.variants = {std::move(g->incomingVar)};
  rhi::Texture t = uploadImage(g->incoming);
  g->incoming = {}; g->incomingVar = {};
  if (!t.id) return 0;
  rhi::destroyTexture(m->textures[(size_t)i]); m->textures[(size_t)i] = t;
  return (int)t.id;
}

} // extern "C"

// See engine_api.h. Everything lives in one static `Api` object; the host is single-threaded (browser main
// thread), so no locking. Native builds compile this too (the GL context then has to exist already), which
// keeps the ABI honest and lets it be unit-tested without a browser.
#include "engine/api/engine_api.h"
#include "engine/api/engine_api_edit.h"
#include "engine/api/engine_api_markers.h"
#include "engine/api/engine_api_hud.h"
#include "engine/app/camera_rig.h"
#include "engine/app/compass.h"
#include "engine/app/world_scene.h"
#include "engine/core/orbit_camera.h"
#include "engine/sim/sim_state.h"
#include "engine/world/sun.h"
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
  SimState state; double timeScale = 1; bool paused = false;
  // look settings
  int quality = 0; bool qualityClouds = true; double skyTime = -1; bool dark = false, flatGround = false;
  // camera
  OrbitCamera orbit; CameraRig rig; Compass compass; std::string subjectId;
  mat4 viewProj; bool viewValid = false;
  // input
  double mx = 0, my = 0, mxPrev = 0, myPrev = 0; bool left = false, right = false, ctrl = false;
  bool keyW = false, keyA = false, keyS = false, keyD = false, keyQ = false, keyE = false, shift = false, up = false, down = false, kl = false, kr = false, space = false;
  PanelLayout panel;   // eng_set_panel: the in-world meja boards
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
  std::string strBuf, lastError;
};
Api* g = nullptr;
void setError(const std::string& e);

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
  out.id = t->id; out.speed = t->speed;
  if (idOut) *idOut = t->id;
  return out.valid();
}

void updateCamera(float dt) {
  if (g->rig.mode == CamMode::Bebas) { g->rig.step(dt, nullptr, {}, {}); return; }
  auto ground = [](float x, float z) { return g->scene.groundScene(x, z); };
  WalkInput in;
  if (g->rig.mode == CamMode::Jalan || g->rig.mode == CamMode::Kabin) {   // jalan = walk, kabin = move the eye
    in.forward = (g->keyW || g->up ? 1.f : 0.f) - (g->keyS || g->down ? 1.f : 0.f);
    in.side = (g->keyD || g->kr ? 1.f : 0.f) - (g->keyA || g->kl ? 1.f : 0.f);
    in.up = (g->keyE ? 1.f : 0.f) - (g->keyQ ? 1.f : 0.f);
    in.run = g->shift;
    in.jump = g->rig.mode == CamMode::Jalan && g->space;
  }
  std::vector<WalkBox> boxes;
  if (g->rig.mode == CamMode::Jalan) { g->scene.collectWalkBoxes(boxes); in.boxes = &boxes; in.mesh = &g->scene.walkCollider(); }
  TrainPath tp; bool has = subjectPath(tp);
  if (has) { tp.timeScale = g->paused ? 0 : g->timeScale; }
  if (!g->rig.step(dt, has ? &tp : nullptr, ground, in)) eng_camera_mode(0);
}

// The static world: terrain data complete -> rails, signals, ... ; then the decor models are requested.
void buildStaticWorld() {
  if (g->ready) return;
  g->scene.terrain().finishDem();
  std::string city;   // the baked city arrives through eng_city_json as a MEMFS file (may come later: built then)
  FILE* f = std::fopen("/ayana-city.json", "rb");
  if (f) { std::fclose(f); city = "/ayana-city.json"; }
  if (!g->scene.buildStatic(g->world, g->map, ENG_API_FONT, city, [](const std::string& s) { std::printf("[ayana] %s\n", s.c_str()); })) {
    setError("world build failed"); return;
  }
  vec3 st = g->scene.stationScene();
  g->orbit.target = st; g->orbit.distance = 160; g->orbit.pitch = radians(18); g->orbit.yaw = radians(35);
  g->orbit.near = 1; g->orbit.far = 40000; g->orbit.fovY = radians(52);
  g->compass.init([](float x, float z) { return g->scene.groundScene(x, z); });
  g->ready = true;
  g->scene.primeStreaming(st);   // every tile inside the radii is requested at once (the host limits the fetches)
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
void setError(const std::string& e) { g->lastError = e; std::fprintf(stderr, "[ayana] %s\n", e.c_str()); }

// Track-node bbox of the loaded world (terrain origin when there is no terrain index).
void worldBbox(double bb[4]) {
  bb[0] = bb[1] = 1e30; bb[2] = bb[3] = -1e30;
  for (const Json& n : g->world["graph"]["nodes"].arr) {
    double x = n["x"].numberOr(0), y = n["y"].numberOr(0);
    bb[0] = std::fmin(bb[0], x); bb[1] = std::fmin(bb[1], y); bb[2] = std::fmax(bb[2], x); bb[3] = std::fmax(bb[3], y);
  }
  if (bb[0] > bb[2]) bb[0] = bb[1] = bb[2] = bb[3] = 0;
}

// No usable terrain: flat ground, the static world is built right away.
void terrainNone(const std::string& why) {
  setError(why + " - continuing without terrain (flat ground)");
  double bb[4]; worldBbox(bb);
  g->scene.terrain().loadNone(bb);
  g->tilesPending = 0;
  buildStaticWorld();
}

} // namespace

// HUD projection bridge (engine_api_hud.cpp): the last drawn frame's view for the DOM overlays.
bool eng_hud_view(EngHudView& v) {
  if (!g || !g->ready || !g->viewValid) return false;
  v.scene = &g->scene; v.world = &g->world; v.viewProj = g->viewProj; v.eye = camEye(); v.w = g->w; v.h = g->h;
  return true;
}

// Marker overlay hooks (engine_api_markers.cpp): no-ops until that source is linked in.
__attribute__((weak)) void eng_markers_draw(ModelRenderer&, vec3, float, int) {}
__attribute__((weak)) void eng_markers_destroy(void) {}

// Editing bridge (engine_api_edit.cpp): the scene + the save object, edited in place.
bool eng_edit_ctx(EngEditCtx& c) {
  if (!g) return false;
  c.scene = &g->scene; c.world = &g->world; c.summary = &g->summary; c.map = g->map;
  c.viewProj = g->viewProj; c.viewValid = g->viewValid; c.eye = g->ready ? camEye() : vec3{};
  c.w = g->w; c.h = g->h; c.dpr = g->dpr; c.ready = g->ready;
  return true;
}

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
  eng_markers_destroy();
  g->compass.shutdown(); g->scene.destroy();
  delete g; g = nullptr;
}

KEEP int eng_load_world(const char* worldJson, const char* summaryJson, const char* mapSlug, const char* catalogJson) {
  if (!g || !g->gl) return 0;
  g->lastError.clear();
  std::string err;
  g->world = Json::parse(worldJson ? worldJson : "", &err);
  if (!err.empty()) { setError("world: " + err); return 0; }
  if (!g->world["graph"]["nodes"].size()) { setError("world: no graph.nodes"); return 0; }
  g->summary = Json::parse(summaryJson ? summaryJson : "{}", &err);
  if (!err.empty()) { std::fprintf(stderr, "[ayana] summary: %s\n", err.c_str()); g->summary = Json{}; }
  g->map = mapSlug ? mapSlug : "";
  g->ready = g->decorDone = g->terrainDone = false; g->decorPending.clear(); g->tilesPending = 0;
  g->scene.setWorld(g->world);
  g->panel = PanelLayout{}; g->scene.setPanelLayout(nullptr);
  if (!g->scene.catalog().loadFromText(catalogJson ? catalogJson : "{}", [](const std::string& id, const std::string&) { ++g->modelsPending; request("model", id); }))
    setError("catalog: " + g->scene.catalog().error());
  std::remove("/ayana-city.json");
  request("city", (g->map == "bks" ? "bekasi" : g->map) + ".json");
  request("terrain", g->map + "/index.json");
  return 1;
}

// A missing (bytes null / len 0) or invalid index degrades to "no terrain": the world is still built (flat
// ground at rail height), eng_last_error() says why, and the return is 0. Otherwise the DEM tiles are
// requested and the count returned (0 = nothing to wait for, the world is built already).
KEEP int eng_terrain_index(const uint8_t* bytes, int len) {
  if (!g || !g->world.isObject()) return 0;
  if (g->ready) { setError("terrain index: world already built"); return 0; }
  if (!bytes || len <= 0) { terrainNone("terrain index missing"); return 0; }
  std::string err;
  Json idx = Json::parse(std::string_view((const char*)bytes, (size_t)len), &err);
  if (!err.empty()) { terrainNone("terrain index: " + err); return 0; }
  if (!g->scene.terrain().loadIndex(idx, err)) { terrainNone("terrain index: " + err); return 0; }
  g->scene.terrain().setRequestFn([](const Terrain::TileRef& t) { request("terrain", g->map + "/" + t.path()); });
  std::vector<Terrain::TileRef> tiles = g->scene.terrain().demTilesWanted();
  g->tilesPending = (int)tiles.size();
  for (const Terrain::TileRef& t : tiles) request("terrain", g->map + "/" + t.path());
  if (g->tilesPending == 0) buildStaticWorld();
  return g->tilesPending;
}

KEEP const char* eng_last_error(void) { return g ? g->lastError.c_str() : ""; }

// A DEM answer (delivered or failed) counts down to the static build; satellite answers just stream in.
static void afterTile(const char* dir, bool ok, int z, int x, int y, const std::string& err) {
  if (!ok) { if (!err.empty()) std::fprintf(stderr, "[ayana] tile %s %d/%d/%d rejected: %s\n", dir, z, x, y, err.c_str()); g->scene.terrain().failTile({dir, z, x, y}); }
  if (!std::strcmp(dir, "dem") && --g->tilesPending <= 0 && !g->ready) { g->tilesPending = 0; buildStaticWorld(); }
}

KEEP int eng_terrain_tile(const char* dir, int z, int x, int y, const uint8_t* bytes, int len) {
  if (!g || !dir) return 0;
  std::span<const uint8_t> b(bytes, (size_t)std::max(0, len));
  bool ok = false; std::string err;
  if (!std::strcmp(dir, "dem")) ok = b.size() >= 4 && g->scene.terrain().provideDem(z, x, y, std::span<const float>((const float*)b.data(), b.size() / 4));
  else if (!std::strncmp(dir, "sat/", 4) && !b.empty()) ok = g->scene.terrain().provideSatFile(std::atoi(dir + 4), z, x, y, b, err);
  afterTile(dir, ok, z, x, y, err);
  return ok;
}

KEEP int eng_terrain_tile_rgba(const char* dir, int z, int x, int y, int w, int h, const uint8_t* rgba) {
  if (!g || !dir) return 0;
  bool ok = false; std::string err;
  if (!std::strcmp(dir, "dem")) ok = g->scene.terrain().provideDemRgba(z, x, y, w, h, rgba);
  else if (!std::strncmp(dir, "sat/", 4)) ok = g->scene.terrain().provideSatRgba(std::atoi(dir + 4), z, x, y, w, h, rgba, err);
  afterTile(dir, ok, z, x, y, err);
  return ok;
}

KEEP void eng_terrain_tile_fail(const char* dir, int z, int x, int y) { if (g && dir) afterTile(dir, false, z, x, y, ""); }

KEEP int eng_city_json(const uint8_t* bytes, int len) {
  if (!g || len <= 0) return 0;
  FILE* f = std::fopen("/ayana-city.json", "wb");
  if (!f) return 0;
  std::fwrite(bytes, 1, (size_t)len, f); std::fclose(f);
  if (g->ready) { auto ground = [](double wx, double wy) { return g->scene.groundHeight(wx, wy); }; g->scene.city().build("/ayana-city.json", g->scene.origin(), g->scene.graph(), ground); }
  return 1;
}

KEEP int eng_set_panel(const char* panelJson) {
  if (!g) return 0;
  std::string err;
  Json j = Json::parse(panelJson ? panelJson : "{}", &err);
  if (!err.empty()) { std::fprintf(stderr, "[ayana] panel: %s\n", err.c_str()); return 0; }
  g->panel = parsePanelLayout(j);
  g->scene.setPanelLayout(g->panel.ok ? &g->panel : nullptr);
  return g->panel.ok;
}

KEEP int eng_rail_atlas_rgba(int w, int h, const uint8_t* rgba) {
  if (!g || !g->gl || w < 1 || h < 1 || !rgba) return 0;
  rhi::Texture t = rhi::createTexture(w, h, rhi::Format::RGBA8, std::as_bytes(std::span(rgba, (size_t)w * h * 4)), true, true, rhi::Wrap::Clamp, rhi::Wrap::Repeat);
  if (!t.id) return 0;
  g->scene.rails().setAtlas(t);
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
  // LOD context (profilKam acuan): iconic rail line, wesel scale, glow detail; kabin hides the ribbons
  g->scene.refDistance = g->rig.acuan(g->orbit.distance);
  g->scene.cabView = g->rig.mode == CamMode::Kabin;
  g->scene.updateStreaming(g->rig.mode != CamMode::Bebas ? g->rig.look() : g->orbit.target, dt);
  if (g->rig.mode == CamMode::Bebas && g->ready) g->orbit.keepAboveGround([](float x, float z) { return g->scene.groundScene(x, z); });
  float aspect = (float)w / (float)h;
  mat4 view = camView(), proj = camProj(aspect);
  vec3 eye = camEye();
  g->viewProj = proj * view; g->viewValid = true;
  bool awan = g->scene.layers.awan; g->scene.layers.awan = awan && g->qualityClouds;
  g->scene.draw(g->viewProj, view, eye, camFovY(), h, g->skyTime >= 0 ? g->skyTime : g->state.clock, g->paused ? 0.f : dt, g->timeScale, g->rig.fogDensity(g->scene.worldWidth()), false);
  g->scene.layers.awan = awan;
  if (!g->hoverId.empty() && g->scene.objectPos(g->hoverId, g->hoverSignal, g->hoverPos)) g->scene.drawHoverRing(g->hoverPos, eye);
  { Frustum frustum(g->viewProj); g->scene.trains().draw(g->scene.renderer(), &frustum); }
  g->scene.drawOverlays(eye, camFovY());   // editor: selection box, ghost, ukur, gizmo
  eng_markers_draw(g->scene.renderer(), eye, camFovY(), h);   // editor markers (engine_api_markers.cpp)
  if (g->rig.mode == CamMode::Bebas) g->compass.draw(g->scene.renderer(), g->orbit);
  g->scene.renderer().flushTransparent();
  if (g->frame == 0) rhi::checkErrors("first frame");
  ++g->frame;
}

KEEP int eng_ready(void) { return g && g->ready; }
KEEP void eng_set_paused(int paused) { if (g) g->paused = paused != 0; }
KEEP int eng_set_layer(const char* name, int on) {
  if (!g || !name) return 0;
  std::string n = name; bool v = on != 0; SceneLayers& L = g->scene.layers;
  if (n == "pita") L.pita = v; else if (n == "wesel") L.wesel = v; else if (n == "pohon") L.pohon = v;
  else if (n == "awan") L.awan = v; else if (n == "kota") L.kota = v;
  else if (n == "benang") g->scene.benangAlways = v;   // iconic rail line forced at every distance
  else if (n == "label" || n == "papan" || n == "tepi" || n == "pelat") return 1;   // DOM-side layers
  else return 0;
  if (!L.wesel && !g->hoverSignal && !g->hoverId.empty()) g->hoverId.clear();   // a hidden arrow keeps no hover ring
  return 1;
}

// ---- quality / look ----
static void applyGroundColors() {
  // TEMA.hampar (backdrop plane) per theme; untextured tiles: WARNA_UBIN_MUAT while loading, hampar in the plain mode
  vec3 hampar = rgb(g->dark ? 0x151c24u : 0x9db089u);
  g->scene.terrain().setGroundColors(hampar, g->flatGround ? hampar : rgb(0x1a2027u));
}
static const float kTreeRadius[] = {3200, 2600, 2000, 1400, 900};   // TINGKAT_MUTU rVeg
KEEP int eng_set_quality(int tier) {
  if (!g) return 0;
  g->quality = std::clamp(tier, 0, 4);
  g->scene.trees().viewRadius = kTreeRadius[g->quality];
  g->qualityClouds = g->quality < 2;   // TINGKAT_MUTU awan
  return g->quality;
}
KEEP void eng_set_tree_radius(float metres) { if (g && metres > 0) g->scene.trees().viewRadius = metres; }
KEEP void eng_set_tree_density(float k) { if (g) g->scene.trees().setDensity(k); }
KEEP void eng_set_sky_time(double sec) { if (g) g->skyTime = sec; }
KEEP void eng_set_theme(int dark) { if (!g) return; g->dark = dark != 0; g->scene.sky().darkTheme = g->dark; applyGroundColors(); }
KEEP void eng_reset_imagery(int flat) {
  if (!g) return;
  g->flatGround = flat != 0; applyGroundColors();
  if (g->ready) g->scene.terrain().dropImagery();
}

KEEP const char* eng_stats(void) {
  if (!g) return "{}";
  char b[768];
  const Terrain::Stats& ts = g->scene.terrain().stats;
  std::snprintf(b, sizeof b, "{\"fps\":%.1f,\"drawCalls\":%u,\"culled\":%u,\"buildMs\":%.0f,\"ready\":%s,\"decor\":%s,\"tilesPending\":%d,\"decorPending\":%zu,\"modelsRequested\":%d,\"frame\":%d,"
                "\"terrain\":{\"near\":%d,\"far\":%d,\"patches\":%d,\"resident\":%d,\"requested\":%d,\"pendingJobs\":%d,\"trees\":%zu},\"summary\":\"%s\"}",
                g->fps, g->scene.renderer().drawCalls, g->scene.renderer().culled, g->scene.stats().buildMs, g->ready ? "true" : "false", g->decorDone ? "true" : "false",
                g->tilesPending, g->decorPending.size(), g->modelsPending, g->frame, ts.nearTiles, ts.farTiles, ts.patches, ts.resident, ts.requested, ts.pendingJobs, g->scene.trees().stats.trees,
                SimProcessEscapeShim(g->scene.stats().summary).c_str());
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
KEEP void eng_compass_settings(int rotation, float speed, int show) {
  if (!g) return;
  g->compass.settings.rotation = rotation != 0; g->compass.settings.speed = speed > 0 ? speed : 1; g->compass.settings.show = show != 0;
}
KEEP void eng_side_flip(void) { if (g) g->rig.sisiSamping = -g->rig.sisiSamping; }
// profilKam().atur per mode: {min, max} and the rig field
static float* rigParam(float& lo, float& hi) {
  switch (g->rig.mode) {
    case CamMode::Jalan: lo = 45; hi = 95; return &g->rig.fovJalan;
    case CamMode::Kabin: lo = 40; hi = 95; return &g->rig.fovKabin;
    case CamMode::Samping: lo = 8; hi = 160; return &g->rig.jarakSamping;
    case CamMode::Atas: lo = 40; hi = 2000; return &g->rig.tinggiAtas;
    case CamMode::Ekor: lo = 10; hi = 200; return &g->rig.jarakEkor;
    default: return nullptr;
  }
}
KEEP void eng_set_rig_param(float value) { if (!g) return; float lo, hi; if (float* p = rigParam(lo, hi)) *p = std::clamp(value, lo, hi); }
KEEP float eng_get_rig_param(void) { if (!g) return 0; float lo, hi; float* p = rigParam(lo, hi); return p ? *p : 0.f; }
KEEP void eng_telescope(int held) { if (g) g->rig.teropongTahan = held != 0; }
KEEP void eng_key(const char* code, int down) {
  if (!g || !code) return;
  std::string c = code; bool d = down != 0;
  if (c == "KeyW") g->keyW = d; else if (c == "KeyA") g->keyA = d; else if (c == "KeyS") g->keyS = d; else if (c == "KeyD") g->keyD = d;
  else if (c == "KeyQ") g->keyQ = d; else if (c == "KeyE") g->keyE = d;
  else if (c == "KeyR") { if (d && g->rig.mode == CamMode::Kabin) g->rig.resetKabin(); }   // cab eye back to MATA_KABIN
  else if (c == "ShiftLeft" || c == "ShiftRight") g->shift = d;
  else if (c == "ArrowUp") g->up = d; else if (c == "ArrowDown") g->down = d; else if (c == "ArrowLeft") g->kl = d; else if (c == "ArrowRight") g->kr = d;
  else if (c == "ControlLeft" || c == "ControlRight" || c == "MetaLeft") g->ctrl = d;
  else if (c == "Space") g->space = d;
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
  // subject = the train the rig follows / would follow: the explicit follow id, else (in a train mode) the nearest
  std::string subject = g->subjectId;
  if (subject.empty() && g->rig.mode != CamMode::Bebas && g->ready) { TrainPath tp; subjectPath(tp, &subject); }
  char b[400];
  std::snprintf(b, sizeof b, "{\"mode\":%d,\"eye\":[%.2f,%.2f,%.2f],\"look\":[%.2f,%.2f,%.2f],\"distance\":%.2f,\"yaw\":%.4f,\"pitch\":%.4f,\"fov\":%.2f,\"azimuth\":%d,\"subject\":\"%s\"}",
                (int)g->rig.mode, e.x, e.y, e.z, l.x, l.y, l.z, g->orbit.distance, g->orbit.yaw, g->orbit.pitch, degrees(camFovY()), g->ready ? g->compass.azimuth(g->orbit) : 0,
                SimProcessEscapeShim(subject).c_str());
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
  // Every train is reported (uji3dLabel.ts needs the off-screen ones for the edge markers): `front` = in front of
  // the camera; behind it x/y are the mirrored projection (three's Vector3.project with w < 0 - the side is what
  // matters there). `visible` = inside the viewport. ax/ay = the point 60 m ahead of the nose (screen heading glyph).
  auto proj = [&](vec3 p, float& x, float& y, bool& front) {
    vec4 c = g->viewProj * vec4(p, 1);
    front = c.w > 0; float w = std::fabs(c.w) > 1e-6f ? std::fabs(c.w) : 1e-6f;
    x = (c.x / w * 0.5f + 0.5f) * g->w; y = (1 - (c.y / w * 0.5f + 0.5f)) * g->h;
  };
  for (const TrainLabel& l : g->scene.trains().labels()) {
    float x, y, ax, ay; bool front, afront;
    proj(l.anchor, x, y, front);
    proj(l.anchor + l.dir * 60.f, ax, ay, afront);
    bool visible = front && x >= 0 && y >= 0 && x <= g->w && y <= g->h;
    char b[400];
    std::snprintf(b, sizeof b, "%s{\"id\":\"%s\",\"no\":\"%s\",\"name\":\"%s\",\"state\":\"%s\",\"speed\":%.1f,\"x\":%.1f,\"y\":%.1f,\"d\":%.0f,\"front\":%s,\"visible\":%s,\"ax\":%.1f,\"ay\":%.1f}", out.size() > 1 ? "," : "",
                  SimProcessEscapeShim(l.id).c_str(), SimProcessEscapeShim(l.no).c_str(), SimProcessEscapeShim(l.name).c_str(), SimProcessEscapeShim(l.state).c_str(), l.speed, x, y, length(l.anchor - eye),
                  front ? "true" : "false", visible ? "true" : "false", afront ? ax : x, afront ? ay : y);
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
KEEP int eng_model_begin_glb(const char* slot, const uint8_t* bytes, int len) {
  if (!g || !slot) return 0;
  std::string err;
  bool ok = g->scene.catalog().provideGlb(slot, std::span<const uint8_t>(bytes, (size_t)std::max(0, len)), err);
  if (!ok) std::fprintf(stderr, "[ayana] model %s (glb): %s\n", slot, err.c_str());
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
  if (i < (int)m->texturePending.size() && m->texturePending[(size_t)i]) { m->texturePending[(size_t)i] = 0; --m->texturesPending; }
  return (int)t.id;
}
KEEP void eng_model_textures_unavailable(const char* slot) {
  GpuModel* m = g && slot ? g->scene.catalog().streamedModel(slot) : nullptr;
  if (!m) return;
  m->texturesUnavailable = true;
  for (size_t i = 0; i < m->texturePending.size() && i < m->textures.size(); ++i) {
    if (!m->texturePending[i]) continue;
    // neutral grey (§7.1 loco fallback 0x9aa3ac) instead of the white placeholder; the wrap mode does not matter for 1x1
    const uint8_t px[4] = {0x9a, 0xa3, 0xac, 255};
    rhi::Texture t = rhi::createTexture(1, 1, rhi::Format::RGBA8, std::as_bytes(std::span(px)), false, true);
    if (!t.id) continue;
    rhi::destroyTexture(m->textures[i]); m->textures[i] = t;
    m->texturePending[i] = 0; --m->texturesPending;
  }
}

} // extern "C"

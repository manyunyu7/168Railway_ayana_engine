// WorldScene editing support (docs/SURVEYOR.md): picking against the heightfield / placed hiasan / rail
// centreline, live re-placement of single hiasan entries, garis / node-height / brush edits, the partial
// rebuilds behind them, and the editor overlays (selection box, gizmo, ghost, ukur polyline). The pure
// logic of the reference tools stays in the web client (uji3dTata.ts and friends); only what needed
// three.js (raycasts, gizmo meshes, highlight, dynamic transforms, re-carve, rail rebuild) lives here.
#include "engine/app/world_scene.h"
#include "engine/render/mesh_builder.h"
#include "engine/world/sun.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace eng {

namespace {
constexpr float TOL_PILIH = 16, TIPIS_PX = 16, LUAS_KECIL = 900;   // uji3dTata.ts screen-space pick tolerance
constexpr float GIZMO_HIT_IN = 0.7f, GIZMO_HIT_OUT = 1.3f;          // thick hit ring (cincinKena)
constexpr float KNOB_R = 0.1f;

bool project(const mat4& viewProj, vec3 p, int w, int h, float& x, float& y) {
  vec4 c = viewProj * vec4(p, 1);
  if (c.w <= 1e-6f) return false;
  x = (c.x / c.w * 0.5f + 0.5f) * (float)w; y = (1 - (c.y / c.w * 0.5f + 0.5f)) * (float)h;
  return true;
}
float distToBox(const float b[4], float x, float y) {
  float dx = std::fmax(std::fmax(b[0] - x, 0.f), x - b[2]), dy = std::fmax(std::fmax(b[1] - y, 0.f), y - b[3]);
  return std::hypot(dx, dy);
}
bool thinTarget(const float b[4]) { float w = b[2] - b[0], h = b[3] - b[1]; return std::fmin(w, h) < TIPIS_PX || w * h < LUAS_KECIL; }
float distToSegment(float ax, float ay, float bx, float by, float x, float y) {
  float dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
  float t = l2 > 0 ? std::clamp(((x - ax) * dx + (y - ay) * dy) / l2, 0.f, 1.f) : 0.f;
  return std::hypot(x - (ax + t * dx), y - (ay + t * dy));
}
// Ray vs a model instance: triangle-precise through the collision copy when present, else the primitive AABB.
bool rayModel(const Ray& ray, const GpuModel& m, const mat4& xf, float& tBest) {
  bool hit = false;
  for (size_t ni = 0; ni < m.nodes.size(); ++ni) {
    const Node& n = m.nodes[ni]; if (n.mesh < 0 || n.mesh >= (int)m.meshes.size()) continue;
    mat4 w = xf * m.world[ni];
    for (const GpuPrimitive& prim : m.meshes[(size_t)n.mesh].primitives) {
      float t;
      if (!intersect(ray, prim.bounds.transformed(w), t) || t >= tBest) continue;
      if (prim.collisionPos.empty()) { tBest = t; hit = true; continue; }
      const std::vector<vec3>& P = prim.collisionPos; const std::vector<uint32_t>& I = prim.collisionIdx;
      for (size_t i = 0; i + 2 < I.size(); i += 3) {
        if (I[i] >= P.size() || I[i + 1] >= P.size() || I[i + 2] >= P.size()) continue;
        if (intersect(ray, w.transformPoint(P[I[i]]), w.transformPoint(P[I[i + 1]]), w.transformPoint(P[I[i + 2]]), t) && t < tBest) { tBest = t; hit = true; }
      }
    }
  }
  return hit;
}
Json num(double v) { Json j; j.type = Json::Type::Number; j.num = v; return j; }
Json& ensureArray(Json& parent, const char* key) {
  if (parent.type != Json::Type::Object) parent.type = Json::Type::Object;
  Json& a = parent.obj[key]; if (a.type != Json::Type::Array) { a.type = Json::Type::Array; a.arr.clear(); }
  return a;
}
} // namespace

// ------------------------------------------------------------------ picking
bool WorldScene::rayGround(const Ray& ray, vec3& hit) const {
  float step = 4, t = 0;
  for (int i = 0; i < 4000; ++i) {
    t += step; vec3 p = ray.at(t);
    if (p.y <= groundScene(p.x, p.z)) {
      float a = t - step, b = t;
      for (int k = 0; k < 14; ++k) { float m = (a + b) / 2; vec3 q = ray.at(m); (q.y <= groundScene(q.x, q.z) ? b : a) = m; }
      hit = ray.at((a + b) / 2); return true;
    }
    step = std::fmin(step * 1.05f, 60);
    if (t > 60000) break;
  }
  return false;
}

bool WorldScene::hiasanScreenBox(int objIndex, const mat4& viewProj, int w, int h, float box[4]) const {
  for (const Placed& p : scenery_) {
    if (p.objIndex != objIndex) continue;
    box[0] = box[1] = 1e30f; box[2] = box[3] = -1e30f;
    const AABB& b = p.bounds;
    for (int c = 0; c < 8; ++c) {
      float x, y;
      if (!project(viewProj, {c & 1 ? b.max.x : b.min.x, c & 2 ? b.max.y : b.min.y, c & 4 ? b.max.z : b.min.z}, w, h, x, y)) return false;
      box[0] = std::fmin(box[0], x); box[1] = std::fmin(box[1], y); box[2] = std::fmax(box[2], x); box[3] = std::fmax(box[3], y);
    }
    return true;
  }
  return false;
}

int WorldScene::pickHiasan(const Ray& ray, float px, float py, int w, int h, const mat4& viewProj, vec3 eye) const {
  float tBest = 1e30f; int best = -1;
  for (const Placed& p : scenery_) if (rayModel(ray, *p.model, p.xf, tBest)) best = p.objIndex;
  if (best >= 0) return best;
  // screen-space help for thin / small targets (uji3dTata.ts objDiLayar): nearest box within TOL_PILIH, ties to the camera-nearest
  float dBest = TOL_PILIH, dCam = 1e30f;
  for (const Placed& p : scenery_) {
    float b[4]; if (!hiasanScreenBox(p.objIndex, viewProj, w, h, b) || !thinTarget(b)) continue;
    float d = distToBox(b, px, py); if (d > TOL_PILIH) continue;
    float dk = length(p.bounds.center() - eye);
    if (d < dBest - 0.5f || (std::fabs(d - dBest) <= 0.5f && dk < dCam)) { best = p.objIndex; dBest = d; dCam = dk; }
  }
  return best;
}

bool WorldScene::pickTrack(float px, float py, int w, int h, const mat4& viewProj, float maxPx, int& seg, double& s, int& side) const {
  float best = maxPx; seg = -1;
  for (size_t si = 0; si < graph_.segments.size(); ++si) {
    const TrackSegment& sg = graph_.segments[si];
    float ax = 0, ay = 0; bool aOk = false;
    for (size_t i = 0; i < sg.lut.size(); ++i) {
      const TrackSegment::Lut& l = sg.lut[i];
      float bx, by;
      bool bOk = project(viewProj, origin_.toScene(l.px, l.py, profile_.railHeight((int)si, l.s)), w, h, bx, by);
      if (aOk && bOk) {
        float d = distToSegment(ax, ay, bx, by, px, py);
        if (d < best) {
          best = d; seg = (int)si;
          float dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
          float t = l2 > 0 ? std::clamp(((px - ax) * dx + (py - ay) * dy) / l2, 0.f, 1.f) : 0.f;
          s = sg.lut[i - 1].s + (l.s - sg.lut[i - 1].s) * t;
          side = (dx * (py - ay) - dy * (px - ax)) < 0 ? 1 : -1;   // screen y is down: negative cross = cursor left of the tangent
        }
      }
      ax = bx; ay = by; aOk = bOk;
    }
  }
  return seg >= 0;
}

int WorldScene::pickGaris(const Json& world, float px, float py, int w, int h, const mat4& viewProj, float maxPx) const {
  float best = maxPx; int found = -1;
  const Json& arr = world["hiasan"]["garis"];
  for (size_t gi = 0; gi < arr.size(); ++gi) {
    const Json& pts = arr[gi]["titik"];
    float ax = 0, ay = 0; bool aOk = false;
    for (size_t i = 0; i < pts.size(); ++i) {
      double wx = pts[i]["x"].numberOr(0), wy = pts[i]["y"].numberOr(0);
      float bx, by; bool bOk = project(viewProj, origin_.toScene(wx, wy, terrain_.groundHeight(wx, wy) + 1), w, h, bx, by);
      if (aOk && bOk) { float d = distToSegment(ax, ay, bx, by, px, py); if (d < best) { best = d; found = (int)gi; } }
      else if (bOk && pts.size() == 1) { float d = std::hypot(bx - px, by - py); if (d < best) { best = d; found = (int)gi; } }
      ax = bx; ay = by; aOk = bOk;
    }
  }
  return found;
}

// ------------------------------------------------------------------ hiasan
bool WorldScene::hiasanPlace(int objIndex, const Json& o) {
  auto it = std::find_if(scenery_.begin(), scenery_.end(), [&](const Placed& p) { return p.objIndex == objIndex; });
  GpuModel* m = catalog_.model(o["model"].stringOr(""));
  if (!m) { if (it != scenery_.end()) scenery_.erase(it); return false; }   // missing / not streamed yet
  double wx = o["x"].numberOr(0), wy = o["y"].numberOr(0);
  vec3 p = origin_.toScene(wx, wy, terrain_.groundHeight(wx, wy) + (float)o["naik"].numberOr(0));
  float yaw = radians((float)o["rot"].numberOr(0)), sc = (float)o["skala"].numberOr(1);
  mat4 xf = mat4::translation(p) * mat4::rotationY(yaw) * mat4::scale({sc, sc, sc}) * RollingStock::normalizeTransform(*m, true);
  Placed pl{m, xf, m->bounds.transformed(xf), objIndex};
  if (it != scenery_.end()) *it = pl; else scenery_.push_back(pl);
  return true;
}

bool WorldScene::hiasanSet(Json& world, int objIndex, double wx, double wy, float naik, float rotDeg, float skala) {
  Json& objs = ensureArray(world.obj["hiasan"], "objek");
  if (objIndex < 0 || objIndex >= (int)objs.size()) return false;
  Json& o = objs.arr[(size_t)objIndex];
  o.obj["x"] = num(wx); o.obj["y"] = num(wy); o.obj["naik"] = num(naik); o.obj["rot"] = num(rotDeg); o.obj["skala"] = num(skala);
  hiasanPlace(objIndex, o);
  walkDirty_ = true;
  scanPapan(world);
  return true;
}

int WorldScene::hiasanAdd(Json& world, const Json& obj) {
  if (!obj.isObject()) return -1;
  Json& objs = ensureArray(world.obj["hiasan"], "objek");
  objs.arr.push_back(obj);
  int idx = (int)objs.size() - 1;
  hiasanPlace(idx, objs.arr.back());   // false when the model is not resident yet: shown once it lands (buildDecor re-run)
  walkDirty_ = true;
  scanPapan(world);
  return idx;
}

bool WorldScene::hiasanRemove(Json& world, int objIndex) {
  Json& objs = ensureArray(world.obj["hiasan"], "objek");
  if (objIndex < 0 || objIndex >= (int)objs.size()) return false;
  objs.arr.erase(objs.arr.begin() + objIndex);
  scenery_.erase(std::remove_if(scenery_.begin(), scenery_.end(), [&](const Placed& p) { return p.objIndex == objIndex; }), scenery_.end());
  for (Placed& p : scenery_) if (p.objIndex > objIndex) --p.objIndex;
  if (ov_.hlKind == "hiasan") { if (ov_.hlIndex == objIndex) ov_.hlMode = HighlightMode::Off; else if (ov_.hlIndex > objIndex) --ov_.hlIndex; }
  walkDirty_ = true;
  scanPapan(world);
  return true;
}

void WorldScene::hiasanRefresh(const Json& world) {
  const Json& objs = world["hiasan"]["objek"];
  for (size_t i = 0; i < objs.size(); ++i) hiasanPlace((int)i, objs[i]);
  walkDirty_ = true;
  scanPapan(world);
}

bool WorldScene::hiasanText(Json& world, int objIndex, const std::string& teks, const std::string& ketinggian) {
  Json& objs = ensureArray(world.obj["hiasan"], "objek");
  if (objIndex < 0 || objIndex >= (int)objs.size()) return false;
  Json& o = objs.arr[(size_t)objIndex];
  auto set = [&](const char* key, const std::string& v) { if (v.empty()) o.obj.erase(key); else { Json j; j.type = Json::Type::String; j.str = v; o.obj[key] = j; } };
  set("teks", teks); set("ketinggian", ketinggian);
  scanPapan(world);
  return true;
}

bool WorldScene::hiasanHasBoard(int objIndex) const {
  for (const Placed& p : scenery_) if (p.objIndex == objIndex) return NameBoards::hasBoard(*p.model);
  return false;
}

// ------------------------------------------------------------------ garis / node height / rails / terrain
bool WorldScene::garisSet(Json& world, const Json& patch) {
  if (!patch.isObject()) return false;
  Json& arr = ensureArray(world.obj["hiasan"], "garis");
  int idx = patch["index"].intOr(-1);
  if (patch["remove"].boolOr(false)) {
    if (idx < 0 || idx >= (int)arr.size()) return false;
    arr.arr.erase(arr.arr.begin() + idx);
  } else {
    Json entry = patch; entry.obj.erase("index"); entry.obj.erase("remove");
    if (idx < 0 || idx >= (int)arr.size()) arr.arr.push_back(entry); else arr.arr[(size_t)idx] = entry;
  }
  auto ground = [this](double wx, double wy) { return terrain_.groundHeight(wx, wy); };
  garis_.destroy();
  garis_.build(world["hiasan"], catalog_, origin_, ground);
  walkDirty_ = true;
  return true;
}

bool WorldScene::nodeHeight(Json& world, const std::string& nodeId, double h, bool has) {
  int ni = graph_.nodeIndex(nodeId);
  if (ni < 0) return false;
  TrackNode& n = graph_.nodes[(size_t)ni];
  n.hasHeight = has; n.height = has ? h : 0;
  for (Json& j : world.obj["graph"].obj["nodes"].arr) {
    if (j["id"].stringOr("") != nodeId) continue;
    if (has) j.obj["y"] = num(h); else j.obj.erase("y");
    return true;
  }
  return true;   // graph updated even when the save lacks the node (test worlds)
}

double WorldScene::railsRebuild(const Json& world, const std::string& fontPath) {
  auto t0 = std::chrono::steady_clock::now();
  std::vector<StationZone> stations;
  for (const Json& sc : world["scenery"].arr)
    if (sc["kind"].stringOr("") == "station") stations.push_back({sc["pos"]["x"].numberOr(0), sc["pos"]["y"].numberOr(0), 160});
  const float demBase = terrain_.dem().demBase;
  profile_.build(graph_, terrain_.dem(), stations, demBase);
  rails_.build(graph_, profile_, &terrain_.dem(), demBase);
  terrain_.setRails(rails_.samples());   // marks the built tiles dirty: re-cut under the per-frame budget
  signals_.build(graph_, profile_, world["trackside"], fontPath);
  points_.build(graph_, profile_);
  routes_.init(&graph_, &profile_);
  auto ground = [this](double wx, double wy) { return terrain_.groundHeight(wx, wy); };
  boards_.build(graph_, profile_, world, origin_, ground, fontPath);
  jpl_.build(graph_, world, origin_, ground);
  if (decor_) hiasanRefresh(world);
  ov_.segMeshId.clear();   // the highlight ribbon follows the new profile
  applyState(state_, 1);
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

bool WorldScene::terrainDelta(Json& world, const Json& tanah) {
  if (tanah.isObject()) world.obj["tanah"] = tanah; else world.obj.erase("tanah");
  std::vector<std::pair<double, double>> changed = terrain_.applyBrushDeltas(world["tanah"]);
  if (changed.empty()) return true;
  if (decor_) {
    hiasanRefresh(world);
    auto ground = [this](double wx, double wy) { return terrain_.groundHeight(wx, wy); };
    garis_.destroy(); garis_.build(world["hiasan"], catalog_, origin_, ground);
    walkDirty_ = true;
  }
  return true;
}

bool WorldScene::trackEdit(const Json& world, const std::string& mapSlug, const std::string& fontPath, const Json* summary) {
  (void)mapSlug; (void)summary;
  std::string err;
  TrackGraph fresh;
  if (!fresh.fromJson(world, &err)) { std::fprintf(stderr, "[edit] graph: %s\n", err.c_str()); return false; }
  graph_ = std::move(fresh);
  // The scene origin is kept (the terrain tiles, city and clouds live in scene space; a moved bbox-extreme node
  // would only shift the fog reference). Everything derived from the graph is rebuilt in place.
  railsRebuild(world, fontPath);
  if (decor_) {
    scenery_.clear();
    worldForEdit_ = &world;
    { const Json& objs = world["hiasan"]["objek"]; for (size_t i = 0; i < objs.size(); ++i) hiasanPlace((int)i, objs[i]); }
    auto ground = [this](double wx, double wy) { return terrain_.groundHeight(wx, wy); };
    garis_.destroy(); garis_.build(world["hiasan"], catalog_, origin_, ground);
    scanMeja(world);
    scanPapan(world);
    walkDirty_ = true;
  }
  return true;
}

bool WorldScene::vegMask(Json& world, const Json& stamps) {
  std::vector<Vegetation::Stamp> list;
  if (stamps.isArray()) {
    for (const Json& s : stamps.arr) {
      if (!s.isObject()) continue;
      Vegetation::Stamp st{s["x"].numberOr(0), s["y"].numberOr(0), s["r"].numberOr(0), s["a"].numberOr(0) < 0 ? -1 : 1};
      if (st.r > 0) list.push_back(st);
    }
    world.obj["vegMask"] = stamps;
  } else world.obj.erase("vegMask");
  trees_.setMask(std::move(list));
  return true;
}

float WorldScene::railHeadNear(double wx, double wy) const {
  int bestSeg = -1; double bestS = 0, bd = 250.0 * 250.0;
  for (size_t si = 0; si < graph_.segments.size(); ++si) {
    const TrackSegment& sg = graph_.segments[si];
    if (wx < sg.bx0 - 250 || wx > sg.bx1 + 250 || wy < sg.by0 - 250 || wy > sg.by1 + 250) continue;
    for (const TrackSegment::Lut& l : sg.lut) {
      double d = (l.px - wx) * (l.px - wx) + (l.py - wy) * (l.py - wy);
      if (d < bd) { bd = d; bestSeg = (int)si; bestS = l.s; }
    }
  }
  return bestSeg < 0 ? terrain_.groundHeight(wx, wy) : profile_.railHeight(bestSeg, bestS);
}

bool WorldScene::nodeHandlePos(int ni, vec3& out) const {
  if (ni < 0 || ni >= (int)graph_.nodes.size()) return false;
  const TrackNode& n = graph_.nodes[(size_t)ni];
  float h;
  if (!n.segs.empty()) { const TrackSegment& s = graph_.segments[(size_t)n.segs[0]]; h = profile_.railHeight(n.segs[0], s.a == ni ? 0.0 : s.length); }
  else h = terrain_.groundHeight(n.wx, n.wy);
  out = origin_.toScene(n.wx, n.wy, h + 0.6f);
  return true;
}

// ------------------------------------------------------------------ overlays
void WorldScene::buildOverlayMeshes() {
  if (ov_.built) return;
  ov_.built = true;
  auto flatRing = [](float r0, float r1, int n) {
    MeshBuilder b;
    for (int i = 0; i < n; ++i) {
      float a0 = 2 * PI * (float)i / n, a1 = 2 * PI * (float)(i + 1) / n;
      b.quad({std::cos(a0) * r0, 0, std::sin(a0) * r0}, {std::cos(a0) * r1, 0, std::sin(a0) * r1}, {std::cos(a1) * r1, 0, std::sin(a1) * r1}, {std::cos(a1) * r0, 0, std::sin(a1) * r0});
    }
    return b.upload();
  };
  ov_.ring = flatRing(0.9f, 1.06f, 72);
  ov_.thickRing = flatRing(GIZMO_HIT_IN, GIZMO_HIT_OUT, 48);   // never drawn: hit test only (kept for symmetry / debugging)
  { MeshBuilder b; b.box({0, -0.02f, -0.02f}, {1, 0.02f, 0.02f}); ov_.needle = b.upload(); }
  { MeshBuilder b; b.box({-KNOB_R, -KNOB_R, -KNOB_R}, {KNOB_R, KNOB_R, KNOB_R}); ov_.knob = b.upload(); }
  { MeshBuilder b; b.box({1.1f, -0.03f, -0.03f}, {1.5f, 0.03f, 0.03f}); b.quad({1.5f, 0, -0.14f}, {1.5f, 0, 0.14f}, {1.72f, 0, 0}, {1.72f, 0, 0}); ov_.arrow = b.upload(); }
  { MeshBuilder b; b.box({0, 0, 0}, {1, 1, 1}); ov_.unitBox = b.upload(); }
  {   // unit sphere (lat / long, 8 x 12) for the node handles and the node highlight
    MeshBuilder b; const int NL = 8, NM = 12;
    auto at = [](int i, int j) { float th = PI * (float)i / NL, ph = 2 * PI * (float)j / NM; return vec3{std::sin(th) * std::cos(ph), std::cos(th), std::sin(th) * std::sin(ph)}; };
    for (int i = 0; i < NL; ++i) for (int j = 0; j < NM; ++j) b.quad(at(i, j), at(i + 1, j), at(i + 1, j + 1), at(i, j + 1));
    ov_.sphere = b.upload();
  }
  auto mat = [](vec3 c, float a, bool depth) { Material m; m.name = "overlay"; m.baseColor = {c.x, c.y, c.z, a}; m.emissive = c; m.metallic = 0; m.roughness = 1; m.doubleSided = true; m.unlit = true; m.depthTest = depth; if (a < 1) m.alphaMode = AlphaMode::Blend; return m; };
  ov_.hlMat = mat(rgb(0x4a9fe8u), 1, true);
  ov_.gizmoMat = mat(rgb(0x4a9fe8u), 0.55f, false);
  ov_.gizmoHotMat = mat(rgb(0x9ad0ffu), 0.95f, false);
  ov_.needleMat = mat(rgb(0xffd479u), 1, false);
  ov_.ghostMat = mat(rgb(0x4a9fe8u), 0.42f, true);
  ov_.ukurMat = mat(rgb(0xffd479u), 0.9f, false);
  ov_.handleMat = mat(rgb(0x5da8f2u), 1, false);
  ov_.ghostLineMat = mat(rgb(0x4a9fe8u), 0.9f, false);
}

void WorldScene::destroyOverlays() {
  if (!ov_.built) return;
  for (rhi::Mesh* m : {&ov_.ring, &ov_.thickRing, &ov_.needle, &ov_.knob, &ov_.arrow, &ov_.bar, &ov_.unitBox, &ov_.sphere, &ov_.ukurMesh, &ov_.ghostLines, &ov_.segMesh}) { rhi::destroyMesh(*m); *m = {}; }
  ov_.built = false; ov_.ukur.clear(); ov_.hlMode = HighlightMode::Off; ov_.gizmoKind.clear(); ov_.ghostId.clear(); ov_.handles = false; ov_.ghostLineCount = 0; ov_.segMeshId.clear();
}

void WorldScene::setHighlight(const std::string& kind, int index, HighlightMode mode, const std::string& id) { ov_.hlKind = kind; ov_.hlIndex = index; ov_.hlMode = mode; ov_.hlId = id; }
void WorldScene::setNodeHandles(bool on, int tier) { ov_.handles = on; ov_.handleTier = tier; }

// Thin ribbon + vertical fin along a scene polyline (reads from every angle; the same shape as the ukur line).
static void ribbon(MeshBuilder& b, const std::vector<vec3>& pts, float halfWidth, float fin) {
  for (size_t i = 1; i < pts.size(); ++i) {
    vec3 a = pts[i - 1], c = pts[i]; vec3 d = c - a; d.y = 0; float l = length(d); if (l < 1e-3f) continue;
    vec3 n = vec3{-d.z, 0, d.x} / l * halfWidth;
    b.quad(a - n, a + n, c + n, c - n);
    if (fin > 0) b.quad(a, a + vec3{0, fin, 0}, c + vec3{0, fin, 0}, c);
  }
}

void WorldScene::setGhostLines(const std::vector<std::vector<std::pair<double, double>>>& lines) {
  rhi::destroyMesh(ov_.ghostLines); ov_.ghostLines = {}; ov_.ghostLineCount = 0;
  MeshBuilder b;
  for (const auto& line : lines) {
    std::vector<vec3> pts;
    for (const auto& [wx, wy] : line) pts.push_back(origin_.toScene(wx, wy, railHeadNear(wx, wy) + 0.9f));
    if (pts.size() >= 2) { ribbon(b, pts, 0.18f, 0.35f); ++ov_.ghostLineCount; }
  }
  if (ov_.ghostLineCount) ov_.ghostLines = b.upload();
}
void WorldScene::setGizmo(const std::string& kind, vec3 pos, float yaw, float scale, int axisHover) {
  ov_.gizmoKind = kind; ov_.gizmoPos = pos; ov_.gizmoYaw = yaw; ov_.gizmoScale = std::fmax(0.05f, scale); ov_.gizmoHover = axisHover;
}
void WorldScene::setGhost(const std::string& modelId, double wx, double wy, float rotDeg, float skala) {
  ov_.ghostId = modelId;
  if (modelId.empty()) return;
  ov_.ghostPos = origin_.toScene(wx, wy, terrain_.groundHeight(wx, wy)); ov_.ghostYaw = radians(rotDeg); ov_.ghostScale = skala;
}
void WorldScene::setUkur(const std::vector<std::pair<double, double>>& pts) {
  ov_.ukur.clear();
  for (const auto& [wx, wy] : pts) ov_.ukur.push_back(origin_.toScene(wx, wy, terrain_.groundHeight(wx, wy) + 0.4f));
  rhi::destroyMesh(ov_.ukurMesh); ov_.ukurMesh = {};
  if (ov_.ukur.size() < 2) return;
  MeshBuilder b;
  for (size_t i = 1; i < ov_.ukur.size(); ++i) {   // a flat ribbon 0.5 m wide plus a vertical fin so it reads from every angle
    vec3 a = ov_.ukur[i - 1], c = ov_.ukur[i]; vec3 d = c - a; d.y = 0; float l = length(d); if (l < 1e-3f) continue;
    vec3 n = vec3{-d.z, 0, d.x} / l * 0.25f;
    b.quad(a - n, a + n, c + n, c - n);
    b.quad(a, a + vec3{0, 0.5f, 0}, c + vec3{0, 0.5f, 0}, c);
  }
  for (vec3 p : ov_.ukur) b.box(p - vec3{0.4f, 0, 0.4f}, p + vec3{0.4f, 0.8f, 0.4f});
  ov_.ukurMesh = b.upload();
}

int WorldScene::gizmoHit(const Ray& ray) const {
  if (ov_.gizmoKind.empty()) return 0;
  // knob: a sphere of KNOB_R * scale * 2 around the needle tip
  vec3 tip = ov_.gizmoPos + vec3{std::cos(ov_.gizmoYaw), 0, -std::sin(ov_.gizmoYaw)} * ov_.gizmoScale;
  { vec3 oc = ray.origin - tip; float b = dot(oc, ray.dir), c = dot(oc, oc) - (KNOB_R * 2.5f * ov_.gizmoScale) * (KNOB_R * 2.5f * ov_.gizmoScale); if (b * b - c >= 0 && -b > 0) return 2; }
  if (std::fabs(ray.dir.y) < 1e-6f) return 0;
  float t = (ov_.gizmoPos.y - ray.origin.y) / ray.dir.y; if (t <= 0) return 0;
  vec3 p = ray.at(t); float r = std::hypot(p.x - ov_.gizmoPos.x, p.z - ov_.gizmoPos.z) / ov_.gizmoScale;
  return r >= GIZMO_HIT_IN && r <= GIZMO_HIT_OUT ? 1 : 0;
}

bool WorldScene::gizmoAngle(const Ray& ray, float& angle) const {
  if (std::fabs(ray.dir.y) < 1e-6f) return false;
  float t = (ov_.gizmoPos.y - ray.origin.y) / ray.dir.y; if (t <= 0) return false;
  vec3 p = ray.at(t);
  angle = std::atan2(-(p.z - ov_.gizmoPos.z), p.x - ov_.gizmoPos.x);
  return true;
}

void WorldScene::drawOverlays(vec3 eye, float fovY) {
  buildOverlayMeshes();
  // selection box: 12 bars along the placed model's bounds (Box3Helper), blue / amber when locked
  if (ov_.hlMode != HighlightMode::Off) {
    AABB b; bool have = false;
    if (ov_.hlKind == "hiasan") for (const Placed& p : scenery_) if (p.objIndex == ov_.hlIndex) { b = p.bounds; have = true; }
    if (ov_.hlKind == "garis" && worldForEdit_) {
      const Json& g = (*worldForEdit_)["hiasan"]["garis"][(size_t)std::max(0, ov_.hlIndex)];
      for (const Json& pt : g["titik"].arr) { double wx = pt["x"].numberOr(0), wy = pt["y"].numberOr(0); float h = terrain_.groundHeight(wx, wy); b.expand(origin_.toScene(wx, wy, h)); b.expand(origin_.toScene(wx, wy, h + 2.5f)); have = true; }
    }
    if (have && b.valid()) {
      Material m = ov_.hlMat; if (ov_.hlMode == HighlightMode::Locked) { m.baseColor = {0.94f, 0.65f, 0.23f, 1}; m.emissive = {0.94f, 0.65f, 0.23f}; }
      float th = std::fmax(0.05f, length(b.center() - eye) / 700);
      vec3 mn = b.min, mx = b.max;
      auto bar = [&](vec3 a, vec3 c) {
        vec3 lo = vmin(a, c) - vec3{th, th, th}, hi = vmax(a, c) + vec3{th, th, th};
        renderer_.drawMesh(ov_.unitBox, m, {}, mat4::translation(lo) * mat4::scale(hi - lo));
      };
      for (int c = 0; c < 8; ++c) {
        vec3 p{c & 1 ? mx.x : mn.x, c & 2 ? mx.y : mn.y, c & 4 ? mx.z : mn.z};
        if (!(c & 1)) bar(p, {mx.x, p.y, p.z});
        if (!(c & 2)) bar(p, {p.x, mx.y, p.z});
        if (!(c & 4)) bar(p, {p.x, p.y, mx.z});
      }
    }
  }
  // screen-sized helpers: radius in metres for `px` pixels at distance d
  const float perPx = 2 * std::tan(fovY / 2) / (float)std::max(1, viewportH_);
  auto sphereAt = [&](vec3 p, float radius, const Material& m) { renderer_.drawMesh(ov_.sphere, m, {}, mat4::translation(p) * mat4::scale({radius, radius, radius})); };
  // node handles (editorRel3d.ts segarkanTitikRel): every node with a segment, 4.5 px dots, semantic colours
  if (ov_.handles) {
    Material m = ov_.handleMat;
    for (size_t ni = 0; ni < graph_.nodes.size(); ++ni) {
      const TrackNode& n = graph_.nodes[ni];
      if (n.segs.empty()) continue;
      bool wesel = n.isPoint(), penting = wesel || n.hasHeight || n.segs.size() == 1;
      if (ov_.handleTier == 1 && !penting) continue;
      vec3 p; if (!nodeHandlePos((int)ni, p)) continue;
      float d = length(p - eye); if (d > 2500) continue;
      vec3 c = wesel ? rgb(0xffb82eu) : n.hasHeight ? rgb(0xf359ccu) : n.segs.size() == 1 ? rgb(0xf2f2f2u) : rgb(0x5da8f2u);
      m.baseColor = {c.x, c.y, c.z, 1}; m.emissive = c;
      sphereAt(p, std::fmax(0.25f, 4.5f * d * perPx), m);
    }
  }
  // node highlight (sorotNode: green sphere, screen-sized like the point arrows) / segment ribbon (gambarSorotSeg)
  if (ov_.hlMode != HighlightMode::Off && ov_.hlKind == "node") {
    vec3 p;
    if (nodeHandlePos(graph_.nodeIndex(ov_.hlId), p)) {
      Material m = ov_.handleMat; vec3 c = rgb(0x39c07au); m.baseColor = {c.x, c.y, c.z, 1}; m.emissive = c;
      sphereAt(p, std::fmax(0.8f, length(p - eye) / 190), m);
    }
  }
  if (ov_.hlMode != HighlightMode::Off && ov_.hlKind == "segment") {
    int si = graph_.segIndex(ov_.hlId);
    if (si >= 0) {
      if (ov_.segMeshId != ov_.hlId) {
        rhi::destroyMesh(ov_.segMesh); ov_.segMesh = {}; ov_.segMeshId = ov_.hlId;
        std::vector<vec3> pts;
        for (const TrackSegment::Lut& l : graph_.segments[(size_t)si].lut) pts.push_back(origin_.toScene(l.px, l.py, profile_.railHeight(si, l.s) + 0.9f));
        MeshBuilder b; ribbon(b, pts, 0.3f, 0.4f);
        if (!b.indices.empty()) ov_.segMesh = b.upload();
      }
      if (ov_.segMesh.indexCount) renderer_.drawMesh(ov_.segMesh, ov_.ukurMat, {}, mat4::identity());
    }
  }
  if (ov_.ghostLineCount && ov_.ghostLines.indexCount) renderer_.drawMesh(ov_.ghostLines, ov_.ghostLineMat, {}, mat4::identity());
  // ghost
  if (!ov_.ghostId.empty()) {
    if (GpuModel* m = catalog_.model(ov_.ghostId)) {
      mat4 xf = mat4::translation(ov_.ghostPos) * mat4::rotationY(ov_.ghostYaw) * mat4::scale({ov_.ghostScale, ov_.ghostScale, ov_.ghostScale}) * RollingStock::normalizeTransform(*m, true);
      renderer_.draw(*m, xf, nullptr, nullptr, &ov_.ghostMat);
    }
  }
  // ukur polyline
  if (ov_.ukurMesh.indexCount) renderer_.drawMesh(ov_.ukurMesh, ov_.ukurMat, {}, mat4::identity());
  // gizmo (after everything: depth-test-off overlay)
  if (!ov_.gizmoKind.empty()) {
    mat4 base = mat4::translation(ov_.gizmoPos + vec3{0, 0.12f, 0}) * mat4::rotationY(ov_.gizmoYaw) * mat4::scale({ov_.gizmoScale, ov_.gizmoScale, ov_.gizmoScale});
    renderer_.drawMesh(ov_.ring, ov_.gizmoHover == 1 ? ov_.gizmoHotMat : ov_.gizmoMat, {}, base);
    renderer_.drawMesh(ov_.needle, ov_.needleMat, {}, base);
    Material knob = ov_.needleMat; if (ov_.gizmoHover == 2) { knob.baseColor = {1, 1, 1, 1}; knob.emissive = {1, 1, 1}; }
    renderer_.drawMesh(ov_.knob, knob, {}, base * mat4::translation({1, 0, 0}));
    if (ov_.gizmoKind == "move")
      for (int k = 0; k < 4; ++k) renderer_.drawMesh(ov_.arrow, ov_.gizmoHover == 3 + k ? ov_.gizmoHotMat : ov_.gizmoMat, {}, base * mat4::rotationY((float)k * PI / 2));
  }
}

} // namespace eng

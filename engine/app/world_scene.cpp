#include "engine/app/world_scene.h"
#include "engine/world/sun.h"
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace eng {

void WorldScene::initGpu() { renderer_.init(); sky_.init(); }

// origin = bbox centre of track nodes (spec §2.2); first station scenery object = camera target
void WorldScene::setWorld(const Json& world) {
  const Json& nodes = world["graph"]["nodes"];
  double x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30;
  for (const Json& n : nodes.arr) { double x = n["x"].num, y = n["y"].num; x0 = std::fmin(x0, x); x1 = std::fmax(x1, x); y0 = std::fmin(y0, y); y1 = std::fmax(y1, y); }
  if (nodes.size() == 0) { x0 = x1 = y0 = y1 = 0; }
  origin_ = {(x0 + x1) / 2, (y0 + y1) / 2};
  worldW_ = (float)std::fmax(std::fmax(x1 - x0, y1 - y0), 800.0);   // lebarDunia (corridor fog)
  stationScene_ = {};
  for (const Json& s : world["scenery"].arr)
    if (s["kind"].stringOr("") == "station") { stationScene_ = origin_.toScene(s["pos"]["x"].num, s["pos"]["y"].num, 0); break; }
}

namespace {
bool readFile(const std::string& path, std::vector<uint8_t>& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return !out.empty();
}
std::vector<float> asFloats(const std::vector<uint8_t>& b) { std::vector<float> f(b.size() / 4); std::memcpy(f.data(), b.data(), f.size() * 4); return f; }
} // namespace

// Per-tile files are preferred (only the neighbourhood is read); the monolithic pair is the fallback.
bool WorldScene::loadTerrain(const std::string& terrainDir, const std::string& mapSlug, std::string& error) {
  tileDir_.clear();
  const std::string dir = terrainDir + "/" + mapSlug;
  std::ifstream idx(dir + "/index.json");
  if (idx) {
    std::stringstream ss; ss << idx.rdbuf();
    std::string err; Json j = Json::parse(ss.str(), &err);
    if (err.empty() && j["target"].stringOr("") == "desktop" && terrain_.loadIndex(j, err)) {
      tileDir_ = dir;
      terrain_.setRequestFn([this](const Terrain::TileRef& t) {
        std::vector<uint8_t> bytes; std::string e; bool ok = false;
        if (readFile(tileDir_ + "/" + t.path(), bytes)) {
          if (t.dir == "dem") ok = terrain_.provideDem(t.z, t.x, t.y, asFloats(bytes));
          else ok = terrain_.provideSatFile(std::atoi(t.dir.c_str() + 4), t.z, t.x, t.y, bytes, e);
        }
        if (!ok) { terrain_.failTile(t); if (!e.empty()) std::fprintf(stderr, "terrain tile %s: %s\n", t.path().c_str(), e.c_str()); }
      });
      for (const Terrain::TileRef& t : terrain_.demTilesWanted()) {
        std::vector<uint8_t> bytes;
        if (!readFile(dir + "/" + t.path(), bytes) || !terrain_.provideDem(t.z, t.x, t.y, asFloats(bytes))) terrain_.failTile(t);
      }
      terrain_.finishDem();
      return true;
    }
    if (!err.empty()) std::fprintf(stderr, "terrain: %s/index.json: %s (using the monolithic files)\n", dir.c_str(), err.c_str());
  }
  return terrain_.load(terrainDir + "/" + mapSlug + ".dem", terrainDir + "/" + mapSlug + ".sat", error);
}

void WorldScene::updateStreaming(vec3 centre, float dt) {
  if (!built_) return;
  terrain_.update(centre, dt);
  if (decor_) trees_.update(terrain_);
}

void WorldScene::primeStreaming(vec3 centre) {
  if (!built_) return;
  terrain_.prime(centre);
  if (decor_) trees_.update(terrain_, 0);
}

bool WorldScene::buildStatic(const Json& world, const std::string& mapSlug, const std::string& fontPath, const std::string& cityPath, Log log) {
  std::string err;
  auto t0 = std::chrono::steady_clock::now();
  if (!graph_.fromJson(world, &err)) { std::fprintf(stderr, "graph: %s\n", err.c_str()); return false; }
  std::vector<StationZone> stations;
  for (const Json& sc : world["scenery"].arr)
    if (sc["kind"].stringOr("") == "station") stations.push_back({sc["pos"]["x"].numberOr(0), sc["pos"]["y"].numberOr(0), 160});
  const float demBase = terrain_.dem().demBase;
  profile_.build(graph_, terrain_.dem(), stations, demBase);
  rails_.build(graph_, profile_, &terrain_.dem(), demBase);
  terrain_.setBrushDeltas(world["tanah"]);
  terrain_.setRails(rails_.samples());
  terrain_.build();
  { const double* bb = terrain_.dem().bbox; clouds_.build((float)(bb[0] - origin_.ox), (float)(bb[1] - origin_.oz), (float)(bb[2] - origin_.ox), (float)(bb[3] - origin_.oz)); }
  signals_.build(graph_, profile_, world["trackside"], fontPath);
  points_.build(graph_, profile_);
  routes_.init(&graph_, &profile_);
  auto ground = [this](double wx, double wy) { return terrain_.groundHeight(wx, wy); };
  boards_.build(graph_, profile_, world, origin_, ground, fontPath);
  jpl_.build(graph_, world, origin_, ground);
  if (!cityPath.empty()) city_.build(cityPath, origin_, graph_, ground);
  stock_.init(catalog_); trains_.init(stock_);
  if (!fontPath.empty()) { meja_.setFont(fontPath); papan_.setFont(fontPath); }
  // camera height follows the ground at the station
  stationScene_.y = terrain_.groundHeight(stationScene_.x + origin_.ox, stationScene_.z + origin_.oz);
  built_ = true;
  stats_.buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  char m[400];
  std::snprintf(m, sizeof m, "world built in %.0f ms: %.1f km track, %d points, %zu signals, rails %u tris, terrain %zu tris, %zu boards, %zu jpl, city %zu bldg/%u tris/%d meshes (%.0f ms); map %s",
                stats_.buildMs, graph_.totalLength() / 1000, graph_.pointCount(), signals_.signals().size(), rails_.stats().tris, terrain_.stats.triangles, boards_.stats.boards, jpl_.crossings().size(), city_.stats.buildings, city_.stats.tris, city_.stats.meshes, city_.stats.buildMs, mapSlug.c_str());
  stats_.summary = m;
  if (log) log(m);
  return true;
}

// hiasan objects (spec §5.4): position on carved ground, yaw = rot degrees; hiasan.garis (spec §3.5) spline
// objects; trees from the satellite green mask, kept out of the hiasan footprints (§4.4).
void WorldScene::buildDecor(const Json& world, bool testGaris, const Json* summary) {
  auto t0 = std::chrono::steady_clock::now();
  scenery_.clear();
  worldForEdit_ = &world;
  { const Json& objs = world["hiasan"]["objek"]; for (size_t i = 0; i < objs.size(); ++i) hiasanPlace((int)i, objs[i]); }   // missing / not streamed yet = skipped
  auto ground = [this](double wx, double wy) { return terrain_.groundHeight(wx, wy); };
  {
    Json hiasan = world["hiasan"];
    if (testGaris && summary) injectTestGaris(hiasan, *summary, stationScene_, origin_, nullptr);
    garis_.destroy();
    garis_.build(hiasan, catalog_, origin_, ground);
  }
  buildWalkCollider();
  scanMeja(world);
  scanPapan(world);
  std::vector<AABB> footprints;
  for (const Placed& p : scenery_) footprints.push_back(p.bounds);
  trees_.build(terrain_, catalog_, footprints);
  { Json none; vegMask(const_cast<Json&>(world), world["vegMask"].isArray() ? world["vegMask"] : none); }   // player brush mask from the save
  stock_.forget();
  decor_ = true;
  double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  char m[200];
  std::snprintf(m, sizeof m, "; decor %.0f ms: %zu hiasan (%zu meja), %zu garis/%zu tiles, %zu trees, walk %zu tris (%.0f ms)", ms, scenery_.size(), meja_.count(), garis_.stats.lines, garis_.stats.tiles, trees_.stats.trees, walk_.triangles(), walk_.stats.buildMs);
  stats_.summary += m;
}

void WorldScene::applyState(const SimState& st, double timeScale) {
  for (const SimSignal& s : st.signals) signals_.setAspect(s.id, s.aspect);
  for (const SimPoint& p : st.points) points_.setState(p.id, p.setting, !p.lockedBy.empty());
  // lit number panel while a route from the signal takes a diverging leg (keretaVisual3d.ts ruteMasukBelok)
  for (const SignalInstance& sg : signals_.signals()) {
    bool belok = false;
    for (const SimRoute& r : st.routes) {
      if (r.entry != sg.id) continue;
      for (const auto& [nodeId, legSeg] : r.junctions) {
        int ni = graph_.nodeIndex(nodeId); if (ni < 0) continue;
        const TrackNode& n = graph_.nodes[(size_t)ni];
        if (n.isPoint() && n.legs[1] >= 0 && graph_.segments[(size_t)n.legs[1]].id == legSeg) belok = true;
      }
    }
    signals_.setAngkaLit(sg.id, belok);
  }
  signals_.animate();
  routes_.update(st);
  trains_.update(st, origin_, &profile_, timeScale);
  jpl_.setState(st.jpl);
  state_ = st;
}

void WorldScene::draw(const mat4& viewProj, const mat4& view, vec3 eye, float fovY, int viewportH, double clock, float dt, double timeScale,
                      float fogDensity, bool drawTrains) {
  { double lon, lat; worldToLonLat(origin_.ox, origin_.oz, lon, lat); applySun(clock, lon, lat, light_, sky_); }
  light_.fogDensity = fogDensity;
  viewportH_ = std::max(1, viewportH); fovY_ = fovY; eye_ = eye;
  sky_.draw(viewProj.inverse(), eye);
  renderer_.beginFrame(viewProj, eye, light_);
  Frustum frustum(viewProj);
  terrain_.draw(renderer_, &frustum);
  if (layers.pohon) trees_.draw(renderer_, eye, &frustum);
  double hh = std::fmod(clock / 3600.0, 24.0); bool night = hh < 6 || hh >= 18;
  rails_.draw(renderer_, &frustum, refDistance, night, benangAlways);
  drawScenery(frustum);
  if (meja_.count()) { meja_.update(state_, eye, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count()); meja_.draw(renderer_, eye); }
  if (papan_.count()) papan_.draw(renderer_, eye, &frustum);
  signals_.setView(fovY, viewportH, night); trains_.setView(fovY, viewportH, night);
  signals_.draw(renderer_, eye, &frustum);
  if (layers.wesel) points_.draw(renderer_, &frustum, refDistance);
  boards_.draw(renderer_, &frustum);
  jpl_.animate(dt * (float)std::fmax(1.0, timeScale));
  jpl_.draw(renderer_, &frustum);
  if (layers.kota) city_.draw(renderer_, &frustum);
  if (layers.awan) clouds_.draw(viewProj, view, eye, sky_, light_, dt);
  garis_.draw(renderer_, &frustum);
  routes_.draw(renderer_, layers.pita, cabView);   // blended ribbons before the trains' own transparent parts
  if (drawTrains) trains_.draw(renderer_, &frustum);
}

GpuModel* WorldScene::pickLod(Placed& p, vec3 eye) {
  if (p.lodCount == 0) return p.model;
  vec3 ext = p.bounds.extent();
  float size = std::max(ext.x, std::max(ext.y, ext.z));
  float dist = std::max(1.0f, length(p.bounds.center() - eye) - size * 0.5f);
  float px = size * (float)viewportH_ / (2 * dist * std::tan(fovY_ * 0.5f));   // projected height in framebuffer pixels
  int level = 0;
  for (int n = 0; n < p.lodCount; ++n) if (px < lodPixels[n]) level = n + 1;
  for (; level > 0; --level) {
    GpuModel*& m = p.lod[level - 1];
    if (!m) m = catalog_.model(AssetCatalog::lodId(p.id, level));   // not loaded yet: ask again (streamed hosts answer later)
    if (m && m->textured()) return m;
  }
  return p.model;
}

void WorldScene::drawScenery(const Frustum& frustum) {
  for (auto& [m, g] : hiasanGroups_) g.mats.clear();
  stats_.hiasanDrawn = stats_.hiasanInstanced = stats_.hiasanLod = 0;
  for (Placed& p : scenery_) {
    if (!p.model->textured() || !frustum.contains(p.bounds)) continue;   // skipped while textures stream
    GpuModel* m = pickLod(p, eye_);
    hiasanGroups_[m].mats.push_back(p.xf);
    ++stats_.hiasanDrawn; if (m != p.model) ++stats_.hiasanLod;
  }
  for (auto& [model, g] : hiasanGroups_) {
    if (g.mats.empty()) continue;
    if (g.mats.size() == 1) { renderer_.draw(*model, g.mats[0], &frustum); continue; }   // per-primitive culling for singles
    if (g.mats.size() > g.cap) {   // (re)allocate with headroom
      if (g.buf.id) rhi::destroyBuffer(g.buf);
      g.cap = std::max<size_t>(16, g.mats.size() * 2);
      g.buf = rhi::createDynamicBuffer(g.cap * sizeof(mat4));
    }
    rhi::updateBuffer(g.buf, std::as_bytes(std::span(g.mats)));
    renderer_.drawInstanced(*model, mat4::identity(), (uint32_t)g.mats.size(), g.buf, g.mats[0].transformPoint({}));
    stats_.hiasanInstanced += (unsigned)g.mats.size();
  }
}

void WorldScene::destroy() {
  for (auto& [m, g] : hiasanGroups_) if (g.buf.id) rhi::destroyBuffer(g.buf);
  hiasanGroups_.clear();
  trains_.shutdown(); trees_.destroy(); garis_.destroy(); clouds_.destroy(); boards_.destroy(); jpl_.destroy(); city_.destroy();
  meja_.destroy(); papan_.destroy(); catalog_.destroy(); rails_.destroy(); terrain_.destroy(); signals_.destroy(); points_.destroy(); routes_.destroy();
  scenery_.clear(); walk_.clear(); walkDirty_ = false;
  destroyOverlays();
  renderer_.shutdown(); sky_.shutdown();
  built_ = decor_ = false;
}

// Scenery triangles for the walker: every primitive of every placed hiasan model (walls + floors + ceilings; a
// primitive whose material is named after a platform — "peron", "platform" — is flagged so its 1 m edge is a
// kerb), and the garis tiles as floors only (uji3dSpline.ts rabaKelas: peron/jalan/jembatan/tanggul/sawah/rel;
// spline fences stay passable by design). Vehicles are not here (boxes, per step).
// In-world meja boards inside the placed station models.
void WorldScene::scanMeja(const Json& world) {
  std::vector<MejaBoard::Placed> placed; std::vector<MejaBoard::Station> stations;
  for (const Placed& p : scenery_) placed.push_back({p.model, p.xf});
  for (const Json& s : world["scenery"].arr) if (s["kind"].stringOr("") == "station") stations.push_back({s["code"].stringOr(""), s["pos"]["x"].numberOr(0), s["pos"]["y"].numberOr(0)});
  meja_.scan(placed, stations, origin_);
}

void WorldScene::scanPapan(const Json& world) {
  const Json& objs = world["hiasan"]["objek"];
  std::vector<NameBoards::Placed> placed;
  for (const Placed& p : scenery_) {
    if (p.objIndex < 0 || p.objIndex >= (int)objs.size()) continue;
    const Json& o = objs[(size_t)p.objIndex];
    std::string teks = o["teks"].stringOr(""), ketinggian = o["ketinggian"].stringOr("");
    if (teks.empty() && ketinggian.empty()) continue;
    placed.push_back({p.model, p.xf, p.objIndex, teks, ketinggian});
  }
  papan_.scan(placed);
}

void WorldScene::buildWalkCollider() const {
  walkDirty_ = false;
  walk_.clear();
  auto peronName = [](std::string n) {
    for (char& c : n) c = (char)std::tolower((unsigned char)c);
    return n.find("peron") != std::string::npos || n.find("platform") != std::string::npos;
  };
  for (const Placed& p : scenery_)
    for (size_t ni = 0; ni < p.model->nodes.size(); ++ni) {
      const Node& n = p.model->nodes[ni];
      if (n.mesh < 0 || n.mesh >= (int)p.model->meshes.size()) continue;
      mat4 xf = p.xf * p.model->world[ni];
      for (const GpuPrimitive& prim : p.model->meshes[(size_t)n.mesh].primitives) {
        bool peron = prim.material >= 0 && prim.material < (int)p.model->materials.size() && peronName(p.model->materials[(size_t)prim.material].name);
        walk_.addMesh(prim.collisionPos, prim.collisionIdx, xf, peron, false);
      }
    }
  garis_.collectWalk(walk_, catalog_);
  walk_.finish();
}

void WorldScene::collectWalkBoxes(std::vector<WalkBox>& out) const {
  for (const VehicleInstance& v : trains_.vehicles()) if (v.bounds.valid()) out.push_back({v.bounds, true});
}

void WorldScene::pickAt(float px, float py, int w, int h, const mat4& viewProj, vec3 eye, std::string& sigId, std::string& ptId) const {
  std::string bs, bp; float ds = 1e9f, dw = 1e9f;
  for (const ScreenPoint& sp : signals_.screenPositions(viewProj, w, h, eye)) {
    if (!sp.visible) continue; float d = std::hypot(sp.x - px, sp.y - py); if (d < ds) { ds = d; bs = sp.id; }
  }
  if (layers.wesel) for (const ScreenPoint& sp : points_.screenPositions(viewProj, w, h, refDistance)) {
    if (!sp.visible) continue; float d = std::hypot(sp.x - px, sp.y - py); if (d < dw) { dw = d; bp = sp.id; }
  }
  bool hitSig = ds <= 26, hitPt = dw <= 40;
  sigId.clear(); ptId.clear();
  if (hitSig && (!hitPt || ds < dw || ds < 18)) sigId = bs; else if (hitPt) ptId = bp;
}

bool WorldScene::objectPos(const std::string& id, bool signal, vec3& out) const {
  if (signal) { int i = signals_.indexOf(id); if (i < 0) return false; out = signals_.signals()[(size_t)i].pos; return true; }
  for (const PointInstance& p : points_.points()) if (p.nodeId == id) { out = p.pos; return true; }
  return false;
}

// Debug: a 220 m island platform 5 m beside the station track, a blue fence 11 m on the other side and a
// concrete wall 16 m out, as `hiasan.garis` entries (world coordinates), so the spline tiling can be seen.
void WorldScene::injectTestGaris(Json& hiasan, const Json& summary, vec3& stationScene, const WorldOrigin& origin, Log log) {
  const Json& segs = summary["segments"];
  const Json* best = nullptr; double bd = 1e30; double sx = stationScene.x + origin.ox, sy = stationScene.z + origin.oz;
  for (const Json& sg : segs.arr) {
    const Json& poly = sg["poly"];
    for (size_t i = 0; i + 1 < poly.size(); i += 2) { double d = std::hypot(poly[i].num - sx, poly[i + 1].num - sy); if (d < bd) { bd = d; best = &sg; } }
  }
  if (!best) return;
  const Json& poly = (*best)["poly"];
  size_t n = poly.size() / 2, ic = 0; bd = 1e30;
  for (size_t i = 0; i < n; ++i) { double d = std::hypot(poly[2 * i].num - sx, poly[2 * i + 1].num - sy); if (d < bd) { bd = d; ic = i; } }
  ic = std::min(n - 1, ic + 40);   // 160 m past the building so the tiles are not hidden by the station GLB
  { size_t a = ic > 0 ? ic - 1 : ic, b = ic + 1 < n ? ic + 1 : ic; double tx = poly[2 * b].num - poly[2 * a].num, ty = poly[2 * b + 1].num - poly[2 * a + 1].num, l = std::hypot(tx, ty);
    if (l > 0 && !std::getenv("ENG_TARGET")) stationScene = origin.toScene(poly[2 * ic].num - ty / l * 3, poly[2 * ic + 1].num + tx / l * 3, 0); }   // aim the default camera at the platform
  auto line = [&](const char* kelas, double offset, double halfLen, double naik) {
    Json g; g.type = Json::Type::Object;
    Json k; k.type = Json::Type::String; k.str = kelas; g.obj["kelas"] = k;
    Json nk; nk.type = Json::Type::Number; nk.num = naik; g.obj["naik"] = nk;
    Json pts; pts.type = Json::Type::Array;
    double acc = 0;
    for (size_t i = ic; i + 1 < n && acc < halfLen; ++i) acc += std::hypot(poly[2 * i + 2].num - poly[2 * i].num, poly[2 * i + 3].num - poly[2 * i + 1].num);
    size_t i0 = ic, i1 = ic; acc = 0;
    while (i0 > 0 && acc < halfLen) { acc += std::hypot(poly[2 * i0].num - poly[2 * i0 - 2].num, poly[2 * i0 + 1].num - poly[2 * i0 - 1].num); --i0; }
    acc = 0; while (i1 + 1 < n && acc < halfLen) { acc += std::hypot(poly[2 * i1 + 2].num - poly[2 * i1].num, poly[2 * i1 + 3].num - poly[2 * i1 + 1].num); ++i1; }
    for (size_t i = i0; i <= i1; i += 4) {
      size_t a = i > 0 ? i - 1 : i, b = i + 1 < n ? i + 1 : i;
      double tx = poly[2 * b].num - poly[2 * a].num, ty = poly[2 * b + 1].num - poly[2 * a + 1].num, l = std::hypot(tx, ty); if (l <= 0) continue;
      Json pt; pt.type = Json::Type::Object;
      Json px; px.type = Json::Type::Number; px.num = poly[2 * i].num - ty / l * offset; pt.obj["x"] = px;
      Json py; py.type = Json::Type::Number; py.num = poly[2 * i + 1].num + tx / l * offset; pt.obj["y"] = py;
      pts.arr.push_back(pt);
    }
    g.obj["titik"] = pts;
    if (hiasan.type != Json::Type::Object) hiasan.type = Json::Type::Object;
    Json& arr = hiasan.obj["garis"]; if (arr.type != Json::Type::Array) arr.type = Json::Type::Array;
    arr.arr.push_back(g);
  };
  line("peron-kanopi", 5, 110, 0);
  line("bn-pager-rel-biru", -11, 150, 0);
  line("tembok-beton-cc0", -16, 150, 0);
  if (log) log("ENG_TEST_GARIS: platform + fence + wall injected along " + (*best)["id"].stringOr(""));
}

} // namespace eng

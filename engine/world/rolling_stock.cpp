#include "engine/world/rolling_stock.h"
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <regex>
#include "engine/core/json.h"

namespace eng {

std::string flatNodeName(const std::string& s) {
  std::string r;
  for (unsigned char c : s) if (std::isalnum(c)) r += (char)std::tolower(c);
  return r;
}

bool lightRole(const std::string& nodeName, LightRole& role) {
  std::string k; for (unsigned char c : nodeName) k += (char)std::tolower(c);
  if (!k.empty() && k[0] == 'a') { size_t n = k.size() > 1 && (k[1] == '.' || k[1] == '_') ? 2 : 1; k = k.substr(n); }
  auto starts = [&](const char* pfx) { return k.compare(0, std::strlen(pfx), pfx) == 0; };
  auto digitAfter = [&](const char* pfx) { size_t n = std::strlen(pfx); return starts(pfx) && k.size() > n && std::isdigit((unsigned char)k[n]); };
  if (digitAfter("light")) { role = LightRole::Sorot; return true; }
  if (starts("ditch_white")) { role = LightRole::Muka; return true; }
  if (starts("ditch_red") || starts("s21-") || starts("s21_") || digitAfter("red") || starts("coronacenter")) { role = LightRole::Akhiran; return true; }
  return false;
}

std::vector<LightPoint> fallbackLights(vec3 sz) {
  float x = sz.x / 2 * 0.97f, y = sz.y * 0.41f, z = sz.z / 2 * 0.64f;
  return {{LightRole::Sorot, 1, {x, y, z}}, {LightRole::Sorot, 1, {x, y, -z}}, {LightRole::Akhiran, -1, {-x, y, 0}}};
}

bool isInteriorName(const std::string& s) {
  std::string k; for (unsigned char c : s) k += (char)std::tolower(c);
  return k.find("interior") != std::string::npos;
}

namespace {

bool isAttachName(const std::string& flat, const char* prefix, int& index) {
  size_t n = std::strlen(prefix);
  if (flat.compare(0, n, prefix) != 0 || flat.size() == n) return false;
  for (size_t i = n; i < flat.size(); ++i) if (!std::isdigit((unsigned char)flat[i])) return false;
  index = std::atoi(flat.c_str() + n);
  return true;
}

AABB transformedBounds(const AABB& b, const mat4& m) {
  AABB r;
  for (int c = 0; c < 8; ++c)
    r.expand(m.transformPoint({c & 1 ? b.max.x : b.min.x, c & 2 ? b.max.y : b.min.y, c & 4 ? b.max.z : b.min.z}));
  return r;
}

bool hasBogieNode(const GpuModel& m) {
  int idx;
  for (const Node& n : m.nodes) if (isAttachName(flatNodeName(n.name), "abog", idx)) return true;
  return false;
}

const std::string* lookupAttach(const std::map<std::string, std::string>& map, const std::string& nodeName) {
  for (const auto& [k, v] : map) if (k != "*" && flatNodeName(k) == flatNodeName(nodeName)) return &v;
  auto it = map.find("*");
  return it == map.end() ? nullptr : &it->second;
}

} // namespace

std::vector<ManifesBogie> bacaManifesBogie(const GpuModel& body) {
  std::vector<ManifesBogie> out;
  for (const Node& n : body.nodes) {
    auto it = n.extras.find("bogie"); if (it == n.extras.end()) continue;
    std::string err; Json j = Json::parse(it->second, &err);
    if (!err.empty() || !j.isArray()) continue;
    for (const Json& m : j.arr) {
      ManifesBogie b; b.titik = m["titik"].stringOr(""); b.kuid = m["kuid"].stringOr(""); b.balik = m["balik"].boolOr(false);
      if (!b.titik.empty()) out.push_back(b);
    }
    if (!out.empty()) return out;
  }
  return out;
}

namespace {

// dunia3dKonst.ts KACA_KABIN: slots whose glass is OPAQUE in the file (dark windows from outside) and must be made
// see-through from the cab by code; the regex matches the material name. KULIT_DUA_SISI: slots kept double-sided
// in the cab (one-layer skin with outward normals).
const char* kacaKabin(const std::string& slot) {
  if (slot == "nryJr205KuhaBadan") return "^CL-Glass";
  return nullptr;
}
bool kulitDuaSisi(const std::string& slot) { return slot == "nryJr205KuhaBadan"; }

const std::vector<std::string>* lookupCargo(const std::map<std::string, std::vector<std::string>>& map, const std::string& nodeName) {
  // per-point entry wins over "*" even when it is the empty list (JSON null = deliberately empty)
  for (const auto& [k, v] : map) if (k != "*" && flatNodeName(k) == flatNodeName(nodeName)) return &v;
  auto it = map.find("*");
  return it == map.end() ? nullptr : &it->second;
}

} // namespace

std::vector<Material> cabMaterialsFor(const std::string& slotId, const std::vector<Material>& asli) {
  const char* alias = AssetCatalog::saranaSlot(slotId);
  const std::string slot = alias ? alias : slotId;
  const bool duaSisi = kulitDuaSisi(slot);
  const char* kaca = kacaKabin(slot);
  std::regex re; if (kaca) re = std::regex(kaca);
  std::vector<Material> out = asli;
  for (Material& m : out) {
    if (!duaSisi) m.doubleSided = false;
    if (kaca && std::regex_search(m.name, re)) { m.alphaMode = AlphaMode::Blend; m.baseColor.w = RollingStock::KACA_OPASITAS; }
  }
  return out;
}

void RollingStock::sembunyikanInterior(const VehicleProto& p, std::vector<mat4>& pose) {
  for (int i : p.interiorNodes) if (i >= 0 && (size_t)i < pose.size()) pose[(size_t)i] = pose[(size_t)i] * mat4::scale({0, 0, 0});
}

uint32_t RollingStock::cargoHash(const std::string& kunci, size_t i) {
  std::string s = kunci + "#" + std::to_string(i);
  uint32_t h = 2166136261u;
  for (unsigned char c : s) { h ^= c; h *= 16777619u; }
  return h;
}

mat4 RollingStock::normalizeTransform(const GpuModel& m, bool dropToGround, vec3* sizeOut) {
  AABB bb = m.bounds;
  vec3 sz = bb.max - bb.min;
  mat4 rot;
  bool sumbuX = false;   // a node declaring `extras.sumbuX` pins the orientation (§7.3)
  for (const Node& n : m.nodes) if (n.extras.count("sumbuX")) sumbuX = true;
  if (!sumbuX && sz.z > sz.x) { rot = mat4::rotationY(radians(90)); bb = transformedBounds(bb, rot); sz = bb.max - bb.min; }
  vec3 c = bb.center();
  mat4 t = mat4::translation({-c.x, dropToGround ? -bb.min.y : 0.f, -c.z});
  if (sizeOut) *sizeOut = sz;
  return t * rot;
}

const VehicleProto* RollingStock::proto(const std::string& slotId) {
  auto it = protos_.find(slotId);
  if (it != protos_.end()) return it->second.body ? &it->second : nullptr;
  VehicleProto p;
  const CatalogEntry* entry = catalog_ ? catalog_->find(slotId) : nullptr;
  GpuModel* body = entry ? catalog_->model(slotId) : nullptr;
  if (!body) { protos_[slotId] = p; return nullptr; }
  p.body = body;
  p.normalize = normalizeTransform(*body, !hasBogieNode(*body), &p.size);
  p.bounds = transformedBounds(body->bounds, p.normalize);

  // panjang: extras.panjangKopling -> |x(a.limfront) - x(a.limback)| -> bbox x
  p.panjang = p.size.x > 0 ? p.size.x : 1;
  float limFront = 0, limBack = 0; bool hasFront = false, hasBack = false, hasExtra = false;
  for (size_t i = 0; i < body->nodes.size(); ++i) {
    const Node& n = body->nodes[i];
    float pk = n.extraNumber("panjangKopling", 0);
    if (pk > 1) { p.panjang = pk; hasExtra = true; }
    std::string flat = flatNodeName(n.name);
    vec3 wp = (p.normalize * body->world[i]).transformPoint({0, 0, 0});
    if (flat == "alimfront") { limFront = wp.x; hasFront = true; }
    if (flat == "alimback") { limBack = wp.x; hasBack = true; }
  }
  if (!hasExtra && hasFront && hasBack && std::fabs(limFront - limBack) > 1) p.panjang = std::fabs(limFront - limBack);

  // attachments: bogies and couplings (odd coupling ends rotated pi). Bogie precedence (bogie.ts rencanaBogie):
  // catalog per-point -> catalog "*" -> the body's own extras.bogie manifest (kuid -> catalog entry declaring that
  // kuid; `balik` = Trainz `reversed`, a half turn about the vertical axis). A manifest kuid nobody declares, or a
  // point the catalog emptied with null, stays empty: a car without bogies still reads as a car.
  const std::vector<ManifesBogie> manifes = bacaManifesBogie(*body);
  for (size_t i = 0; i < body->nodes.size(); ++i) {
    const Node& n = body->nodes[i];
    std::string flat = flatNodeName(n.name);
    int idx = 0; const std::string* id = nullptr; bool flip = false; std::string dariManifes;
    if (isAttachName(flat, "abog", idx)) {
      id = lookupAttach(entry->bogie, n.name);
      if (!id) for (const ManifesBogie& m : manifes) if (flatNodeName(m.titik) == flat) {
        dariManifes = catalog_->idByKuid(m.kuid); if (!dariManifes.empty()) { id = &dariManifes; flip = m.balik; }
        break;
      }
    }
    else if (isAttachName(flat, "akopling", idx)) { id = lookupAttach(entry->kopling, n.name); flip = idx % 2 == 1; }
    if (isAttachName(flat, "amuatan", idx)) {   // cargo: every variant kept, one shown per vehicle (train_visual)
      const std::vector<std::string>* ids = lookupCargo(entry->muatan, n.name);
      if (!ids || ids->empty()) continue;
      CargoSlot slot; slot.local = p.normalize * body->world[i];
      for (const std::string& mid : *ids) {
        GpuModel* part = catalog_->model(mid);
        slot.varian.push_back(part);
        if (part) p.bounds.expand(transformedBounds(part->bounds, slot.local));
      }
      p.muatan.push_back(std::move(slot));
      continue;
    }
    if (!id || id->empty()) continue;
    GpuModel* part = catalog_->model(*id);
    if (!part) continue;
    mat4 local = p.normalize * body->world[i];
    if (flip) local = local * mat4::rotationY(radians(180));
    p.parts.push_back({part, local});
    p.bounds.expand(transformedBounds(part->bounds, local));
  }
  // light attach points (sarana3d.ts muatSatu): tail points not in the outer third are dropped (third-party
  // `a.s21` nodes sit at one fixed coordinate for the whole pack); no tail point left -> copy the headlights
  for (size_t i = 0; i < body->nodes.size(); ++i) {
    LightRole role;
    if (!lightRole(body->nodes[i].name, role)) continue;
    vec3 wp = (p.normalize * body->world[i]).transformPoint({0, 0, 0});
    p.lights.push_back({role, wp.x >= 0 ? 1 : -1, wp});
  }
  float edge = p.size.x / 2 * 0.66f;
  for (size_t i = p.lights.size(); i-- > 0;) if (p.lights[i].role == LightRole::Akhiran && std::fabs(p.lights[i].pos.x) < edge) p.lights.erase(p.lights.begin() + (long)i);
  bool hasTail = false; for (const LightPoint& l : p.lights) if (l.role == LightRole::Akhiran) hasTail = true;
  if (!p.lights.empty() && !hasTail) { size_t n = p.lights.size(); for (size_t i = 0; i < n; ++i) if (p.lights[i].role == LightRole::Sorot) p.lights.push_back({LightRole::Akhiran, p.lights[i].end, p.lights[i].pos}); }
  p.lightsFromModel = !p.lights.empty();
  if (p.lights.empty()) p.lights = fallbackLights(p.size);

  // interior nodes (sakelarInterior): by node name (the GPU model keeps no mesh names; the GLTF loader named the
  // three.js Mesh after the node, so `SM_CC206_Interior.005` is a node name)
  for (size_t i = 0; i < body->nodes.size(); ++i)
    if (body->nodes[i].mesh >= 0 && isInteriorName(body->nodes[i].name)) p.interiorNodes.push_back((int)i);
  // cab materials (sisiTunggal): the catalog slot decides (the alias `krl-kuha` -> nryJr205KuhaBadan too)
  p.cabMaterials = cabMaterialsFor(slotId, body->materials);

  // clips: doors (`pintu-kiri|kanan`) are scrubbed per frame; pantographs (`panto-*`) baked folded are
  // frozen at the last frame (a KRL in service is always on the wire)
  if (!body->animations.empty()) {
    restPose(body->nodes, p.restLocal);
    for (size_t i = 0; i < body->animations.size(); ++i) {
      const Animation& a = body->animations[i];
      if (a.name == "pintu-kiri" || a.name == "pintu-kanan") { p.doorClips.push_back((int)i); p.doorDuration = std::fmax(p.doorDuration, a.duration); }
      else if (a.name.rfind("panto-", 0) == 0) scrubAnimation(body->nodes, a, a.duration, p.restLocal);
    }
    computeWorld(body->nodes, body->roots, p.restLocal, p.restWorld);
  }
  if (!p.interiorNodes.empty()) { p.worldFar = p.restWorld.empty() ? body->world : p.restWorld; sembunyikanInterior(p, p.worldFar); }
  std::printf("[stock] %s: panjang %.2f m, size %.2f x %.2f x %.2f, %zu parts, %zu cargo points, %zu lights%s, %zu door clips%s, %zu interior nodes\n", slotId.c_str(), p.panjang, p.size.x, p.size.y, p.size.z, p.parts.size(),
              p.muatan.size(), p.lights.size(), p.lightsFromModel ? "" : " (synthetic)", p.doorClips.size(), p.restWorld.empty() ? "" : ", animated", p.interiorNodes.size());
  protos_[slotId] = p;
  return &protos_[slotId];
}

} // namespace eng

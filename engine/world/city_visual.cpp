#include "engine/world/city_visual.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
namespace eng {
namespace city {
void lonLatToWorld(double lon, double lat, double& wx, double& wy) {
  constexpr double R = 6378137.0;
  wx = R * (lon * PI / 180.0);
  wy = -R * std::log(std::tan(PI / 4.0 + (lat * PI / 180.0) / 2.0));
}
}
namespace {
// Cincin terkode (kota3d.ts uraiCincin): dengan `asal` = bulat 1e-6° relatif, titik ke-2 dst selisih;
// tanpa asal = lon/lat mentah. Hasil = koordinat SCENE xz (x, z berselang-seling).
std::vector<float> uraiCincin(const Json& r, const Json& asal, const WorldOrigin& origin) {
  std::vector<float> out;
  if (!r.isArray() || r.size() < 2) return out;
  const bool relatif = asal.isArray() && asal.size() == 2 && asal[0].isNumber() && asal[1].isNumber();
  double x = 0, y = 0;
  for (size_t i = 0; i + 1 < r.size(); i += 2) {
    if (!r[i].isNumber() || !r[i + 1].isNumber()) return {};
    double lon, lat;
    if (relatif) {
      x = i == 0 ? r[0].num : x + r[i].num;
      y = i == 0 ? r[1].num : y + r[i + 1].num;
      lon = asal[0].num + x / 1e6; lat = asal[1].num + y / 1e6;
    } else { lon = r[i].num; lat = r[i + 1].num; }
    double wx, wy; city::lonLatToWorld(lon, lat, wx, wy);
    const float sx = (float)(wx - origin.ox), sz = (float)(wy - origin.oz);
    if (!std::isfinite(sx) || !std::isfinite(sz)) return {};
    out.push_back(sx); out.push_back(sz);
  }
  return out;
}
bool diDalam(float px, float pz, const std::vector<float>& r) {   // ray casting, kota3d.ts diDalam
  bool ya = false;
  for (size_t i = 0, j = r.size() - 2; i < r.size(); j = i, i += 2) {
    const float xi = r[i], zi = r[i + 1], xj = r[j], zj = r[j + 1];
    if ((zi > pz) != (zj > pz) && px < ((xj - xi) * (pz - zi)) / (zj - zi) + xi) ya = !ya;
  }
  return ya;
}
int64_t kunciSel(int gx, int gz) { return ((int64_t)gx << 32) ^ (uint32_t)gz; }
} // namespace

void CityVisuals::PolyGrid::add(Poly p) {
  if (p.r.size() < 6) return;
  p.x0 = p.z0 = 1e30f; p.x1 = p.z1 = -1e30f;
  for (size_t i = 0; i < p.r.size(); i += 2) { p.x0 = std::fmin(p.x0, p.r[i]); p.x1 = std::fmax(p.x1, p.r[i]); p.z0 = std::fmin(p.z0, p.r[i+1]); p.z1 = std::fmax(p.z1, p.r[i+1]); }
  const int id = (int)polys.size();
  for (int gx = (int)std::floor(p.x0 / cell); gx <= (int)std::floor(p.x1 / cell); ++gx)
    for (int gz = (int)std::floor(p.z0 / cell); gz <= (int)std::floor(p.z1 / cell); ++gz) cells[kunciSel(gx, gz)].push_back(id);
  polys.push_back(std::move(p));
}
bool CityVisuals::PolyGrid::contains(float x, float z) const {
  auto it = cells.find(kunciSel((int)std::floor(x / cell), (int)std::floor(z / cell)));
  if (it == cells.end()) return false;
  for (int id : it->second) {
    const Poly& p = polys[(size_t)id];
    if (x < p.x0 || x > p.x1 || z < p.z0 || z > p.z1) continue;
    if (diDalam(x, z, p.r)) return true;
  }
  return false;
}

// Data tanah OSM: dibaca SEBELUM kit divalidasi supaya save kota lama (tanpa kit) tetap menghalangi pohon.
void CityVisuals::parseOsm(const Json& json, const WorldOrigin& origin) {
  const Json& asal = json["asal"];
  if (json["bangunan"].isArray()) for (const Json& b : json["bangunan"].arr) {
    Poly p; p.r = uraiCincin(b["r"], asal, origin);
    if (p.r.size() < 6) continue;
    jejak_.add(std::move(p)); ++stats.footprints;
  }
  if (json["hijau"].isArray()) for (const Json& g : json["hijau"].arr) {
    Poly p; p.r = uraiCincin(g["r"], asal, origin);
    if (p.r.size() < 6) continue;
    hijau_.add(std::move(p)); ++stats.green;
  }
  // pohon eksplisit: titik natural=tree + deret tree_row tiap 8 m (kota3d.ts pohonTetap)
  { std::vector<float> t = uraiCincin(json["pohon"], asal, origin); for (size_t i = 0; i + 1 < t.size(); i += 2) fixed_.push_back({t[i], t[i+1]}); }
  if (json["baris"].isArray()) for (const Json& g : json["baris"].arr) {
    std::vector<float> ll = uraiCincin(g["r"], asal, origin);
    for (size_t i = 0; i + 3 < ll.size(); i += 2) {
      const float ax = ll[i], az = ll[i+1], bx = ll[i+2], bz = ll[i+3];
      const int n = std::max(1, (int)std::lround(std::hypot(bx - ax, bz - az) / 8.f));
      for (int j = 0; j < n; ++j) { const float t = (float)j / n; fixed_.push_back({ax + (bx - ax) * t, az + (bz - az) * t}); }
    }
  }
  stats.fixedTrees = fixed_.size();
}

bool CityVisuals::build(const std::string& path, const WorldOrigin& origin, const TrackGraph&, const GroundFn& ground, AssetCatalog& catalog) {
  destroy();
  std::ifstream f(path); if (!f) return false;
  const auto start = std::chrono::steady_clock::now();
  std::stringstream ss; ss << f.rdbuf(); std::string error;
  const Json json = Json::parse(ss.str(), &error); const Json& kit = json["kit"];
  if (error.empty()) parseOsm(json, origin);
  if (!error.empty() || kit["versi"].numberOr(0) != 1 || kit["pustaka"].stringOr("") != "kota-kit" || !kit["instansi"].isArray()) {
    std::fprintf(stderr, "kota: %s belum memakai kit v1; panggang/migrasikan ulang. Bangunan lama tidak ditampilkan.\n", path.c_str());
    return false;
  }
  catalog_ = &catalog;
  for (const Json& b : kit["instansi"].arr) {
    const auto finite = [&](const char* key) { return b[key].isNumber() && std::isfinite(b[key].num); };
    if (!finite("x") || !finite("y") || !finite("rot") || !finite("skala") || !finite("lebar") || !finite("dalam") || !finite("tinggi")) continue;
    float scale = (float)b["skala"].num, w = (float)b["lebar"].num, d = (float)b["dalam"].num, h = (float)b["tinggi"].num;
    std::string node = b["node"].stringOr("");
    if (node.empty() || scale <= 0 || scale > 100 || w <= 0 || d <= 0 || h <= 0) continue;
    const double x = b["x"].num, y = b["y"].num;
    const float yaw = (float)b["rot"].num * PI / 180.f;
    // Alas mengikuti titik terendah tapak; sedikit tertanam untuk menutup celah medan.
    float base = ground(x, y);
    for (int a : {-1,1}) for (int c : {-1,1}) {
      float dx = a*w*.5f, dz = c*d*.5f;
      base = std::fmin(base, ground(x + dx*std::cos(yaw) + dz*std::sin(yaw), y - dx*std::sin(yaw) + dz*std::cos(yaw)));
    }
    const vec3 p = origin.toScene(x, y, base - .15f);
    const mat4 pose = mat4::translation(p) * mat4::rotationY(yaw);
    AABB box; box.expand(vec3{-w*.5f,0,-d*.5f}); box.expand(vec3{w*.5f,h,d*.5f}); box = box.transformed(pose);
    {   // persegi tapak kit (terputar) ikut menghalangi pohon — kit boleh sedikit melampaui poligon OSM-nya
      Poly q; const float cs = std::cos(yaw), sn = std::sin(yaw);
      for (auto [a, c] : {std::pair{-1,-1}, std::pair{1,-1}, std::pair{1,1}, std::pair{-1,1}}) {
        const float dx = a*w*.5f, dz = c*d*.5f;
        q.r.push_back(p.x + dx*cs + dz*sn); q.r.push_back(p.z - dx*sn + dz*cs);
      }
      jejak_.add(std::move(q));
    }
    Placement placement{node, pose * mat4::scale({scale,scale,scale}), box, box.center(), h};
    auto& chunk = chunks_[{(int)std::floor(p.x/640.f),(int)std::floor(p.z/640.f)}];
    chunk.bounds.expand(box); chunk.placements.push_back(std::move(placement)); ++stats.buildings;
  }
  stats.loaded = true; stats.chunks = (int)chunks_.size();
  stats.buildMs = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
  return true;
}
void CityVisuals::draw(ModelRenderer& renderer, const Frustum* frustum) {
  stats.drawn = stats.tris = 0; stats.meshes = 0;
  if (!catalog_ || !stats.buildings) return;
  GpuModel* library = catalog_->model("kota-kit");
  if (!library || !library->textured() || library->texturesUnavailable) return;
  std::map<std::string,int> nodes;
  for (size_t i=0;i<library->nodes.size();++i) if (library->nodes[i].mesh>=0) nodes[library->nodes[i].name]=(int)i;
  for (auto& [id,batch] : batches_) batch.matrices.clear();
  for (const auto& [key,chunk] : chunks_) {
    if (frustum && !frustum->contains(chunk.bounds)) continue;
    for (const Placement& p : chunk.placements) {
      const float distance = length(p.center-renderer.eye());
      if (distance > std::fmin(5000.f,1800.f+p.height*20.f) || (frustum && !frustum->contains(p.bounds))) continue;
      const int lod = distance < 150 ? 0 : distance < 450 ? 1 : 2;
      auto found = nodes.find(p.node+"_LOD"+std::to_string(lod));
      if (found == nodes.end()) continue; // Tidak pernah mengganti model hilang dengan ekstrusi lama.
      batches_[found->second].matrices.push_back(p.transform);
    }
  }
  for (auto& [node,batch] : batches_) {
    if (batch.matrices.empty()) continue;
    if (batch.matrices.size()>batch.capacity) {
      if (batch.buffer.id) rhi::destroyBuffer(batch.buffer);
      batch.capacity = batch.matrices.size()*2;
      batch.buffer = rhi::createDynamicBuffer(batch.capacity*sizeof(mat4));
    }
    rhi::updateBuffer(batch.buffer,std::as_bytes(std::span(batch.matrices)));
    renderer.drawNodeInstanced(*library,node,(uint32_t)batch.matrices.size(),batch.buffer);
    stats.drawn += (unsigned)batch.matrices.size();
    const auto& mesh = library->meshes[(size_t)library->nodes[(size_t)node].mesh];
    stats.meshes += (int)mesh.primitives.size();
    for (const auto& primitive : mesh.primitives) stats.tris += (primitive.mesh.indexCount/3)*(unsigned)batch.matrices.size();
  }
}
void CityVisuals::destroy() {
  for (auto& [id,batch] : batches_) if (batch.buffer.id) rhi::destroyBuffer(batch.buffer);
  batches_.clear(); chunks_.clear(); catalog_ = nullptr; stats = {};
  jejak_.clear(); hijau_.clear(); fixed_.clear();
}
} // namespace eng

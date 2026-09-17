#include "engine/world/city_visual.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
namespace eng {
bool CityVisuals::build(const std::string& path, const WorldOrigin& origin, const TrackGraph&, const GroundFn& ground, AssetCatalog& catalog) {
  destroy();
  std::ifstream f(path); if (!f) return false;
  const auto start = std::chrono::steady_clock::now();
  std::stringstream ss; ss << f.rdbuf(); std::string error;
  const Json json = Json::parse(ss.str(), &error); const Json& kit = json["kit"];
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
}
} // namespace eng

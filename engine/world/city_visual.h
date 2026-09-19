// Kota hanya memakai pustaka Blender bertekstur bake; jejak OSM lama wajib dimigrasikan.
// Selain instansi kit, berkas kota membawa data TANAH OSM untuk vegetasi (port kota3d.ts
// jejak / hijauOSM / pohonTetap): jejak bangunan (pohon tak tumbuh menembus rumah, termasuk
// tapak tanpa kit cocok), poligon hijau (penguat masker citra) dan pohon eksplisit
// (natural=tree + tree_row, ditanam tanpa lewat masker). Disajikan lewat VegetationOsm.
#pragma once
#include "engine/world/asset_catalog.h"
#include "engine/world/coords.h"
#include "engine/world/track_graph.h"
#include "engine/world/vegetation.h"
#include <functional>
#include <map>
#include <unordered_map>
namespace eng {
namespace city {
// Web Mercator, y dibalik (geo.ts lonLatToWorld) — sama dengan koordinat DUNIA save.
void lonLatToWorld(double lon, double lat, double& wx, double& wy);
}
class CityVisuals : public VegetationOsm {
public:
  struct Stats { bool loaded = false; size_t buildings = 0, skippedNearTrack = 0, footprints = 0, green = 0, fixedTrees = 0; unsigned tris = 0; int meshes = 0, chunks = 0; double buildMs = 0; unsigned drawn = 0; };
  using GroundFn = std::function<float(double, double)>;
  bool build(const std::string& path, const WorldOrigin& origin, const TrackGraph& graph, const GroundFn& ground, AssetCatalog& catalog);
  void draw(ModelRenderer& renderer, const Frustum* frustum = nullptr);
  void destroy();
  Stats stats;
  // VegetationOsm — koordinat SCENE xz. Jejak = poligon OSM `bangunan` + persegi instansi kit (kit boleh
  // sedikit melampaui tapaknya). Data ini tetap terisi walau kit tidak sah (tapak lama tetap menghalangi).
  bool insideBuilding(float x, float z) const override { return jejak_.contains(x, z); }
  bool insideGreen(float x, float z) const override { return hijau_.contains(x, z); }
  std::span<const vec2> fixedTrees() const override { return fixed_; }
private:
  struct Placement { std::string node; mat4 transform; AABB bounds; vec3 center; float height; };
  struct Chunk { AABB bounds; std::vector<Placement> placements; };
  struct Batch { std::vector<mat4> matrices; rhi::Buffer buffer{}; size_t capacity = 0; };
  // Poligon berindeks sel supaya uji per calon pohon murah (kota3d.ts SEL_BGN 40 / SEL_HJU 128).
  struct Poly { float x0, z0, x1, z1; std::vector<float> r; };
  struct PolyGrid {
    float cell = 40;
    std::vector<Poly> polys;
    std::unordered_map<int64_t, std::vector<int>> cells;
    void add(Poly p);
    bool contains(float x, float z) const;
    void clear() { polys.clear(); cells.clear(); }
  };
  void parseOsm(const Json& json, const WorldOrigin& origin);
  std::map<std::pair<int,int>, Chunk> chunks_;
  std::map<int, Batch> batches_;
  AssetCatalog* catalog_ = nullptr; // Kepemilikan model/tekstur tetap di katalog.
  PolyGrid jejak_{40, {}, {}}, hijau_{128, {}, {}};
  std::vector<vec2> fixed_;
};
} // namespace eng

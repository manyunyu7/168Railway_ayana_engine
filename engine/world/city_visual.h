// Kota hanya memakai pustaka Blender bertekstur bake; jejak OSM lama wajib dimigrasikan.
#pragma once
#include "engine/world/asset_catalog.h"
#include "engine/world/coords.h"
#include "engine/world/track_graph.h"
#include <functional>
#include <map>
namespace eng {
class CityVisuals {
public:
  struct Stats { bool loaded = false; size_t buildings = 0, skippedNearTrack = 0; unsigned tris = 0; int meshes = 0, chunks = 0; double buildMs = 0; unsigned drawn = 0; };
  using GroundFn = std::function<float(double, double)>;
  bool build(const std::string& path, const WorldOrigin& origin, const TrackGraph& graph, const GroundFn& ground, AssetCatalog& catalog);
  void draw(ModelRenderer& renderer, const Frustum* frustum = nullptr);
  void destroy();
  Stats stats;
private:
  struct Placement { std::string node; mat4 transform; AABB bounds; vec3 center; float height; };
  struct Chunk { AABB bounds; std::vector<Placement> placements; };
  struct Batch { std::vector<mat4> matrices; rhi::Buffer buffer{}; size_t capacity = 0; };
  std::map<std::pair<int,int>, Chunk> chunks_;
  std::map<int, Batch> batches_;
  AssetCatalog* catalog_ = nullptr; // Kepemilikan model/tekstur tetap di katalog.
};
} // namespace eng

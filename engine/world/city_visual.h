// Baked OSM city (docs/world-spec.md §5.2, port of ppka-wannabe-2 src/tiga/kota3d.ts): building
// footprints from `public/kota/<slug>.json` extruded into walls plus a flat or gabled roof.
// Rules: skip any building with a vertex < 30 m from a track; base = lowest carved ground under the
// ring - 0.5; height from OSM (0 = unknown → 8 flat / 4.2 gable); flat roof when the OSM type is in the
// flat list or h > 8; gable ridge along the long side of the minimum-area bounding rectangle, rise
// clamp(0.7·halfwidth, 0.9, 2.8), overhang 0.5; wall/roof palettes picked by a position hash.
// Geometry is batched per 640 m chunk (frustum-culled) and per palette colour (no vertex colours in
// our vertex format) — walls and roofs tinted by the material's base colour over a procedural
// texture. Roads are not rendered (product decision 2026-08-03). At most 60 000 buildings.
#pragma once
#include "engine/asset/model.h"
#include "engine/math/geometry.h"
#include "engine/render/model_renderer.h"
#include "engine/world/coords.h"
#include "engine/world/track_graph.h"
#include <cmath>
#include <functional>
#include <string>
#include <vector>

namespace eng {

namespace city {
constexpr float CLEARANCE = 30;        // BEBAS_M (the file's `bebas` overrides)
constexpr int MAX_BUILDINGS = 60000;   // MAKS_BGN
constexpr float CHUNK = 640;           // SISI_PETAK
constexpr float OVERHANG = 0.5f;
constexpr int PALETTE = 7;
constexpr uint32_t WALL_COLOURS[PALETTE] = {0xd9cfbc, 0xe4dfd5, 0xcfd6c4, 0xc7d2d9, 0xd3c3ab, 0xe0d3c3, 0xcbc4b4};
constexpr uint32_t ROOF_COLOURS[PALETTE] = {0x9c4f30, 0x8a4429, 0x6d4632, 0x7f8a90, 0x66757e, 0xa8583a, 0x5f6a70};
// 0..1 deterministic from position (acak): the same building gets the same colours on every load.
inline double hash01(double x, double z, double k) { double s = std::sin(x * 12.9898 + z * 78.233 + k * 37.719) * 43758.5453; return s - std::floor(s); }
bool flatRoofType(const std::string& k);
// Minimum-area bounding rectangle of a ring (x, z): centre, unit long axis, half extents (A >= B).
struct MinRect { float cx, cz, ux, uz, A, B; };
bool minRect(const std::vector<vec2>& ring, MinRect& out);
// Ear-clipping triangulation of a simple polygon; appends index triples.
void triangulate(const std::vector<vec2>& ring, std::vector<int>& out);
} // namespace city

class CityVisuals {
public:
  struct Stats { bool loaded = false; size_t buildings = 0, skippedNearTrack = 0; unsigned tris = 0; int meshes = 0, chunks = 0; double buildMs = 0; unsigned drawn = 0; };
  using GroundFn = std::function<float(double wx, double wy)>;

  // Loads and builds; returns false (silently, stats.loaded = false) when the file is absent.
  bool build(const std::string& jsonPath, const WorldOrigin& origin, const TrackGraph& graph, const GroundFn& ground);
  void draw(ModelRenderer& r, const Frustum* frustum = nullptr);
  void destroy();
  Stats stats;

private:
  struct Part { rhi::Mesh mesh; AABB bounds; int palette; bool roof; };
  struct Chunk { AABB bounds; std::vector<Part> parts; };
  std::vector<Chunk> chunks_;
  rhi::Texture wallTex_{}, roofTex_{};
  Material wallMat_[city::PALETTE], roofMat_[city::PALETTE];
};

} // namespace eng

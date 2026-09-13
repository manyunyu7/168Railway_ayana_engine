// Rail/bridge/tunnel mesh generation per 640 m chunk (docs/world-spec.md §3.3, port of
// ppka-wannabe-2 src/tiga/bangun3d.ts bangunRel). Ballast + two rails are extruded along 4 m
// samples of every segment; sleepers live in the procedural 512² texture.
#pragma once
#include "engine/asset/model.h"
#include "engine/math/geometry.h"
#include "engine/render/model_renderer.h"
#include "engine/rhi/rhi.h"
#include "engine/world/height_source.h"
#include "engine/world/track_graph.h"
#include <vector>

namespace eng {

struct RailChunk {
  AABB bounds;
  rhi::Mesh ballast, rails, bridge, tunnel, truss;   // indexCount == 0 when the chunk has none
  int cx = 0, cz = 0;                          // chunk cell (floor(scene / 640))
  uint32_t tris = 0;
};

class RailBuilder {
public:
  static constexpr float CHUNK = 640;          // PETAK_DUNIA
  static constexpr float SAMPLE_STEP = 4;      // LANGKAH_SAMPEL
  static constexpr float TEX_LENGTH = 7.76f;   // PANJANG_TEX_REL
  static constexpr float GAUGE_HALF = 0.534f;  // REL_L_DALAM
  static constexpr float BALLAST_FOOT = -0.580f;

  // ground (optional) is used only for bridge shape / piers (raw DEM; demBase subtracted). Bridge shape:
  // `jenisJembatan` override, else bentukJembatan(span, height) — viaduct (twin columns) when the deck is
  // >= 25 m above ground, Warren through-truss when the structure chain spans >= 50 m and sits >= 8 m up,
  // plain deck otherwise (uji3dJembatan.ts:265-269).
  void build(const TrackGraph& g, const RailProfile& profile, const HeightSource* ground = nullptr, float demBase = 0);
  void draw(ModelRenderer& r, const Frustum* frustum = nullptr) const;
  void destroy();

  const std::vector<RailSample>& samples() const { return samples_; }   // every 12 m, for terrain carving
  const std::vector<RailChunk>& chunks() const { return chunks_; }

  struct Stats { int chunks = 0; uint32_t tris = 0; int bridgeSegs = 0, tunnelSegs = 0, piers = 0, trussSegs = 0, viaductSegs = 0; double buildMs = 0; };
  const Stats& stats() const { return stats_; }

  Material ballastMat, railMat, concreteMat, tunnelMat, steelMat;
  rhi::Texture texture{};                      // procedural ballast/sleeper/rail atlas (sRGB)

private:
  void paintTexture();
  std::vector<RailChunk> chunks_;
  std::vector<RailSample> samples_;
  Stats stats_;
};

} // namespace eng

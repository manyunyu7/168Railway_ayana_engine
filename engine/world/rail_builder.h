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
  rhi::Buffer sleepers{}; uint32_t sleeperCount = 0; // instance matrices of the meshed sleepers (drawn within SLEEPER_RANGE)
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
  static constexpr float SLEEPER_RANGE = 420;  // chunks nearer than this draw meshed sleepers over the painted ones
  // Meshed sleepers are OFF by default: the boxes do not blend with the painted sleepers of the photo atlas
  // (different pitch / look). Set before build() to lay them (also lowers the crib and lays the turnout's long ones).
  bool meshedSleepers = false;

  // ground (optional) is used only for bridge shape / piers (raw DEM; demBase subtracted). Bridge shape:
  // `jenisJembatan` override, else bentukJembatan(span, height) — viaduct (twin columns) when the deck is
  // >= 25 m above ground, Warren through-truss when the structure chain spans >= 50 m and sits >= 8 m up,
  // plain deck otherwise (uji3dJembatan.ts:265-269).
  void build(const TrackGraph& g, const RailProfile& profile, const HeightSource* ground = nullptr, float demBase = 0);
  // refDist = the camera mode's reference distance (dunia3d.ts profilKam acuan): the iconic centreline
  // ("rel-ikonik", bangun3d.ts:494-505 — a 1 px yellow line 0.5 m over the rail head, TEMA.garis) appears
  // when it exceeds BENANG_MUNCUL 700 m and hides again below BENANG_HILANG 500 m; `always` forces it
  // (layer "benang"). The RHI has no lines, so it is a strip of up-facing quads whose width tracks the
  // reference distance (≈ 1.6 px), rebuilt when the width bucket changes. `dark` = night colour.
  void draw(ModelRenderer& r, const Frustum* frustum = nullptr, float refDist = 0, bool dark = false, bool always = false) const;
  bool iconicVisible() const { return iconicOn_; }
  void destroy();

  // Turnout (wesel) blades: per point node two blades (the through leg's rail on the diverging side and the
  // diverging leg's rail on the through side), each as a closed and an open mesh; draw() shows the closed one
  // for the set leg and the open one (pulled 125 mm off the stock rail at the toe) for the other.
  void setPointState(const std::string& nodeId, int setting);

  const std::vector<RailSample>& samples() const { return samples_; }   // every 12 m, for terrain carving
  const std::vector<RailChunk>& chunks() const { return chunks_; }

  struct BridgeInfo { std::string seg; BridgeShape shape; float span, height; };   // per host bridge segment
  struct Stats { int chunks = 0; uint32_t tris = 0; int bridgeSegs = 0, tunnelSegs = 0, piers = 0, trussSegs = 0, viaductSegs = 0; int bedRings = 0, stripRings = 0, turnouts = 0, frogs = 0, sleepers = 0; double buildMs = 0; std::vector<BridgeInfo> bridges; };
  const Stats& stats() const { return stats_; }

  Material ballastMat, railMat, concreteMat, tunnelMat, steelMat, sleeperMat;
  rhi::Texture texture{};                      // ballast/sleeper/rail atlas (sRGB): the catalog's `tekstur.rel1067`
                                               // picture when available, else the procedural painter (same layout)
  // Replaces the atlas (the builder owns and frees it). Same UV layout as the procedural one: 512² atlas,
  // 1 tile = 7.76 m along v, sleeper band u 0.266..0.463, rail body / head u 0.775 / 0.800..0.822
  // (keretaVisual3d.ts:251-262 swaps the picture into the same materials). The web adapter supplies it here.
  void setAtlas(rhi::Texture atlas);
  bool loadAtlas(const std::string& eimgPath);   // EIMG file (tools/imgconv) -> setAtlas
  // Native: the `.eimg` for the catalog's `tekstur.rel1067` picture — `<cacheDir>/<stem>.eimg`, converted from
  // `<ppkaRoot>/public/model3d/<berkas>` with `<toolDir>/imgconv --max 512 --wrap-s clamp --flip` when missing
  // (the picture is a pilot asset and stays out of the repo). "" when the picture or the tool is unavailable.
  static std::string prepareAtlas(const std::string& ppkaRoot, const std::string& cacheDir, const std::string& toolDir);

private:
  void paintTexture();
  std::vector<RailChunk> chunks_;
  static constexpr int BLADE_STEPS = 5;        // closed .. open blade meshes (animated over BLADE_TIME)
  struct TurnoutMesh { std::string nodeId; rhi::Mesh blade[2][BLADE_STEPS]; AABB bounds; int setting = 0; mutable float pos[2]{0, 1}; };
  std::vector<TurnoutMesh> turnouts_;
  mutable double lastTick_ = 0;
  GpuModel sleeperModel_;
  std::vector<RailSample> samples_;
  std::vector<std::vector<vec3>> centre_;      // per-segment centreline (scene, rail head + 0.5) for the iconic line
  mutable rhi::Mesh iconic_; mutable float iconicWidth_ = 0; mutable bool iconicOn_ = false;
  Stats stats_;
};

} // namespace eng

// Turntables ("meja putar", ppka-wannabe-2 src/engine/mejaPutar.ts). One scenery object `kind:'mejaPutar'`
// (pos = pivot, w = diameter) + its BRIDGE = the track segment whose two ends sit on the pit rim. The pit
// (floor + wall + coping) is static; the bridge deck and its two rails turn about the pivot by the angle the
// sim sends (`meja:[{id, angle}]`, 0…π while turning a loco, 0 at rest — the deck is symmetric, so the snap
// back to 0 when the sim mirrors the loco is invisible). The bridge segment is handed to RailBuilder::skipSegs
// so the static rail mesh does not stay behind while the deck turns.
#pragma once
#include "engine/core/json.h"
#include "engine/render/model_renderer.h"
#include "engine/sim/sim_state.h"
#include "engine/world/coords.h"
#include "engine/world/rail_profile.h"
#include "engine/world/track_graph.h"
#include <set>
#include <string>
#include <vector>

namespace eng {

namespace mejaputar {
constexpr float RIM_TOL = 2.5f;         // bridge end ↔ rim distance tolerance (TOL_BIBIR)
constexpr float PIT_DEPTH = 1.25f;      // pit floor below rail head
constexpr float DECK_DOWN = 0.36f;      // deck top below rail head (rail + base plate)
constexpr float DECK_H = 0.55f, DECK_W = 3.1f;
constexpr float RAIL_HALF = 0.534f + 0.042f, RAIL_W = 0.074f, RAIL_H = 0.176f;
constexpr float COPING_W = 0.45f;
} // namespace mejaputar

struct MejaPutarSpot {
  std::string id, segId;
  vec3 pivot;          // scene, y = rail head height at the pivot
  float radius = 10;
  float yaw0 = 0;      // bridge axis yaw at rest (rotationY convention of train_visual: atan2(-dz, dx))
  float angle = 0;     // from the sim
};

class MejaPutarVisuals {
public:
  // Bridge segments of every turntable in `world` (call BEFORE RailBuilder::build to fill skipSegs).
  static std::set<std::string> bridgeSegments(const TrackGraph& g, const Json& world);
  void build(const TrackGraph& g, const RailProfile& profile, const Json& world);
  void setState(const std::vector<SimMeja>& state);
  void draw(ModelRenderer& r, const Frustum* frustum = nullptr) const;
  void destroy();
  const std::vector<MejaPutarSpot>& spots() const { return spots_; }

private:
  std::vector<MejaPutarSpot> spots_;
  struct Part { rhi::Mesh mesh; AABB bounds; bool used = false; };
  Part pit_, coping_;                       // static, every turntable baked together
  std::vector<Part> deck_, rails_;          // per turntable, local to its pivot (turned at draw time)
  Material pitMat_, copingMat_, deckMat_, railMat_;
};

} // namespace eng

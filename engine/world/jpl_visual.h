// JPL level crossings (docs/world-spec.md §5.6, port of ppka-wannabe-2 src/tiga/uji3dJPL.ts and the
// gate logic of bangun3d.ts:657-691). One scenery object `kind:'jpl'` = one crossing = two barrier
// leaves on opposite road sides and opposite sides of the track: posts at (±a, ±b) in the road frame,
// a = max(7, span of crossed tracks + 3.2), b = 3.6; foundation, pipe post, lamp head with two lamps
// and a crossbuck are static; the 6 m red/white arm pivots at 1.05 m about the leaf's local X:
// open 1 = raised, 0 = horizontal. Whether a crossing is closed comes from the sim (bridge `step`
// → `jpl:[{id, closed}]`, `World.jplClosed`: a train within 350 m on any track within 25 m); the arm
// moves in DETIK_PALANG = 5 s of sim time and the lamps alternate every 460 ms while closed.
#pragma once
#include "engine/asset/model.h"
#include "engine/core/json.h"
#include "engine/render/model_renderer.h"
#include "engine/sim/sim_process.h"
#include "engine/world/coords.h"
#include "engine/world/track_graph.h"
#include <functional>
#include <string>
#include <vector>

namespace eng {

namespace jpl {
constexpr float FOUNDATION = 0.55f, FOUNDATION_H = 0.25f;
constexpr float POST_D = 0.110f, POST_H = 2.35f;
constexpr float ARM_L = 6.0f, ARM_H = 0.140f, ARM_T = 0.090f, ARM_Y = 1.05f; constexpr int ARM_STRIPES = 8;
constexpr float LAMP_Y = 2.05f, LAMP_R = 0.105f, LAMP_GAP = 0.34f, LAMP_OUT = 0.08f, HEAD_L = 0.85f;
constexpr float CROSS_L = 0.95f, CROSS_W = 0.11f, CROSS_Y = 1.62f;
constexpr float FROM_RAIL = 7.0f, RAIL_CLEAR = 3.2f, FROM_EDGE = 3.6f;   // dariRel, bebasRel, dariTepi
constexpr float SPAN_RADIUS = 45, ROAD_HALF = 6;                        // RADIUS_RENTANG_JPL, setengahJalan
constexpr float SNAP_RADIUS = 60;                                       // nearestOnTrack max distance
constexpr float CLOSE_SECONDS = 5;                                      // DETIK_PALANG
constexpr double BLINK_MS = 460;
// Post distance from the crossing point along the road, given the farthest crossed track (jarakTiangJPL).
inline float postDistance(float span) { return std::fmax(FROM_RAIL, span + RAIL_CLEAR); }
} // namespace jpl

struct JplGate { vec3 pos; float rotY = 0; mat4 base; vec3 lamp[2]; };
struct JplCrossing { std::string id, label; vec3 pos; int gate0 = 0; bool closed = false; float open = 1; };

class JplVisuals {
public:
  struct Stats { unsigned staticTris = 0, armTris = 0, lampTris = 0; };
  using GroundFn = std::function<float(double wx, double wy)>;

  void build(const TrackGraph& g, const Json& world, const WorldOrigin& origin, const GroundFn& ground);
  void setState(const std::vector<SimJpl>& state);   // from SimState::jpl
  // Steps the arms by `dtSim` sim seconds (real dt × max(1, time scale); 0 when paused).
  void animate(float dtSim);
  void draw(ModelRenderer& r, const Frustum* frustum = nullptr) const;
  void destroy();
  const std::vector<JplCrossing>& crossings() const { return crossings_; }
  const std::vector<JplGate>& gates() const { return gates_; }
  Stats stats;

  // Pure placement helpers (testable): two leaves for a crossing at scene (x, z), road yaw = -rot.
  static void gatesFor(float x, float z, float roadYaw, float span, JplGate out[2]);
  // Farthest crossed track from the crossing point, measured along the road (rentangRelJPL).
  static float trackSpan(const std::vector<std::pair<double, double>>& samples, double px, double py, double rot);

private:
  std::vector<JplCrossing> crossings_;
  std::vector<JplGate> gates_;
  struct Batch { rhi::Mesh mesh; AABB bounds; bool used = false; };
  Batch foundation_, post_, head_, cross_;
  rhi::Mesh arm_, lamp_; AABB armBounds_; bool built_ = false;
  rhi::Texture armTex_{};
  Material foundationMat_, postMat_, headMat_, crossMat_, armMat_, lampOff_, lampOn_;
};

} // namespace eng

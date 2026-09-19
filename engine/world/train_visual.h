// Train visuals: places every vehicle of every sim train per docs/world-spec.md §7.4 from the
// bridge's per-vehicle coupler points (front/rear) with body sway (§7.5), draws GLB prototypes
// (RollingStock) or a box fallback, scrubs door clips by the dwell timer (keretaVisual3d.ts
// `tPintu`), and adds head/tail light coronas (day and night) plus the Semboyan 21 tail markers on the
// last vehicle. Exposes per-train label anchors for the HUD.
#pragma once
#include "engine/render/mesh_builder.h"
#include "engine/sim/sim_process.h"
#include "engine/world/coords.h"
#include "engine/world/height_source.h"
#include "engine/world/rolling_stock.h"
#include "engine/world/train_cull.h"
#include <map>
#include <string>
#include <vector>

namespace eng {

struct VehicleInstance {
  const VehicleProto* proto = nullptr;   // nullptr = box fallback
  mat4 place;          // translation * rotY(yaw) * rotZ(pitch) * rotX(roll), scene space (flipped for the rear KRL cab)
  float scale = 1;     // len / panjang (models only)
  float len = 0;
  bool loco = false;
  vec3 color;          // box colour
  vec3 centre;         // scene position of the car centre on the rail head
  AABB bounds;         // scene-space, for culling
  int pose = -1;       // index into TrainVisuals' per-frame node poses (door clips scrubbed); -1 = proto rest pose
  uint32_t muatan = 0; // RollingStock::cargoHash(train no, car index): which cargo variant this car shows
  // A standing / crawling train (< 0.3 m/s) is walk-through for the pedestrian camera (keretaVisual3d.ts
  // `tembus`, Henry's choice: cross to the other platform without walking round a 250 m consist); anything
  // moving stays a wall. WorldScene::collectWalkBoxes skips these.
  bool tembus = false;
  bool kabin = false;      // the front vehicle of the cab-view subject: drawn with the proto's cab materials (single-sided skin, glass see-through)
  bool interior = true;    // interior meshes shown (near the camera or ridden); false = drawn with the interior nodes collapsed
};

struct TrainLabel { std::string id, no, name, state; float speed = 0; vec3 anchor; vec3 dir; };   // dir = unit travel direction (scene XZ)

// One lit lamp: a directional additive corona (keretaVisual3d.ts pasangLampuKA)
struct TrainLight { vec3 pos, aim; bool tail = false; };
// Semboyan 21 unit on the last vehicle: `frame` = mount point on the body wall (unit extends to +Z of it; the
// right-hand unit is the mirrored mesh)
struct TailMarker { mat4 frame; bool right = false; };

class TrainVisuals {
public:
  void init(RollingStock& stock);
  void shutdown();
  // timeScale: session time scale (sway amplitude damping, §7.5 skalaLaju)
  void update(const SimState& st, const WorldOrigin& origin, const RailProfile* rail, double timeScale = 1);
  // Camera data for the coronas + the night switch (corona opacity 1 at night / 0.72 by day, S21 lanterns; same rule as signals).
  void setView(float fovY, int viewportH, bool night) { fovY_ = fovY; viewportH_ = viewportH; night_ = night; }
  // Cull context for the next update() (train_cull.h): the camera eye (scene), the fog's far edge (<= 0 = no
  // fog), the mode's reference distance (>= AMBANG_LOD = every train is an iconic box, the old `detail`
  // flag), and the camera's subject train (never culled).
  // `cabView`: the camera sits in the subject's cab -> its front vehicle gets the cab treatment (VehicleInstance::kabin).
  void setCull(vec3 eye, float fogFar, float refDistance, const std::string& subjectId, bool cabView = false) {
    cullEye_ = eye; cullFogFar_ = fogFar; cullRef_ = refDistance; cullSubject_ = subjectId; cullValid_ = true; cabView_ = cabView;
  }
  struct CullCount { unsigned penuh = 0, ringan = 0, sembunyi = 0; };
  const CullCount& cullCount() const { return cacah_; }
  TingkatKA tier(const std::string& trainId) const { auto it = tingkat_.find(trainId); return it == tingkat_.end() ? TingkatKA::Penuh : (TingkatKA)it->second; }
  void draw(ModelRenderer& r, const Frustum* frustum = nullptr);
  const std::vector<VehicleInstance>& vehicles() const { return vehicles_; }
  const std::vector<TrainLabel>& labels() const { return labels_; }   // one per train, front vehicle centre + 4.5 m
  const std::vector<TrainLight>& lights() const { return lights_; }
  float doorTime(const std::string& trainId) const { auto it = doors_.find(trainId); return it == doors_.end() ? 0.f : it->second; }
  unsigned drawn = 0, culledVehicles = 0;   // per draw() stats
  static vec3 boxColor(const std::string& sarana, bool loco);
private:
  void drawLights(ModelRenderer& r);
  RollingStock* stock_ = nullptr;
  rhi::Mesh box_{}, billboard_{};
  rhi::Texture coronaTex_{};
  // Semboyan 21 meshes (left-hand unit; the right-hand one is mirrored in Z): day plate, day/night arm and
  // lantern housing, red glass (faces -X), green glass (faces +X)
  rhi::Mesh s21Plate_{}, s21Iron_{}, s21Housing_{}, s21Red_{}, s21Green_{};
  Material coronaMat_;
  std::vector<Material> coronaPool_;
  std::vector<VehicleInstance> vehicles_;
  std::vector<TrainLabel> labels_;
  std::vector<TrainLight> lights_;
  std::vector<TailMarker> markers_;
  std::vector<std::vector<mat4>> poses_;
  std::map<std::string, float> doors_;   // per train: door clip time (s, sim time), 0 = closed
  float doorDuration_ = 2;               // max over door clips seen (keretaVisual3d.ts durasiPintu), ≥ 2 s
  double lastClock_ = -1;
  float fovY_ = 0.9f; int viewportH_ = 720; bool night_ = false;
  // per-train cull tier of the last update (hysteresis needs the previous tier); trains gone from the state are dropped
  std::map<std::string, int> tingkat_;
  CullCount cacah_;
  vec3 cullEye_; float cullFogFar_ = 0, cullRef_ = 0; std::string cullSubject_; bool cullValid_ = false, cabView_ = false;
  void drawCab(ModelRenderer& r, const VehicleInstance& v, const mat4& base, const std::vector<mat4>* pose, const Frustum* frustum);
};

} // namespace eng

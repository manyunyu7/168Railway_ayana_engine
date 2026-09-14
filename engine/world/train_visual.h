// Train visuals: places every vehicle of every sim train per docs/world-spec.md §7.4 from the
// bridge's per-vehicle coupler points (front/rear) with body sway (§7.5), draws GLB prototypes
// (RollingStock) or a box fallback, scrubs door clips by the dwell timer (keretaVisual3d.ts
// `tPintu`), and adds head/tail light coronas at night plus the Semboyan 21 tail markers on the
// last vehicle. Exposes per-train label anchors for the HUD.
#pragma once
#include "engine/render/mesh_builder.h"
#include "engine/sim/sim_process.h"
#include "engine/world/coords.h"
#include "engine/world/height_source.h"
#include "engine/world/rolling_stock.h"
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
  // Camera data for the coronas + the night switch (lights and lanterns on; same rule as signals).
  void setView(float fovY, int viewportH, bool night) { fovY_ = fovY; viewportH_ = viewportH; night_ = night; }
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
};

} // namespace eng

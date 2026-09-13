// Train visuals: places every vehicle of every sim train per docs/world-spec.md §7.4 from the
// bridge's per-vehicle coupler points (front/rear), draws GLB prototypes (RollingStock) or a box
// fallback, and exposes per-train label anchors for the HUD. Sway (§7.5) is not applied yet.
#pragma once
#include "engine/render/mesh_builder.h"
#include "engine/sim/sim_process.h"
#include "engine/world/coords.h"
#include "engine/world/height_source.h"
#include "engine/world/rolling_stock.h"
#include <string>
#include <vector>

namespace eng {

struct VehicleInstance {
  const VehicleProto* proto = nullptr;   // nullptr = box fallback
  mat4 place;          // translation * rotY(yaw) * rotZ(pitch) * rotX(roll), scene space
  float scale = 1;     // len / panjang (models only)
  float len = 0;
  bool loco = false;
  vec3 color;          // box colour
  vec3 centre;         // scene position of the car centre on the rail head
  AABB bounds;         // scene-space, for culling
};

struct TrainLabel { std::string id, no, name, state; float speed = 0; vec3 anchor; };

class TrainVisuals {
public:
  void init(RollingStock& stock);
  void shutdown();
  void update(const SimState& st, const WorldOrigin& origin, const RailProfile* rail);
  void draw(ModelRenderer& r, const Frustum* frustum = nullptr);
  const std::vector<VehicleInstance>& vehicles() const { return vehicles_; }
  const std::vector<TrainLabel>& labels() const { return labels_; }   // one per train, front vehicle centre + 4.5 m
  unsigned drawn = 0, culledVehicles = 0;   // per draw() stats
  static vec3 boxColor(const std::string& sarana, bool loco);
private:
  RollingStock* stock_ = nullptr;
  rhi::Mesh box_{};
  std::vector<VehicleInstance> vehicles_;
  std::vector<TrainLabel> labels_;
};

} // namespace eng

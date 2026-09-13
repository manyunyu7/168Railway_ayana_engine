// Points (wesel) arrows (docs/world-spec.md §6.4): one arrow per leg, 1.45 m to that leg's side,
// 2.2 m above the rail, pointing sideways toward its leg; colour from the setting/lock state.
#pragma once
#include "engine/asset/model.h"
#include "engine/render/model_renderer.h"
#include "engine/world/height_source.h"
#include "engine/world/signal_visual.h"   // ScreenPoint
#include "engine/world/track_graph.h"
#include <string>
#include <vector>

namespace eng {

struct PointInstance {
  std::string nodeId;
  int legSeg[2] = {-1, -1};
  vec3 pos;               // node at rail height (scene)
  mat4 arrow[2];          // per-leg arrow transform
  int setting = 0;
  bool locked = false;
};

class PointVisuals {
public:
  static constexpr float ARROW_SCALE = 2.4f;    // PANAH_M
  static constexpr float ARROW_OFFSET = 1.45f;  // PANAH_JARAK
  static constexpr float ARROW_HEIGHT = 2.2f;   // PANAH_TINGGI
  static constexpr float PICK_RADIUS_PX = 40;

  void build(const TrackGraph& g, const RailProfile& profile);
  void setState(const std::string& nodeId, int setting, bool locked);
  void draw(ModelRenderer& r, const Frustum* frustum = nullptr) const;
  std::vector<ScreenPoint> screenPositions(const mat4& viewProj, int w, int h) const;   // arrow centre per point
  void destroy();
  const std::vector<PointInstance>& points() const { return points_; }

private:
  std::vector<PointInstance> points_;
  rhi::Mesh arrow_;
  AABB arrowBounds_;
  Material mat_[4];   // set, set+locked, unset, unset+locked
};

} // namespace eng

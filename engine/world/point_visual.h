// Points (wesel) arrows (docs/world-spec.md §6.4, bangun3d.ts:694-761): one arrow per leg, 1.45 m to
// that leg's side, 2.2 m above the rail, pointing sideways toward its leg; colour from the
// setting/lock state. Locked: camera-facing padlock at 4.1 m. Within 340 m: per-leg ground glow
// (30 x 12) and a vertical curtain (30 x 7) 12 m along the leg, in the leg's colour — only while the camera
// mode's reference distance is under AMBANG_LOD (900 m, the "detail" flag). The whole group scales by
// skalaWesel = max(1, refDist / 240) about the node (bangun3d.ts perbaruiWesel).
#pragma once
#include "engine/asset/model.h"
#include "engine/render/model_renderer.h"
#include "engine/world/height_source.h"
#include "engine/world/signal_visual.h"   // ScreenPoint
#include "engine/world/track_graph.h"
#include <algorithm>
#include <string>
#include <vector>

namespace eng {

struct PointInstance {
  std::string nodeId;
  int legSeg[2] = {-1, -1};
  vec3 pos;               // node at rail height (scene)
  mat4 arrow[2];          // per-leg arrow transform
  vec3 legDir[2];         // unit direction from the node along each leg (scene, horizontal)
  float legSide[2]{};     // +1 = arrow on the `left` side of the facing direction, -1 = right
  vec3 left;              // unit left of the facing direction
  int setting = 0;
  bool locked = false;
};

class PointVisuals {
public:
  static constexpr float ARROW_SCALE = 2.4f;    // PANAH_M
  static constexpr float ARROW_OFFSET = 1.45f;  // PANAH_JARAK
  static constexpr float ARROW_HEIGHT = 2.2f;   // PANAH_TINGGI
  static constexpr float PICK_RADIUS_PX = 40;
  static constexpr float GLOW_RANGE = 340;      // KABUT_JARAK
  static constexpr float GLOW_LENGTH = 30, GLOW_WIDTH = 12, CURTAIN_HEIGHT = 7;   // KABUT_PANJANG/_TINGGI

  void build(const TrackGraph& g, const RailProfile& profile);
  void setState(const std::string& nodeId, int setting, bool locked);
  static constexpr float LOD_THRESHOLD = 900;   // AMBANG_LOD
  // Uses the eye given to ModelRenderer::beginFrame for the padlock billboard and glow range. refDist = the
  // camera mode's reference distance (0 = unknown: scale 1, detail on).
  void draw(ModelRenderer& r, const Frustum* frustum = nullptr, float refDist = 0) const;
  static float scaleFor(float refDist) { return std::max(1.f, refDist / 240); }
  // arrow centre per point (refDist as in draw: the pick anchor rises with the scale)
  std::vector<ScreenPoint> screenPositions(const mat4& viewProj, int w, int h, float refDist = 0) const;
  void destroy();
  const std::vector<PointInstance>& points() const { return points_; }

private:
  std::vector<PointInstance> points_;
  rhi::Mesh arrow_, glow_, padDark_, padOrange_, padHole_;
  rhi::Texture glowTex_{};
  AABB arrowBounds_;
  Material mat_[4], glowMat_[4], curtainMat_[4];   // set, set+locked, unset, unset+locked
  Material padMat_[3];                             // dark disc, orange, keyhole
};

} // namespace eng

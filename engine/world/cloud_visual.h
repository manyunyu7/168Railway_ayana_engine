// Sprite clouds (spec §9.2, dunia3d.ts bangunAwan): camera-facing soft billboards pinned in world space
// over the corridor bbox + 6 km margin, 0.35 per km² clamped to 40..220, in two layers (3:1) —
// large low cumulus at 760–1100 m and a thin blanket at 1250–1750 m. Alpha = procedural radial
// blobs (8 texture variants × 3 opacities), no depth write, tinted by the light ladder's cloud
// colour (Sky::cloudTint), fogged like the world. Draw after the opaque world (depth-tested, so
// terrain in front still occludes) and before the HUD. Drift is a slow uniform wind.
#pragma once
#include "engine/math/math.h"
#include "engine/render/model_renderer.h"
#include "engine/render/sky.h"
#include "engine/rhi/rhi.h"

namespace eng {

class CloudVisual {
public:
  static constexpr float MARGIN = 6000, DENSITY = 0.35f, Y_LOW = 760;   // AWAN_MARGIN / AWAN_KEPADATAN / AWAN_Y_RENDAH
  static constexpr int MIN_N = 40, MAX_N = 220, VARIANTS = 8;              // AWAN_MIN / AWAN_MAKS / AWAN_VARIAN
  // Corridor bbox in scene metres (xz); sprites are scattered over it grown by MARGIN.
  void build(float x0, float z0, float x1, float z1, uint32_t seed = 1);
  void draw(const mat4& viewProj, const mat4& view, vec3 eye, const Sky& sky, const Lighting& light, float dt);
  void destroy();
  vec2 wind{1.2f, 0.4f};   // m/s
  int count() const { return count_; }
private:
  rhi::Program prog_{}; rhi::Mesh mesh_{}; rhi::Texture tex_{};
  int uVP_ = -1, uRight_ = -1, uUp_ = -1, uEye_ = -1, uTint_ = -1, uFog_ = -1, uFogD_ = -1, uDrift_ = -1, uBox_ = -1;
  float x0_ = 0, z0_ = 0, lx_ = 1, lz_ = 1; vec2 drift_{};
  int count_ = 0;
};

} // namespace eng

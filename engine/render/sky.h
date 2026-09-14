// Gradient sky drawn as a full-screen triangle behind everything (depth test off).
#pragma once
#include "engine/math/math.h"
#include "engine/rhi/rhi.h"

namespace eng {

class Sky {
public:
  void init();
  void shutdown();
  vec3 zenith{0.25f, 0.45f, 0.85f}, horizon{0.75f, 0.82f, 0.9f}, ground{0.45f, 0.42f, 0.38f};
  vec3 sunDir{0.4f, 0.8f, 0.5f};
  vec3 cloudTint{1, 1, 1};   // sprite cloud colour from the light ladder (CloudVisual reads it)
  // Host UI theme (dunia3dKonst.ts TEMA). The clock owns the sky in both themes; the dark theme only keeps the
  // fog / dome ground tinted towards its `kabut` 0x121821, the light one towards 0xcadced (applySun, sun.h).
  bool darkTheme = false;
  void draw(const mat4& invViewProj, vec3 eye);
private:
  rhi::Program prog_; rhi::Mesh tri_;
  int uInvVP_ = -1, uEye_ = -1, uZenith_ = -1, uHorizon_ = -1, uGround_ = -1, uSun_ = -1;
};

} // namespace eng

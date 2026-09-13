// Trainz-style camera "compass" (port of ppka-wannabe-2 src/tiga/kompas3d.ts).
// The compass is the camera's FOCUS POINT on the ground: short right-click flies it there,
// holding right-click glides it (speed from cursor distance to screen centre), Ctrl+arrows nudge it.
// The orbit camera simply follows the focus with its current offset.
#pragma once
#include "engine/core/orbit_camera.h"
#include "engine/math/geometry.h"
#include "engine/render/model_renderer.h"
#include "engine/rhi/rhi.h"
#include <functional>

namespace eng {

constexpr float COMPASS_DEAD_ZONE = 0.06f;   // normalised screen radius with no motion
constexpr float COMPASS_JUMP_LIMIT = 4;      // short-click jump ≤ 4× camera distance
constexpr float COMPASS_JUMP_SECONDS = 0.55f;
constexpr float COMPASS_FADE_SECONDS = 1.4f;

// Pure math (mirrors the TS functions, tested there in tests/kompas3d.ts).
float glideSpeed(float r, float distance, float k = 1);                 // lajuLuncur
vec2 glideDirection(float nx, float ny, float ox, float oz);            // arahLuncur (not normalised)
float rotationRate(float nx, float r);                                   // putarRotation (rad/s)
vec2 clampJump(vec2 d, float distance);                                  // batasLompat
int azimuthDegrees(float lx, float lz);                                  // 0 = north, 90 = east

struct CompassSettings { bool rotation = true; float speed = 1; bool show = true; };

class Compass {
public:
  // groundHeight: carved terrain height at scene (x, z). Must outlive the compass.
  void init(std::function<float(float, float)> groundHeight);
  void shutdown();
  CompassSettings settings;

  // Per frame. mouse in framebuffer pixels; rightDown = RMB state; dt seconds.
  // Returns true when the compass consumed the right button (so callers don't orbit with it).
  bool update(OrbitCamera& cam, double mx, double my, int w, int h, bool rightDown, bool ctrl,
              bool left, bool right, bool up, bool down, float dt, const mat4& invViewProj);
  void draw(ModelRenderer& r, const OrbitCamera& cam);
  bool visible() const { return opacity_ > 0.01f; }
  int azimuth(const OrbitCamera& cam) const;

private:
  void wake();
  bool groundHit(const Ray& ray, vec3& out) const;   // ray-march against the heightfield
  std::function<float(float, float)> ground_;
  rhi::Mesh ring_, cross_, north_, viewArrow_;
  Material mat_;
  float opacity_ = 0, fadeAt_ = 0, time_ = 0;
  bool gliding_ = false, pressed_ = false; double pressX_ = 0, pressY_ = 0; float pressT_ = 0;
  struct Jump { vec3 from, to; float u; bool active = false; } jump_;
};

} // namespace eng

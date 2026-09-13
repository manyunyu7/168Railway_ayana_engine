// Camera orbiting a target point. Y up.
#pragma once
#include "engine/math/math.h"

namespace eng {

struct OrbitCamera {
  vec3 target{0, 0, 0};
  float distance = 5, yaw = radians(35), pitch = radians(25);
  float fovY = radians(50), near = 0.05f, far = 2000;

  vec3 position() const {
    return target + vec3{std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw)} * distance;
  }
  mat4 view() const { return mat4::lookAt(position(), target, {0, 1, 0}); }
  mat4 projection(float aspect) const { return mat4::perspective(fovY, aspect, near, far); }

  void rotate(float dx, float dy) {
    yaw -= dx * 0.005f; pitch += dy * 0.005f;
    const float limit = radians(89);
    if (pitch > limit) pitch = limit;
    if (pitch < -limit) pitch = -limit;
  }
  void zoom(float steps) { distance *= std::pow(0.9f, steps); if (distance < near * 4) distance = near * 4; }
};

} // namespace eng

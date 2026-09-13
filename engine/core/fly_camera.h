// Free-fly camera (WASD + mouse look). Y up.
#pragma once
#include "engine/math/math.h"

namespace eng {

struct FlyCamera {
  vec3 position{0, 5, 10};
  float yaw = 0, pitch = -0.2f;       // radians; yaw 0 looks down -Z
  float fovY = radians(55), near = 0.3f, far = 3000;
  float speed = 20;                   // m/s

  vec3 forward() const { return {-std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch)}; }
  vec3 right() const { return {std::cos(yaw), 0, -std::sin(yaw)}; }
  mat4 view() const { return mat4::lookAt(position, position + forward(), {0, 1, 0}); }
  mat4 projection(float aspect) const { return mat4::perspective(fovY, aspect, near, far); }

  void look(float dx, float dy) {
    yaw -= dx * 0.003f; pitch -= dy * 0.003f;
    const float lim = radians(89); if (pitch > lim) pitch = lim; if (pitch < -lim) pitch = -lim;
  }
  void move(float fwd, float side, float up, float dt) {
    position += (forward() * fwd + right() * side + vec3{0, 1, 0} * up) * (speed * dt);
  }
};

} // namespace eng

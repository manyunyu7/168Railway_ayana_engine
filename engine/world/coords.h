// World <-> scene coordinates (see docs/world-spec.md §2).
// World = EPSG:3857 metres with Y flipped (south-positive), kept in double.
// Scene = float, origin at the track-node bbox centre: x = wx - ox, z = wy - oz, y = height (up).
#pragma once
#include "engine/math/math.h"

namespace eng {

struct WorldOrigin {
  double ox = 0, oz = 0;
  vec3 toScene(double wx, double wy, float height) const { return {(float)(wx - ox), height, (float)(wy - oz)}; }
  void toWorld(vec3 p, double& wx, double& wy) const { wx = p.x + ox; wy = p.z + oz; }
  // Yaw for a 2-D world tangent (tx, ty): local +X -> (cos yaw, -sin yaw) in (x, z).
  static float yawFromTangent(double tx, double ty) { return (float)std::atan2(-ty, tx); }
  static mat4 yawMatrix(float yaw) { return mat4::rotationY(yaw); }
};

} // namespace eng

// Geometric helpers: AABB, ray, plane, frustum.
#pragma once
#include "engine/math/math.h"

namespace eng {

struct AABB {
  vec3 min{1e30f, 1e30f, 1e30f}, max{-1e30f, -1e30f, -1e30f};
  bool valid() const { return min.x <= max.x; }
  void expand(vec3 p) { min = vmin(min, p); max = vmax(max, p); }
  void expand(const AABB& b) { if (b.valid()) { expand(b.min); expand(b.max); } }
  vec3 center() const { return (min + max) * 0.5f; }
  vec3 extent() const { return (max - min) * 0.5f; }
  AABB transformed(const mat4& m) const {
    AABB r;
    for (int c = 0; c < 8; ++c)
      r.expand(m.transformPoint({c & 1 ? max.x : min.x, c & 2 ? max.y : min.y, c & 4 ? max.z : min.z}));
    return r;
  }
};

struct Ray {
  vec3 origin, dir;   // dir normalized
  vec3 at(float t) const { return origin + dir * t; }
};

// Slab test. Returns entry distance in tOut (>= 0) when hit.
inline bool intersect(const Ray& r, const AABB& b, float& tOut) {
  float t0 = 0, t1 = 1e30f;
  const float* o = &r.origin.x; const float* d = &r.dir.x; const float* mn = &b.min.x; const float* mx = &b.max.x;
  for (int i = 0; i < 3; ++i) {
    float inv = 1.f / d[i];
    float a = (mn[i] - o[i]) * inv, c = (mx[i] - o[i]) * inv;
    if (a > c) { float t = a; a = c; c = t; }
    if (a > t0) t0 = a;
    if (c < t1) t1 = c;
    if (t0 > t1) return false;
  }
  tOut = t0; return true;
}

// Möller–Trumbore, both faces.
inline bool intersect(const Ray& r, vec3 a, vec3 b, vec3 c, float& tOut) {
  vec3 e1 = b - a, e2 = c - a, p = cross(r.dir, e2);
  float det = dot(e1, p);
  if (det > -1e-8f && det < 1e-8f) return false;
  float inv = 1.f / det; vec3 s = r.origin - a;
  float u = dot(s, p) * inv; if (u < 0 || u > 1) return false;
  vec3 q = cross(s, e1);
  float v = dot(r.dir, q) * inv; if (v < 0 || u + v > 1) return false;
  float t = dot(e2, q) * inv; if (t <= 0) return false;
  tOut = t; return true;
}

struct Plane { vec3 n; float d; float distance(vec3 p) const { return dot(n, p) + d; } };

// Six planes extracted from a view-projection matrix (Gribb/Hartmann). Normals point inward.
struct Frustum {
  Plane planes[6];
  explicit Frustum(const mat4& vp) {
    auto row = [&](int r) { return vec4{vp.m[0][r], vp.m[1][r], vp.m[2][r], vp.m[3][r]}; };
    vec4 r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
    auto set = [&](int i, vec4 v) {
      float l = length(v.xyz()); planes[i] = {v.xyz() / l, v.w / l};
    };
    set(0, {r3.x + r0.x, r3.y + r0.y, r3.z + r0.z, r3.w + r0.w});  // left
    set(1, {r3.x - r0.x, r3.y - r0.y, r3.z - r0.z, r3.w - r0.w});  // right
    set(2, {r3.x + r1.x, r3.y + r1.y, r3.z + r1.z, r3.w + r1.w});  // bottom
    set(3, {r3.x - r1.x, r3.y - r1.y, r3.z - r1.z, r3.w - r1.w});  // top
    set(4, {r3.x + r2.x, r3.y + r2.y, r3.z + r2.z, r3.w + r2.w});  // near
    set(5, {r3.x - r2.x, r3.y - r2.y, r3.z - r2.z, r3.w - r2.w});  // far
  }
  bool contains(const AABB& b) const {
    vec3 c = b.center(), e = b.extent();
    for (const Plane& p : planes) {
      float r = e.x * std::fabs(p.n.x) + e.y * std::fabs(p.n.y) + e.z * std::fabs(p.n.z);
      if (p.distance(c) + r < 0) return false;
    }
    return true;
  }
};

// Ray through a pixel (0..1 normalized screen coords, y down) for the given camera.
inline Ray screenRay(float sx, float sy, const mat4& invViewProj) {
  vec3 n = invViewProj.transformPoint({sx * 2 - 1, 1 - sy * 2, -1});
  vec3 f = invViewProj.transformPoint({sx * 2 - 1, 1 - sy * 2, 1});
  return {n, normalize(f - n)};
}

} // namespace eng

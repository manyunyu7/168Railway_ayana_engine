// Centripetal Catmull-Rom spline through 2-D points + the tile placement of hiasan "garis"
// (docs/world-spec.md §3.5, port of ppka-wannabe-2 src/tiga/uji3dSpline.ts bingkaiGaris). Pure
// geometry, no GPU: scene-space xz points in, per-tile frames out.
#pragma once
#include "engine/math/math.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

namespace eng {

namespace spline {
constexpr float DUPLICATE_MIN = 0.05f;   // RAPAT_MIN: closer points count as one (centripetal CR gives NaN on twins)
constexpr float STEP_MIN = 0.25f;        // LANGKAH_MIN
constexpr int ARC_DIV_MIN = 200, ARC_DIV_MAX = 6000;   // arcLengthDivisions = clamp(ceil(chord / 0.25))
}

// One tile ready to place: y = surface height, yaw about Y, pitch about the tile's own +X (Euler YZX),
// scale on the long axis only.
struct SplineFrame { float x, y, z, yaw, pitch, scale; };

struct SplineFrameOptions {
  float step = 1;                                        // planting distance (m) — from the catalog, never the mesh bbox
  std::function<float(float, float)> height;             // surface height at scene (x, z); empty = 0
  bool centred = true;                                   // true: tiles centred in their slot (bodies); false: exactly at k·step incl. 0 and L (posts)
  bool scaleRemainder = true;                            // 'skala' (default) vs 'buang' for the end remainder
  bool flat = false;                                     // datar: one straight grade end-to-end, constant pitch
};

// Centripetal Catmull-Rom (three.js CatmullRomCurve3 'centripetal', open) with an arc-length table.
class CatmullRom {
public:
  // Returns false when fewer than 2 distinct points remain (or zero length).
  bool set(const std::vector<vec2>& pts) {
    pts_.clear();
    for (vec2 p : pts) if (pts_.empty() || std::hypot(p.x - pts_.back().x, p.y - pts_.back().y) >= spline::DUPLICATE_MIN) pts_.push_back(p);
    if (pts_.size() < 2) return false;
    float chord = 0; for (size_t i = 1; i < pts_.size(); ++i) chord += std::hypot(pts_[i].x - pts_[i - 1].x, pts_[i].y - pts_[i - 1].y);
    int div = std::clamp((int)std::ceil(chord / 0.25f), spline::ARC_DIV_MIN, spline::ARC_DIV_MAX);
    lengths_.assign((size_t)div + 1, 0);
    vec2 prev = point(0);
    for (int i = 1; i <= div; ++i) { vec2 c = point((float)i / (float)div); lengths_[(size_t)i] = lengths_[(size_t)i - 1] + std::hypot(c.x - prev.x, c.y - prev.y); prev = c; }
    return length() > 1e-6f;
  }
  float length() const { return lengths_.empty() ? 0.f : lengths_.back(); }
  size_t pointCount() const { return pts_.size(); }

  // Position at parameter t in [0,1] (uniform over segments, like three.js getPoint).
  vec2 point(float t) const {
    size_t n = pts_.size();
    float p = (float)(n - 1) * t;
    size_t i = (size_t)std::floor(p); float w = p - (float)i;
    if (i >= n - 1) { i = n - 2; w = 1; }
    vec2 p1 = pts_[i], p2 = pts_[i + 1];
    vec2 p0 = i > 0 ? pts_[i - 1] : vec2{2 * p1.x - p2.x, 2 * p1.y - p2.y};
    vec2 p3 = i + 2 < n ? pts_[i + 2] : vec2{2 * p2.x - p1.x, 2 * p2.y - p1.y};
    return centripetal(p0, p1, p2, p3, w);
  }
  // Unit tangent at t (central difference, like three.js getTangent).
  vec2 tangent(float t) const {
    float d = 1e-4f, t1 = std::max(0.f, t - d), t2 = std::min(1.f, t + d);
    vec2 a = point(t1), b = point(t2), v{b.x - a.x, b.y - a.y};
    float l = std::hypot(v.x, v.y); return l > 0 ? vec2{v.x / l, v.y / l} : vec2{1, 0};
  }
  // Parameter for a fraction u of the arc length (three.js getUtoTmapping).
  float tAtArc(float u) const {
    float target = std::clamp(u, 0.f, 1.f) * length();
    size_t lo = 0, hi = lengths_.size() - 1;
    while (lo < hi) { size_t mid = (lo + hi) / 2; if (lengths_[mid] < target) lo = mid + 1; else hi = mid; }
    size_t i = lo; if (i == 0) return 0;
    float before = lengths_[i - 1], seg = lengths_[i] - before;
    float f = seg > 0 ? (target - before) / seg : 0;
    return ((float)(i - 1) + f) / (float)(lengths_.size() - 1);
  }
  vec2 pointAtArc(float u) const { return point(tAtArc(u)); }
  vec2 tangentAtArc(float u) const { return tangent(tAtArc(u)); }

private:
  static vec2 centripetal(vec2 p0, vec2 p1, vec2 p2, vec2 p3, float t) {
    const float pow_ = 0.25f;   // centripetal: alpha 0.5 applied to squared distances
    auto dist = [&](vec2 a, vec2 b) { float dx = a.x - b.x, dy = a.y - b.y; return std::pow(dx * dx + dy * dy, pow_); };
    float dt0 = dist(p0, p1), dt1 = dist(p1, p2), dt2 = dist(p2, p3);
    if (dt1 < 1e-4f) dt1 = 1;
    if (dt0 < 1e-4f) dt0 = dt1;
    if (dt2 < 1e-4f) dt2 = dt1;
    auto cubic = [&](float x0, float x1, float x2, float x3) {
      float t1 = (x1 - x0) / dt0 - (x2 - x0) / (dt0 + dt1) + (x2 - x1) / dt1;
      float t2 = (x2 - x1) / dt1 - (x3 - x1) / (dt1 + dt2) + (x3 - x2) / dt2;
      t1 *= dt1; t2 *= dt1;
      float c0 = x1, c1 = t1, c2 = -3 * x1 + 3 * x2 - 2 * t1 - t2, c3 = 2 * x1 - 2 * x2 + t1 + t2;
      return c0 + c1 * t + c2 * t * t + c3 * t * t * t;
    };
    return {cubic(p0.x, p1.x, p2.x, p3.x), cubic(p0.y, p1.y, p2.y, p3.y)};
  }
  std::vector<vec2> pts_;
  std::vector<float> lengths_;
};

// bingkaiGaris: tiles along the curve. Body tiles at s = (i+0.5)·step with the remainder tile scaled
// (when > 2 % of a step); posts (centred=false) at i·step plus one at L when the remainder > 35 %.
// yaw = atan2(−t.z, t.x) (three.js rotation.y convention); flat classes take one straight grade.
inline std::vector<SplineFrame> splineFrames(const CatmullRom& c, const SplineFrameOptions& o) {
  std::vector<SplineFrame> out;
  float L = c.length(); if (L <= 1e-6f) return out;
  float step = std::max(spline::STEP_MIN, o.step);
  auto heightAt = [&](float s) { if (!o.height) return 0.f; vec2 p = c.pointAtArc(std::clamp(s / L, 0.f, 1.f)); return o.height(p.x, p.y); };
  bool flat = o.flat && o.height;
  float y0 = flat ? heightAt(0) : 0, y1 = flat ? heightAt(L) : 0;
  float pitchFlat = flat ? std::atan2(y1 - y0, L) : 0;
  auto place = [&](float s, float scale) {
    float u = std::clamp(s / L, 0.f, 1.f);
    vec2 p = c.pointAtArc(u), t = c.tangentAtArc(u);
    float yaw = std::atan2(-t.y, t.x), half = step * scale / 2;
    if (flat) { out.push_back({p.x, y0 + (y1 - y0) * u, p.y, yaw, pitchFlat, scale}); return; }
    float y = o.height ? o.height(p.x, p.y) : 0;
    float pitch = o.height ? std::atan2(heightAt(s + half) - heightAt(s - half), half * 2) : 0;   // slope from BOTH tile ends
    out.push_back({p.x, y, p.y, yaw, pitch, scale});
  };
  int n = (int)std::floor(L / step + 1e-6f);
  if (o.centred) {
    for (int i = 0; i < n; ++i) place(((float)i + 0.5f) * step, 1);
    float rem = L - (float)n * step;
    if (rem > step * 0.02f && o.scaleRemainder) place((float)n * step + rem / 2, rem / step);
  } else {
    for (int i = 0; i <= n; ++i) place((float)i * step, 1);
    if (L - (float)n * step > step * 0.35f) place(L, 1);
  }
  return out;
}

// Instance matrix for a frame (+ a height offset): translation · Ry(yaw) · Rz(pitch) · scale(sx,1,1).
inline mat4 splineFrameMatrix(const SplineFrame& f, float naik = 0) {
  return mat4::translation({f.x, f.y + naik, f.z}) * mat4::rotationY(f.yaw) * quat::axisAngle({0, 0, 1}, f.pitch).toMat4() * mat4::scale({f.scale, 1, 1});
}

} // namespace eng

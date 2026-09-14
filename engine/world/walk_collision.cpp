#include "engine/world/walk_collision.h"
#include <algorithm>
#include <chrono>
#include <cmath>

namespace eng {

void WalkCollider::clear() { tris_.clear(); offsets_.clear(); items_.clear(); nx_ = nz_ = 0; stats = {}; }

void WalkCollider::addTriangle(vec3 a, vec3 b, vec3 c, bool peron, bool floorOnly) {
  vec3 n = cross(b - a, c - a); float l = length(n);
  if (!(l > 1e-9f) || !std::isfinite(l)) return;   // degenerate
  n = n / l;
  Tri t; t.a = a; t.b = b; t.c = c; t.n = n;
  t.ymin = std::min(a.y, std::min(b.y, c.y)); t.ymax = std::max(a.y, std::max(b.y, c.y));
  t.horizontal = std::fabs(n.y) > FLOOR_NY; t.wall = !t.horizontal && !floorOnly; t.peron = peron;
  if (!t.horizontal && !t.wall) return;   // nothing to collide with
  tris_.push_back(t);
}

void WalkCollider::addQuad(vec3 a, vec3 b, vec3 c, vec3 d, bool peron, bool floorOnly) { addTriangle(a, b, c, peron, floorOnly); addTriangle(a, c, d, peron, floorOnly); }

void WalkCollider::addBox(const AABB& b, bool peron, bool floorOnly) {
  const vec3& mn = b.min; const vec3& mx = b.max;
  vec3 a{mn.x, mn.y, mn.z}, bb{mx.x, mn.y, mn.z}, c{mx.x, mx.y, mn.z}, d{mn.x, mx.y, mn.z};
  vec3 e{mn.x, mn.y, mx.z}, f{mx.x, mn.y, mx.z}, g{mx.x, mx.y, mx.z}, h{mn.x, mx.y, mx.z};
  addQuad(f, e, h, g, peron, floorOnly); addQuad(a, bb, c, d, peron, floorOnly);
  addQuad(bb, f, g, c, peron, floorOnly); addQuad(e, a, d, h, peron, floorOnly);
  addQuad(d, c, g, h, peron, floorOnly); addQuad(e, f, bb, a, peron, floorOnly);
}

void WalkCollider::addMesh(std::span<const vec3> pos, std::span<const uint32_t> idx, const mat4& xf, bool peron, bool floorOnly) {
  std::vector<vec3> w(pos.size());
  for (size_t i = 0; i < pos.size(); ++i) w[i] = xf.transformPoint(pos[i]);
  tris_.reserve(tris_.size() + idx.size() / 3);
  for (size_t i = 0; i + 2 < idx.size(); i += 3) {
    uint32_t i0 = idx[i], i1 = idx[i + 1], i2 = idx[i + 2];
    if (i0 >= w.size() || i1 >= w.size() || i2 >= w.size()) continue;
    addTriangle(w[i0], w[i1], w[i2], peron, floorOnly);
  }
}

// Counting sort of the triangles into the cells their (x, z) bounds cover. Cell size grows when the extent
// would need millions of cells (a stray far-away object must not blow the grid up).
void WalkCollider::finish() {
  auto t0 = std::chrono::steady_clock::now();
  offsets_.clear(); items_.clear(); nx_ = nz_ = 0; stats.floors = stats.walls = 0;
  if (tris_.empty()) return;
  float x0 = 1e30f, z0 = 1e30f, x1 = -1e30f, z1 = -1e30f;
  for (const Tri& t : tris_) {
    for (const vec3* p : {&t.a, &t.b, &t.c}) { x0 = std::min(x0, p->x); x1 = std::max(x1, p->x); z0 = std::min(z0, p->z); z1 = std::max(z1, p->z); }
    if (t.horizontal) ++stats.floors; if (t.wall) ++stats.walls;
  }
  cell_ = CELL;
  while ((double)((x1 - x0) / cell_ + 1) * (double)((z1 - z0) / cell_ + 1) > 2e6) cell_ *= 2;
  x0_ = x0 - 0.01f; z0_ = z0 - 0.01f;
  nx_ = (int)std::floor((x1 - x0_) / cell_) + 1; nz_ = (int)std::floor((z1 - z0_) / cell_) + 1;
  size_t cells = (size_t)nx_ * (size_t)nz_;
  std::vector<uint32_t> count(cells + 1, 0);
  auto range = [&](const Tri& t, int& cx0, int& cx1, int& cz0, int& cz1) {
    float ax = std::min(t.a.x, std::min(t.b.x, t.c.x)), bx = std::max(t.a.x, std::max(t.b.x, t.c.x));
    float az = std::min(t.a.z, std::min(t.b.z, t.c.z)), bz = std::max(t.a.z, std::max(t.b.z, t.c.z));
    cx0 = std::clamp((int)std::floor((ax - x0_) / cell_), 0, nx_ - 1); cx1 = std::clamp((int)std::floor((bx - x0_) / cell_), 0, nx_ - 1);
    cz0 = std::clamp((int)std::floor((az - z0_) / cell_), 0, nz_ - 1); cz1 = std::clamp((int)std::floor((bz - z0_) / cell_), 0, nz_ - 1);
  };
  for (const Tri& t : tris_) {
    int cx0, cx1, cz0, cz1; range(t, cx0, cx1, cz0, cz1);
    for (int cz = cz0; cz <= cz1; ++cz) for (int cx = cx0; cx <= cx1; ++cx) ++count[(size_t)cz * nx_ + cx + 1];
  }
  offsets_.assign(cells + 1, 0);
  for (size_t i = 0; i < cells; ++i) offsets_[i + 1] = offsets_[i] + count[i + 1];
  items_.resize(offsets_[cells]);
  std::vector<uint32_t> fill(offsets_.begin(), offsets_.end() - 1);
  for (uint32_t i = 0; i < tris_.size(); ++i) {
    int cx0, cx1, cz0, cz1; range(tris_[i], cx0, cx1, cz0, cz1);
    for (int cz = cz0; cz <= cz1; ++cz) for (int cx = cx0; cx <= cx1; ++cx) items_[fill[(size_t)cz * nx_ + cx]++] = i;
  }
  stats.cells = cells;
  stats.buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

bool WalkCollider::cellOf(float x, float z, int& cx, int& cz) const {
  if (nx_ == 0) return false;
  cx = (int)std::floor((x - x0_) / cell_); cz = (int)std::floor((z - z0_) / cell_);
  return cx >= 0 && cx < nx_ && cz >= 0 && cz < nz_;
}

std::span<const uint32_t> WalkCollider::cellItems(int cx, int cz) const {
  size_t c = (size_t)cz * nx_ + cx;
  return {items_.data() + offsets_[c], items_.data() + offsets_[c + 1]};
}

bool WalkCollider::floorBelow(vec3 p, float down, FloorHit& out) const {
  int cx, cz; if (!cellOf(p.x, p.z, cx, cz)) return false;
  Ray r{p, {0, -1, 0}}; float best = down; bool hit = false;
  for (uint32_t i : cellItems(cx, cz)) {
    const Tri& t = tris_[i];
    if (!t.horizontal || t.ymin > p.y || t.ymax < p.y - best) continue;
    float d; if (intersect(r, t.a, t.b, t.c, d) && d <= best) { best = d; out = {p.y - d, (bool)t.peron}; hit = true; }
  }
  return hit;
}

bool WalkCollider::ceilingAbove(vec3 p, float up, float& y) const {
  int cx, cz; if (!cellOf(p.x, p.z, cx, cz)) return false;
  Ray r{p, {0, 1, 0}}; float best = up; bool hit = false;
  for (uint32_t i : cellItems(cx, cz)) {
    const Tri& t = tris_[i];
    if (!t.horizontal || t.ymax < p.y || t.ymin > p.y + best) continue;
    float d; if (intersect(r, t.a, t.b, t.c, d) && d <= best) { best = d; y = p.y + d; hit = true; }
  }
  return hit;
}

bool WalkCollider::wallRay(vec3 p, float dx, float dz, float maxDist, WallHit& out) const {
  if (nx_ == 0 || maxDist <= 0) return false;
  vec3 q = p + vec3{dx, 0, dz} * maxDist;
  int cx0 = (int)std::floor((std::min(p.x, q.x) - x0_) / cell_), cx1 = (int)std::floor((std::max(p.x, q.x) - x0_) / cell_);
  int cz0 = (int)std::floor((std::min(p.z, q.z) - z0_) / cell_), cz1 = (int)std::floor((std::max(p.z, q.z) - z0_) / cell_);
  if (cx1 < 0 || cz1 < 0 || cx0 >= nx_ || cz0 >= nz_) return false;
  cx0 = std::max(cx0, 0); cz0 = std::max(cz0, 0); cx1 = std::min(cx1, nx_ - 1); cz1 = std::min(cz1, nz_ - 1);
  Ray r{p, {dx, 0, dz}}; float best = maxDist; bool hit = false;
  for (int cz = cz0; cz <= cz1; ++cz) for (int cx = cx0; cx <= cx1; ++cx)
    for (uint32_t i : cellItems(cx, cz)) {
      const Tri& t = tris_[i];
      if (!t.wall || t.ymin > p.y || t.ymax < p.y) continue;
      float d; if (intersect(r, t.a, t.b, t.c, d) && d < best) { best = d; out = {d, t.n.x, t.n.z, t.ymax, (bool)t.peron}; hit = true; }
    }
  return hit;
}

} // namespace eng

// Triangle-mesh collision for the walk camera (uji3dJalanRaba.ts RabaTiga, with a spatial hash instead of
// three.js raycasts). World-space triangles of the placed scenery (station GLBs, `garis` platforms) are
// classified by their normal — horizontal (|ny| > 0.7) = floor from above and ceiling from below, the rest =
// wall (the sign is not trusted: third-party assets are double-sided and inconsistently wound) — and binned
// in a 2-D grid of CELL metres over (x, z). Queries are rays: a floor/ceiling ray is one cell, a horizontal
// wall ray of a frame's step (< 1 m) touches at most four. Nothing here knows about the walker: CameraRig
// decides what a hit means (step-up, kerb, slide). Vehicles stay AABBs (WalkBox), the ground a height field.
#pragma once
#include "engine/math/geometry.h"
#include "engine/math/math.h"
#include <cstdint>
#include <span>
#include <vector>

namespace eng {

class WalkCollider {
public:
  static constexpr float CELL = 4;
  static constexpr float FLOOR_NY = 0.7f;   // |normal.y| above this = horizontal (ramps up to 45° are floors)

  void clear();
  // World-space triangle. `peron` marks platform geometry (a 1.1 m kerb may be stepped onto its floor);
  // `floorOnly` drops the wall classification (reference: spline platforms/roads/decks are floors only).
  void addTriangle(vec3 a, vec3 b, vec3 c, bool peron = false, bool floorOnly = false);
  void addQuad(vec3 a, vec3 b, vec3 c, vec3 d, bool peron = false, bool floorOnly = false);
  void addBox(const AABB& b, bool peron = false, bool floorOnly = false);
  // Indexed mesh (model space) placed by `xf`.
  void addMesh(std::span<const vec3> pos, std::span<const uint32_t> idx, const mat4& xf, bool peron = false, bool floorOnly = false);
  void finish();   // builds the grid; queries before this see nothing
  bool empty() const { return tris_.empty(); }
  size_t triangles() const { return tris_.size(); }

  struct FloorHit { float y = 0; bool peron = false; };
  // Nearest horizontal triangle straight below p within `down` metres (the top of the thing under the feet).
  bool floorBelow(vec3 p, float down, FloorHit& out) const;
  // Nearest horizontal triangle straight above p within `up` metres (its underside).
  bool ceilingAbove(vec3 p, float up, float& y) const;
  struct WallHit { float dist = 0; float nx = 0, nz = 0; float top = 0; bool peron = false; };
  // Nearest wall along the horizontal ray from p in (dx, dz) (normalized) within `maxDist`. `top` = the
  // triangle's highest point (a low riser / kerb is not a wall for the walker), (nx, nz) = horizontal normal.
  bool wallRay(vec3 p, float dx, float dz, float maxDist, WallHit& out) const;

  struct Stats { size_t floors = 0, walls = 0, cells = 0; double buildMs = 0; };
  Stats stats;

private:
  struct Tri { vec3 a, b, c; vec3 n; float ymin, ymax; uint8_t horizontal, wall, peron; };
  std::vector<Tri> tris_;
  // grid (CSR): cell (cx, cz) -> tris_ indices offsets_[cell] .. offsets_[cell + 1]
  float x0_ = 0, z0_ = 0, cell_ = CELL; int nx_ = 0, nz_ = 0;
  std::vector<uint32_t> offsets_, items_;
  bool cellOf(float x, float z, int& cx, int& cz) const;
  std::span<const uint32_t> cellItems(int cx, int cz) const;
};

} // namespace eng

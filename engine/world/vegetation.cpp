#include "engine/world/vegetation.h"
#include "engine/world/asset_catalog.h"
#include "engine/world/slippy.h"
#include "engine/world/terrain.h"
#include <algorithm>
#include <chrono>
#include <cmath>

namespace eng {
using namespace vegetation;

namespace {
// vegetasi.ts acak(): deterministic 0..1 from position, so the same forest grows on every load.
double acak(double x, double z, double k) {
  double s = std::sin(x * 12.9898 + z * 78.233 + k * 37.719) * 43758.5453;
  return s - std::floor(s);
}
} // namespace

void Vegetation::build(const Terrain& terrain, AssetCatalog& catalog, std::span<const AABB> exclude) {
  auto t0 = std::chrono::steady_clock::now();
  destroy();
  exclude_.assign(exclude.begin(), exclude.end());
  for (const std::string& id : catalog.idsByCategory("vegetasi")) {
    GpuModel* m = catalog.model(id);
    if (!m) continue;
    vec3 sz = m->bounds.max - m->bounds.min;
    if (sz.y <= 0) continue;
    vec3 c = m->bounds.center();
    ModelSlot slot{m, {}, {}, {}};
    slot.norm = mat4::scale({1 / sz.y, 1 / sz.y, 1 / sz.y}) * mat4::translation({-c.x, -m->bounds.min.y, -c.z});   // unit height, trunk at origin
    slot.instances = rhi::createDynamicBuffer((size_t)INSTANCE_CAP * sizeof(mat4));
    for (GpuMesh& gm : m->meshes) for (GpuPrimitive& p : gm.primitives) rhi::attachInstances(p.mesh, slot.instances);
    models_.push_back(slot);
  }
  stats = {};
  stats.models = (int)models_.size();
  if (models_.empty()) return;

  const WorldOrigin& org = terrain.origin();
  const SatImage& sat = terrain.sat();
  if (sat.layers.empty()) return;
  const SatLayer& nearL = sat.layers[0];
  // cell range: the near satellite layer's extent, in scene space; only cells within reach of the rails
  double sx0 = nearL.ts * nearL.tx0 - slippy::CIRCUMFERENCE / 2 - org.ox, sz0 = nearL.ts * nearL.ty0 - slippy::CIRCUMFERENCE / 2 - org.oz;
  double sx1 = sx0 + nearL.ts * nearL.nx, sz1 = sz0 + nearL.ts * nearL.ny;
  int c0x = (int)std::floor(sx0 / CELL), c1x = (int)std::floor(sx1 / CELL), c0z = (int)std::floor(sz0 / CELL), c1z = (int)std::floor(sz1 / CELL);
  for (int cz = c0z; cz <= c1z; ++cz)
    for (int cx = c0x; cx <= c1x; ++cx) {
      double x0 = cx * (double)CELL, z0 = cz * (double)CELL;
      if (!terrain.railInBox(x0 + CELL / 2 + org.ox, z0 + CELL / 2 + org.oz, CELL, SCATTER_MARGIN)) continue;
      cells_.push_back({cx, cz, -2, {}, {}});
    }
  version_ = terrain.imageryVersion() - 1;   // force a full pass
  update(terrain, 0);
  stats.buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// Scatters one 192 m cell from the finest resident imagery (vegetasi.ts sebarSel); no imagery = no trees.
void Vegetation::scatter(const Terrain& terrain, Cell& cell) {
  const WorldOrigin& org = terrain.origin();
  const float spacing = SPACING_BASE / std::sqrt(DENSITY);
  const int perCell = std::max(1, (int)std::lround(CELL * CELL / (spacing * spacing)));
  const float meanV = terrain.sat().meanBrightness;
  const double x0 = cell.cx * (double)CELL, z0 = cell.cz * (double)CELL;
  cell.trees.clear(); cell.bounds = {};
  for (int i = 0; i < perCell; ++i) {
    double x = x0 + acak(cell.cx, cell.cz, i * 3 + 1) * CELL, z = z0 + acak(cell.cx, cell.cz, i * 3 + 2) * CELL;
    double wx = x + org.ox, wy = z + org.oz;
    float rgb[3];
    if (!terrain.satColor(wx, wy, MASK_RADIUS, rgb)) continue;
    float sum = rgb[0] + rgb[1] + rgb[2] + 1e-6f;
    float exg = (2 * rgb[1] - rgb[0] - rgb[2]) / sum, v = std::max({rgb[0], rgb[1], rgb[2]});
    float green = std::clamp((exg - EXG_MIN) / EXG_RANGE, 0.f, 1.f);
    if (green <= 0) continue;
    float dark = std::clamp((meanV + DARK_OFFSET - v) / DARK_RANGE, 0.f, 1.f);
    float p = green * (BASE_WEIGHT + (1 - BASE_WEIGHT) * dark);
    if (acak(x, z, 5) >= p) continue;
    if (terrain.railDistance((float)x, (float)z) < CLEARANCE) continue;
    bool blocked = false;
    for (const AABB& b : exclude_) if (x >= b.min.x && x <= b.max.x && z >= b.min.z && z <= b.max.z) { blocked = true; break; }
    if (blocked) continue;
    float y = terrain.groundHeight(wx, wy);
    float k = 0.72f + (float)acak(x, z, 6) * 0.75f;
    Tree t{(float)x, y, (float)z, k, (float)(acak(x, z, 8) * 2 * M_PI), (float)acak(x, z, 10), (uint8_t)(std::floor(acak(x, z, 9) * models_.size()))};
    t.model = (uint8_t)std::min<size_t>(t.model, models_.size() - 1);
    cell.bounds.expand({t.x, t.y, t.z}); cell.bounds.expand({t.x, t.y + TREE_HEIGHT * k, t.z});
    cell.trees.push_back(t);
  }
  if (!cell.trees.empty()) { cell.bounds.expand({(float)x0, cell.bounds.min.y, (float)z0}); cell.bounds.expand({(float)(x0 + CELL), cell.bounds.max.y, (float)(z0 + CELL)}); }
}

void Vegetation::update(const Terrain& terrain, int maxCells) {
  if (models_.empty() || cells_.empty()) return;
  if (terrain.imageryVersion() != version_) { version_ = terrain.imageryVersion(); scan_ = 0; scanning_ = true; }
  if (!scanning_) return;
  const WorldOrigin& org = terrain.origin();
  int done = 0;
  for (; scan_ < cells_.size(); ++scan_) {
    Cell& c = cells_[scan_];
    int64_t key = terrain.imageryKeyAt((c.cx + 0.5) * CELL + org.ox, (c.cz + 0.5) * CELL + org.oz);
    if (key == c.key) continue;
    if (maxCells > 0 && done >= maxCells) break;
    stats.trees -= c.trees.size();
    if (key < 0) { c.trees.clear(); c.bounds = {}; } else scatter(terrain, c);
    c.key = key;
    stats.trees += c.trees.size();
    ++done;
  }
  if (scan_ >= cells_.size()) scanning_ = false;
  stats.cells = 0; for (const Cell& c : cells_) if (!c.trees.empty()) ++stats.cells;
}

void Vegetation::draw(ModelRenderer& r, vec3 eye, const Frustum* frustum) {
  stats.drawn = stats.cellsDrawn = 0;
  if (models_.empty() || cells_.empty()) return;
  for (ModelSlot& m : models_) m.mats.clear();
  // cells by distance from the eye to the cell box (selSekitar), nearest first
  struct Near { float d; const Cell* c; };
  std::vector<Near> order;
  for (const Cell& c : cells_) {
    if (c.trees.empty()) continue;
    float px = std::clamp(eye.x, c.bounds.min.x, c.bounds.max.x), pz = std::clamp(eye.z, c.bounds.min.z, c.bounds.max.z);
    float d = std::hypot(px - eye.x, pz - eye.z);
    if (d > VIEW_RADIUS) continue;
    if (frustum && !frustum->contains(c.bounds)) continue;
    order.push_back({d, &c});
  }
  std::sort(order.begin(), order.end(), [](const Near& a, const Near& b) { return a.d < b.d; });
  unsigned total = 0;
  for (const Near& n : order) {
    float threshold = n.d <= FULL_RADIUS ? 1.f : std::max(0.05f, FULL_RADIUS * FULL_RADIUS / (n.d * n.d));   // ambangLOD
    ++stats.cellsDrawn;
    for (const Tree& t : n.c->trees) {
      if (t.rank >= threshold) continue;
      if (total >= (unsigned)INSTANCE_CAP) break;
      ModelSlot& m = models_[t.model];
      float s = TREE_HEIGHT * t.scale;
      m.mats.push_back(mat4::translation({t.x, t.y, t.z}) * mat4::rotationY(t.rot) * mat4::scale({s, s, s}) * m.norm);
      ++total;
    }
    if (total >= (unsigned)INSTANCE_CAP) break;
  }
  for (ModelSlot& m : models_) {
    if (m.mats.empty()) continue;
    rhi::updateBuffer(m.instances, std::as_bytes(std::span(m.mats)));
    r.drawInstanced(*m.model, mat4::identity(), (uint32_t)m.mats.size());
  }
  stats.drawn = total;
}

void Vegetation::destroy() {
  for (ModelSlot& m : models_) rhi::destroyBuffer(m.instances);
  models_.clear(); cells_.clear(); exclude_.clear(); scanning_ = false; scan_ = 0;
}

} // namespace eng

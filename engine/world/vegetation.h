// Vegetation (docs/world-spec.md §4.4, port of ppka-wannabe-2 src/tiga/vegetasi.ts): instanced GLB trees
// scattered per 192 m cell where the satellite imagery is green and dark (ExG mask), away from the
// track and outside given footprints. Cells within reach of the rails are scattered from the imagery
// that is resident: build() does every cell whose imagery is there, update() (re)scatters cells whose
// finest imagery tile changed since (streaming arrivals / evictions), a few cells per call. draw() picks
// the cells around the eye, thins them with distance and uploads one instance buffer per tree model.
#pragma once
#include "engine/math/geometry.h"
#include "engine/world/coords.h"
#include "engine/render/model_renderer.h"
#include "engine/rhi/rhi.h"
#include <span>
#include <vector>

namespace eng {

class AssetCatalog;
class Terrain;

namespace vegetation {
constexpr float CELL = 192;                 // SEL_VEG
constexpr float SPACING_BASE = 11;          // JARAK_BASIS (m at density 1)
constexpr float DENSITY = 2;                // RAPAT_BAKU
constexpr float CLEARANCE = 30;             // bebas: no trees this close to a rail centreline
constexpr float FULL_RADIUS = 500;          // R_PENUH: every tree shown; beyond, 1/d² thinning
constexpr float VIEW_RADIUS = 2000;         // instances beyond this are skipped
constexpr float TREE_HEIGHT = 9;            // POHON_TINGGI (m at scale 1)
constexpr float EXG_MIN = 0.05f, EXG_RANGE = 0.18f, DARK_OFFSET = 0.06f, DARK_RANGE = 0.16f, BASE_WEIGHT = 0.35f;
constexpr float MASK_RADIUS = 4;            // half-window (m) of the imagery average per candidate
constexpr float SCATTER_MARGIN = 2200;      // cells farther than this from any rail are not scattered
constexpr int INSTANCE_CAP = 20000;
} // namespace vegetation

class Vegetation {
public:
  struct Stats { size_t trees = 0; int cells = 0, models = 0; double buildMs = 0; unsigned drawn = 0, cellsDrawn = 0; };

  // Scatters trees. terrain must be loaded with rails registered; exclude = scene-space footprints
  // (xz used) such as station buildings. Tree models = catalog `objek` entries with kategori 'vegetasi'.
  // `budgetMs` > 0 scatters only what fits in that many milliseconds and leaves the rest to update():
  // the world draws (without those trees) between the slices instead of freezing for seconds on a phone.
  void build(const Terrain& terrain, AssetCatalog& catalog, std::span<const AABB> exclude = {}, double budgetMs = 0);
  // Follows the streamed imagery: cells whose finest resident tile changed are re-scattered, at most
  // `maxCells` per call (0 = all) and at most `budgetMs` of work (0 = no time limit; a cell is never
  // split, so one slow cell may overshoot). Cheap when nothing changed (imageryVersion check).
  void update(const Terrain& terrain, int maxCells = 48, double budgetMs = 0);
  bool scattering() const { return scanning_ || enumerating_; }   // true while cells are still being found or filled
  void draw(ModelRenderer& r, vec3 eye, const Frustum* frustum = nullptr);
  void destroy();
  Stats stats;
  // Host knobs (dunia3d.ts laci KAMERA): draw radius per quality tier (TINGKAT_MUTU rVeg / JARAK_SENTUH), and
  // the density multiplier (`Kerapatan pohon` 0..16, RAPAT_BAKU 2). setDensity() re-scatters every cell.
  float viewRadius = vegetation::VIEW_RADIUS;
  float density = vegetation::DENSITY;
  void setDensity(float k);
  // Player brush mask (world.vegMask, vegetasi3d.ts nilaiVegMask): circular stamps in WORLD metres, the
  // LAST stamp covering a point wins; a = -1 forces no tree, +1 forces growth (still subject to the rail
  // clearance and footprints). setMask() replaces the list; only the cells under stamps that differ from
  // the previous list (added / removed / changed) are re-scattered on the next update() calls, so the
  // brush stroke that appends one stamp costs one or two cells.
  struct Stamp { double x, y, r; int a; };
  void setMask(std::vector<Stamp> stamps);
  const std::vector<Stamp>& mask() const { return mask_; }

private:
  struct Tree { float x, y, z, scale, rot, rank; uint8_t model; };
  struct Cell { int cx, cz; int64_t key = -2; AABB bounds; std::vector<Tree> trees; };   // key: Terrain::imageryKeyAt at the centre (-2 = never scattered)
  struct ModelSlot { GpuModel* model; mat4 norm; rhi::Buffer instances; std::vector<mat4> mats; };
  void scatter(const Terrain& terrain, Cell& cell);
  std::vector<Cell> cells_;
  std::vector<ModelSlot> models_;
  std::vector<AABB> exclude_;
  std::vector<Stamp> mask_;
  std::vector<Stamp> pendingDirty_;   // stamps whose cells are re-scattered on the next update()
  int maskAt(double wx, double wy) const;
  void dirtyCellsUnder(const Stamp& st, const WorldOrigin& org);
  unsigned version_ = 0; size_t scan_ = 0; bool scanning_ = false;
  // Finding which cells can hold trees is itself a few thousand rail-proximity tests — 50 ms on a Mac,
  // a second on a phone — so it runs in slices too, from this cursor, before any scattering starts.
  bool enumerating_ = false; int encx_ = 0, encz_ = 0, enc0x_ = 0, enc1x_ = 0, enc1z_ = 0;
  void enumerateCells(const Terrain& terrain, double budgetMs);
};

} // namespace eng

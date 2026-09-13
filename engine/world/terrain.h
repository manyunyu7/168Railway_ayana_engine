// Terrain ("tanah", docs/world-spec.md §4): DEM height source, rail-corridor carving and the
// satellite-textured ground mesh. Data comes from the engine's own binary files written offline by
// tools/fetch_tiles (little-endian):
//
//   <map>.dem   magic "EDEM" u32 version(1)  f64 bbox x0 y0 x1 y1 (track-node bbox)
//               u32 nLayers { i32 zoom tx0 ty0 nx ny n;  u8 present[nx*ny];  f32 h[nx*ny*n*n] }
//               layer 0 = fine (z13, track bbox ± 5 km), layer 1 = far (z10, bbox ± 2 km);
//               heights are raw Terrarium metres a.s.l., n×n samples per tile, row-major, tiles row-major.
//   <map>.sat   magic "ESAT" u32 version(1)
//               u32 nLayers { i32 zoom tx0 ty0 nx ny px;  per tile: u8 present, RGB8[px*px] (sRGB, row 0 = north) }
//               layer 0 = near ground tiles (z14, bbox ± 2.5 km), layer 1 = far layer (z10..12, bbox ± 2 km).
#pragma once
#include "engine/math/geometry.h"
#include "engine/render/model_renderer.h"
#include "engine/world/coords.h"
#include "engine/world/height_source.h"
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace eng {

struct MeshBuilder;

// ---- constants (dunia3dKonst.ts) ----
namespace terrain {
constexpr int DEM_Z = 13, DEM_Z_FAR = 10, TILE_Z = 14;
constexpr float CARVE_INNER = 9, CARVE_OUTER = 60;            // UKIR_DALAM / UKIR_LUAR
constexpr float LOW_OUTER = 20, LOW_INNER = 8, LOW_MAX_DROP = 1.5f, LOW_IGNORE_DIFF = 4;   // RENDAH_*
constexpr float BALLAST_FOOT = -0.580f;                      // BALAS_KAKI
constexpr float PLATEAU_OFFSET = BALLAST_FOOT - 0.04f;       // plateau below the rail head
constexpr float GRID_CELL = 64;                              // SEL_GRID (rail hash grid)
constexpr float CHORD_MAX = 20;                              // consecutive RailSamples further apart start a new chord
constexpr float BLOCK = 128, CELL_NEAR = 8, CELL_MID = 30, CELL_FAR = 60;   // BLOK_TANAH, SEL_TANAH_*
constexpr float BLOCK_MARGIN = CARVE_OUTER + 8, MID_MARGIN = 500;           // MARGIN_BLOK, MARGIN_SEDANG
constexpr float SKIRT_MIN = 4, SKIRT_FACTOR = 0.45f;                        // ROK_MIN, ROK_FAKTOR
constexpr int FAR_CELLS_PER_TILE = 6;                                       // SEL_JAUH_PER_UBIN
constexpr float BACKDROP_SIZE = 80000;
} // namespace terrain

struct DemLayer {
  int zoom = 0, tx0 = 0, ty0 = 0, nx = 0, ny = 0, n = 0;
  double ts = 0, x0 = 0, y0 = 0;          // tile size and world origin of tile (tx0, ty0)
  std::vector<uint8_t> present;
  std::vector<float> h;
  bool covers(double wx, double wy) const;                    // inside the layer and the tile is present
  float sample(double wx, double wy) const;                   // bilinear inside one tile (§4.1); caller checks covers()
  float sampleClamped(double wx, double wy) const;            // nearest present tile, uv clamped
};

class Dem : public HeightSource {
public:
  bool load(const std::string& path, std::string& error);
  float rawHeight(double wx, double wy) const override;       // fine layer, then far, then nearest edge
  float heightScene(double wx, double wy) const { return rawHeight(wx, wy) - demBase; }
  WorldOrigin origin() const { return {(bbox[0] + bbox[2]) / 2, (bbox[1] + bbox[3]) / 2}; }

  double bbox[4]{};        // track-node bbox x0 y0 x1 y1
  float demBase = 0;       // raw height at the bbox centre
  float rawMin = 0;        // lowest sample of all layers
  std::vector<DemLayer> layers;
};

struct SatLayer {
  int zoom = 0, tx0 = 0, ty0 = 0, nx = 0, ny = 0, px = 0;
  std::vector<uint8_t> present, rgb;
  bool has(int tx, int ty) const;
  const uint8_t* tile(int tx, int ty) const;   // RGB8 px*px
};

struct SatImage {
  bool load(const std::string& path, std::string& error);
  std::vector<SatLayer> layers;
};

class Terrain {
public:
  struct Stats { size_t vertices = 0, triangles = 0; int nearTiles = 0, farTiles = 0; double loadMs = 0, buildMs = 0; };

  // Reads the two data files; the world origin becomes the track-node bbox centre.
  bool load(const std::string& demPath, const std::string& satPath, std::string& error);
  // Registers rail centreline chords for carving. Consecutive at-grade samples closer than
  // CHORD_MAX form a chord; call before build().
  void setRails(std::span<const RailSample> samples);
  void build();      // ground meshes + textures (needs a GL context)
  void draw(ModelRenderer& r, const Frustum* frustum = nullptr);
  void destroy();

  float groundHeight(double wx, double wy) const;    // carved ground, scene y (tanahTerukir)
  float rawHeight(double wx, double wy) const { return dem_.rawHeight(wx, wy); }
  const Dem& dem() const { return dem_; }
  const WorldOrigin& origin() const { return origin_; }
  Stats stats;

private:
  struct Chord { float x, z, y, x2, z2, y2, b, b2; };
  struct Nearest { float d, y, b, yLow, dLow; };
  struct Tile { rhi::Mesh mesh; rhi::Texture tex; AABB bounds; mat4 xf; bool textured = false; };

  void addChord(vec3 a, vec3 b, float ba, float bb);
  bool nearestRail(float x, float z, Nearest& out) const;
  bool railInBox(double cx, double cy, double side, double margin) const;
  float carveBase(const Nearest& n) const;
  Tile buildNearTile(int tx, int ty);
  Tile buildFarTile(const SatLayer& far, int tx, int ty);
  void upload(Tile& t, MeshBuilder& mb, const SatLayer* layer, int tx, int ty, vec3 pos);

  Dem dem_; SatImage sat_; WorldOrigin origin_;
  std::unordered_map<int64_t, std::vector<Chord>> railGrid_;
  std::vector<Tile> near_, far_;
  Tile backdrop_{};
  Material ground_, backdropMat_;
  bool built_ = false;
};

} // namespace eng

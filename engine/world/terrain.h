// Terrain ("tanah", docs/world-spec.md §4): DEM height source, rail-corridor carving and the
// satellite-textured ground mesh, streamed around the camera like the reference (ubinStream.ts).
//
// Data: the DEM core (z13 corridor + z10 far, small) is loaded eagerly and never evicted, so
// groundHeight() works from the first frame; satellite imagery streams by distance. Sources:
//   <map>.dem   magic "EDEM" u32 version(1)  f64 bbox x0 y0 x1 y1 (track-node bbox)
//               u32 nLayers { i32 zoom tx0 ty0 nx ny n;  u8 present[nx*ny];  f32 h[nx*ny*n*n] }
//               layer 0 = fine (z13, track bbox ± 5 km), layer 1 = far (z10, bbox ± 2 km);
//               heights are raw Terrarium metres a.s.l., n×n samples per tile, row-major, tiles row-major.
//   <map>.sat   magic "ESAT" u32 version(2)
//               u32 nLayers { i32 zoom tx0 ty0 nx ny px;  u8 present[nx*ny];  RGB8[px*px] per PRESENT tile,
//               tiles row-major (sRGB, row 0 = north) }
//               layer 0 = near ground tiles (z14, bbox ± 2.5 km), layer 1 = far layer (z10..12, bbox ± 2 km),
//               layers 2.. = detail (z16/z17 around the track and stations, spec DETAIL_TANAH); the ground
//               mesh textures every 153 m block with the finest resident layer covering it.
//   <map>/index.json + per-tile files (tools/fetch_tiles): the same layers; the host answers requestTile()
//               with provide*() — from files (native), or straight from the tile servers (web, PNG decoded
//               by the browser). The monolithic .sat is an in-memory source answering the same requests.
//
// Streaming (update()): centre = the camera focus. Near z14 tiles live while their edge is within
// R_LOAD (4000 m) and are evicted beyond R_EVICT (6000 m); detail z16 within 1500/2200 m, z17 within
// 450/800 m; the far layer and the DEM are resident for good. At most REQUESTS_PER_CHECK (2) requests
// per CHECK_INTERVAL (220 ms), nearest first; at most GPU_JOBS_PER_FRAME (2) mesh builds / texture
// uploads per frame. prime() fills the radius without throttling (native start, deterministic captures).
#pragma once
#include "engine/asset/model.h"
#include "engine/core/json.h"
#include "engine/math/geometry.h"
#include "engine/render/model_renderer.h"
#include "engine/world/coords.h"
#include "engine/world/height_source.h"
#include <cstdint>
#include <functional>
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
constexpr float MOUTH_INNER = 4.5f, MOUTH_OUTER = 13;            // corridor radii right at a tunnel mouth (engine addition, see groundHeight)
constexpr float LOW_OUTER = 20, LOW_INNER = 8, LOW_MAX_DROP = 1.5f, LOW_IGNORE_DIFF = 4;   // RENDAH_*
constexpr float BALLAST_FOOT = -0.580f;                      // BALAS_KAKI
constexpr float PLATEAU_OFFSET = BALLAST_FOOT - 0.04f;       // plateau below the rail head
constexpr float GRID_CELL = 64;                              // SEL_GRID (rail hash grid)
constexpr float CHORD_MAX = 20;                              // consecutive RailSamples further apart start a new chord
constexpr float DELTA_GRID = 8;                              // KISI_DELTA (brush deltas, world.tanah)
constexpr float DECK_BOTTOM = BALLAST_FOOT - 1.4f;          // DEK_BAWAH (deck slab bottom rel. rail head)
constexpr float BRIDGE_INNER = 7, BRIDGE_OUTER = 26, BRIDGE_CLEAR = 1.5f;   // JBT_DALAM / JBT_LUAR / JBT_RUANG
// BLOK_TANAH is 128 m in the reference (19 per tile); 16 per tile (152.9 m) keeps blocks aligned with the
// z16 (4×4) and z17 (2×2) detail tile grids so every block has exactly one texture.
constexpr int BLOCKS_PER_TILE = 16;
constexpr float CELL_NEAR = 8, CELL_MID = 30, CELL_FAR = 60;   // SEL_TANAH_*
constexpr float BLOCK_MARGIN = CARVE_OUTER + 8, MID_MARGIN = 500;           // MARGIN_BLOK, MARGIN_SEDANG
constexpr float SKIRT_MIN = 4, SKIRT_FACTOR = 0.45f;                        // ROK_MIN, ROK_FAKTOR
constexpr int FAR_CELLS_PER_TILE = 6;                                       // SEL_JAUH_PER_UBIN
constexpr float BACKDROP_SIZE = 80000;
// streaming (ubinStream.ts, DETAIL_TANAH)
constexpr float R_LOAD = 4000, R_EVICT = 6000;                              // UBIN_R_MUAT / UBIN_R_BUANG (to the tile edge)
constexpr float R16_IN = 1500, R16_OUT = 2200, R17_IN = 450, R17_OUT = 800; // detail rings
constexpr int REQUESTS_PER_CHECK = 2;                                       // UBIN_PER_PERIKSA
constexpr float CHECK_INTERVAL = 0.22f;                                     // UBIN_PERIKSA_MS
constexpr int GPU_JOBS_PER_FRAME = 2;                                       // mesh builds + texture uploads per frame
constexpr float RECUT_DELAY = 0.25f;                                        // s after the last detail arrival before a tile is re-cut
} // namespace terrain

struct DemLayer {
  int zoom = 0, tx0 = 0, ty0 = 0, nx = 0, ny = 0, n = 0;
  double ts = 0, x0 = 0, y0 = 0;          // tile size and world origin of tile (tx0, ty0)
  std::vector<uint8_t> present;           // tile delivered
  std::vector<uint8_t> indexed;           // tile exists at the source (index.json); streaming requests these
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
  void finish();           // rawMin / demBase over the present tiles (after load / streamed tiles)

  double bbox[4]{};        // track-node bbox x0 y0 x1 y1
  float demBase = 0;       // raw height at the bbox centre
  float rawMin = 0;        // lowest sample of all layers
  std::vector<DemLayer> layers;
};

// One satellite layer = a tile grid at one zoom. `indexed` says the source has imagery; the runtime state
// tracks what is resident. Resident tiles keep an RGB8 copy (satColor / vegetation mask) and a GPU texture
// (uploaded lazily under the per-frame budget).
struct SatLayer {
  enum State : uint8_t { Absent, Requested, Resident, Failed };
  struct Res { std::vector<uint8_t> rgb; int rgbPx = 0; Image image; rhi::Texture tex; bool uploaded = false; };
  int zoom = 0, tx0 = 0, ty0 = 0, nx = 0, ny = 0, px = 0;
  double ts = 0, mpp = 0;                      // tile size (m) and metres per pixel (px = source texture size)
  std::vector<uint8_t> indexed;                // imagery exists at the source
  std::vector<uint8_t> state;                  // State per tile
  std::unordered_map<int, Res> res;            // resident tiles by tile index (j * nx + i)
  float inR = 0, outR = 0;                     // streaming radii (0 = resident for good)
  bool inside(int tx, int ty) const { int i = tx - tx0, j = ty - ty0; return i >= 0 && j >= 0 && i < nx && j < ny; }
  int index(int tx, int ty) const { return (ty - ty0) * nx + (tx - tx0); }
  bool has(int tx, int ty) const;              // resident imagery
  bool covers(double wx, double wy) const;     // a resident tile contains the point
  const Res* tile(int tx, int ty) const;       // resident record (nullptr otherwise)
  int residentCount() const { return (int)res.size(); }
  double edgeDistance(int tx, int ty, double wx, double wy) const;   // world distance from a point to the tile box
};

struct SatImage {
  bool load(const std::string& path, std::string& error);   // monolithic file: every tile becomes an in-memory source
  void finish();                               // detail order
  std::vector<SatLayer> layers;
  std::vector<int> detail;                     // indices of layers finer than the near layer, finest first
  float meanBrightness = 0.4f;                 // mean max(R,G,B) of the near layer (vegetation mask); index.json `mean`
  bool meanFixed = false;                      // from the file / index (else a running mean over the delivered near tiles)
  // in-memory source (monolithic file): per layer, per tile index -> RGB8 px*px
  std::vector<std::unordered_map<int, std::vector<uint8_t>>> store;
};

class Terrain {
public:
  struct Stats { size_t vertices = 0, triangles = 0; int nearTiles = 0, farTiles = 0, patches = 0, detailPatches = 0, resident = 0, requested = 0, pendingJobs = 0; double loadMs = 0, buildMs = 0; };
  struct TileRef { std::string dir; int z = 0, x = 0, y = 0; std::string path() const; };   // dir "dem" | "sat/<i>"
  using RequestFn = std::function<void(const TileRef&)>;

  // Monolithic files (native fallback): the DEM is complete, the satellite tiles become an in-memory source
  // that answers streaming requests without the callback. The world origin = the track-node bbox centre.
  bool load(const std::string& demPath, const std::string& satPath, std::string& error);
  // Streaming source: describe the layers from index.json (tools/fetch_tiles), set the request callback,
  // feed the DEM tiles (demTilesWanted() -> provideDem / failTile), then finishDem() before build().
  bool loadIndex(const Json& index, std::string& error);
  // No terrain at all (index missing / invalid): no layers, every height 0 -> flat ground at rail height with
  // the backdrop plane just under the rail head; groundHeight() still carves; streaming is a no-op. `bbox` =
  // the track-node bbox (world origin), nullptr = origin (0, 0).
  void loadNone(const double* bbox);
  bool hasTerrain() const { return !dem_.layers.empty() && !sat_.layers.empty(); }
  void setRequestFn(RequestFn fn) { request_ = std::move(fn); }
  std::vector<TileRef> demTilesWanted() const;                 // indexed DEM tiles not delivered yet
  bool provideDem(int z, int x, int y, std::span<const float> heights);              // n*n Terrarium metres, row-major
  bool provideDemRgba(int z, int x, int y, int w, int h, const uint8_t* rgba);       // decoded Terrarium PNG
  bool provideSat(int layer, int z, int x, int y, Image image, std::string& error);  // RGBA8 pixels or EIMG variants
  bool provideSatRgba(int layer, int z, int x, int y, int w, int h, const uint8_t* rgba, std::string& error);
  bool provideSatFile(int layer, int z, int x, int y, std::span<const uint8_t> bytes, std::string& error);   // EIMG record
  void failTile(const TileRef& t);
  void finishDem();
  bool demComplete() const;                                    // every indexed DEM tile delivered or failed
  bool streamed() const { return streamed_; }
  // Registers rail centreline chords for carving. Consecutive at-grade samples closer than
  // CHORD_MAX form a chord; consecutive bridge samples (bridgeBlend >= 0) form deck chords for the
  // trough under the slab. Call before build().
  void setRails(std::span<const RailSample> samples);
  // Brush deltas `world.tanah` ({kisi, delta:{"gx,gz": m}}), bilinear on the 8 m grid, added to the DEM
  // before carving (§4.1). Accepts a missing/null object. Call before build().
  void setBrushDeltas(const Json& tanah);
  float brushDelta(double wx, double wy) const;
  void build();      // backdrop + streaming state (needs a GL context); tiles then come through update()/prime()
  // Streaming step: `centre` = camera focus in scene space, dt = real seconds. Issues requests (throttled),
  // builds / re-cuts / textures tiles under the GPU budget, evicts beyond the radii.
  void update(vec3 centre, float dt);
  // Unthrottled fill: requests everything inside the radii and finishes every job that can be done now
  // (synchronous sources: the whole neighbourhood is built on return).
  void prime(vec3 centre);
  void draw(ModelRenderer& r, const Frustum* frustum = nullptr);
  void destroy();

  float groundHeight(double wx, double wy) const;    // carved ground, scene y (tanahTerukir)
  float rawHeight(double wx, double wy) const { return dem_.rawHeight(wx, wy); }
  // Distance (m) from a scene xz position to the nearest registered at-grade rail chord; 1e30 when
  // farther than CARVE_OUTER.
  float railDistance(float x, float z) const;
  // Is any rail chord inside the world box (cx, cy, side) grown by margin?
  bool railInBox(double cx, double cy, double side, double margin) const;
  // Mean sRGB colour (0..1) of the finest resident imagery in a ±radius m window around a world point;
  // false when no imagery covers it.
  bool satColor(double wx, double wy, float radius, float rgb[3]) const;
  // Finest resident satellite layer covering a world point (index into sat().layers; -1 = none).
  int finestLayerAt(double wx, double wy) const;
  // Identity of the finest resident tile at a point (layer << 40 | tile index), -1 when none: changes when
  // the imagery there changes (vegetation re-scatter key).
  int64_t imageryKeyAt(double wx, double wy) const;
  unsigned imageryVersion() const { return imageryVersion_; }   // bumped on every arrival / eviction
  const Dem& dem() const { return dem_; }
  const SatImage& sat() const { return sat_; }
  const WorldOrigin& origin() const { return origin_; }
  Stats stats;

private:
  struct Chord { float x, z, y, x2, z2, y2, b, b2; };
  struct Nearest { float d, y, b, yLow, dLow; };
  struct Key { int layer, tx, ty; bool operator==(const Key& o) const { return layer == o.layer && tx == o.tx && ty == o.ty; } };
  struct Patch { Key key; rhi::Mesh mesh; AABB bounds; uint32_t verts = 0; };
  struct NearTile { int tx, ty; std::vector<Patch> patches; mat4 xf; bool built = false, dirty = false; float dirtyAge = 0; };
  struct FarTile { int tx, ty; rhi::Mesh mesh; AABB bounds; mat4 xf; bool built = false; uint32_t verts = 0; };
  struct Job { int kind; int layer, tx, ty; double dist; };   // 0 build near, 1 build far, 2 upload texture

  void resetLayers();
  bool loadIndexLayers(const Json& index, std::string& error);
  void addChord(vec3 a, vec3 b, float ba, float bb);
  bool nearestRail(float x, float z, Nearest& out) const;
  bool nearestDeck(float x, float z, float& d, float& y, float& b) const;
  float carveBase(const Nearest& n) const;
  void buildNearTile(NearTile& t);
  void buildFarTile(FarTile& t);
  void freeNear(NearTile& t);
  void streamCheck(double wx, double wy, bool unthrottled);
  void runJobs(double wx, double wy, int budget);
  void issue(int layer, int tx, int ty);
  void evict(int layer, int tx, int ty);
  void markDirty(int layer, int tx, int ty);
  void noteImagery();
  const rhi::Texture* textureOf(const Key& k) const;

  Dem dem_; SatImage sat_; WorldOrigin origin_;
  std::unordered_map<int64_t, std::vector<Chord>> railGrid_, bridgeGrid_;
  std::unordered_map<int64_t, float> delta_;   // brush deltas keyed by grid cell
  std::vector<NearTile> near_;
  std::vector<FarTile> far_;
  struct Backdrop { rhi::Mesh mesh; mat4 xf; } backdrop_{};
  Material ground_, backdropMat_, loadingMat_;
  bool built_ = false, streamed_ = false;
  RequestFn request_;
  float checkTimer_ = 0; bool firstCheck_ = true;
  unsigned imageryVersion_ = 0;
  double meanSum_ = 0; size_t meanN_ = 0;      // running mean brightness (near tiles seen)
};

} // namespace eng

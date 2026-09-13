#include "engine/world/terrain.h"
#include "engine/render/mesh_builder.h"
#include "engine/world/slippy.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace eng {
using namespace terrain;

// ------------------------------------------------------------------ file reading
namespace {
struct Reader {
  std::ifstream f;
  bool ok = true;
  explicit Reader(const std::string& p) : f(p, std::ios::binary) { ok = (bool)f; }
  int32_t i32() { int32_t v = 0; f.read((char*)&v, 4); ok = ok && (bool)f; return v; }
  double f64() { double v = 0; f.read((char*)&v, 8); ok = ok && (bool)f; return v; }
  bool bytes(void* dst, size_t n) { f.read((char*)dst, (std::streamsize)n); ok = ok && (bool)f; return ok; }
  bool magic(const char* m) { char b[4] = {}; bytes(b, 4); return ok && !std::memcmp(b, m, 4); }
};
inline float smoothstep01(float t) { return t * t * (3 - 2 * t); }
inline int64_t cellKey(int i, int j) { return ((int64_t)i << 32) ^ (uint32_t)j; }
} // namespace

// ------------------------------------------------------------------ DemLayer / Dem
bool DemLayer::covers(double wx, double wy) const {
  int i = (int)std::floor((wx - x0) / ts), j = (int)std::floor((wy - y0) / ts);
  return i >= 0 && j >= 0 && i < nx && j < ny && present[(size_t)j * nx + i];
}

float DemLayer::sample(double wx, double wy) const {
  int i = std::clamp((int)std::floor((wx - x0) / ts), 0, nx - 1), j = std::clamp((int)std::floor((wy - y0) / ts), 0, ny - 1);
  const float* t = h.data() + ((size_t)j * nx + i) * n * n;
  double fx = (wx - (x0 + i * ts)) / ts * (n - 1), fy = (wy - (y0 + j * ts)) / ts * (n - 1);
  int px = std::clamp((int)fx, 0, n - 1), py = std::clamp((int)fy, 0, n - 1);
  int px1 = std::min(n - 1, px + 1), py1 = std::min(n - 1, py + 1);
  float sx = (float)(fx - px), sy = (float)(fy - py);
  sx = std::clamp(sx, 0.f, 1.f); sy = std::clamp(sy, 0.f, 1.f);
  float a = t[py * n + px] * (1 - sx) + t[py * n + px1] * sx;
  float b = t[py1 * n + px] * (1 - sx) + t[py1 * n + px1] * sx;
  return a * (1 - sy) + b * sy;
}

float DemLayer::sampleClamped(double wx, double wy) const {
  int best = -1; double bd = 1e300;
  for (int j = 0; j < ny; ++j)
    for (int i = 0; i < nx; ++i) {
      if (!present[(size_t)j * nx + i]) continue;
      double d = std::fabs(wx - (x0 + (i + 0.5) * ts)) + std::fabs(wy - (y0 + (j + 0.5) * ts));
      if (d < bd) { bd = d; best = j * nx + i; }
    }
  if (best < 0) return 0;
  int i = best % nx, j = best / nx;
  double tx0 = x0 + i * ts, ty0 = y0 + j * ts;
  return sample(std::clamp(wx, tx0, tx0 + ts - 1e-6), std::clamp(wy, ty0, ty0 + ts - 1e-6));
}

bool Dem::load(const std::string& path, std::string& error) {
  Reader r(path);
  if (!r.ok) { error = "cannot open " + path; return false; }
  if (!r.magic("EDEM") || r.i32() != 1) { error = path + ": not an EDEM v1 file"; return false; }
  for (double& v : bbox) v = r.f64();
  int nl = r.i32();
  if (!r.ok || nl < 1 || nl > 8) { error = path + ": bad header"; return false; }
  layers.resize((size_t)nl);
  rawMin = 1e30f;
  for (DemLayer& L : layers) {
    L.zoom = r.i32(); L.tx0 = r.i32(); L.ty0 = r.i32(); L.nx = r.i32(); L.ny = r.i32(); L.n = r.i32();
    if (!r.ok || L.nx < 1 || L.ny < 1 || L.n < 2 || L.nx * L.ny > 4096) { error = path + ": bad layer"; return false; }
    L.ts = slippy::tileSizeMeter(L.zoom); L.x0 = slippy::tileOriginX(L.tx0, L.zoom); L.y0 = slippy::tileOriginY(L.ty0, L.zoom);
    L.present.resize((size_t)L.nx * L.ny);
    L.h.resize((size_t)L.nx * L.ny * L.n * L.n);
    if (!r.bytes(L.present.data(), L.present.size()) || !r.bytes(L.h.data(), L.h.size() * 4)) { error = path + ": truncated"; return false; }
    for (size_t t = 0; t < L.present.size(); ++t)
      if (L.present[t]) for (size_t k = 0; k < (size_t)L.n * L.n; ++k) rawMin = std::min(rawMin, L.h[t * L.n * L.n + k]);
  }
  if (rawMin > 1e29f) rawMin = 0;
  demBase = rawHeight((bbox[0] + bbox[2]) / 2, (bbox[1] + bbox[3]) / 2);
  return true;
}

float Dem::rawHeight(double wx, double wy) const {
  for (const DemLayer& L : layers) if (L.covers(wx, wy)) return L.sample(wx, wy);
  return layers.empty() ? 0.f : layers.back().sampleClamped(wx, wy);
}

// ------------------------------------------------------------------ satellite
bool SatLayer::has(int tx, int ty) const {
  int i = tx - tx0, j = ty - ty0;
  return i >= 0 && j >= 0 && i < nx && j < ny && present[(size_t)j * nx + i];
}
bool SatLayer::covers(double wx, double wy) const {
  return has(slippy::worldToTileX(wx, zoom), slippy::worldToTileY(wy, zoom));
}
const uint8_t* SatLayer::tile(int tx, int ty) const {
  return rgb.data() + (size_t)slot[(size_t)(ty - ty0) * nx + (tx - tx0)] * px * px * 3;
}

bool SatImage::load(const std::string& path, std::string& error) {
  Reader r(path);
  if (!r.ok) { error = "cannot open " + path; return false; }
  if (!r.magic("ESAT")) { error = path + ": not an ESAT file"; return false; }
  int ver = r.i32();
  if (ver != 2) { error = path + ": ESAT v" + std::to_string(ver) + " is not supported (v2 expected; re-run fetch_tiles)"; return false; }
  int nl = r.i32();
  if (!r.ok || nl < 1 || nl > 16) { error = path + ": bad header"; return false; }
  layers.resize((size_t)nl);
  for (SatLayer& L : layers) {
    L.zoom = r.i32(); L.tx0 = r.i32(); L.ty0 = r.i32(); L.nx = r.i32(); L.ny = r.i32(); L.px = r.i32();
    if (!r.ok || L.nx < 1 || L.ny < 1 || L.px < 1 || L.px > 4096 || L.nx * L.ny > 65536) { error = path + ": bad layer"; return false; }
    L.ts = slippy::tileSizeMeter(L.zoom); L.mpp = L.ts / L.px;
    L.present.resize((size_t)L.nx * L.ny);
    if (!r.bytes(L.present.data(), L.present.size())) { error = path + ": truncated"; return false; }
    L.slot.assign(L.present.size(), -1);
    int32_t count = 0;
    for (size_t t = 0; t < L.present.size(); ++t) if (L.present[t]) L.slot[t] = count++;
    L.rgb.resize((size_t)count * L.px * L.px * 3);
    if (!r.bytes(L.rgb.data(), L.rgb.size())) { error = path + ": truncated"; return false; }
  }
  detail.clear();
  for (int i = 0; i < nl; ++i) if (layers[(size_t)i].zoom > TILE_Z) detail.push_back(i);
  std::sort(detail.begin(), detail.end(), [&](int a, int b) { return layers[(size_t)a].mpp < layers[(size_t)b].mpp; });
  // mean brightness of the near layer (vegetation mask reference, vegetasi.ts terRata), subsampled
  double sum = 0; size_t n = 0;
  for (const SatLayer& L : layers) {
    if (L.zoom != TILE_Z) continue;
    for (size_t i = 0; i + 2 < L.rgb.size(); i += 3 * 16) { sum += std::max({L.rgb[i], L.rgb[i + 1], L.rgb[i + 2]}); ++n; }
  }
  meanBrightness = n ? (float)(sum / n / 255.0) : 0.4f;
  return true;
}

int Terrain::finestLayerAt(double wx, double wy) const {
  for (int i : sat_.detail) if (sat_.layers[(size_t)i].covers(wx, wy)) return i;
  if (!sat_.layers.empty() && sat_.layers[0].covers(wx, wy)) return 0;
  return -1;
}

bool Terrain::satColor(double wx, double wy, float radius, float rgb[3]) const {
  int li = finestLayerAt(wx, wy);
  if (li < 0) return false;
  const SatLayer* L = &sat_.layers[(size_t)li];
  int tx = slippy::worldToTileX(wx, L->zoom), ty = slippy::worldToTileY(wy, L->zoom);
  const uint8_t* t = L->tile(tx, ty);
  double fx = (wx - slippy::tileOriginX(tx, L->zoom)) / L->ts * L->px, fy = (wy - slippy::tileOriginY(ty, L->zoom)) / L->ts * L->px;
  int r = std::max(0, (int)(radius / L->mpp));
  int x0 = std::clamp((int)fx - r, 0, L->px - 1), x1 = std::clamp((int)fx + r, 0, L->px - 1);
  int y0 = std::clamp((int)fy - r, 0, L->px - 1), y1 = std::clamp((int)fy + r, 0, L->px - 1);
  double acc[3] = {0, 0, 0}; int n = 0;
  for (int y = y0; y <= y1; ++y)
    for (int x = x0; x <= x1; ++x) { const uint8_t* p = t + ((size_t)y * L->px + x) * 3; acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2]; ++n; }
  for (int c = 0; c < 3; ++c) rgb[c] = (float)(acc[c] / n / 255.0);
  return true;
}

// ------------------------------------------------------------------ Terrain: data + carving
bool Terrain::load(const std::string& demPath, const std::string& satPath, std::string& error) {
  auto t0 = std::chrono::steady_clock::now();
  if (!dem_.load(demPath, error)) return false;
  if (!sat_.load(satPath, error)) return false;
  origin_ = dem_.origin();
  ground_ = {}; ground_.baseColor = {0.81f, 0.81f, 0.81f, 1}; ground_.metallic = 0; ground_.roughness = 1;
  backdropMat_ = {}; backdropMat_.baseColor = {0.616f, 0.690f, 0.537f, 1}; backdropMat_.metallic = 0; backdropMat_.roughness = 1;
  stats.loadMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  return true;
}

void Terrain::addChord(vec3 a, vec3 b, float ba, float bb) {
  int64_t k = cellKey((int)std::floor((a.x + b.x) * 0.5f / GRID_CELL), (int)std::floor((a.z + b.z) * 0.5f / GRID_CELL));
  railGrid_[k].push_back({a.x, a.z, a.y, b.x, b.z, b.y, ba, bb});
}

void Terrain::setRails(std::span<const RailSample> samples) {
  railGrid_.clear();
  for (size_t i = 1; i < samples.size(); ++i) {
    const RailSample &p = samples[i - 1], &q = samples[i];
    if (!p.atGrade || !q.atGrade) continue;
    vec3 a = origin_.toScene(p.wx, p.wy, p.railY), b = origin_.toScene(q.wx, q.wy, q.railY);
    float dx = a.x - b.x, dz = a.z - b.z;
    if (dx * dx + dz * dz > CHORD_MAX * CHORD_MAX) continue;
    addChord(a, b, p.mouthBlend, q.mouthBlend);
  }
}

bool Terrain::nearestRail(float x, float z, Nearest& out) const {
  int cx = (int)std::floor(x / GRID_CELL), cz = (int)std::floor(z / GRID_CELL);
  const int r = (int)std::ceil(CARVE_OUTER / GRID_CELL);
  float best = 1e30f, by = 0, bb = 1, low = 1e30f, dLow = 1e30f;
  for (int i = -r; i <= r; ++i)
    for (int j = -r; j <= r; ++j) {
      auto it = railGrid_.find(cellKey(cx + i, cz + j));
      if (it == railGrid_.end()) continue;
      for (const Chord& p : it->second) {
        float dx = p.x2 - p.x, dz = p.z2 - p.z, L2 = dx * dx + dz * dz;
        float t = L2 > 0 ? std::clamp(((x - p.x) * dx + (z - p.z) * dz) / L2, 0.f, 1.f) : 0.f;
        float d = std::hypot(p.x + dx * t - x, p.z + dz * t - z);
        float y = p.y + (p.y2 - p.y) * t, b = p.b + (p.b2 - p.b) * t;
        if (d < best) { best = d; by = y; bb = b; }
        if (d <= LOW_OUTER && b > 0 && y < low) { low = y; dLow = d; }
      }
    }
  if (best >= 1e30f) return false;
  out = {best, by, bb, low >= 1e30f ? by : low, low >= 1e30f ? best : dLow};
  return true;
}

// Rail height used as the carving datum: pulled down toward the lowest rail nearby (double track).
float Terrain::carveBase(const Nearest& n) const {
  float diff = n.y - n.yLow;
  if (diff <= 0 || diff > LOW_IGNORE_DIFF) return n.y;
  float t = n.dLow <= LOW_INNER ? 1.f : std::max(0.f, 1 - (n.dLow - LOW_INNER) / (LOW_OUTER - LOW_INNER));
  return n.y - std::min(diff, LOW_MAX_DROP) * smoothstep01(t);
}

float Terrain::railDistance(float x, float z) const {
  Nearest n;
  return nearestRail(x, z, n) ? n.d : 1e30f;
}

float Terrain::groundHeight(double wx, double wy) const {
  float h = dem_.heightScene(wx, wy);
  if (railGrid_.empty()) return h;
  Nearest n;
  if (nearestRail((float)(wx - origin_.ox), (float)(wy - origin_.oz), n) && n.d <= CARVE_OUTER && n.b > 0) {
    float t = n.d <= CARVE_INNER ? 1.f : 1 - (n.d - CARVE_INNER) / (CARVE_OUTER - CARVE_INNER);
    float w = smoothstep01(t) * n.b;
    h = h * (1 - w) + (carveBase(n) + PLATEAU_OFFSET) * w;
  }
  return h;
}

bool Terrain::railInBox(double cx, double cy, double side, double margin) const {
  float x0 = (float)(cx - side / 2 - origin_.ox - margin), x1 = (float)(cx + side / 2 - origin_.ox + margin);
  float z0 = (float)(cy - side / 2 - origin_.oz - margin), z1 = (float)(cy + side / 2 - origin_.oz + margin);
  int i0 = (int)std::floor(x0 / GRID_CELL) - 1, i1 = (int)std::floor(x1 / GRID_CELL) + 1;
  int j0 = (int)std::floor(z0 / GRID_CELL) - 1, j1 = (int)std::floor(z1 / GRID_CELL) + 1;
  for (int j = j0; j <= j1; ++j)
    for (int i = i0; i <= i1; ++i) {
      auto it = railGrid_.find(cellKey(i, j));
      if (it == railGrid_.end()) continue;
      for (const Chord& p : it->second) {
        if (std::max(p.x, p.x2) < x0 || std::min(p.x, p.x2) > x1) continue;
        if (std::max(p.z, p.z2) < z0 || std::min(p.z, p.z2) > z1) continue;
        return true;
      }
    }
  return false;
}

// ------------------------------------------------------------------ meshes
void Terrain::upload(Tile& t, MeshBuilder& mb, const SatLayer* layer, int tx, int ty, vec3 pos) {
  t.mesh = mb.upload();
  t.xf = mat4::translation(pos);
  t.bounds = mb.bounds.transformed(t.xf);
  if (layer && layer->has(tx, ty)) {
    const uint8_t* px = layer->tile(tx, ty);
    t.tex = rhi::createTexture(layer->px, layer->px, rhi::Format::RGB8,
                               std::as_bytes(std::span(px, (size_t)layer->px * layer->px * 3)), true, true);
    t.textured = true;
  }
  stats.vertices += mb.vertices.size();
  stats.triangles += mb.indices.size() / 3;
}

// Ground mesh of one z14 tile (dunia3d.ts geoTanah): K×K blocks, cell size by tier (near rails / near
// a near block / far). Each block is textured by the finest present satellite layer whose tile contains
// it (z17 > z16 > z14) and appended to the patch mesh of that texture tile, so one draw per texture.
// UVs from position over the texture tile; normals from central differences of the carved height field
// (consistent across patch seams); skirts at density seams and z14 tile edges.
void Terrain::buildNearTile(int tx, int ty) {
  const double ts = slippy::tileSizeMeter(TILE_Z);
  const double cx = slippy::tileOriginX(tx, TILE_Z) + ts / 2, cy = slippy::tileOriginY(ty, TILE_Z) + ts / 2;
  const int K = BLOCKS_PER_TILE;
  const double bs = ts / K;
  const float cellOf[3] = {CELL_NEAR, CELL_MID, CELL_FAR};
  const int R = (int)std::ceil(MID_MARGIN / bs), W = K + 2 * R;

  std::vector<uint8_t> core((size_t)W * W, 0);
  bool corridor = railInBox(cx, cy, ts, MID_MARGIN + bs);
  if (corridor)
    for (int j = 0; j < W; ++j)
      for (int i = 0; i < W; ++i)
        core[(size_t)j * W + i] = railInBox(cx - ts / 2 + (i - R + 0.5) * bs, cy - ts / 2 + (j - R + 0.5) * bs, bs, BLOCK_MARGIN);
  std::vector<int> tier((size_t)K * K, 2);
  for (int bz = 0; bz < K; ++bz)
    for (int bx = 0; bx < K; ++bx) {
      if (core[(size_t)(bz + R) * W + (bx + R)]) { tier[(size_t)bz * K + bx] = 0; continue; }
      bool near1 = false;
      for (int dj = -R; dj <= R && !near1; ++dj)
        for (int di = -R; di <= R; ++di)
          if (core[(size_t)(bz + R + dj) * W + (bx + R + di)]) { near1 = true; break; }
      tier[(size_t)bz * K + bx] = near1 ? 1 : 2;
    }
  auto cellsOf = [&](int b) { return std::max(1, (int)std::lround(bs / cellOf[tier[(size_t)b]])); };

  // texture source per block: (layer index, tile) of the finest detail layer containing the block centre
  struct Key { int layer, tx, ty; bool operator==(const Key& o) const { return layer == o.layer && tx == o.tx && ty == o.ty; } };
  struct Patch { Key key; MeshBuilder mb; };
  std::vector<Patch> patches;
  auto patchFor = [&](double wx, double wy) -> Patch& {
    Key k{0, tx, ty};
    for (int li : sat_.detail) {
      const SatLayer& L = sat_.layers[(size_t)li];
      int dx = slippy::worldToTileX(wx, L.zoom), dy = slippy::worldToTileY(wy, L.zoom);
      if (L.has(dx, dy)) { k = {li, dx, dy}; break; }
    }
    for (Patch& p : patches) if (p.key == k) return p;
    patches.push_back({k, {}});
    return patches.back();
  };

  for (int bz = 0; bz < K; ++bz)
    for (int bx = 0; bx < K; ++bx) {
      const int b = bz * K + bx, n = cellsOf(b);
      const double x0 = -ts / 2 + bx * bs, z0 = -ts / 2 + bz * bs, cs = bs / n;
      Patch& P = patchFor(cx + x0 + bs / 2, cy + z0 + bs / 2);
      const SatLayer& L = sat_.layers[(size_t)P.key.layer];
      const double ux0 = slippy::tileOriginX(P.key.tx, L.zoom) - cx, uz0 = slippy::tileOriginY(P.key.ty, L.zoom) - cy;
      MeshBuilder& mb = P.mb;
      // heights with a one-cell halo for central-difference normals
      const int H = n + 3;
      std::vector<float> hg((size_t)H * H);
      for (int j = 0; j < H; ++j)
        for (int i = 0; i < H; ++i) hg[(size_t)j * H + i] = groundHeight(cx + x0 + cs * (i - 1), cy + z0 + cs * (j - 1));
      auto node = [&](double lx, double lz, float y, vec3 nrm, float drop) {
        float u = (float)((lx - ux0) / L.ts), v = (float)((lz - uz0) / L.ts);   // row 0 of the image = north = small lz
        u = (0.5f + u * (L.px - 1)) / L.px; v = (0.5f + v * (L.px - 1)) / L.px;
        return mb.vertex({(float)lx, y - drop, (float)lz}, nrm, {u, v});
      };
      const uint32_t base = (uint32_t)mb.vertices.size();
      for (int j = 0; j <= n; ++j)
        for (int i = 0; i <= n; ++i) {
          auto h = [&](int a, int c) { return hg[(size_t)(c + 1) * H + (a + 1)]; };
          vec3 nrm = normalize(vec3{(h(i - 1, j) - h(i + 1, j)) / (float)(2 * cs), 1.f, (h(i, j - 1) - h(i, j + 1)) / (float)(2 * cs)});
          node(x0 + cs * i, z0 + cs * j, h(i, j), nrm, 0);
        }
      for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
          uint32_t a = base + (uint32_t)(j * (n + 1) + i);
          mb.triangle(a, a + n + 1, a + 1); mb.triangle(a + 1, a + n + 1, a + n + 2);
        }
      // skirts: only at seams that differ in density, and always at tile edges
      auto at = [&](int i, int j) { return mb.vertices[base + (size_t)(j * (n + 1) + i)].pos; };
      auto wall = [&](vec3 a, vec3 c, float ox, float oz, float depth) {
        if (-(c.z - a.z) * ox + (c.x - a.x) * oz < 0) std::swap(a, c);   // front face outward
        vec3 nrm{ox, 0, oz};
        uint32_t t0 = node(a.x, a.z, a.y, nrm, 0); node(c.x, c.z, c.y, nrm, 0); node(a.x, a.z, a.y, nrm, depth); node(c.x, c.z, c.y, nrm, depth);
        mb.triangle(t0, t0 + 2, t0 + 1); mb.triangle(t0 + 1, t0 + 2, t0 + 3);
      };
      auto skirtDepth = [&](int dx, int dz) -> float {
        int nx = bx + dx, nz = bz + dz;
        bool edge = nx < 0 || nz < 0 || nx >= K || nz >= K;
        float cellN = edge ? CELL_FAR : cellOf[tier[(size_t)(nz * K + nx)]];
        if (!edge && std::max(1, (int)std::lround(bs / cellN)) == n) return 0;
        return std::max(SKIRT_MIN, SKIRT_FACTOR * std::max(cellN, cellOf[tier[(size_t)b]]));
      };
      float sN = skirtDepth(0, -1), sS = skirtDepth(0, 1), sW = skirtDepth(-1, 0), sE = skirtDepth(1, 0);
      for (int i = 0; i < n; ++i) {
        if (sN > 0) wall(at(i, 0), at(i + 1, 0), 0, -1, sN);
        if (sS > 0) wall(at(i, n), at(i + 1, n), 0, 1, sS);
        if (sW > 0) wall(at(0, i), at(0, i + 1), -1, 0, sW);
        if (sE > 0) wall(at(n, i), at(n, i + 1), 1, 0, sE);
      }
    }
  for (Patch& P : patches) {
    Tile t{};
    upload(t, P.mb, &sat_.layers[(size_t)P.key.layer], P.key.tx, P.key.ty, origin_.toScene(cx, cy, 0));
    near_.push_back(t);
    ++stats.patches; if (P.key.layer != 0) ++stats.detailPatches;
  }
}

// Far layer: one coarse tile, uncarved DEM, cells aligned with the z14 grid, holes where near tiles exist.
Terrain::Tile Terrain::buildFarTile(const SatLayer& far, int tx, int ty) {
  const double ts = slippy::tileSizeMeter(far.zoom), ts14 = slippy::tileSizeMeter(TILE_Z);
  const double cx = slippy::tileOriginX(tx, far.zoom) + ts / 2, cy = slippy::tileOriginY(ty, far.zoom) + ts / 2;
  const int N = std::max(1, (int)std::lround(ts / ts14)) * FAR_CELLS_PER_TILE;
  const double bs = ts / N;
  const SatLayer& nearL = sat_.layers[0];
  MeshBuilder mb;
  Tile t{};
  for (int j = 0; j <= N; ++j)
    for (int i = 0; i <= N; ++i) {
      double lx = -ts / 2 + ts * i / N, lz = -ts / 2 + ts * j / N;
      float u = (0.5f + (float)i / N * (far.px - 1)) / far.px, v = (0.5f + (float)j / N * (far.px - 1)) / far.px;
      mb.vertex({(float)lx, dem_.heightScene(cx + lx, cy + lz), (float)lz}, {0, 1, 0}, {u, v});
    }
  for (int j = 0; j < N; ++j)
    for (int i = 0; i < N; ++i) {
      double wx = cx - ts / 2 + (i + 0.5) * bs, wy = cy - ts / 2 + (j + 0.5) * bs;
      int qx = slippy::worldToTileX(wx, TILE_Z), qy = slippy::worldToTileY(wy, TILE_Z);
      if (qx >= nearL.tx0 && qx < nearL.tx0 + nearL.nx && qy >= nearL.ty0 && qy < nearL.ty0 + nearL.ny) continue;
      uint32_t a = (uint32_t)(j * (N + 1) + i);
      mb.triangle(a, a + N + 1, a + 1); mb.triangle(a + 1, a + N + 1, a + N + 2);
    }
  if (!mb.empty()) { mb.computeSmoothNormals(); upload(t, mb, &far, tx, ty, origin_.toScene(cx, cy, 0)); }
  return t;
}

void Terrain::build() {
  auto t0 = std::chrono::steady_clock::now();
  destroy();
  stats.vertices = stats.triangles = 0; stats.nearTiles = stats.farTiles = stats.patches = stats.detailPatches = 0;
  if (sat_.layers.empty()) return;
  const SatLayer& nearL = sat_.layers[0];
  for (int ty = nearL.ty0; ty < nearL.ty0 + nearL.ny; ++ty)
    for (int tx = nearL.tx0; tx < nearL.tx0 + nearL.nx; ++tx) { buildNearTile(tx, ty); ++stats.nearTiles; }
  if (sat_.layers.size() > 1) {
    const SatLayer& farL = sat_.layers[1];
    for (int ty = farL.ty0; ty < farL.ty0 + farL.ny; ++ty)
      for (int tx = farL.tx0; tx < farL.tx0 + farL.nx; ++tx) {
        Tile t = buildFarTile(farL, tx, ty);
        if (t.mesh.indexCount) { far_.push_back(t); ++stats.farTiles; }
      }
  }
  // backdrop plane far below everything
  float y = std::min(-12.f, dem_.rawMin - dem_.demBase - 30);
  const float s = BACKDROP_SIZE / 2;
  MeshBuilder mb;
  mb.quad({-s, y, s}, {s, y, s}, {s, y, -s}, {-s, y, -s});
  backdrop_ = {}; upload(backdrop_, mb, nullptr, 0, 0, {0, 0, 0});
  built_ = true;
  stats.buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void Terrain::draw(ModelRenderer& r, const Frustum* frustum) {
  if (!built_) return;
  auto drawTile = [&](Tile& t) {
    if (frustum && !frustum->contains(t.bounds)) { ++r.culled; return; }
    r.drawMesh(t.mesh, t.textured ? ground_ : backdropMat_, t.tex, t.xf);
  };
  r.drawMesh(backdrop_.mesh, backdropMat_, {}, backdrop_.xf);
  for (Tile& t : far_) drawTile(t);
  for (Tile& t : near_) drawTile(t);
}

void Terrain::destroy() {
  auto kill = [](Tile& t) { rhi::destroyMesh(t.mesh); rhi::destroyTexture(t.tex); t = {}; };
  for (Tile& t : near_) kill(t);
  for (Tile& t : far_) kill(t);
  if (built_) kill(backdrop_);
  near_.clear(); far_.clear(); built_ = false;
}

} // namespace eng

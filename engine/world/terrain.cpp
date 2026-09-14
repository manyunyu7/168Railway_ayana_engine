#include "engine/world/terrain.h"
#include "engine/asset/emod.h"
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
constexpr uint8_t DEM_FAILED = 2;   // DemLayer::present: 0 absent, 1 delivered, 2 fetch failed (stays flat)

// Streaming radii per satellite layer (ubinStream.ts / DETAIL_TANAH): near layer 4000/6000, far layer resident,
// detail by zoom (z16 1500/2200, z17 450/800).
void radiiFor(size_t layerIndex, int zoom, float& inR, float& outR) {
  if (layerIndex == 1) { inR = outR = 0; return; }
  if (layerIndex == 0 || zoom <= 15) { inR = R_LOAD; outR = R_EVICT; return; }
  if (zoom == 16) { inR = R16_IN; outR = R16_OUT; return; }
  inR = R17_IN; outR = R17_OUT;
}
} // namespace

// ------------------------------------------------------------------ DemLayer / Dem
bool DemLayer::covers(double wx, double wy) const {
  int i = (int)std::floor((wx - x0) / ts), j = (int)std::floor((wy - y0) / ts);
  return i >= 0 && j >= 0 && i < nx && j < ny && present[(size_t)j * nx + i] == 1;
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
      if (present[(size_t)j * nx + i] != 1) continue;
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
  for (DemLayer& L : layers) {
    L.zoom = r.i32(); L.tx0 = r.i32(); L.ty0 = r.i32(); L.nx = r.i32(); L.ny = r.i32(); L.n = r.i32();
    if (!r.ok || L.nx < 1 || L.ny < 1 || L.n < 2 || L.nx * L.ny > 4096) { error = path + ": bad layer"; return false; }
    L.ts = slippy::tileSizeMeter(L.zoom); L.x0 = slippy::tileOriginX(L.tx0, L.zoom); L.y0 = slippy::tileOriginY(L.ty0, L.zoom);
    L.present.resize((size_t)L.nx * L.ny);
    L.h.resize((size_t)L.nx * L.ny * L.n * L.n);
    if (!r.bytes(L.present.data(), L.present.size()) || !r.bytes(L.h.data(), L.h.size() * 4)) { error = path + ": truncated"; return false; }
    L.indexed = L.present;
  }
  finish();
  return true;
}

void Dem::finish() {
  rawMin = 1e30f;
  for (const DemLayer& L : layers)
    for (size_t t = 0; t < L.present.size(); ++t)
      if (L.present[t] == 1) for (size_t k = 0; k < (size_t)L.n * L.n; ++k) rawMin = std::min(rawMin, L.h[t * L.n * L.n + k]);
  if (rawMin > 1e29f) rawMin = 0;
  demBase = rawHeight((bbox[0] + bbox[2]) / 2, (bbox[1] + bbox[3]) / 2);
}

float Dem::rawHeight(double wx, double wy) const {
  for (const DemLayer& L : layers) if (L.covers(wx, wy)) return L.sample(wx, wy);
  return layers.empty() ? 0.f : layers.back().sampleClamped(wx, wy);
}

// ------------------------------------------------------------------ satellite
bool SatLayer::has(int tx, int ty) const { return inside(tx, ty) && state[(size_t)index(tx, ty)] == Resident; }
bool SatLayer::covers(double wx, double wy) const {
  return has(slippy::worldToTileX(wx, zoom), slippy::worldToTileY(wy, zoom));
}
const SatLayer::Res* SatLayer::tile(int tx, int ty) const {
  if (!has(tx, ty)) return nullptr;
  auto it = res.find(index(tx, ty));
  return it == res.end() ? nullptr : &it->second;
}
double SatLayer::edgeDistance(int tx, int ty, double wx, double wy) const {
  double x0 = slippy::tileOriginX(tx, zoom), y0 = slippy::tileOriginY(ty, zoom);
  double dx = std::max({x0 - wx, 0.0, wx - (x0 + ts)}), dy = std::max({y0 - wy, 0.0, wy - (y0 + ts)});
  return std::hypot(dx, dy);
}

bool SatImage::load(const std::string& path, std::string& error) {
  Reader r(path);
  if (!r.ok) { error = "cannot open " + path; return false; }
  if (!r.magic("ESAT")) { error = path + ": not an ESAT file"; return false; }
  int ver = r.i32();
  if (ver != 2) { error = path + ": ESAT v" + std::to_string(ver) + " is not supported (v2 expected; re-run fetch_tiles)"; return false; }
  int nl = r.i32();
  if (!r.ok || nl < 1 || nl > 16) { error = path + ": bad header"; return false; }
  layers.resize((size_t)nl); store.resize((size_t)nl);
  double sum = 0; size_t n = 0;
  for (size_t li = 0; li < layers.size(); ++li) {
    SatLayer& L = layers[li];
    L.zoom = r.i32(); L.tx0 = r.i32(); L.ty0 = r.i32(); L.nx = r.i32(); L.ny = r.i32(); L.px = r.i32();
    if (!r.ok || L.nx < 1 || L.ny < 1 || L.px < 1 || L.px > 4096 || L.nx * L.ny > 65536) { error = path + ": bad layer"; return false; }
    L.ts = slippy::tileSizeMeter(L.zoom); L.mpp = L.ts / L.px;
    L.indexed.resize((size_t)L.nx * L.ny);
    if (!r.bytes(L.indexed.data(), L.indexed.size())) { error = path + ": truncated"; return false; }
    L.state.assign(L.indexed.size(), SatLayer::Absent);
    radiiFor(li, L.zoom, L.inR, L.outR);
    for (size_t t = 0; t < L.indexed.size(); ++t) {
      if (!L.indexed[t]) continue;
      std::vector<uint8_t> rgb((size_t)L.px * L.px * 3);
      if (!r.bytes(rgb.data(), rgb.size())) { error = path + ": truncated"; return false; }
      if (L.zoom == TILE_Z) for (size_t i = 0; i + 2 < rgb.size(); i += 3 * 16) { sum += std::max({rgb[i], rgb[i + 1], rgb[i + 2]}); ++n; }
      store[li][(int)t] = std::move(rgb);
    }
  }
  // mean brightness of the near layer (vegetation mask reference, vegetasi.ts terRata), subsampled
  meanBrightness = n ? (float)(sum / n / 255.0) : 0.4f; meanFixed = true;
  finish();
  return true;
}

void SatImage::finish() {
  int nl = (int)layers.size();
  detail.clear();
  for (int i = 0; i < nl; ++i) if (layers[(size_t)i].zoom > TILE_Z) detail.push_back(i);
  std::sort(detail.begin(), detail.end(), [&](int a, int b) { return layers[(size_t)a].mpp < layers[(size_t)b].mpp; });
}

int Terrain::finestLayerAt(double wx, double wy) const {
  for (int i : sat_.detail) if (sat_.layers[(size_t)i].covers(wx, wy)) return i;
  if (!sat_.layers.empty() && sat_.layers[0].covers(wx, wy)) return 0;
  return -1;
}

int64_t Terrain::imageryKeyAt(double wx, double wy) const {
  int li = finestLayerAt(wx, wy);
  if (li < 0) return -1;
  const SatLayer& L = sat_.layers[(size_t)li];
  return ((int64_t)li << 40) | (int64_t)L.index(slippy::worldToTileX(wx, L.zoom), slippy::worldToTileY(wy, L.zoom));
}

bool Terrain::satColor(double wx, double wy, float radius, float rgb[3]) const {
  int li = finestLayerAt(wx, wy);
  if (li < 0) return false;
  const SatLayer* L = &sat_.layers[(size_t)li];
  int tx = slippy::worldToTileX(wx, L->zoom), ty = slippy::worldToTileY(wy, L->zoom);
  const SatLayer::Res* t = L->tile(tx, ty);
  if (!t || t->rgbPx < 1) return false;
  const int P = t->rgbPx; const double mpp = L->ts / P;
  double fx = (wx - slippy::tileOriginX(tx, L->zoom)) / L->ts * P, fy = (wy - slippy::tileOriginY(ty, L->zoom)) / L->ts * P;
  int r = std::max(0, (int)(radius / mpp));
  int x0 = std::clamp((int)fx - r, 0, P - 1), x1 = std::clamp((int)fx + r, 0, P - 1);
  int y0 = std::clamp((int)fy - r, 0, P - 1), y1 = std::clamp((int)fy + r, 0, P - 1);
  double acc[3] = {0, 0, 0}; int n = 0;
  for (int y = y0; y <= y1; ++y)
    for (int x = x0; x <= x1; ++x) { const uint8_t* p = t->rgb.data() + ((size_t)y * P + x) * 3; acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2]; ++n; }
  for (int c = 0; c < 3; ++c) rgb[c] = (float)(acc[c] / n / 255.0);
  return true;
}

// ------------------------------------------------------------------ Terrain: data sources
void Terrain::resetLayers() {
  destroy();
  dem_ = Dem{}; sat_ = SatImage{}; near_.clear(); far_.clear();
  checkTimer_ = 0; firstCheck_ = true; imageryVersion_ = 0; meanSum_ = 0; meanN_ = 0;
  ground_ = {}; ground_.baseColor = {0.81f, 0.81f, 0.81f, 1}; ground_.metallic = 0; ground_.roughness = 1;
  backdropMat_ = {}; backdropMat_.baseColor = {0.616f, 0.690f, 0.537f, 1}; backdropMat_.metallic = 0; backdropMat_.roughness = 1;
  loadingMat_ = {}; loadingMat_.baseColor = {0.102f, 0.125f, 0.153f, 1}; loadingMat_.metallic = 0; loadingMat_.roughness = 1;   // 0x1a2027 (dunia3d.ts)
}

bool Terrain::load(const std::string& demPath, const std::string& satPath, std::string& error) {
  auto t0 = std::chrono::steady_clock::now();
  resetLayers(); streamed_ = false;
  if (!dem_.load(demPath, error)) return false;
  if (!sat_.load(satPath, error)) return false;
  origin_ = dem_.origin();
  stats.loadMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  return true;
}

std::string Terrain::TileRef::path() const { return dir + "/" + std::to_string(z) + "_" + std::to_string(x) + "_" + std::to_string(y) + ".bin"; }

// index.json (tools/fetch_tiles): { bbox:[x0,y0,x1,y1], dem:[{dir,zoom,tx0,ty0,nx,ny,px,present:"0101.."}], sat:[...], mean }
// A rejected index leaves the terrain in the "none" state (loadNone with whatever bbox it had) so the caller
// can carry on without terrain; nothing indexes into a half-built layer list.
bool Terrain::loadIndex(const Json& index, std::string& error) {
  if (loadIndexLayers(index, error)) return true;
  double bb[4]; for (int i = 0; i < 4; ++i) bb[i] = dem_.bbox[i];
  loadNone(bb);
  return false;
}

bool Terrain::loadIndexLayers(const Json& index, std::string& error) {
  resetLayers(); streamed_ = true;
  if (!index.isObject()) { error = "index.json: not an object"; return false; }
  const Json& bb = index["bbox"];
  if (bb.size() != 4) { error = "index.json: bbox missing"; return false; }
  for (int i = 0; i < 4; ++i) dem_.bbox[i] = bb[(size_t)i].numberOr(0);
  auto presentOf = [](const Json& L, size_t n) { std::vector<uint8_t> p(n, 0); const std::string& s = L["present"].str; for (size_t i = 0; i < n && i < s.size(); ++i) p[i] = s[i] == '1'; return p; };
  for (const Json& L : index["dem"].arr) {
    DemLayer d; d.zoom = L["zoom"].intOr(0); d.tx0 = L["tx0"].intOr(0); d.ty0 = L["ty0"].intOr(0); d.nx = L["nx"].intOr(0); d.ny = L["ny"].intOr(0); d.n = L["px"].intOr(256);
    if (d.nx < 1 || d.ny < 1 || d.n < 2 || d.nx * d.ny > 4096) { error = "index.json: bad dem layer"; return false; }
    d.ts = slippy::tileSizeMeter(d.zoom); d.x0 = slippy::tileOriginX(d.tx0, d.zoom); d.y0 = slippy::tileOriginY(d.ty0, d.zoom);
    d.indexed = presentOf(L, (size_t)d.nx * d.ny);
    d.present.assign(d.indexed.size(), 0);
    d.h.assign((size_t)d.nx * d.ny * d.n * d.n, 0.f);
    dem_.layers.push_back(std::move(d));
  }
  for (const Json& L : index["sat"].arr) {
    SatLayer s; s.zoom = L["zoom"].intOr(0); s.tx0 = L["tx0"].intOr(0); s.ty0 = L["ty0"].intOr(0); s.nx = L["nx"].intOr(0); s.ny = L["ny"].intOr(0); s.px = L["px"].intOr(256);
    if (s.nx < 1 || s.ny < 1 || s.px < 1 || s.nx * s.ny > 65536) { error = "index.json: bad sat layer"; return false; }
    s.ts = slippy::tileSizeMeter(s.zoom); s.mpp = s.ts / s.px;
    s.indexed = presentOf(L, (size_t)s.nx * s.ny);
    s.state.assign(s.indexed.size(), SatLayer::Absent);
    radiiFor(sat_.layers.size(), s.zoom, s.inR, s.outR);
    sat_.layers.push_back(std::move(s));
  }
  if (dem_.layers.empty() || sat_.layers.empty()) { error = "index.json: no layers"; return false; }
  sat_.store.resize(sat_.layers.size());
  if (index["mean"].isNumber()) { sat_.meanBrightness = (float)index["mean"].num; sat_.meanFixed = true; }
  sat_.finish();
  origin_ = dem_.origin();
  return true;
}

void Terrain::loadNone(const double* bbox) {
  resetLayers(); streamed_ = true;
  for (int i = 0; i < 4; ++i) dem_.bbox[i] = bbox ? bbox[i] : 0;
  dem_.finish();
  origin_ = dem_.origin();
}

std::vector<Terrain::TileRef> Terrain::demTilesWanted() const {
  std::vector<TileRef> out;
  for (const DemLayer& d : dem_.layers)
    for (int j = 0; j < d.ny; ++j) for (int i = 0; i < d.nx; ++i) { size_t t = (size_t)j * d.nx + i; if (d.indexed[t] && !d.present[t]) out.push_back({"dem", d.zoom, d.tx0 + i, d.ty0 + j}); }
  return out;
}

bool Terrain::demComplete() const {
  for (const DemLayer& d : dem_.layers)
    for (size_t t = 0; t < d.indexed.size(); ++t) if (d.indexed[t] && !d.present[t]) return false;
  return true;
}

bool Terrain::provideDem(int z, int x, int y, std::span<const float> heights) {
  for (DemLayer& d : dem_.layers) {
    if (d.zoom != z || x < d.tx0 || y < d.ty0 || x >= d.tx0 + d.nx || y >= d.ty0 + d.ny) continue;
    size_t t = (size_t)(y - d.ty0) * d.nx + (x - d.tx0);
    if (heights.size() != (size_t)d.n * d.n) { d.present[t] = DEM_FAILED; return false; }   // wrong size: stays absent
    std::memcpy(d.h.data() + t * d.n * d.n, heights.data(), heights.size() * 4);
    d.present[t] = 1;
    return true;
  }
  return false;
}

// Terrarium: h = R*256 + G + B/256 - 32768 (row 0 = north). A tile larger than n×n is point-sampled.
bool Terrain::provideDemRgba(int z, int x, int y, int w, int h, const uint8_t* rgba) {
  for (DemLayer& d : dem_.layers) {
    if (d.zoom != z || x < d.tx0 || y < d.ty0 || x >= d.tx0 + d.nx || y >= d.ty0 + d.ny) continue;
    if (w < 1 || h < 1 || !rgba) return provideDem(z, x, y, {});
    std::vector<float> hs((size_t)d.n * d.n);
    for (int j = 0; j < d.n; ++j)
      for (int i = 0; i < d.n; ++i) {
        int sx = std::min(w - 1, (int)((int64_t)i * w / d.n)), sy = std::min(h - 1, (int)((int64_t)j * h / d.n));
        const uint8_t* p = rgba + ((size_t)sy * w + sx) * 4;
        hs[(size_t)j * d.n + i] = p[0] * 256.f + p[1] + p[2] / 256.f - 32768.f;
      }
    return provideDem(z, x, y, hs);
  }
  return false;
}

void Terrain::finishDem() { dem_.finish(); }

bool Terrain::provideSat(int layer, int z, int x, int y, Image im, std::string& error) {
  if (layer < 0 || (size_t)layer >= sat_.layers.size()) { error = "bad layer"; return false; }
  SatLayer& s = sat_.layers[(size_t)layer];
  if (s.zoom != z || !s.inside(x, y)) { error = "tile outside the layer"; return false; }
  int t = s.index(x, y);
  if (!s.indexed[(size_t)t]) { error = "tile not in the index"; return false; }
  if (s.state[(size_t)t] == SatLayer::Absent) { error = "tile not requested (out of range now)"; return false; }
  // rgb copy for satColor / the vegetation mask: RGBA8 pixels, else the RGBA8 variant (small fallback on the web)
  const MipLevel* rgba = nullptr;
  for (const ImageVariant& v : im.variants) if (v.format == TexFormat::RGBA8 && !v.mips.empty()) rgba = &v.mips[0];
  int P = rgba ? rgba->width : (im.pixels.empty() ? 0 : im.width);
  const uint8_t* src = rgba ? rgba->data.data() : im.pixels.data();
  if (P < 1 || !src || (!rgba && im.width != im.height)) { error = "no pixels"; s.state[(size_t)t] = SatLayer::Failed; return false; }
  SatLayer::Res& r = s.res[t];
  rhi::destroyTexture(r.tex); r = {};
  r.rgbPx = P; r.rgb.resize((size_t)P * P * 3);
  for (size_t k = 0; k < (size_t)P * P; ++k) std::memcpy(r.rgb.data() + k * 3, src + k * 4, 3);
  im.wrapS = im.wrapT = 1;
  r.image = std::move(im);
  s.state[(size_t)t] = SatLayer::Resident;
  if (layer == 0 && !sat_.meanFixed) {   // running mean over the near tiles seen (vegetasi.ts terRata)
    for (size_t i = 0; i + 2 < r.rgb.size(); i += 3 * 16) { meanSum_ += std::max({r.rgb[i], r.rgb[i + 1], r.rgb[i + 2]}); ++meanN_; }
    sat_.meanBrightness = meanN_ ? (float)(meanSum_ / (double)meanN_ / 255.0) : 0.4f;
  }
  markDirty(layer, x, y);
  noteImagery();
  return true;
}

bool Terrain::provideSatRgba(int layer, int z, int x, int y, int w, int h, const uint8_t* rgba, std::string& error) {
  if (w < 1 || h < 1 || !rgba) { error = "no pixels"; return false; }
  Image im; im.width = w; im.height = h; im.channels = 4;
  im.pixels.assign(rgba, rgba + (size_t)w * h * 4);
  return provideSat(layer, z, x, y, std::move(im), error);
}

bool Terrain::provideSatFile(int layer, int z, int x, int y, std::span<const uint8_t> bytes, std::string& error) {
  Image im;
  if (!loadImageFile(bytes, im, error)) return false;
  return provideSat(layer, z, x, y, std::move(im), error);
}

void Terrain::failTile(const TileRef& t) {
  if (t.dir == "dem") {
    for (DemLayer& d : dem_.layers)
      if (d.zoom == t.z && t.x >= d.tx0 && t.y >= d.ty0 && t.x < d.tx0 + d.nx && t.y < d.ty0 + d.ny) { size_t k = (size_t)(t.y - d.ty0) * d.nx + (t.x - d.tx0); if (!d.present[k]) d.present[k] = DEM_FAILED; }
    return;
  }
  if (t.dir.rfind("sat/", 0) != 0) return;
  int li = std::atoi(t.dir.c_str() + 4);
  if (li < 0 || (size_t)li >= sat_.layers.size()) return;
  SatLayer& s = sat_.layers[(size_t)li];
  if (s.zoom == t.z && s.inside(t.x, t.y) && s.state[(size_t)s.index(t.x, t.y)] == SatLayer::Requested) s.state[(size_t)s.index(t.x, t.y)] = SatLayer::Failed;
}

// ------------------------------------------------------------------ carving
void Terrain::addChord(vec3 a, vec3 b, float ba, float bb) {
  int64_t k = cellKey((int)std::floor((a.x + b.x) * 0.5f / GRID_CELL), (int)std::floor((a.z + b.z) * 0.5f / GRID_CELL));
  railGrid_[k].push_back({a.x, a.z, a.y, b.x, b.z, b.y, ba, bb});
}

void Terrain::setRails(std::span<const RailSample> samples) {
  railGrid_.clear(); bridgeGrid_.clear();
  for (size_t i = 1; i < samples.size(); ++i) {
    const RailSample &p = samples[i - 1], &q = samples[i];
    vec3 a = origin_.toScene(p.wx, p.wy, p.railY), b = origin_.toScene(q.wx, q.wy, q.railY);
    float dx = a.x - b.x, dz = a.z - b.z;
    if (dx * dx + dz * dz > CHORD_MAX * CHORD_MAX) continue;
    if (p.atGrade && q.atGrade) addChord(a, b, p.mouthBlend, q.mouthBlend);
    if (p.bridgeBlend >= 0 && q.bridgeBlend >= 0) {   // deck bottom chord (medan3d.ts tambahJbtRuas)
      int64_t k = cellKey((int)std::floor((a.x + b.x) * 0.5f / GRID_CELL), (int)std::floor((a.z + b.z) * 0.5f / GRID_CELL));
      bridgeGrid_[k].push_back({a.x, a.z, a.y + DECK_BOTTOM, b.x, b.z, b.y + DECK_BOTTOM, p.bridgeBlend, q.bridgeBlend});
    }
  }
  for (NearTile& t : near_) if (t.built) { t.dirty = true; t.dirtyAge = 1e9f; }   // rails changed after tiles were built: re-cut
}

void Terrain::setBrushDeltas(const Json& tanah) {
  delta_.clear();
  if (!tanah.isObject()) return;
  const Json& d = tanah["delta"];
  if (!d.isObject()) return;
  for (const auto& [key, v] : d.obj) {
    int gx = 0, gz = 0;
    if (std::sscanf(key.c_str(), "%d,%d", &gx, &gz) != 2) continue;
    delta_[cellKey(gx, gz)] = (float)v.numberOr(0);
  }
}

// Bilinear brush delta on the 8 m grid (medan3d.ts deltaDi).
float Terrain::brushDelta(double wx, double wy) const {
  if (delta_.empty()) return 0;
  double fx = wx / DELTA_GRID, fz = wy / DELTA_GRID;
  int gx = (int)std::floor(fx), gz = (int)std::floor(fz);
  float sx = (float)(fx - gx), sz = (float)(fz - gz);
  auto d = [&](int a, int b) { auto it = delta_.find(cellKey(a, b)); return it == delta_.end() ? 0.f : it->second; };
  float a = d(gx, gz) * (1 - sx) + d(gx + 1, gz) * sx, b = d(gx, gz + 1) * (1 - sx) + d(gx + 1, gz + 1) * sx;
  return a * (1 - sz) + b * sz;
}

bool Terrain::nearestDeck(float x, float z, float& dOut, float& yOut, float& bOut) const {
  if (bridgeGrid_.empty()) return false;
  int cx = (int)std::floor(x / GRID_CELL), cz = (int)std::floor(z / GRID_CELL);
  const int r = (int)std::ceil(BRIDGE_OUTER / GRID_CELL);
  float best = 1e30f;
  for (int i = -r; i <= r; ++i)
    for (int j = -r; j <= r; ++j) {
      auto it = bridgeGrid_.find(cellKey(cx + i, cz + j));
      if (it == bridgeGrid_.end()) continue;
      for (const Chord& p : it->second) {
        float dx = p.x2 - p.x, dz = p.z2 - p.z, L2 = dx * dx + dz * dz;
        float t = L2 > 0 ? std::clamp(((x - p.x) * dx + (z - p.z) * dz) / L2, 0.f, 1.f) : 0.f;
        float d = std::hypot(p.x + dx * t - x, p.z + dz * t - z);
        if (d < best) { best = d; yOut = p.y + (p.y2 - p.y) * t; bOut = p.b + (p.b2 - p.b) * t; }
      }
    }
  dOut = best;
  return best < 1e30f;
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

// tanahTerukir: DEM + brush delta -> rail carve -> bridge trough (lowering only).
float Terrain::groundHeight(double wx, double wy) const {
  float h = dem_.heightScene(wx, wy) + brushDelta(wx, wy);
  if (railGrid_.empty() && bridgeGrid_.empty()) return h;
  float x = (float)(wx - origin_.ox), z = (float)(wy - origin_.oz);
  Nearest n;
  if (nearestRail(x, z, n) && n.d <= CARVE_OUTER) {
    // Toward a tunnel mouth (b -> 0) the reference fades the whole carve out, which leaves the portal
    // buried when the DEM sits above the rail there. Instead the corridor NARROWS with b (inner 9 -> 4 m,
    // outer 60 -> 12 m) so the hill still stands over the tunnel but a short cutting opens the portal.
    float inner = MOUTH_INNER + (CARVE_INNER - MOUTH_INNER) * n.b, outer = MOUTH_OUTER + (CARVE_OUTER - MOUTH_OUTER) * n.b;
    if (n.d <= outer) {
      float t = n.d <= inner ? 1.f : 1 - (n.d - inner) / (outer - inner);
      float w = smoothstep01(t);
      h = h * (1 - w) + (carveBase(n) + PLATEAU_OFFSET) * w;
    }
  }
  float jd, jy, jb;
  if (nearestDeck(x, z, jd, jy, jb) && jd <= BRIDGE_OUTER && jb > 0) {
    float ceiling = jy - BRIDGE_CLEAR;
    if (h > ceiling) {
      float t = jd <= BRIDGE_INNER ? 1.f : 1 - (jd - BRIDGE_INNER) / (BRIDGE_OUTER - BRIDGE_INNER);
      h += (ceiling - h) * smoothstep01(t) * jb;
    }
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
const rhi::Texture* Terrain::textureOf(const Key& k) const {
  if (k.layer < 0 || (size_t)k.layer >= sat_.layers.size()) return nullptr;
  const SatLayer::Res* r = sat_.layers[(size_t)k.layer].tile(k.tx, k.ty);
  return r && r->uploaded && r->tex.id ? &r->tex : nullptr;
}

void Terrain::freeNear(NearTile& t) {
  for (Patch& p : t.patches) rhi::destroyMesh(p.mesh);
  t.patches.clear(); t.built = false; t.dirty = false;
}

bool Terrain::nearTileBuilt(int tx, int ty) const {
  for (const NearTile& t : near_) if (t.tx == tx && t.ty == ty) return t.built;
  return false;
}

// Ground mesh of one z14 tile (dunia3d.ts geoTanah): K×K blocks, cell size by tier (near rails / near
// a near block / far). Each block is textured by the finest RESIDENT satellite layer whose tile contains
// it (z17 > z16 > z14) and appended to the patch mesh of that texture tile, so one draw per texture; a
// block with no imagery yet is keyed to the z14 tile and drawn in the loading colour until it arrives.
// UVs from position over the texture tile; normals from central differences of the carved height field
// (consistent across patch seams); skirts at density seams and z14 tile edges.
void Terrain::buildNearTile(NearTile& nt) {
  const bool wasBuilt = nt.built;
  freeNear(nt);
  const int tx = nt.tx, ty = nt.ty;
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

  // texture source per block: (layer index, tile) of the finest resident detail layer containing the block centre
  struct Build { Key key; MeshBuilder mb; };
  std::vector<Build> patches;
  auto patchFor = [&](double wx, double wy) -> Build& {
    Key k{0, tx, ty};
    for (int li : sat_.detail) {
      const SatLayer& L = sat_.layers[(size_t)li];
      int dx = slippy::worldToTileX(wx, L.zoom), dy = slippy::worldToTileY(wy, L.zoom);
      if (L.has(dx, dy)) { k = {li, dx, dy}; break; }
    }
    for (Build& p : patches) if (p.key == k) return p;
    patches.push_back({k, {}});
    return patches.back();
  };

  for (int bz = 0; bz < K; ++bz)
    for (int bx = 0; bx < K; ++bx) {
      const int b = bz * K + bx, n = cellsOf(b);
      const double x0 = -ts / 2 + bx * bs, z0 = -ts / 2 + bz * bs, cs = bs / n;
      Build& P = patchFor(cx + x0 + bs / 2, cy + z0 + bs / 2);
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
  nt.xf = mat4::translation(origin_.toScene(cx, cy, 0));
  for (Build& P : patches) {
    Patch p{P.key, P.mb.upload(), P.mb.bounds.transformed(nt.xf), (uint32_t)P.mb.vertices.size()};
    nt.patches.push_back(p);
  }
  if (!wasBuilt) markFarDirty(tx, ty);   // a new hole in the far layer
  nt.built = true; nt.dirty = false; nt.dirtyAge = 0;
}

// Far layer: one coarse tile, uncarved DEM, cells aligned with the z14 grid, holes only where a near tile
// is BUILT (dunia3d.ts segarkanLubangJauh: ubinHidup) — the near layer's RANGE covers the whole corridor,
// but only the tiles within R_LOAD ever get a mesh, so punching the range would leave the backdrop showing
// beyond a few km.
int Terrain::farTileGeometry(int tx, int ty, MeshBuilder& mb) const {
  if (sat_.layers.size() < 2) return 0;
  const SatLayer& far = sat_.layers[1];
  const double ts = slippy::tileSizeMeter(far.zoom), ts14 = slippy::tileSizeMeter(TILE_Z);
  const double cx = slippy::tileOriginX(tx, far.zoom) + ts / 2, cy = slippy::tileOriginY(ty, far.zoom) + ts / 2;
  const int N = std::max(1, (int)std::lround(ts / ts14)) * FAR_CELLS_PER_TILE;
  const double bs = ts / N;
  for (int j = 0; j <= N; ++j)
    for (int i = 0; i <= N; ++i) {
      double lx = -ts / 2 + ts * i / N, lz = -ts / 2 + ts * j / N;
      float u = (0.5f + (float)i / N * (far.px - 1)) / far.px, v = (0.5f + (float)j / N * (far.px - 1)) / far.px;
      mb.vertex({(float)lx, dem_.heightScene(cx + lx, cy + lz), (float)lz}, {0, 1, 0}, {u, v});
    }
  int open = 0;
  for (int j = 0; j < N; ++j)
    for (int i = 0; i < N; ++i) {
      double wx = cx - ts / 2 + (i + 0.5) * bs, wy = cy - ts / 2 + (j + 0.5) * bs;
      if (nearTileBuilt(slippy::worldToTileX(wx, TILE_Z), slippy::worldToTileY(wy, TILE_Z))) continue;
      uint32_t a = (uint32_t)(j * (N + 1) + i);
      mb.triangle(a, a + N + 1, a + 1); mb.triangle(a + 1, a + N + 1, a + N + 2);
      ++open;
    }
  return open;
}

void Terrain::buildFarTile(FarTile& ft) {
  const SatLayer& far = sat_.layers[1];
  const double ts = slippy::tileSizeMeter(far.zoom);
  const double cx = slippy::tileOriginX(ft.tx, far.zoom) + ts / 2, cy = slippy::tileOriginY(ft.ty, far.zoom) + ts / 2;
  MeshBuilder mb;
  int open = farTileGeometry(ft.tx, ft.ty, mb);
  rhi::destroyMesh(ft.mesh); ft.mesh = {}; ft.verts = 0;
  ft.xf = mat4::translation(origin_.toScene(cx, cy, 0));
  if (open > 0) { mb.computeSmoothNormals(); ft.mesh = mb.upload(); ft.bounds = mb.bounds.transformed(ft.xf); ft.verts = (uint32_t)mb.vertices.size(); }
  ft.built = true; ft.dirty = false;
}

void Terrain::build() {
  auto t0 = std::chrono::steady_clock::now();
  destroy();
  stats.vertices = stats.triangles = 0; stats.nearTiles = stats.farTiles = stats.patches = stats.detailPatches = 0;
  // backdrop plane far below everything; without terrain it IS the ground (flat, just under the rail head)
  float y = hasTerrain() ? std::min(-12.f, dem_.rawMin - dem_.demBase - 30) : PLATEAU_OFFSET;
  const float s = BACKDROP_SIZE / 2;
  MeshBuilder mb;
  mb.quad({-s, y, s}, {s, y, s}, {s, y, -s}, {-s, y, -s});
  backdrop_ = {mb.upload(), mat4::identity()};
  // the far layer's meshes exist for good (textured when the imagery arrives)
  if (sat_.layers.size() > 1) {
    const SatLayer& farL = sat_.layers[1];
    for (int ty = farL.ty0; ty < farL.ty0 + farL.ny; ++ty)
      for (int tx = farL.tx0; tx < farL.tx0 + farL.nx; ++tx) far_.push_back({tx, ty, {}, {}, mat4::identity(), false, false, 0});
  }
  built_ = true; firstCheck_ = true; checkTimer_ = 0;
  stats.buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// ------------------------------------------------------------------ streaming
void Terrain::noteImagery() { ++imageryVersion_; }

// A detail tile arrived / left: the z14 tile containing it is re-cut (its blocks change texture source).
void Terrain::markDirty(int layer, int tx, int ty) {
  if (layer <= 1) return;
  int shift = sat_.layers[(size_t)layer].zoom - TILE_Z;
  if (shift < 0) return;
  int qx = tx >> shift, qy = ty >> shift;
  for (NearTile& t : near_) if (t.tx == qx && t.ty == qy && t.built) { t.dirty = true; t.dirtyAge = 0; }
}

void Terrain::markFarDirty(int tx14, int ty14) {
  if (sat_.layers.size() < 2) return;
  int shift = TILE_Z - sat_.layers[1].zoom;
  if (shift < 0) return;
  int fx = tx14 >> shift, fy = ty14 >> shift;
  for (FarTile& f : far_) if (f.tx == fx && f.ty == fy && f.built) f.dirty = true;
}

void Terrain::issue(int layer, int tx, int ty) {
  SatLayer& s = sat_.layers[(size_t)layer];
  int t = s.index(tx, ty);
  s.state[(size_t)t] = SatLayer::Requested;
  ++stats.requested;
  const auto& st = sat_.store[(size_t)layer];
  auto it = st.find(t);
  if (it != st.end()) {   // in-memory source (monolithic file)
    Image im; im.width = im.height = s.px; im.channels = 4; im.pixels.resize((size_t)s.px * s.px * 4);
    const std::vector<uint8_t>& rgb = it->second;
    for (size_t k = 0; k < (size_t)s.px * s.px; ++k) { std::memcpy(&im.pixels[k * 4], &rgb[k * 3], 3); im.pixels[k * 4 + 3] = 255; }
    std::string err; provideSat(layer, s.zoom, tx, ty, std::move(im), err);
    return;
  }
  if (request_) request_({"sat/" + std::to_string(layer), s.zoom, tx, ty});
  else s.state[(size_t)t] = SatLayer::Failed;
}

void Terrain::evict(int layer, int tx, int ty) {
  SatLayer& s = sat_.layers[(size_t)layer];
  int t = s.index(tx, ty);
  auto it = s.res.find(t);
  if (it != s.res.end()) { rhi::destroyTexture(it->second.tex); s.res.erase(it); }
  s.state[(size_t)t] = SatLayer::Absent;
  markDirty(layer, tx, ty);
  noteImagery();
}

// One streaming check (ubinStream.ts pilihUbin): decide the near tiles alive, evict imagery beyond the
// radii, request what is missing (nearest first, REQUESTS_PER_CHECK unless unthrottled).
void Terrain::streamCheck(double wx, double wy, bool unthrottled) {
  if (sat_.layers.empty()) return;
  const SatLayer& nearL = sat_.layers[0];
  // near tiles: alive within R_LOAD (edge distance), dropped beyond R_EVICT
  for (size_t i = 0; i < near_.size();) {
    NearTile& t = near_[i];
    if (nearL.edgeDistance(t.tx, t.ty, wx, wy) > R_EVICT) { if (t.built) markFarDirty(t.tx, t.ty); freeNear(t); near_[i] = near_.back(); near_.pop_back(); } else ++i;
  }
  {
    int c0 = slippy::worldToTileX(wx, TILE_Z), r0 = slippy::worldToTileY(wy, TILE_Z), rr = (int)std::ceil(R_LOAD / nearL.ts) + 1;
    for (int ty = r0 - rr; ty <= r0 + rr; ++ty)
      for (int tx = c0 - rr; tx <= c0 + rr; ++tx) {
        if (!nearL.inside(tx, ty) || nearL.edgeDistance(tx, ty, wx, wy) > R_LOAD) continue;
        bool have = false;
        for (const NearTile& t : near_) if (t.tx == tx && t.ty == ty) { have = true; break; }
        if (!have) near_.push_back({tx, ty, {}, mat4::identity(), false, false, 0});
      }
  }
  // imagery: evictions, then candidates
  struct Cand { int layer, tx, ty; double d; };
  std::vector<Cand> cands;
  for (size_t li = 0; li < sat_.layers.size(); ++li) {
    SatLayer& L = sat_.layers[li];
    bool forever = L.inR <= 0;
    if (!forever)
      for (int j = 0; j < L.ny; ++j)
        for (int i = 0; i < L.nx; ++i) {
          size_t t = (size_t)j * L.nx + i;
          if (L.state[t] == SatLayer::Absent || L.state[t] == SatLayer::Requested) continue;
          if (L.edgeDistance(L.tx0 + i, L.ty0 + j, wx, wy) > L.outR) evict((int)li, L.tx0 + i, L.ty0 + j);
        }
    int c0 = slippy::worldToTileX(wx, L.zoom), r0 = slippy::worldToTileY(wy, L.zoom);
    int rr = forever ? std::max(L.nx, L.ny) : (int)std::ceil(L.inR / L.ts) + 1;
    for (int ty = r0 - rr; ty <= r0 + rr; ++ty)
      for (int tx = c0 - rr; tx <= c0 + rr; ++tx) {
        if (!L.inside(tx, ty)) continue;
        size_t t = (size_t)L.index(tx, ty);
        if (!L.indexed[t] || L.state[t] != SatLayer::Absent) continue;
        double d = L.edgeDistance(tx, ty, wx, wy);
        if (!forever && d > L.inR) continue;
        cands.push_back({(int)li, tx, ty, d});
      }
  }
  // the far layer (resident for good, <= 40 tiles) is requested eagerly: it is the only ground beyond R_LOAD
  size_t n = 0;
  for (size_t i = 0; i < cands.size(); ++i) {
    const Cand& c = cands[i];
    if (sat_.layers[(size_t)c.layer].inR <= 0) issue(c.layer, c.tx, c.ty); else cands[n++] = c;
  }
  cands.resize(n);
  std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.d != b.d ? a.d < b.d : a.layer < b.layer; });
  int budget = unthrottled ? (int)cands.size() : REQUESTS_PER_CHECK;
  for (int i = 0; i < budget && i < (int)cands.size(); ++i) issue(cands[(size_t)i].layer, cands[(size_t)i].tx, cands[(size_t)i].ty);
}

void Terrain::checkStreaming(vec3 centre, bool unthrottled) {
  if (sat_.layers.empty()) return;
  double wx, wy; origin_.toWorld(centre, wx, wy);
  streamCheck(wx, wy, unthrottled);
  firstCheck_ = false; checkTimer_ = 0;
}

// GPU work under a budget: near tile builds / re-cuts, far tile builds, texture uploads — nearest first.
void Terrain::runJobs(double wx, double wy, int budget) {
  const bool immediate = budget < 0;
  std::vector<Job> jobs;
  const SatLayer& nearL = sat_.layers[0];
  for (size_t i = 0; i < near_.size(); ++i) {
    const NearTile& t = near_[i];
    if (!t.built || (t.dirty && (immediate || t.dirtyAge >= RECUT_DELAY))) jobs.push_back({0, 0, t.tx, t.ty, nearL.edgeDistance(t.tx, t.ty, wx, wy) + (t.built ? 1 : 0)});
  }
  if (sat_.layers.size() > 1)
    for (const FarTile& f : far_) if (!f.built || f.dirty) jobs.push_back({1, 1, f.tx, f.ty, sat_.layers[1].edgeDistance(f.tx, f.ty, wx, wy)});
  for (size_t li = 0; li < sat_.layers.size(); ++li) {
    const SatLayer& L = sat_.layers[li];
    for (const auto& [t, r] : L.res) if (!r.uploaded) jobs.push_back({2, (int)li, L.tx0 + t % L.nx, L.ty0 + t / L.nx, L.edgeDistance(L.tx0 + t % L.nx, L.ty0 + t / L.nx, wx, wy)});
  }
  std::sort(jobs.begin(), jobs.end(), [](const Job& a, const Job& b) { return a.dist < b.dist; });
  stats.pendingJobs = (int)jobs.size();
  int n = immediate ? (int)jobs.size() : std::min(budget, (int)jobs.size());
  for (int i = 0; i < n; ++i) {
    const Job& j = jobs[(size_t)i];
    if (j.kind == 0) { for (NearTile& t : near_) if (t.tx == j.tx && t.ty == j.ty) { buildNearTile(t); break; } }
    else if (j.kind == 1) { for (FarTile& f : far_) if (f.tx == j.tx && f.ty == j.ty) { buildFarTile(f); break; } }
    else {
      SatLayer& L = sat_.layers[(size_t)j.layer];
      auto it = L.res.find(L.index(j.tx, j.ty));
      if (it == L.res.end()) continue;
      SatLayer::Res& r = it->second;
      r.tex = uploadImage(r.image); r.uploaded = true;
      r.image = {};   // pixels stay in the rgb copy
    }
  }
  stats.pendingJobs -= n;
  // live counts
  stats.nearTiles = 0; stats.patches = stats.detailPatches = 0; stats.vertices = stats.triangles = 0; stats.resident = 0; stats.farTiles = 0;
  for (const NearTile& t : near_) { if (!t.built) continue; ++stats.nearTiles; for (const Patch& p : t.patches) { ++stats.patches; if (p.key.layer != 0) ++stats.detailPatches; stats.triangles += p.mesh.indexCount / 3; stats.vertices += p.verts; } }
  for (const FarTile& f : far_) if (f.built && f.mesh.indexCount) { ++stats.farTiles; stats.triangles += f.mesh.indexCount / 3; stats.vertices += f.verts; }
  for (const SatLayer& L : sat_.layers) stats.resident += L.residentCount();
}

void Terrain::update(vec3 centre, float dt) {
  if (!built_ || sat_.layers.empty()) return;
  double wx, wy; origin_.toWorld(centre, wx, wy);
  checkTimer_ += dt;
  for (NearTile& t : near_) if (t.dirty) t.dirtyAge += dt;
  if (firstCheck_ || checkTimer_ >= CHECK_INTERVAL) { streamCheck(wx, wy, false); checkTimer_ = 0; firstCheck_ = false; }
  runJobs(wx, wy, GPU_JOBS_PER_FRAME);
}

void Terrain::prime(vec3 centre) {
  if (!built_ || sat_.layers.empty()) return;
  double wx, wy; origin_.toWorld(centre, wx, wy);
  streamCheck(wx, wy, true);
  firstCheck_ = false; checkTimer_ = 0;
  runJobs(wx, wy, -1);
}

void Terrain::draw(ModelRenderer& r, const Frustum* frustum) {
  if (!built_) return;
  r.drawMesh(backdrop_.mesh, backdropMat_, {}, backdrop_.xf);
  for (FarTile& f : far_) {
    if (!f.built || !f.mesh.indexCount) continue;
    if (frustum && !frustum->contains(f.bounds)) { ++r.culled; continue; }
    const rhi::Texture* tex = textureOf({1, f.tx, f.ty});
    r.drawMesh(f.mesh, tex ? ground_ : loadingMat_, tex ? *tex : rhi::Texture{}, f.xf);   // 0x1a2027 until the imagery arrives (WARNA_UBIN_MUAT)
  }
  for (NearTile& t : near_) {
    if (!t.built) continue;
    for (Patch& p : t.patches) {
      if (frustum && !frustum->contains(p.bounds)) { ++r.culled; continue; }
      const rhi::Texture* tex = textureOf(p.key);
      r.drawMesh(p.mesh, tex ? ground_ : loadingMat_, tex ? *tex : rhi::Texture{}, t.xf);
    }
  }
}

void Terrain::destroy() {
  for (NearTile& t : near_) freeNear(t);
  for (FarTile& f : far_) { rhi::destroyMesh(f.mesh); f = {}; }
  for (SatLayer& L : sat_.layers) for (auto& [t, r] : L.res) rhi::destroyTexture(r.tex);
  for (SatLayer& L : sat_.layers) { L.res.clear(); std::fill(L.state.begin(), L.state.end(), (uint8_t)SatLayer::Absent); }
  if (built_) rhi::destroyMesh(backdrop_.mesh);
  backdrop_ = {};
  near_.clear(); far_.clear(); built_ = false;
}

} // namespace eng

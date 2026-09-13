// Offline texture encoding shared by tools/convert and tools/fetch_tiles (never part of the runtime).
// Builds the EMOD v5 image variants from RGBA8 pixels: a mip chain (stb_image_resize, sRGB-aware) encoded
// per level as ETC2 (own ETC1 + EAC-alpha encoder: ETC1 blocks are valid ETC2_RGB blocks) or BC1/BC3
// (stb_dxt), plus an optional small RGBA8 fallback for GPUs without the format.
//
// Include after stb_image_resize2.h (with its implementation) and define TEXCOMP_IMPLEMENTATION in one TU.
#pragma once
#include "engine/asset/model.h"
#include <cstdint>
#include <string>
#include <vector>

namespace texcomp {

enum class Target { Desktop, Web, Android };
inline Target parseTarget(const std::string& s, bool& ok) {
  ok = true;
  if (s == "desktop") return Target::Desktop;
  if (s == "web") return Target::Web;
  if (s == "android") return Target::Android;
  ok = false; return Target::Desktop;
}
// Compressed format the target uses for an image with / without alpha
inline eng::TexFormat formatFor(Target t, bool alpha) {
  return t == Target::Desktop ? (alpha ? eng::TexFormat::BC3 : eng::TexFormat::BC1) : (alpha ? eng::TexFormat::ETC2_RGBA : eng::TexFormat::ETC2_RGB);
}
inline int defaultFallback(Target t) { return t == Target::Web ? 256 : 0; }   // longest side of the RGBA8 fallback; 0 = none

struct Options { Target target = Target::Desktop; int fallback = -1; bool highQuality = true; };

bool hasAlpha(const uint8_t* rgba, int w, int h);
// Full chain from w x h RGBA8 down to 1x1 (sRGB filtering unless linear).
std::vector<eng::MipLevel> buildMips(const uint8_t* rgba, int w, int h, bool linear);
// Encodes every level of an RGBA8 chain into `format` (ETC2_RGB/ETC2_RGBA/BC1/BC3).
std::vector<eng::MipLevel> encodeChain(const std::vector<eng::MipLevel>& rgba, eng::TexFormat format, bool highQuality);
// The variants for an image: [compressed chain, RGBA8 fallback ≤ opt.fallback px] (fallback -1 = target default).
std::vector<eng::ImageVariant> buildVariants(const uint8_t* rgba, int w, int h, bool linear, const Options& opt);
// Adds the fallback variant (if any) to an already compressed chain (e.g. transcoded from KTX2).
void appendFallback(std::vector<eng::ImageVariant>& out, const uint8_t* rgba, int w, int h, bool linear, const Options& opt);
size_t variantBytes(const std::vector<eng::ImageVariant>& v);

} // namespace texcomp

#ifdef TEXCOMP_IMPLEMENTATION
#define STB_DXT_IMPLEMENTATION
#include "tools/third_party/stb_dxt.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace texcomp {

bool hasAlpha(const uint8_t* rgba, int w, int h) {
  for (size_t i = 3, n = (size_t)w * h * 4; i < n; i += 4) if (rgba[i] != 255) return true;
  return false;
}

std::vector<eng::MipLevel> buildMips(const uint8_t* rgba, int w, int h, bool linear) {
  std::vector<eng::MipLevel> out;
  out.push_back({w, h, std::vector<uint8_t>(rgba, rgba + (size_t)w * h * 4)});
  while (w > 1 || h > 1) {
    int nw = std::max(1, w / 2), nh = std::max(1, h / 2);
    eng::MipLevel l{nw, nh, std::vector<uint8_t>((size_t)nw * nh * 4)};
    const eng::MipLevel& p = out.back();
    if (linear) stbir_resize_uint8_linear(p.data.data(), w, h, 0, l.data.data(), nw, nh, 0, STBIR_RGBA);
    else stbir_resize_uint8_srgb(p.data.data(), w, h, 0, l.data.data(), nw, nh, 0, STBIR_RGBA);
    out.push_back(std::move(l));
    w = nw; h = nh;
  }
  return out;
}

// ---- ETC1 (= ETC2 RGB base modes) -------------------------------------------------------------------
static const int ETC1_MOD[8][2] = {{2, 8}, {5, 17}, {9, 29}, {13, 42}, {18, 60}, {24, 80}, {33, 106}, {47, 183}};
static inline int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
static inline int q5(int v) { return (v * 31 + 127) / 255; }
static inline int e5(int q) { return (q << 3) | (q >> 2); }
static inline int q4(int v) { return (v * 15 + 127) / 255; }
static inline int e4(int q) { return q * 17; }

// Error of a 2x4 / 4x2 subblock for a base colour and table; fills 2-bit indices (pixel order x*4+y).
static long subblockError(const int px[16][3], const int* idx8, const int base[3], int table, int outIdx[8]) {
  long err = 0;
  for (int k = 0; k < 8; ++k) {
    const int* c = px[idx8[k]];
    int mods[4] = {ETC1_MOD[table][0], ETC1_MOD[table][1], -ETC1_MOD[table][0], -ETC1_MOD[table][1]};
    int best = 0; long bestE = 1L << 60;
    for (int m = 0; m < 4; ++m) {
      long e = 0;
      for (int ch = 0; ch < 3; ++ch) { int d = clamp255(base[ch] + mods[m]) - c[ch]; e += d * d; }
      if (e < bestE) { bestE = e; best = m; }
    }
    outIdx[k] = best; err += bestE;
  }
  return err;
}

// Encodes one 4x4 RGBA block (row-major rgba[16*4]) to 8 bytes of ETC1.
static void encodeEtc1Block(const uint8_t* rgba, uint8_t out[8]) {
  int px[16][3];   // pixel order x*4+y (ETC bit order)
  for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) for (int c = 0; c < 3; ++c) px[x * 4 + y][c] = rgba[(y * 4 + x) * 4 + c];
  long bestErr = 1L << 62; uint64_t bestBits = 0;
  for (int flip = 0; flip < 2; ++flip) {
    int idxA[8], idxB[8];   // subblock pixel lists: flip 0 = left/right 2x4 columns, flip 1 = top/bottom 4x2 rows
    for (int k = 0; k < 8; ++k) {
      if (!flip) { idxA[k] = k; idxB[k] = 8 + k; }                                  // x 0-1 then x 2-3
      else { int x = k / 2, y = k % 2; idxA[k] = x * 4 + y; idxB[k] = x * 4 + y + 2; }   // y 0-1 then y 2-3
    }
    int avgA[3] = {0, 0, 0}, avgB[3] = {0, 0, 0};
    for (int k = 0; k < 8; ++k) for (int c = 0; c < 3; ++c) { avgA[c] += px[idxA[k]][c]; avgB[c] += px[idxB[k]][c]; }
    for (int c = 0; c < 3; ++c) { avgA[c] = (avgA[c] + 4) / 8; avgB[c] = (avgB[c] + 4) / 8; }
    // differential (5-bit + 3-bit delta) when representable, else individual 4-bit
    int qa[3], qb[3], baseA[3], baseB[3]; bool diff = true;
    for (int c = 0; c < 3; ++c) { qa[c] = q5(avgA[c]); qb[c] = q5(avgB[c]); int d = qb[c] - qa[c]; if (d < -4 || d > 3) diff = false; }
    if (diff) for (int c = 0; c < 3; ++c) { baseA[c] = e5(qa[c]); baseB[c] = e5(qb[c]); }
    else for (int c = 0; c < 3; ++c) { qa[c] = q4(avgA[c]); qb[c] = q4(avgB[c]); baseA[c] = e4(qa[c]); baseB[c] = e4(qb[c]); }
    int bestTA = 0, bestTB = 0, idxOutA[8], idxOutB[8], tmp[8]; long eA = 1L << 60, eB = 1L << 60;
    for (int t = 0; t < 8; ++t) {
      long e = subblockError(px, idxA, baseA, t, tmp); if (e < eA) { eA = e; bestTA = t; std::memcpy(idxOutA, tmp, sizeof tmp); }
      e = subblockError(px, idxB, baseB, t, tmp); if (e < eB) { eB = e; bestTB = t; std::memcpy(idxOutB, tmp, sizeof tmp); }
    }
    if (eA + eB >= bestErr) continue;
    bestErr = eA + eB;
    uint64_t hi = 0;
    if (diff) {
      for (int c = 0; c < 3; ++c) { int d = (qb[c] - qa[c]) & 7; hi |= (uint64_t)qa[c] << (59 - c * 8); hi |= (uint64_t)d << (56 - c * 8); }
      hi |= 1ull << 33;
    } else {
      for (int c = 0; c < 3; ++c) { hi |= (uint64_t)qa[c] << (60 - c * 8); hi |= (uint64_t)qb[c] << (56 - c * 8); }
    }
    hi |= (uint64_t)bestTA << 37; hi |= (uint64_t)bestTB << 34; hi |= (uint64_t)flip << 32;
    uint64_t lo = 0;
    for (int k = 0; k < 8; ++k) {
      int pa = idxA[k], pb = idxB[k];
      lo |= (uint64_t)(idxOutA[k] & 1) << pa; lo |= (uint64_t)(idxOutA[k] >> 1) << (16 + pa);
      lo |= (uint64_t)(idxOutB[k] & 1) << pb; lo |= (uint64_t)(idxOutB[k] >> 1) << (16 + pb);
    }
    bestBits = hi | lo;
  }
  for (int i = 0; i < 8; ++i) out[i] = (uint8_t)(bestBits >> (56 - i * 8));   // big-endian
}

// ---- EAC alpha (the A8 half of ETC2_RGBA) --------------------------------------------------------------
static const int EAC_TABLE[16][8] = {
  {-3, -6, -9, -15, 2, 5, 8, 14}, {-3, -7, -10, -13, 2, 6, 9, 12}, {-2, -5, -8, -13, 1, 4, 7, 12}, {-2, -4, -6, -13, 1, 3, 5, 12},
  {-3, -6, -8, -12, 2, 5, 7, 11}, {-3, -7, -9, -11, 2, 6, 8, 10},  {-4, -7, -8, -11, 3, 6, 7, 10},  {-3, -5, -8, -11, 2, 4, 7, 10},
  {-2, -6, -8, -10, 1, 5, 7, 9},  {-2, -5, -8, -10, 1, 4, 7, 9},   {-2, -4, -8, -10, 1, 3, 7, 9},   {-2, -5, -7, -10, 1, 4, 6, 9},
  {-3, -4, -7, -10, 2, 3, 6, 9},  {-1, -2, -3, -10, 0, 1, 2, 9},   {-4, -6, -8, -9, 3, 5, 7, 8},    {-3, -5, -7, -9, 2, 4, 6, 8}};

static void encodeEacBlock(const uint8_t* rgba, uint8_t out[8]) {
  int a[16]; int lo = 255, hi = 0;
  for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) { a[x * 4 + y] = rgba[(y * 4 + x) * 4 + 3]; lo = std::min(lo, a[x * 4 + y]); hi = std::max(hi, a[x * 4 + y]); }
  if (lo == hi) {   // flat block: base = value, table 13 index 4 (modifier 0) for every pixel
    out[0] = (uint8_t)lo; out[1] = (1 << 4) | 13;
    uint64_t bits = 0; for (int k = 0; k < 16; ++k) bits |= 4ull << (45 - k * 3);
    for (int i = 0; i < 6; ++i) out[2 + i] = (uint8_t)(bits >> (40 - i * 8));
    return;
  }
  long bestErr = 1L << 60; int bestBase = 0, bestMult = 1, bestTab = 0, bestIdx[16] = {};
  int mean = 0; for (int v : a) mean += v; mean = (mean + 8) / 16;
  for (int tab = 0; tab < 16; ++tab) {
    int range = std::max(-EAC_TABLE[tab][3], EAC_TABLE[tab][7]);
    for (int mult = std::max(1, (hi - lo) / (2 * range)); mult <= std::min(15, (hi - lo) / range + 2); ++mult)
      for (int base = std::max(0, mean - 8); base <= std::min(255, mean + 8); base += 2) {
        long e = 0; int idx[16];
        for (int k = 0; k < 16; ++k) {
          int bi = 0, be = 1 << 20;
          for (int i = 0; i < 8; ++i) { int d = clamp255(base + mult * EAC_TABLE[tab][i]) - a[k]; d *= d; if (d < be) { be = d; bi = i; } }
          idx[k] = bi; e += be;
          if (e >= bestErr) break;
        }
        if (e < bestErr) { bestErr = e; bestBase = base; bestMult = mult; bestTab = tab; std::memcpy(bestIdx, idx, sizeof idx); }
      }
  }
  out[0] = (uint8_t)bestBase; out[1] = (uint8_t)((bestMult << 4) | bestTab);
  uint64_t bits = 0;
  for (int k = 0; k < 16; ++k) bits |= (uint64_t)bestIdx[k] << (45 - k * 3);
  for (int i = 0; i < 6; ++i) out[2 + i] = (uint8_t)(bits >> (40 - i * 8));
}

static void extractBlock(const eng::MipLevel& l, int bx, int by, uint8_t blk[64]) {
  for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) {
    int sx = std::min(bx * 4 + x, l.width - 1), sy = std::min(by * 4 + y, l.height - 1);   // clamp-pad partial blocks
    std::memcpy(blk + (y * 4 + x) * 4, &l.data[((size_t)sy * l.width + sx) * 4], 4);
  }
}

std::vector<eng::MipLevel> encodeChain(const std::vector<eng::MipLevel>& rgba, eng::TexFormat format, bool hq) {
  std::vector<eng::MipLevel> out;
  for (const eng::MipLevel& l : rgba) {
    eng::MipLevel o{l.width, l.height, std::vector<uint8_t>(eng::texLevelBytes(format, l.width, l.height))};
    int nbx = (l.width + 3) / 4, nby = (l.height + 3) / 4;
    uint8_t blk[64];
    for (int by = 0; by < nby; ++by)
      for (int bx = 0; bx < nbx; ++bx) {
        extractBlock(l, bx, by, blk);
        uint8_t* dst = &o.data[((size_t)by * nbx + bx) * (format == eng::TexFormat::ETC2_RGB || format == eng::TexFormat::BC1 ? 8 : 16)];
        switch (format) {
          case eng::TexFormat::ETC2_RGB: encodeEtc1Block(blk, dst); break;
          case eng::TexFormat::ETC2_RGBA: encodeEacBlock(blk, dst); encodeEtc1Block(blk, dst + 8); break;
          case eng::TexFormat::BC1: stb_compress_dxt_block(dst, blk, 0, hq ? STB_DXT_HIGHQUAL : STB_DXT_NORMAL); break;
          case eng::TexFormat::BC3: stb_compress_dxt_block(dst, blk, 1, hq ? STB_DXT_HIGHQUAL : STB_DXT_NORMAL); break;
          default: break;
        }
      }
    out.push_back(std::move(o));
  }
  return out;
}

void appendFallback(std::vector<eng::ImageVariant>& out, const uint8_t* rgba, int w, int h, bool linear, const Options& opt) {
  int fb = opt.fallback < 0 ? defaultFallback(opt.target) : opt.fallback;
  if (fb <= 0) return;
  int fw = w, fh = h;
  while (fw > fb || fh > fb) { fw = std::max(1, fw / 2); fh = std::max(1, fh / 2); }
  eng::ImageVariant v; v.format = eng::TexFormat::RGBA8;
  eng::MipLevel l{fw, fh, std::vector<uint8_t>((size_t)fw * fh * 4)};
  if (fw == w && fh == h) std::memcpy(l.data.data(), rgba, l.data.size());
  else if (linear) stbir_resize_uint8_linear(rgba, w, h, 0, l.data.data(), fw, fh, 0, STBIR_RGBA);
  else stbir_resize_uint8_srgb(rgba, w, h, 0, l.data.data(), fw, fh, 0, STBIR_RGBA);
  v.mips.push_back(std::move(l));
  out.push_back(std::move(v));
}

std::vector<eng::ImageVariant> buildVariants(const uint8_t* rgba, int w, int h, bool linear, const Options& opt) {
  std::vector<eng::ImageVariant> out;
  eng::ImageVariant v; v.format = formatFor(opt.target, hasAlpha(rgba, w, h));
  v.mips = encodeChain(buildMips(rgba, w, h, linear), v.format, opt.highQuality);
  out.push_back(std::move(v));
  appendFallback(out, rgba, w, h, linear, opt);
  return out;
}

size_t variantBytes(const std::vector<eng::ImageVariant>& vs) {
  size_t n = 0; for (const auto& v : vs) for (const auto& m : v.mips) n += m.data.size(); return n;
}

} // namespace texcomp
#endif // TEXCOMP_IMPLEMENTATION

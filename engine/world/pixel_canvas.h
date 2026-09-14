// CPU-side RGBA8 canvas + the bitmap font (assets/font.efnt) rasterised into it — the engine's stand-in for
// the reference client's HTMLCanvas textures (uji3dSinyal.ts, mejaLayan3d.ts): signal plates, the number
// panel strips, the meja board. Row 0 = top (upload as-is: v = 0 samples the top row). Header-only.
#pragma once
#include "engine/math/math.h"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace eng {

struct BitmapFont {
  struct Glyph { uint16_t x0, y0, x1, y1; float xoff, yoff, xadvance; };
  std::map<uint32_t, Glyph> glyphs; std::vector<uint8_t> px; int w = 0, h = 0;
  float pixelHeight = 28, ascent = 22, descent = -5;
  bool loaded() const { return !px.empty(); }
  bool load(const std::string& path) {
    std::ifstream f(path, std::ios::binary); if (!f) return false;
    auto get = [&](auto& v) { f.read((char*)&v, sizeof v); };
    char magic[4]; f.read(magic, 4); uint32_t ver; get(ver);
    if (std::memcmp(magic, "EFNT", 4) != 0 || ver != 1) return false;
    uint16_t aw, ah; get(aw); get(ah); w = aw; h = ah; float lineGap;
    get(pixelHeight); get(ascent); get(descent); get(lineGap);
    uint32_t n; get(n);
    for (uint32_t i = 0; i < n; ++i) { uint32_t cp; Glyph g; get(cp); get(g.x0); get(g.y0); get(g.x1); get(g.y1); get(g.xoff); get(g.yoff); get(g.xadvance); glyphs[cp] = g; }
    px.resize((size_t)w * h); f.read((char*)px.data(), (std::streamsize)px.size());
    return (bool)f;
  }
  float measure(const std::string& s, float size) const {
    float scale = size / pixelHeight, x = 0;
    for (unsigned char c : s) if (auto it = glyphs.find(c); it != glyphs.end()) x += it->second.xadvance * scale;
    return x;
  }
  float sample(float x, float y) const {   // bilinear coverage 0..1
    int x0 = (int)std::floor(x), y0 = (int)std::floor(y); float fx = x - (float)x0, fy = y - (float)y0;
    auto at = [&](int i, int j) { return i < 0 || j < 0 || i >= w || j >= h ? 0.f : (float)px[(size_t)j * w + i] / 255.f; };
    return (at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx) * (1 - fy) + (at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx) * fy;
  }
};

class PixelCanvas {
public:
  int w = 0, h = 0; std::vector<uint8_t> px;
  PixelCanvas() = default;
  PixelCanvas(int width, int height) { reset(width, height); }
  void reset(int width, int height) { w = width; h = height; px.assign((size_t)w * h * 4, 0); }
  static vec4 rgb(uint32_t c, float a = 1) { return {(float)((c >> 16) & 255) / 255.f, (float)((c >> 8) & 255) / 255.f, (float)(c & 255) / 255.f, a}; }

  // source-over blend of `c` (alpha in c.w) at pixel (i, j)
  void blend(int i, int j, vec4 c) {
    if (i < 0 || j < 0 || i >= w || j >= h || c.w <= 0) return;
    uint8_t* o = &px[((size_t)j * w + i) * 4];
    float a = std::fmin(1.f, c.w), da = (float)o[3] / 255.f, oa = a + da * (1 - a);
    for (int k = 0; k < 3; ++k) o[k] = (uint8_t)std::lround(((&c.x)[k] * 255 * a + (float)o[k] * da * (1 - a)) / (oa > 0 ? oa : 1));
    o[3] = (uint8_t)std::lround(oa * 255);
  }
  void fill(uint32_t c, float a = 1) { rect(0, 0, (float)w, (float)h, c, a); }
  void rect(float x0, float y0, float rw, float rh, uint32_t c, float a = 1) {
    vec4 col = rgb(c, a);
    for (int j = std::max(0, (int)std::floor(y0)); j < std::min(h, (int)std::ceil(y0 + rh)); ++j)
      for (int i = std::max(0, (int)std::floor(x0)); i < std::min(w, (int)std::ceil(x0 + rw)); ++i) blend(i, j, col);
  }
  void strokeRect(float x0, float y0, float rw, float rh, float lw, uint32_t c, float a = 1) {
    rect(x0, y0, rw, lw, c, a); rect(x0, y0 + rh - lw, rw, lw, c, a); rect(x0, y0, lw, rh, c, a); rect(x0 + rw - lw, y0, lw, rh, c, a);
  }
  void circle(float cx, float cy, float r, uint32_t c, float a = 1) {
    vec4 col = rgb(c, a);
    for (int j = std::max(0, (int)std::floor(cy - r - 1)); j <= std::min(h - 1, (int)std::ceil(cy + r + 1)); ++j)
      for (int i = std::max(0, (int)std::floor(cx - r - 1)); i <= std::min(w - 1, (int)std::ceil(cx + r + 1)); ++i) {
        float d = std::hypot((float)i + 0.5f - cx, (float)j + 0.5f - cy) - r;
        float cov = std::fmax(0.f, std::fmin(1.f, 0.5f - d));   // 1 px anti-aliased edge
        if (cov > 0) blend(i, j, {col.x, col.y, col.z, col.w * cov});
      }
  }
  void ring(float cx, float cy, float r, float lw, uint32_t c, float a = 1) {
    vec4 col = rgb(c, a);
    for (int j = std::max(0, (int)std::floor(cy - r - lw)); j <= std::min(h - 1, (int)std::ceil(cy + r + lw)); ++j)
      for (int i = std::max(0, (int)std::floor(cx - r - lw)); i <= std::min(w - 1, (int)std::ceil(cx + r + lw)); ++i) {
        float d = std::fabs(std::hypot((float)i + 0.5f - cx, (float)j + 0.5f - cy) - r) - lw / 2;
        float cov = std::fmax(0.f, std::fmin(1.f, 0.5f - d));
        if (cov > 0) blend(i, j, {col.x, col.y, col.z, col.w * cov});
      }
  }
  // Anti-aliased line with round caps (capsule) of width lw.
  void line(float x0, float y0, float x1, float y1, float lw, uint32_t c, float a = 1) {
    vec4 col = rgb(c, a); float r = lw / 2;
    float dx = x1 - x0, dy = y1 - y0, l2 = dx * dx + dy * dy;
    for (int j = std::max(0, (int)std::floor(std::fmin(y0, y1) - r - 1)); j <= std::min(h - 1, (int)std::ceil(std::fmax(y0, y1) + r + 1)); ++j)
      for (int i = std::max(0, (int)std::floor(std::fmin(x0, x1) - r - 1)); i <= std::min(w - 1, (int)std::ceil(std::fmax(x0, x1) + r + 1)); ++i) {
        float qx = (float)i + 0.5f - x0, qy = (float)j + 0.5f - y0;
        float t = l2 > 0 ? std::fmax(0.f, std::fmin(1.f, (qx * dx + qy * dy) / l2)) : 0;
        float d = std::hypot(qx - dx * t, qy - dy * t) - r;
        float cov = std::fmax(0.f, std::fmin(1.f, 0.5f - d));
        if (cov > 0) blend(i, j, {col.x, col.y, col.z, col.w * cov});
      }
  }
  // Rounded rectangle centred at (cx, cy), `len` along the direction `ang` (radians), `thick` across, radius = thick/2.
  void capsule(float cx, float cy, float ang, float len, float thick, uint32_t c, float a = 1) {
    float hx = std::cos(ang) * (len - thick) / 2, hy = std::sin(ang) * (len - thick) / 2;
    line(cx - hx, cy - hy, cx + hx, cy + hy, thick, c, a);
  }
  // Vertical gradient: alpha a0 at y0 -> a1 at y1 (colour c), across the whole width.
  void gradientV(float y0, float y1, uint32_t c, float a0, float a1) {
    float lo = std::fmin(y0, y1), hi = std::fmax(y0, y1);
    for (int j = std::max(0, (int)std::floor(lo)); j < std::min(h, (int)std::ceil(hi)); ++j) {
      float t = y1 == y0 ? 0 : ((float)j + 0.5f - y0) / (y1 - y0);
      vec4 col = rgb(c, a0 + (a1 - a0) * std::fmax(0.f, std::fmin(1.f, t)));
      for (int i = 0; i < w; ++i) blend(i, j, col);
    }
  }
  // Text centred at (cx, cy), `size` px tall; `bold` adds a half-pixel second tap.
  void text(const BitmapFont& f, const std::string& s, float cx, float cy, float size, uint32_t c, bool bold = true, float a = 1) {
    if (!f.loaded()) return;
    float scale = size / f.pixelHeight, x = cx - f.measure(s, size) / 2, baseline = cy + (f.ascent + f.descent) / 2 * scale;
    vec4 col = rgb(c, a);
    for (unsigned char ch : s) {
      auto it = f.glyphs.find(ch); if (it == f.glyphs.end()) continue;
      const BitmapFont::Glyph& g = it->second;
      float gx0 = x + g.xoff * scale, gy0 = baseline + g.yoff * scale, gw = (float)(g.x1 - g.x0), gh = (float)(g.y1 - g.y0);
      for (int j = (int)std::floor(gy0); j < (int)std::ceil(gy0 + gh * scale); ++j)
        for (int i = (int)std::floor(gx0); i < (int)std::ceil(gx0 + gw * scale); ++i) {
          float u = (float)g.x0 + ((float)i + 0.5f - gx0) / scale - 0.5f, v = (float)g.y0 + ((float)j + 0.5f - gy0) / scale - 0.5f;
          float cov = f.sample(u, v); if (bold) cov = std::fmin(1.f, cov + f.sample(u - 0.5f / scale, v) * 0.6f);
          if (cov > 0) blend(i, j, {col.x, col.y, col.z, col.w * cov});
        }
      x += g.xadvance * scale;
    }
  }
  void textLeft(const BitmapFont& f, const std::string& s, float x, float cy, float size, uint32_t c, bool bold = true, float a = 1) {
    text(f, s, x + f.measure(s, size) / 2, cy, size, c, bold, a);
  }
};

} // namespace eng

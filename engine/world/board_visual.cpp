#include "engine/world/board_visual.h"
#include "engine/render/font_bytes.h"
#include "engine/render/mesh_builder.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>

namespace eng {

namespace {
using namespace boards;

vec4 rgb(uint32_t c) { return {(float)((c >> 16) & 255) / 255.f, (float)((c >> 8) & 255) / 255.f, (float)(c & 255) / 255.f, 1}; }

// ---- triangle bin for one material (Tampung) ----
struct Bin {
  MeshBuilder mb;
  // Quad CCW seen from `n`; corners bl, br, tr, tl; uv rect (u0,v0 top-left .. u1,v1 bottom-right).
  void quad(vec3 bl, vec3 br, vec3 tr, vec3 tl, vec3 n, float u0 = 0, float v0 = 0, float u1 = 0, float v1 = 0) {
    uint32_t i = mb.vertex(bl, n, {u0, v1});
    mb.vertex(br, n, {u1, v1}); mb.vertex(tr, n, {u1, v0}); mb.vertex(tl, n, {u0, v0});
    mb.quad(i, i + 1, i + 2, i + 3);
  }
  // Box face: centre c + n (|n| = half depth), half-edges u, v; winding fixed so the face looks along n.
  void face(vec3 c, vec3 n, vec3 u, vec3 v) {
    if (dot(cross(u, v), n) < 0) std::swap(u, v);
    vec3 p = c + n;
    quad(p - u - v, p + u - v, p + u + v, p - u + v, normalize(n));
  }
  // Parallelepiped: centre + three half-edges (may be skewed).
  void box(vec3 c, vec3 a, vec3 b, vec3 k) {
    face(c, a, b, k); face(c, a * -1.f, k, b);
    face(c, b, k, a); face(c, b * -1.f, a, k);
    face(c, k, a, b); face(c, k * -1.f, b, a);
  }
};

// ---- bitmap font (assets/font.efnt) rasterised on the CPU for the board atlas ----
struct Font {
  struct Glyph { uint16_t x0, y0, x1, y1; float xoff, yoff, xadvance; };
  std::map<uint32_t, Glyph> glyphs; std::vector<uint8_t> px; int w = 0, h = 0;
  float pixelHeight = 28, ascent = 22, descent = -5;
  bool load(const std::string& path) {
    std::vector<uint8_t> buf;
    if (!readFontFile(path, buf)) return false;
    size_t at = 0; bool ok = true;
    auto take = [&](void* dst, size_t n) { if (at + n > buf.size()) { ok = false; return; } std::memcpy(dst, buf.data() + at, n); at += n; };
    auto get = [&](auto& v) { take(&v, sizeof v); };
    char magic[4]; take(magic, 4); uint32_t ver = 0; get(ver);
    if (!ok || std::memcmp(magic, "EFNT", 4) != 0 || ver != 1) return false;
    uint16_t aw = 0, ah = 0; get(aw); get(ah); w = aw; h = ah; float lineGap = 0;
    get(pixelHeight); get(ascent); get(descent); get(lineGap);
    uint32_t n = 0; get(n);
    for (uint32_t i = 0; i < n && ok; ++i) { uint32_t cp = 0; Glyph g; get(cp); get(g.x0); get(g.y0); get(g.x1); get(g.y1); get(g.xoff); get(g.yoff); get(g.xadvance); glyphs[cp] = g; }
    px.resize((size_t)w * h); take(px.data(), px.size());
    return ok;
  }
  float measure(const std::string& s, float scale) const { float x = 0; for (unsigned char c : s) if (auto it = glyphs.find(c); it != glyphs.end()) x += it->second.xadvance * scale; return x; }
  float sample(float x, float y) const {   // bilinear, coverage 0..1
    int x0 = (int)std::floor(x), y0 = (int)std::floor(y); float fx = x - (float)x0, fy = y - (float)y0;
    auto at = [&](int i, int j) { return i < 0 || j < 0 || i >= w || j >= h ? 0.f : (float)px[(size_t)j * w + i] / 255.f; };
    return (at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx) * (1 - fy) + (at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx) * fy;
  }
  // Draws `s` centred at (cx, cy) with the given pixel size into an RGBA8 buffer (row-major, row 0 top).
  void draw(std::vector<uint8_t>& img, int imgW, int imgH, const std::string& s, float cx, float cy, float size, uint32_t colour) const {
    float scale = size / pixelHeight, x = cx - measure(s, scale) / 2, baseline = cy + (ascent + descent) / 2 * scale;
    vec4 c = rgb(colour);
    for (unsigned char ch : s) {
      auto it = glyphs.find(ch); if (it == glyphs.end()) continue;
      const Glyph& g = it->second;
      float gx0 = x + g.xoff * scale, gy0 = baseline + g.yoff * scale, gw = (float)(g.x1 - g.x0), gh = (float)(g.y1 - g.y0);
      for (int j = (int)std::floor(gy0); j < (int)std::ceil(gy0 + gh * scale); ++j)
        for (int i = (int)std::floor(gx0); i < (int)std::ceil(gx0 + gw * scale); ++i) {
          if (i < 0 || j < 0 || i >= imgW || j >= imgH) continue;
          float u = (float)g.x0 + ((float)i + 0.5f - gx0) / scale - 0.5f, v = (float)g.y0 + ((float)j + 0.5f - gy0) / scale - 0.5f;
          // a second tap half a pixel right = cheap bold
          float a = std::fmin(1.f, sample(u, v) + sample(u - 0.5f / scale, v) * 0.6f);
          if (a <= 0) continue;
          uint8_t* o = &img[((size_t)j * imgW + i) * 4];
          for (int k = 0; k < 3; ++k) o[k] = (uint8_t)std::lround(o[k] * (1 - a) + (&c.x)[k] * 255 * a);
        }
      x += g.xadvance * scale;
    }
  }
};

struct Style { uint32_t bg, frame, ink; };
Style styleOf(BoardSpec::Kind k) {
  switch (k) {
    case BoardSpec::S35: return {0x151515, 0xf4f4f4, 0xf4f4f4};
    case BoardSpec::Taspat: return {0xe8b23b, 0x1a1a1a, 0x1a1a1a};
    default: return {0xf4f6f8, 0x1a1a1a, 0x1a1a1a};
  }
}

void fillRect(std::vector<uint8_t>& img, int W, int x0, int y0, int w, int h, uint32_t colour) {
  for (int j = y0; j < y0 + h; ++j) for (int i = x0; i < x0 + w; ++i) {
    uint8_t* o = &img[((size_t)j * W + i) * 4]; o[0] = (uint8_t)(colour >> 16); o[1] = (uint8_t)(colour >> 8); o[2] = (uint8_t)colour; o[3] = 255;
  }
}

struct Slot { float u0, v0, u1, v1; int px, py; };
} // namespace

void TracksideBoards::build(const TrackGraph& g, const RailProfile& profile, const Json& world, const WorldOrigin& origin,
                            const GroundFn& ground, const std::string& fontPath) {
  destroy();
  boards_.clear(); stats = {};
  struct Mark { vec3 p; float tx, tz; };
  struct Buffer { vec3 p; float hx, hz; };
  struct Platform { vec3 p; float rot, w, h; };
  std::vector<Mark> marks; std::vector<Buffer> buffers; std::vector<Platform> platforms;

  for (const Json& o : world["trackside"].arr) {
    std::string kind = o["kind"].stringOr("");
    if (kind != "s35" && kind != "speed" && kind != "stopmark" && kind != "buffer") continue;
    std::string segId = o["segId"].stringOr(""); int seg = g.segIndex(segId); if (seg < 0) continue;
    double s = o["s"].numberOr(0);
    TrackSample sm = g.sampleAt(seg, s);
    float dir = o["dir"].intOr(1) == -1 ? -1.f : 1.f;
    float tx = (float)sm.tx * dir, tz = (float)sm.ty * dir;   // direction of the trains this board applies to
    vec3 p = origin.toScene(sm.wx, sm.wy, profile.railHeight(segId.c_str(), s));
    if (kind == "stopmark") { marks.push_back({p, tx, tz}); continue; }
    if (kind == "buffer") { buffers.push_back({p, -tx, -tz}); continue; }
    float side = o["sisi"].stringOr("kanan") == "kiri" ? -1.f : 1.f;
    float nx = -tz * side, nz = tx * side;
    BoardSpec b; b.foot = p + vec3{nx * SIDE_OFFSET, 0, nz * SIDE_OFFSET}; b.hx = -tx; b.hz = -tz;
    if (kind == "s35") { b.kind = BoardSpec::S35; b.text = "S35"; }
    else { b.kind = BoardSpec::Taspat; b.text = o["speed"].isNumber() ? std::to_string(o["speed"].intOr(0)) : "?"; }
    boards_.push_back(std::move(b));
  }
  for (const Json& o : world["scenery"].arr) {
    std::string kind = o["kind"].stringOr("");
    double wx = o["pos"]["x"].numberOr(0), wy = o["pos"]["y"].numberOr(0);
    float rot = (float)o["rot"].numberOr(0);
    if (kind == "kmpost") {
      BoardSpec b; b.kind = BoardSpec::KmPost; b.text = o["label"].stringOr(""); if (b.text.empty()) b.text = "KM";
      b.foot = origin.toScene(wx, wy, ground ? ground(wx, wy) : 0); b.hx = std::cos(rot); b.hz = std::sin(rot);
      boards_.push_back(std::move(b));
    } else if (kind == "platform") {
      platforms.push_back({origin.toScene(wx, wy, ground ? ground(wx, wy) : 0), rot, (float)o["w"].numberOr(120), (float)o["h"].numberOr(6)});
    }
  }

  // ---- text atlas: one 256x64 cell per distinct (kind, text) ----
  std::map<std::string, Slot> slots;
  for (const BoardSpec& b : boards_) {
    std::string key = std::to_string((int)b.kind) + ":" + b.text;
    if (slots.count(key)) continue;
    int i = (int)slots.size();
    slots[key] = {0, 0, 0, 0, (i % COLS) * CELL_W, (i / COLS) * CELL_H};
  }
  int rows = std::max(1, ((int)slots.size() + COLS - 1) / COLS), atlasH = 64;
  while (atlasH < rows * CELL_H) atlasH *= 2;
  for (auto& [k, s] : slots) { s.u0 = (float)s.px / ATLAS_W; s.u1 = (float)(s.px + CELL_W) / ATLAS_W; s.v0 = (float)s.py / atlasH; s.v1 = (float)(s.py + CELL_H) / atlasH; }
  {
    Font font; bool haveFont = font.load(fontPath);
    std::vector<uint8_t> img((size_t)ATLAS_W * atlasH * 4);
    fillRect(img, ATLAS_W, 0, 0, ATLAS_W, atlasH, 0x333333);
    const int PAD = 5;
    for (const auto& [key, s] : slots) {
      Style st = styleOf((BoardSpec::Kind)std::atoi(key.c_str()));
      std::string text = key.substr(key.find(':') + 1);
      fillRect(img, ATLAS_W, s.px + PAD, s.py + PAD, CELL_W - PAD * 2, CELL_H - PAD * 2, st.bg);
      // 5 px frame inset 3 px
      int fx = s.px + PAD + 3, fy = s.py + PAD + 3, fw = CELL_W - PAD * 2 - 6, fh = CELL_H - PAD * 2 - 6;
      fillRect(img, ATLAS_W, fx - 2, fy - 2, fw + 4, 5, st.frame); fillRect(img, ATLAS_W, fx - 2, fy + fh - 3, fw + 4, 5, st.frame);
      fillRect(img, ATLAS_W, fx - 2, fy - 2, 5, fh + 4, st.frame); fillRect(img, ATLAS_W, fx + fw - 3, fy - 2, 5, fh + 4, st.frame);
      if (!haveFont) continue;
      float size = 34; const float maxW = CELL_W - PAD * 2 - 22;
      while (size > 12 && font.measure(text, size / font.pixelHeight) > maxW) size -= 2;
      font.draw(img, ATLAS_W, atlasH, text, (float)s.px + CELL_W / 2.f, (float)s.py + CELL_H / 2.f + 1, size, st.ink);
    }
    if (const char* dump = std::getenv("ENG_BOARD_ATLAS")) {   // debug: write the atlas as a PPM
      if (FILE* fp = std::fopen(dump, "wb")) { std::fprintf(fp, "P6\n%d %d\n255\n", ATLAS_W, atlasH); for (size_t i = 0; i < img.size(); i += 4) std::fwrite(&img[i], 1, 3, fp); std::fclose(fp); }
    }
    atlas_ = rhi::createTexture(ATLAS_W, atlasH, rhi::Format::RGBA8, std::as_bytes(std::span(img)), true, true);
  }

  // ---- geometry ----
  Bin face, dark, white, red, grey;
  const vec3 UP{0, 1, 0};
  for (const BoardSpec& b : boards_) {
    vec3 n{b.hx, 0, b.hz}, right{b.hz, 0, -b.hx};   // right of the reader = up x n
    bool km = b.kind == BoardSpec::KmPost;
    float w = km ? KM_W : b.kind == BoardSpec::S35 ? S35_W : TASPAT_W;
    float h = km ? KM_H : b.kind == BoardSpec::S35 ? S35_H : TASPAT_H;
    float cy = km ? KM_PLATE_Y : POLE - h / 2;
    vec3 c = b.foot + vec3{0, cy, 0};
    const Slot& s = slots[std::to_string((int)b.kind) + ":" + b.text];
    auto sheet = [&](Bin& bin, vec3 facing, vec3 rt, bool textured) {
      vec3 p = c + facing * (PLATE_THICK / 2);
      if (textured) bin.quad(p - rt * (w / 2) - UP * (h / 2), p + rt * (w / 2) - UP * (h / 2), p + rt * (w / 2) + UP * (h / 2), p - rt * (w / 2) + UP * (h / 2), facing, s.u0, s.v0, s.u1, s.v1);
      else bin.quad(p - rt * (w / 2) - UP * (h / 2), p + rt * (w / 2) - UP * (h / 2), p + rt * (w / 2) + UP * (h / 2), p - rt * (w / 2) + UP * (h / 2), facing);
    };
    sheet(face, n, right, true);
    if (km) sheet(face, n * -1.f, right * -1.f, true);     // two-faced km post
    else sheet(dark, n * -1.f, right * -1.f, false);
    if (km) white.box(b.foot + vec3{0, KM_POST_H / 2, 0}, {KM_POST_SIDE / 2, 0, 0}, {0, KM_POST_H / 2, 0}, {0, 0, KM_POST_SIDE / 2});
    else dark.box(b.foot + vec3{-n.x * 0.05f, POLE / 2, -n.z * 0.05f}, {POLE_SIDE / 2, 0, 0}, {0, POLE / 2, 0}, {0, 0, POLE_SIDE / 2});
  }
  for (const Mark& m : marks) {   // 10G: white stripe across the track axis
    vec3 u{-m.tz, 0, m.tx};
    white.box(m.p + vec3{0, MARK_T / 2 + 0.01f, 0}, u * (MARK_SPAN / 2), {0, MARK_T / 2, 0}, {m.tx * MARK_W / 2, 0, m.tz * MARK_W / 2});
  }
  for (const Buffer& b : buffers) {   // buffer stop: red beam + two raked legs
    vec3 t{-b.hx, 0, -b.hz}, u{-t.z, 0, t.x};
    red.box(b.p + vec3{0, BUF_BEAM_Y, 0}, u * (BUF_SPAN / 2), {0, BUF_BEAM / 2, 0}, t * (BUF_BEAM / 2));
    for (float side : {-1.f, 1.f}) {
      vec3 top = b.p + vec3{0, BUF_BEAM_Y, 0} + u * (side * BUF_SPAN * 0.33f);
      vec3 bottom = top + t * BUF_BACK - vec3{0, BUF_BEAM_Y, 0};
      vec3 d = top - bottom; float half = length(d) / 2; d = normalize(d);
      vec3 sidev = normalize(cross(d, UP)), upv = normalize(cross(sidev, d));
      dark.box((top + bottom) * 0.5f, d * half, sidev * (BUF_LEG / 2), upv * (BUF_LEG / 2));
    }
  }
  for (const Platform& p : platforms) {   // scenery `platform` box, 0.95 m high (T_PERON)
    vec3 ax{std::cos(p.rot) * p.w / 2, 0, std::sin(p.rot) * p.w / 2}, az{-std::sin(p.rot) * p.h / 2, 0, std::cos(p.rot) * p.h / 2};
    grey.box(p.p + vec3{0, 0.475f, 0}, ax, {0, 0.475f, 0}, az);
  }

  auto upload = [&](Bin& bin, Batch& out) {
    if (bin.mb.empty()) return;
    out.mesh = bin.mb.upload(); out.bounds = bin.mb.bounds; out.used = true;
    stats.tris += (unsigned)(bin.mb.indices.size() / 3); ++stats.drawCalls;
  };
  upload(face, face_); upload(dark, dark_); upload(white, white_); upload(red, red_); upload(grey, grey_);
  faceMat_ = {}; faceMat_.baseColor = {1, 1, 1, 1}; faceMat_.metallic = 0; faceMat_.roughness = 0.85f;
  darkMat_ = {}; darkMat_.baseColor = rgb(0x5c646d); darkMat_.metallic = 0.3f; darkMat_.roughness = 0.7f;
  whiteMat_ = {}; whiteMat_.baseColor = rgb(0xf2f3f5); whiteMat_.metallic = 0; whiteMat_.roughness = 0.9f;
  redMat_ = {}; redMat_.baseColor = rgb(0xc22f2f); redMat_.metallic = 0; redMat_.roughness = 0.8f;
  greyMat_ = {}; greyMat_.baseColor = rgb(0xb9bfc6); greyMat_.metallic = 0; greyMat_.roughness = 0.95f;
  stats.boards = boards_.size(); stats.marks = marks.size(); stats.buffers = buffers.size();
}

void TracksideBoards::draw(ModelRenderer& r, const Frustum* frustum) const {
  auto drawBatch = [&](const Batch& b, const Material& m, rhi::Texture tex) {
    if (!b.used || (frustum && !frustum->contains(b.bounds))) return;
    r.drawMesh(b.mesh, m, tex, mat4::identity());
  };
  drawBatch(face_, faceMat_, atlas_); drawBatch(dark_, darkMat_, {}); drawBatch(white_, whiteMat_, {});
  drawBatch(red_, redMat_, {}); drawBatch(grey_, greyMat_, {});
}

void TracksideBoards::destroy() {
  for (Batch* b : {&face_, &dark_, &white_, &red_, &grey_}) if (b->used) { rhi::destroyMesh(b->mesh); b->used = false; }
  if (atlas_.id) { rhi::destroyTexture(atlas_); atlas_ = {}; }
}

} // namespace eng

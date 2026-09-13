#include "engine/render/text.h"
#include <cstring>
#include <fstream>

namespace eng {

static const char* VS = R"(
layout(location=0) in vec2 aPos;
layout(location=1) in vec2 aUV;
layout(location=2) in vec4 aColor;
uniform vec2 uScreen;
out vec2 vUV; out vec4 vColor;
void main() {
  vUV = aUV; vColor = aColor;
  gl_Position = vec4(aPos.x / uScreen.x * 2.0 - 1.0, 1.0 - aPos.y / uScreen.y * 2.0, 0.0, 1.0);
})";
static const char* FS = R"(
in vec2 vUV; in vec4 vColor;
uniform sampler2D uAtlas;
out vec4 oColor;
void main() {
  float a = vUV.x < 0.0 ? 1.0 : texture(uAtlas, vUV).r;   // uv.x < 0 marks a solid rect
  oColor = vec4(vColor.rgb, vColor.a * a);
})";

bool TextRenderer::load(const std::string& path, std::string& err) {
  std::ifstream f(path, std::ios::binary);
  if (!f) { err = "cannot open " + path; return false; }
  auto get = [&](auto& v) { f.read((char*)&v, sizeof v); };
  char magic[4]; f.read(magic, 4); uint32_t ver; get(ver);
  if (std::memcmp(magic, "EFNT", 4) != 0 || ver != 1) { err = "bad EFNT"; return false; }
  uint16_t w, h; get(w); get(h); atlasW_ = w; atlasH_ = h;
  get(pixelHeight_); get(ascent_); get(descent_); get(lineGap_);
  uint32_t n; get(n);
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t cp; uint16_t x0, y0, x1, y1; Glyph g; get(cp); get(x0); get(y0); get(x1); get(y1); get(g.xoff); get(g.yoff); get(g.xadvance);
    g.x0 = x0; g.y0 = y0; g.x1 = x1; g.y1 = y1; glyphs_[cp] = g;
  }
  std::vector<uint8_t> atlas((size_t)w * h); f.read((char*)atlas.data(), (std::streamsize)atlas.size());
  if (!f) { err = "truncated EFNT"; return false; }
  atlas_ = rhi::createTexture(w, h, rhi::Format::R8, std::as_bytes(std::span(atlas)), false);
  prog_ = rhi::createProgram(VS, FS);
  uScreen_ = rhi::uniformLocation(prog_, "uScreen");
  rhi::useProgram(prog_); rhi::setUniform(rhi::uniformLocation(prog_, "uAtlas"), 0);
  return true;
}

void TextRenderer::shutdown() { rhi::destroyTexture(atlas_); rhi::destroyProgram(prog_); }

float TextRenderer::measure(std::string_view text, float scale) const {
  float x = 0;
  for (unsigned char c : text) { auto it = glyphs_.find(c); if (it != glyphs_.end()) x += it->second.xadvance * scale; }
  return x;
}

void TextRenderer::draw(std::string_view text, float x, float y, vec4 col, float scale) {
  float cx = x, baseline = y + ascent_ * scale;
  for (unsigned char c : text) {
    if (c == '\n') { cx = x; baseline += lineHeight(scale); continue; }
    auto it = glyphs_.find(c); if (it == glyphs_.end()) continue;
    const Glyph& g = it->second;
    float x0 = cx + g.xoff * scale, y0 = baseline + g.yoff * scale;
    float x1 = x0 + (g.x1 - g.x0) * scale, y1 = y0 + (g.y1 - g.y0) * scale;
    float u0 = g.x0 / atlasW_, v0 = g.y0 / atlasH_, u1 = g.x1 / atlasW_, v1 = g.y1 / atlasH_;
    uint32_t b = (uint32_t)verts_.size();
    verts_.push_back({x0, y0, u0, v0, col.x, col.y, col.z, col.w});
    verts_.push_back({x1, y0, u1, v0, col.x, col.y, col.z, col.w});
    verts_.push_back({x1, y1, u1, v1, col.x, col.y, col.z, col.w});
    verts_.push_back({x0, y1, u0, v1, col.x, col.y, col.z, col.w});
    for (uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) idx_.push_back(b + i);
    cx += g.xadvance * scale;
  }
}

void TextRenderer::rect(float x, float y, float w, float h, vec4 col) {
  uint32_t b = (uint32_t)verts_.size();
  verts_.push_back({x, y, -1, 0, col.x, col.y, col.z, col.w});
  verts_.push_back({x + w, y, -1, 0, col.x, col.y, col.z, col.w});
  verts_.push_back({x + w, y + h, -1, 0, col.x, col.y, col.z, col.w});
  verts_.push_back({x, y + h, -1, 0, col.x, col.y, col.z, col.w});
  for (uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) idx_.push_back(b + i);
}

void TextRenderer::flush(int sw, int sh) {
  if (idx_.empty()) return;
  const rhi::Attribute layout[] = {{0, 2, sizeof(V), 0}, {1, 2, sizeof(V), 8}, {2, 4, sizeof(V), 16}};
  rhi::Mesh m = rhi::createMesh(std::as_bytes(std::span(verts_)), layout, idx_);   // rebuilt per frame; small
  rhi::useProgram(prog_);
  rhi::setUniform(uScreen_, (float)sw, (float)sh);
  rhi::bindTexture(0, atlas_);
  rhi::setBlend(true); rhi::setDepthWrite(false); rhi::setCullFace(false);
  rhi::drawMesh(m);
  rhi::setBlend(false); rhi::setDepthWrite(true); rhi::setCullFace(true);
  rhi::destroyMesh(m);
  verts_.clear(); idx_.clear();
}

} // namespace eng

#include "engine/world/name_board.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace eng {

namespace {
constexpr unsigned LATAR = 0x15181b, HURUF = 0xffffff;   // uji3dPapanNama.ts LATAR / HURUF

std::string squeeze(const std::string& s) {   // trim, collapse whitespace, upper-case (ASCII)
  std::string o; bool space = false;
  for (unsigned char c : s) {
    if (std::isspace(c)) { space = !o.empty(); continue; }
    if (space) { o += ' '; space = false; }
    o += (char)std::toupper(c);
  }
  return o;
}
bool contains(const std::string& s, const char* what) { return s.find(what) != std::string::npos; }
} // namespace

std::string NameBoards::normName(const std::string& s) {
  std::string t = squeeze(s);
  if (t.empty()) return "";
  bool prefixed = t.rfind("STASIUN", 0) == 0 && (t.size() == 7 || !std::isalnum((unsigned char)t[7]));
  return prefixed ? t : "STASIUN " + t;
}

std::string NameBoards::normHeight(const std::string& s) {
  std::string t = squeeze(s);
  if (t.empty()) return "";
  // ^([+-]?)\s*(\d+(?:[.,]\d+)?)\s*M?$  ->  "+ 21 M" / "- 3 M"; anything else is kept as typed
  size_t i = 0; char sign = '+';
  if (t[i] == '+' || t[i] == '-') { sign = t[i]; ++i; }
  while (i < t.size() && t[i] == ' ') ++i;
  size_t d0 = i; bool dot = false;
  while (i < t.size() && (std::isdigit((unsigned char)t[i]) || ((t[i] == '.' || t[i] == ',') && !dot && i > d0 && i + 1 < t.size() && std::isdigit((unsigned char)t[i + 1])))) { if (t[i] == '.' || t[i] == ',') dot = true; ++i; }
  if (i == d0) return t;
  std::string digits = t.substr(d0, i - d0);
  while (i < t.size() && t[i] == ' ') ++i;
  if (i < t.size() && t[i] == 'M') ++i;
  if (i != t.size()) return t;
  return std::string(1, sign) + " " + digits + " M";
}

bool NameBoards::hasBoard(const GpuModel& m) {
  for (const Node& n : m.nodes) {
    if (n.mesh < 0 || n.mesh >= (int)m.meshes.size()) continue;
    if (contains(n.name, NAME)) return true;
    for (const GpuPrimitive& p : m.meshes[(size_t)n.mesh].primitives)
      if (p.material >= 0 && p.material < (int)m.materials.size() && contains(m.materials[(size_t)p.material].name, NAME)) return true;
  }
  return false;
}

rhi::Texture NameBoards::texture(const std::string& name, const std::string& height, bool mirrored) {
  std::string key = name + "|" + height + (mirrored ? "|m" : "");
  auto it = cache_.find(key); if (it != cache_.end()) return it->second;
  canvas_.reset(TEX_W, TEX_H);
  canvas_.fill(LATAR);
  const float pad = 26;   // 52 at the reference's 2048x256
  float right = TEX_W - pad;
  if (!height.empty()) {
    float size = std::round(TEX_H * 0.40f);
    float w = font_.measure(height, size);
    canvas_.textLeft(font_, height, TEX_W - pad - w, TEX_H / 2.f + 1, size, HURUF);
    right = TEX_W - pad - w - 40;
  }
  if (!name.empty()) {
    float size = std::round(TEX_H * 0.62f), maxW = right - pad;
    while (size > 22 && font_.measure(name, size) > maxW) size -= 2;   // shrink, never squash (reference)
    canvas_.textLeft(font_, name, pad, TEX_H / 2.f + 1, size, HURUF);
  }
  if (mirrored) for (int y = 0; y < TEX_H; ++y) for (int x = 0; x < TEX_W / 2; ++x) {
    uint8_t* a = &canvas_.px[((size_t)y * TEX_W + (size_t)x) * 4]; uint8_t* c = &canvas_.px[((size_t)y * TEX_W + (size_t)(TEX_W - 1 - x)) * 4];
    for (int k = 0; k < 4; ++k) std::swap(a[k], c[k]);
  }
  if (const char* dump = std::getenv("ENG_PAPAN_DUMP")) {   // debug: the painted board as a PPM
    if (FILE* fp = std::fopen(dump, "wb")) { std::fprintf(fp, "P6\n%d %d\n255\n", TEX_W, TEX_H); for (size_t i = 0; i < canvas_.px.size(); i += 4) std::fwrite(&canvas_.px[i], 1, 3, fp); std::fclose(fp); }
  }
  rhi::Texture t = rhi::createTexture(TEX_W, TEX_H, rhi::Format::RGBA8, std::as_bytes(std::span(canvas_.px)), true, true, rhi::Wrap::Clamp, rhi::Wrap::Clamp);
  cache_[key] = t;
  return t;
}

void NameBoards::scan(const std::vector<Placed>& placed) {
  boards_.clear();
  for (const Placed& p : placed) {
    std::string name = normName(p.teks), height = normHeight(p.ketinggian);
    if (name.empty() && height.empty()) continue;   // plain board: the model's own material stays
    const GpuModel& m = *p.model;
    for (size_t ni = 0; ni < m.nodes.size(); ++ni) {
      const Node& n = m.nodes[ni];
      if (n.mesh < 0 || n.mesh >= (int)m.meshes.size()) continue;
      for (const GpuPrimitive& prim : m.meshes[(size_t)n.mesh].primitives) {
        bool byMat = prim.material >= 0 && prim.material < (int)m.materials.size() && contains(m.materials[(size_t)prim.material].name, NAME);
        if (!contains(n.name, NAME) && !byMat) continue;
        Board b; b.mesh = &prim.mesh; b.xf = p.xf * m.world[ni];
        b.centre = b.xf.transformPoint((prim.bounds.min + prim.bounds.max) * 0.5f);
        b.bounds = prim.bounds.transformed(b.xf);
        { vec3 e = prim.bounds.max - prim.bounds.min; vec3 axis = e.x <= e.y && e.x <= e.z ? vec3{1, 0, 0} : e.y <= e.z ? vec3{0, 1, 0} : vec3{0, 0, 1};
          vec3 nw = b.xf.transformPoint(axis) - b.xf.transformPoint({0, 0, 0}); float l = length(nw); b.normal = l > 1e-6f ? nw / l : vec3{0, 1, 0}; }
        bool mirrored;
        { const auto& w = b.xf.m; mirrored = w[0][0] * (w[1][1] * w[2][2] - w[1][2] * w[2][1]) - w[1][0] * (w[0][1] * w[2][2] - w[0][2] * w[2][1]) + w[2][0] * (w[0][1] * w[1][2] - w[0][2] * w[1][1]) < 0; }
        b.tex = texture(name, height, mirrored);
        boards_.push_back(b);
      }
    }
  }
  mat_ = {}; mat_.name = NAME; mat_.baseColor = {1, 1, 1, 1}; mat_.unlit = true; mat_.doubleSided = true;
}

void NameBoards::draw(ModelRenderer& r, vec3 eye, const Frustum* frustum) const {
  for (const Board& b : boards_) {
    if (!b.tex.id || (frustum && !frustum->contains(b.bounds))) continue;
    float side = dot(b.normal, eye - b.centre) >= 0 ? 1.f : -1.f;
    r.drawMesh(*b.mesh, mat_, b.tex, mat4::translation(b.normal * (0.003f * side)) * b.xf);
  }
}

void NameBoards::destroy() {
  boards_.clear();
  for (auto& [k, t] : cache_) if (t.id) rhi::destroyTexture(t);
  cache_.clear();
}

} // namespace eng

#include "engine/world/route_visual.h"
#include "engine/render/mesh_builder.h"
#include <algorithm>
#include <cmath>

namespace eng {

namespace {
Material ribbonMat(uint32_t c, float alpha) {
  Material m; m.name = "ribbon";
  vec3 rgb{(float)((c >> 16) & 255) / 255.f, (float)((c >> 8) & 255) / 255.f, (float)(c & 255) / 255.f};
  m.baseColor = {rgb.x * 0.2f, rgb.y * 0.2f, rgb.z * 0.2f, alpha};
  m.emissive = rgb;            // reads the same by day and by night, like MeshBasicMaterial
  m.metallic = 0; m.roughness = 1; m.alphaMode = AlphaMode::Blend; m.doubleSided = true;
  return m;
}
} // namespace

rhi::Mesh RouteVisuals::buildRibbon(const std::vector<Span>& spans, float lift) const {
  MeshBuilder b;
  const WorldOrigin& o = graph_->origin();
  for (const Span& sp : spans) {
    int seg = graph_->segIndex(sp.seg);
    if (seg < 0) continue;
    double L = graph_->length(seg);
    double s0 = std::clamp(sp.a, 0.0, L), s1 = sp.b < 0 ? L : std::clamp(sp.b, 0.0, L);
    if (s1 < s0) std::swap(s0, s1);
    if (s1 - s0 < 0.5) continue;
    int n = std::max(1, (int)std::ceil((s1 - s0) / SAMPLE_STEP));
    uint32_t first = (uint32_t)b.vertices.size();
    for (int i = 0; i <= n; ++i) {
      double s = s0 + (s1 - s0) * (double)i / n;
      TrackSample sm = graph_->sampleAt(seg, s);
      float y = profile_->railHeight(sp.seg.c_str(), s) + lift;
      vec3 p = o.toScene(sm.wx, sm.wy, y);
      vec3 side{(float)-sm.ty * WIDTH / 2, 0, (float)sm.tx * WIDTH / 2};
      float v = (float)i / (float)n;
      b.vertex(p + side, {0, 1, 0}, {0, v});
      b.vertex(p - side, {0, 1, 0}, {1, v});
    }
    for (int i = 0; i < n; ++i) {
      uint32_t a = first + (uint32_t)i * 2;
      b.triangle(a, a + 2, a + 1); b.triangle(a + 1, a + 2, a + 3);
    }
  }
  return b.empty() ? rhi::Mesh{} : b.upload();
}

void RouteVisuals::update(const SimState& st) {
  if (!graph_ || !profile_) return;
  // Fingerprint: route ids with their released counts + occupied segments with rounded intervals.
  std::vector<std::string> keys;
  for (const SimRoute& r : st.routes) keys.push_back(r.id + ":" + std::to_string(r.released.size()));
  for (const SimOccupancy& oc : st.occupancy) {
    std::string k = "|" + oc.seg;
    for (const auto& iv : oc.intervals) k += ";" + std::to_string((int)iv.a) + "-" + std::to_string((int)iv.b);
    keys.push_back(std::move(k));
  }
  std::sort(keys.begin(), keys.end());
  std::string fp;
  for (const std::string& k : keys) { fp += k; fp += ','; }
  if (fp == fingerprint_) return;
  fingerprint_ = fp;
  rhi::destroyMesh(route_); rhi::destroyMesh(occupied_);

  std::vector<Span> locked, busy;
  for (const SimRoute& r : st.routes)
    for (const std::string& seg : r.segs)
      if (std::find(r.released.begin(), r.released.end(), seg) == r.released.end()) locked.push_back({seg, 0, -1});
  for (const SimOccupancy& oc : st.occupancy)
    for (const auto& iv : oc.intervals) busy.push_back({oc.seg, iv.a, iv.b});
  if (routeMat_.name.empty()) {
    routeMat_ = ribbonMat(0x1cb0f6, 0.6f);
    occupiedMat_ = ribbonMat(0xff5a36, 0.7f);
    previewMat_ = ribbonMat(0x7fd4ff, 0.6f);
    deadEndMat_ = ribbonMat(0xff4b4b, 0.6f);
  }
  route_ = buildRibbon(locked, LIFT);
  occupied_ = buildRibbon(busy, LIFT + 0.05f);
}

void RouteVisuals::setPreview(const std::vector<std::string>& segs, bool deadEnd) {
  clearPreview();
  if (!graph_ || !profile_) return;
  if (previewMat_.name.empty()) { previewMat_ = ribbonMat(0x7fd4ff, 0.6f); deadEndMat_ = ribbonMat(0xff4b4b, 0.6f); }
  std::vector<Span> spans;
  for (const std::string& s : segs) spans.push_back({s, 0, -1});
  preview_ = buildRibbon(spans, LIFT + 0.10f);
  previewDead_ = deadEnd;
}

void RouteVisuals::clearPreview() { rhi::destroyMesh(preview_); preview_ = {}; }

void RouteVisuals::draw(ModelRenderer& r) const {
  const mat4 I;
  // Ribbons overlap (a locked section that is also occupied must read red, the preview above
  // both): with depth writes off during blending, the draw order is the stacking order.
  if (route_.indexCount) { r.drawMesh(route_, routeMat_, {}, I); r.flushTransparent(); }
  if (occupied_.indexCount) { r.drawMesh(occupied_, occupiedMat_, {}, I); r.flushTransparent(); }
  if (preview_.indexCount) { r.drawMesh(preview_, previewDead_ ? deadEndMat_ : previewMat_, {}, I); r.flushTransparent(); }
}

void RouteVisuals::drawHoverRing(ModelRenderer& r, vec3 base, float scale) {
  if (!ring_.indexCount) {
    MeshBuilder b; const int n = 32; const float r0 = 0.86f, r1 = 1.0f;
    for (int i = 0; i < n; ++i) {
      float a0 = 2 * PI * (float)i / n, a1 = 2 * PI * (float)(i + 1) / n;
      vec3 p00{std::cos(a0) * r0, 0, std::sin(a0) * r0}, p01{std::cos(a0) * r1, 0, std::sin(a0) * r1};
      vec3 p10{std::cos(a1) * r0, 0, std::sin(a1) * r0}, p11{std::cos(a1) * r1, 0, std::sin(a1) * r1};
      b.quad(p00, p01, p11, p10);
    }
    ring_ = b.upload();
    ringMat_ = {}; ringMat_.name = "ring"; ringMat_.baseColor = {1, 1, 1, 1}; ringMat_.emissive = {0.9f, 0.9f, 0.9f};
    ringMat_.metallic = 0; ringMat_.roughness = 1; ringMat_.doubleSided = true;
  }
  r.drawMesh(ring_, ringMat_, {}, mat4::translation(base + vec3{0, 0.05f, 0}) * mat4::scale({scale, 1, scale}));
}

void RouteVisuals::destroy() {
  rhi::destroyMesh(route_); rhi::destroyMesh(occupied_); rhi::destroyMesh(preview_); rhi::destroyMesh(ring_);
  route_ = occupied_ = preview_ = {}; fingerprint_.clear();
}

} // namespace eng

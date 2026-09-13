#include "engine/world/point_visual.h"
#include "engine/render/mesh_builder.h"
#include <cmath>

namespace eng {

namespace {
vec4 rgb(uint32_t c) { return {(float)((c >> 16) & 255) / 255.f, (float)((c >> 8) & 255) / 255.f, (float)(c & 255) / 255.f, 1}; }

// Arrow outline (dunia3dKonst.ts bentukPanah), extruded 0.16 along Z centred on 0.
void arrowMesh(MeshBuilder& b) {
  const vec2 outline[] = {{-0.50f, -0.13f}, {0.04f, -0.13f}, {0.04f, -0.34f}, {0.50f, 0}, {0.04f, 0.34f}, {0.04f, 0.13f}, {-0.50f, 0.13f}};
  const int n = 7; const float hz = 0.08f;
  // faces: shaft rectangle (0,1,5,6) + head triangle (2,3,4) — both convex
  auto face = [&](float z, vec3 nrm, bool flip) {
    uint32_t v[7];
    for (int i = 0; i < n; ++i) v[i] = b.vertex({outline[i].x, outline[i].y, z}, nrm, {0, 0});
    auto tri = [&](int i, int j, int k) { if (flip) b.triangle(v[i], v[k], v[j]); else b.triangle(v[i], v[j], v[k]); };
    tri(0, 1, 5); tri(0, 5, 6); tri(2, 3, 4);
  };
  face(hz, {0, 0, 1}, false); face(-hz, {0, 0, -1}, true);
  for (int i = 0; i < n; ++i) {
    vec2 a = outline[i], c = outline[(i + 1) % n];
    b.quad({a.x, a.y, hz}, {c.x, c.y, hz}, {c.x, c.y, -hz}, {a.x, a.y, -hz});
  }
}

// Direction from the node toward a leg, measured 1..20 m along it (bangun3d.ts arahLeg).
vec3 legDirection(const TrackGraph& g, int node, int seg) {
  const TrackSegment& s = g.segments[(size_t)seg];
  double L = s.length, d = std::min(20.0, std::max(1.0, L * 0.6));
  TrackSample sm = g.sampleAt(seg, s.a == node ? d : L - d);
  const TrackNode& n = g.nodes[(size_t)node];
  vec3 v{(float)(sm.wx - n.wx), 0, (float)(sm.wy - n.wy)};
  return dot(v, v) > 1e-6f ? normalize(v) : vec3{1, 0, 0};
}
} // namespace

void PointVisuals::build(const TrackGraph& g, const RailProfile& profile) {
  destroy();
  MeshBuilder mb; arrowMesh(mb);
  arrow_ = mb.upload(); arrowBounds_ = mb.bounds;
  const uint32_t colours[4] = {0x2fd85a, 0x1ea94a, 0xff3b30, 0x8f342e};
  for (int i = 0; i < 4; ++i) {
    mat_[i] = {}; mat_[i].baseColor = rgb(colours[i]); mat_[i].emissive = rgb(colours[i]).xyz() * 0.8f;
    mat_[i].metallic = 0; mat_[i].roughness = 1; mat_[i].doubleSided = true;
  }
  const WorldOrigin& o = g.origin();
  for (size_t ni = 0; ni < g.nodes.size(); ++ni) {
    const TrackNode& n = g.nodes[ni];
    if (!n.isPoint()) continue;
    PointInstance p; p.nodeId = n.id; p.setting = n.setting;
    const TrackSegment& fs = g.segments[(size_t)n.facingSeg];
    float hy = profile.railHeight(fs.id.c_str(), fs.a == (int)ni ? 0.0 : fs.length);
    p.pos = o.toScene(n.wx, n.wy, hy);
    vec3 forward = -legDirection(g, (int)ni, n.facingSeg);
    vec3 left{forward.z, 0, -forward.x};
    float lat[2]; vec3 dir[2];
    for (int i = 0; i < 2; ++i) { dir[i] = legDirection(g, (int)ni, n.legs[i]); lat[i] = dot(dir[i], left); }
    int dom = std::fabs(lat[0]) >= std::fabs(lat[1]) ? 0 : 1;
    float sideDom = lat[dom] >= 0 ? 1.f : -1.f;
    for (int i = 0; i < 2; ++i) {
      p.legSeg[i] = n.legs[i];
      float side = i == dom ? sideDom : -sideDom;
      vec3 lateral = left * side;
      float yaw = std::atan2(-lateral.z, lateral.x);
      p.arrow[i] = mat4::translation(p.pos + vec3{lateral.x * ARROW_OFFSET, ARROW_HEIGHT, lateral.z * ARROW_OFFSET})
                 * mat4::rotationY(yaw) * mat4::scale({ARROW_SCALE, ARROW_SCALE, ARROW_SCALE});
    }
    points_.push_back(std::move(p));
  }
}

void PointVisuals::setState(const std::string& nodeId, int setting, bool locked) {
  for (PointInstance& p : points_) if (p.nodeId == nodeId) { p.setting = setting == 1 ? 1 : 0; p.locked = locked; return; }
}

void PointVisuals::draw(ModelRenderer& r, const Frustum* frustum) const {
  for (const PointInstance& p : points_)
    for (int i = 0; i < 2; ++i) {
      if (frustum && !frustum->contains(arrowBounds_.transformed(p.arrow[i]))) { ++r.culled; continue; }
      bool set = i == p.setting;
      r.drawMesh(arrow_, mat_[(set ? 0 : 2) + (p.locked ? 1 : 0)], {}, p.arrow[i]);
    }
}

std::vector<ScreenPoint> PointVisuals::screenPositions(const mat4& viewProj, int w, int h) const {
  std::vector<ScreenPoint> out; out.reserve(points_.size());
  for (const PointInstance& p : points_) {
    vec4 c = viewProj * vec4(p.pos + vec3{0, ARROW_HEIGHT, 0}, 1);
    ScreenPoint sp; sp.id = p.nodeId;
    if (c.w > 0) {
      sp.x = (c.x / c.w * 0.5f + 0.5f) * (float)w; sp.y = (1 - (c.y / c.w * 0.5f + 0.5f)) * (float)h;
      sp.visible = sp.x >= 0 && sp.x <= (float)w && sp.y >= 0 && sp.y <= (float)h && c.z / c.w <= 1;
    }
    out.push_back(std::move(sp));
  }
  return out;
}

void PointVisuals::destroy() { points_.clear(); rhi::destroyMesh(arrow_); }

} // namespace eng

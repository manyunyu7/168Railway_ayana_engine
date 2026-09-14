// Markers — see markers.h. Point kinds share five unit meshes (drawn per marker: the colour is a material
// property, the batch is small); line kinds get their own ribbon mesh. Picking / labels are pure projections
// of the stored scene points so they cost nothing per frame.
#include "engine/app/markers.h"
#include "engine/render/mesh_builder.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace eng {

namespace {

vec3 parseColor(const std::string& s, vec3 dflt) {
  if (s.size() != 7 || s[0] != '#') return dflt;
  unsigned v = (unsigned)std::strtoul(s.c_str() + 1, nullptr, 16);
  return {((v >> 16) & 255) / 255.f, ((v >> 8) & 255) / 255.f, (v & 255) / 255.f};
}

Material overlayMat(vec3 c, float a, bool depth) {
  Material m; m.name = "marker"; m.baseColor = {c.x, c.y, c.z, a}; m.emissive = c; m.metallic = 0; m.roughness = 1;
  m.doubleSided = true; m.unlit = true; m.depthTest = depth;
  if (a < 1) m.alphaMode = AlphaMode::Blend;
  return m;
}

// Distance from (px, py) to the segment a-b in screen space.
float segDist(float px, float py, float ax, float ay, float bx, float by) {
  float dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
  float t = l2 > 0 ? std::clamp(((px - ax) * dx + (py - ay) * dy) / l2, 0.f, 1.f) : 0.f;
  return std::hypot(px - (ax + t * dx), py - (ay + t * dy));
}

struct Proj { float x = 0, y = 0, w = 0; bool front = false; };
Proj projectPt(const mat4& vp, vec3 p, int w, int h) {
  Proj r; vec4 c = vp * vec4(p, 1);
  if (c.w <= 0) return r;
  r.front = true; r.w = c.w;
  r.x = (c.x / c.w * 0.5f + 0.5f) * (float)w;
  r.y = (1 - (c.y / c.w * 0.5f + 0.5f)) * (float)h;
  return r;
}

} // namespace

bool Markers::set(const Json& arr, const WorldOrigin& origin, const HeightFn& height) {
  clear();
  if (!arr.isArray()) return false;
  auto scenePt = [&](const Json& j, float naik, int hm) {
    double wx = j["x"].numberOr(0), wy = j["y"].numberOr(0);
    float h = j["h"].type == Json::Type::Number ? (float)j["h"].num : height(wx, wy, hm) + (float)j["naik"].numberOr(naik);
    return origin.toScene(wx, wy, h);
  };
  for (const Json& j : arr.arr) {
    if (!j.isObject()) continue;
    Marker m;
    m.id = j["id"].stringOr("");
    std::string kind = j["kind"].stringOr("sphere");
    m.kind = kind == "disc" ? Kind::Disc : kind == "cube" ? Kind::Cube : kind == "diamond" ? Kind::Diamond
           : kind == "cone" ? Kind::Cone : (kind == "segment" || kind == "polyline" || kind == "line") ? Kind::Line : Kind::Sphere;
    m.color = parseColor(j["color"].stringOr(""), {1, 1, 1});
    m.alpha = (float)std::clamp(j["alpha"].numberOr(1), 0.0, 1.0);
    m.size = (float)std::fmax(0.01, j["size"].numberOr(1));
    m.px = (float)std::fmax(0.0, j["px"].numberOr(0));
    m.yaw = (float)j["yaw"].numberOr(0);
    m.depth = j["depth"].boolOr(false);
    m.label = j["label"].boolOr(false);
    const int hm = j["hm"].intOr(0);
    const float naik = (float)j["naik"].numberOr(0);
    if (m.kind == Kind::Line) {
      for (const Json& p : j["pts"].arr) if (p.isObject()) m.pts.push_back(scenePt(p, naik, p["hm"].type == Json::Type::Number ? p["hm"].intOr(0) : hm));
      if (m.pts.size() < 2) continue;
      MeshBuilder b; const float hw = m.size * 0.5f;
      for (size_t i = 1; i < m.pts.size(); ++i) {   // flat ribbon + vertical fin (reads from every angle)
        vec3 a = m.pts[i - 1], c = m.pts[i]; vec3 d = c - a; d.y = 0; float l = length(d); if (l < 1e-3f) continue;
        vec3 n = vec3{-d.z, 0, d.x} / l * hw;
        b.quad(a - n, a + n, c + n, c - n);
        b.quad(a, a + vec3{0, hw * 2, 0}, c + vec3{0, hw * 2, 0}, c);
      }
      if (b.empty()) continue;
      m.mesh = b.upload();
    } else {
      m.pts.push_back(scenePt(j, naik, hm));
    }
    items_.push_back(std::move(m));
  }
  return true;
}

void Markers::clear() {
  for (Marker& m : items_) if (m.mesh.indexCount) { rhi::destroyMesh(m.mesh); m.mesh = {}; }
  items_.clear();
}

void Markers::destroy() {
  clear();
  if (!built_) return;
  for (rhi::Mesh* m : {&sphere_, &disc_, &cube_, &diamond_, &cone_}) { rhi::destroyMesh(*m); *m = {}; }
  built_ = false;
}

void Markers::buildMeshes() {
  if (built_) return;
  built_ = true;
  {   // unit sphere (radius 1)
    MeshBuilder b; const int R = 8, S = 14;
    for (int r = 0; r < R; ++r) {
      float t0 = PI * (float)r / R - PI / 2, t1 = PI * (float)(r + 1) / R - PI / 2;
      for (int s = 0; s < S; ++s) {
        float a0 = 2 * PI * (float)s / S, a1 = 2 * PI * (float)(s + 1) / S;
        auto P = [](float t, float a) { return vec3{std::cos(t) * std::cos(a), std::sin(t), std::cos(t) * std::sin(a)}; };
        b.quad(P(t0, a0), P(t0, a1), P(t1, a1), P(t1, a0));
      }
    }
    sphere_ = b.upload();
  }
  {   // flat ring on y = 0, outer radius 1, width 0.14 (uji3dSpline cincinGeom proportions)
    MeshBuilder b; const int n = 32; const float r0 = 0.86f, r1 = 1.f;
    for (int i = 0; i < n; ++i) {
      float a0 = 2 * PI * (float)i / n, a1 = 2 * PI * (float)(i + 1) / n;
      b.quad({std::cos(a0) * r0, 0, std::sin(a0) * r0}, {std::cos(a0) * r1, 0, std::sin(a0) * r1}, {std::cos(a1) * r1, 0, std::sin(a1) * r1}, {std::cos(a1) * r0, 0, std::sin(a1) * r0});
    }
    disc_ = b.upload();
  }
  { MeshBuilder b; b.box({-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}); cube_ = b.upload(); }
  {   // octahedron, radius 1 (uji3dObjekRel geoRel), stretched 1.35 vertically like the three marker
    MeshBuilder b; const vec3 up{0, 1.35f, 0}, dn{0, -1.35f, 0};
    const vec3 e[4] = {{1, 0, 0}, {0, 0, 1}, {-1, 0, 0}, {0, 0, -1}};
    for (int i = 0; i < 4; ++i) { const vec3 &a = e[i], &c = e[(i + 1) & 3]; b.quad(a, c, up, up); b.quad(c, a, dn, dn); }
    diamond_ = b.upload();
  }
  {   // square pyramid lying along +X: base at x = -0.5 (0..1 high, ±0.5 wide), tip at x = +0.5 mid-height
    MeshBuilder b; const vec3 tip{0.5f, 0.5f, 0};
    const vec3 c0{-0.5f, 0, -0.5f}, c1{-0.5f, 0, 0.5f}, c2{-0.5f, 1, 0.5f}, c3{-0.5f, 1, -0.5f};
    b.quad(c0, c3, c2, c1);   // base plate
    b.quad(c1, c2, tip, tip); b.quad(c2, c3, tip, tip); b.quad(c3, c0, tip, tip); b.quad(c0, c1, tip, tip);
    cone_ = b.upload();
  }
}

float Markers::worldRadius(const Marker& m, vec3 eye, float fovY, int viewportH) const {
  if (m.px <= 0 || viewportH <= 0) return m.size;
  float dist = std::fmax(0.5f, length(m.pts[0] - eye));
  return m.px * (2 * dist * std::tan(fovY * 0.5f)) / (float)viewportH;
}

vec3 Markers::top(const Marker& m) {
  if (m.kind == Kind::Line) return m.pts[m.pts.size() / 2] + vec3{0, m.size, 0};
  float up = m.kind == Kind::Disc ? 0.f : m.kind == Kind::Cube ? m.size * 0.5f : m.kind == Kind::Cone ? m.size * 0.66f : m.size * 1.35f;
  return m.pts[0] + vec3{0, up, 0};
}

void Markers::draw(ModelRenderer& r, vec3 eye, float fovY, int viewportH) {
  if (items_.empty()) return;
  buildMeshes();
  for (const Marker& m : items_) {
    Material mat = overlayMat(m.color, m.alpha, m.depth);
    if (m.kind == Kind::Line) { if (m.mesh.indexCount) r.drawMesh(m.mesh, mat, {}, mat4::identity()); continue; }
    const float s = worldRadius(m, eye, fovY, viewportH);
    mat4 xf = mat4::translation(m.pts[0]) * mat4::rotationY(m.yaw);
    switch (m.kind) {
      case Kind::Sphere: r.drawMesh(sphere_, mat, {}, xf * mat4::scale({s, s, s})); break;
      case Kind::Disc: r.drawMesh(disc_, mat, {}, xf * mat4::scale({s, 1, s})); break;
      case Kind::Cube: r.drawMesh(cube_, mat, {}, xf * mat4::scale({s, s, s})); break;
      case Kind::Diamond: r.drawMesh(diamond_, mat, {}, xf * mat4::scale({s, s, s})); break;
      case Kind::Cone: r.drawMesh(cone_, mat, {}, xf * mat4::scale({s, s * 0.66f, s * 0.47f})); break;   // 1.9 : 1.25 : 0.9 like KERUCUT_*
      case Kind::Line: break;
    }
  }
}

std::string Markers::pick(float px, float py, int w, int h, const mat4& viewProj, vec3 eye, float maxPx) const {
  const Marker* best = nullptr; float bd = maxPx, bdist = 1e30f;
  for (const Marker& m : items_) {
    float d;
    if (m.kind == Kind::Line) {
      d = 1e30f;
      Proj prev = projectPt(viewProj, m.pts[0], w, h);
      for (size_t i = 1; i < m.pts.size(); ++i) {
        Proj cur = projectPt(viewProj, m.pts[i], w, h);
        if (prev.front && cur.front) d = std::fmin(d, segDist(px, py, prev.x, prev.y, cur.x, cur.y));
        prev = cur;
      }
    } else {
      Proj p = projectPt(viewProj, m.pts[0], w, h);
      if (!p.front) continue;
      // projected radius: `px` markers keep their screen radius, metric ones scale with 1 / w (approx: fovY-free
      // ratio from the clip w of a point one radius above the centre)
      float rPx = m.px;
      if (rPx <= 0) {
        Proj q = projectPt(viewProj, m.pts[0] + vec3{0, m.kind == Kind::Disc ? 0.f : m.size, 0}, w, h);
        rPx = q.front ? std::hypot(q.x - p.x, q.y - p.y) : 0;
        if (m.kind == Kind::Disc) { Proj e = projectPt(viewProj, m.pts[0] + vec3{m.size, 0, 0}, w, h); rPx = e.front ? std::hypot(e.x - p.x, e.y - p.y) : 0; }
      }
      d = std::fmax(0.f, std::hypot(p.x - px, p.y - py) - rPx);
    }
    float dist = length((m.kind == Kind::Line ? m.pts[m.pts.size() / 2] : m.pts[0]) - eye);
    if (d < bd || (d == bd && best && dist < bdist)) { bd = d; bdist = dist; best = &m; }
  }
  return best ? best->id : "";
}

std::vector<MarkerScreen> Markers::screen(const mat4& viewProj, int w, int h, vec3 eye) const {
  std::vector<MarkerScreen> out;
  for (const Marker& m : items_) {
    if (!m.label) continue;
    vec3 t = top(m); Proj p = projectPt(viewProj, t, w, h);
    if (!p.front) continue;
    out.push_back({m.id, p.x, p.y, length(t - eye), p.x >= 0 && p.x <= (float)w && p.y >= 0 && p.y <= (float)h});
  }
  return out;
}

bool Markers::anchor(const std::string& id, vec3& out) const {
  for (const Marker& m : items_) if (m.id == id) { out = top(m); return true; }
  return false;
}

} // namespace eng

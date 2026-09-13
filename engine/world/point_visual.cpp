#include "engine/world/point_visual.h"
#include "engine/render/mesh_builder.h"
#include <chrono>
#include <cmath>

namespace eng {

namespace {
vec4 rgb(uint32_t c) { return {(float)((c >> 16) & 255) / 255.f, (float)((c >> 8) & 255) / 255.f, (float)(c & 255) / 255.f, 1}; }
vec4 rgbLin(uint32_t c) { vec4 v = rgb(c); return {std::pow(v.x, 2.2f), std::pow(v.y, 2.2f), std::pow(v.z, 2.2f), 1}; }
constexpr uint32_t COLOURS[4] = {0x2fd85a, 0x1ea94a, 0xff3b30, 0x8f342e};   // set, set+locked, unset, unset+locked
constexpr float PADLOCK_Y = 4.1f;   // PANAH_TINGGI + 1.9

Material unlit(vec4 c, bool blend) {
  Material m; m.baseColor = c; m.unlit = true; m.doubleSided = true; m.metallic = 0; m.roughness = 1;
  m.alphaMode = blend ? AlphaMode::Blend : AlphaMode::Opaque; return m;
}

// texKabut: white radial gradient in alpha (0.95 centre, 0.38 at 40 %, 0 at the edge).
rhi::Texture makeGlowTexture() {
  const int S = 64; std::vector<uint8_t> px((size_t)S * S * 4);
  for (int y = 0; y < S; ++y) for (int x = 0; x < S; ++x) {
    float d = std::hypot((float)x + 0.5f - S / 2.f, (float)y + 0.5f - S / 2.f) / (S / 2.f);
    float a = d < 0.4f ? 0.95f + (0.38f - 0.95f) * (d / 0.4f) : d < 1 ? 0.38f * (1 - (d - 0.4f) / 0.6f) : 0;
    uint8_t* o = &px[((size_t)y * S + x) * 4]; o[0] = o[1] = o[2] = 255; o[3] = (uint8_t)(a * 255);
  }
  return rhi::createTexture(S, S, rhi::Format::RGBA8, std::as_bytes(std::span(px)), true, false);
}

// 2D helpers in the XY plane (z = 0), facing +Z; used for the padlock sprite (texGembokBuat, 128 px = 1 unit).
void circle2d(MeshBuilder& b, vec2 c, float r, int n = 24) {
  uint32_t ci = b.vertex({c.x, c.y, 0}, {0, 0, 1}, {});
  for (int i = 0; i < n; ++i) {
    float a0 = 2 * PI * (float)i / n, a1 = 2 * PI * (float)(i + 1) / n;
    uint32_t v0 = b.vertex({c.x + std::cos(a0) * r, c.y + std::sin(a0) * r, 0}, {0, 0, 1}, {});
    uint32_t v1 = b.vertex({c.x + std::cos(a1) * r, c.y + std::sin(a1) * r, 0}, {0, 0, 1}, {});
    b.triangle(ci, v0, v1);
  }
}
void arc2d(MeshBuilder& b, vec2 c, float r, float w, float a0, float a1, int n = 16) {   // thick arc, round-ish ends
  for (int i = 0; i < n; ++i) {
    float t0 = a0 + (a1 - a0) * (float)i / n, t1 = a0 + (a1 - a0) * (float)(i + 1) / n;
    vec3 p00{c.x + std::cos(t0) * (r - w / 2), c.y + std::sin(t0) * (r - w / 2), 0}, p01{c.x + std::cos(t0) * (r + w / 2), c.y + std::sin(t0) * (r + w / 2), 0};
    vec3 p10{c.x + std::cos(t1) * (r - w / 2), c.y + std::sin(t1) * (r - w / 2), 0}, p11{c.x + std::cos(t1) * (r + w / 2), c.y + std::sin(t1) * (r + w / 2), 0};
    b.quad(p00, p10, p11, p01);
  }
  circle2d(b, {c.x + std::cos(a0) * r, c.y + std::sin(a0) * r}, w / 2, 10);
  circle2d(b, {c.x + std::cos(a1) * r, c.y + std::sin(a1) * r}, w / 2, 10);
}
void rect2d(MeshBuilder& b, float x0, float y0, float x1, float y1) { b.quad({x0, y0, 0}, {x1, y0, 0}, {x1, y1, 0}, {x0, y1, 0}); }
// canvas px (y down, 128 square) -> unit sprite coords (centre 0, y up)
vec2 px(float x, float y) { return {(x - 64) / 128, (64 - y) / 128}; }

// Camera-facing quad basis at `pos`, `size` metres wide/high, unit XY geometry facing +Z (toward the camera).
mat4 billboard(vec3 pos, vec3 eye, float size, float toward = 0) {
  vec3 f = eye - pos; float l = length(f); f = l > 1e-6f ? f / l : vec3{0, 0, 1};
  vec3 r = cross({0, 1, 0}, f); float rl = length(r); r = rl > 1e-6f ? r / rl : vec3{1, 0, 0};
  vec3 u = cross(f, r); pos = pos + f * toward;
  mat4 m;
  m.m[0][0] = r.x * size; m.m[0][1] = r.y * size; m.m[0][2] = r.z * size;
  m.m[1][0] = u.x * size; m.m[1][1] = u.y * size; m.m[1][2] = u.z * size;
  m.m[2][0] = f.x; m.m[2][1] = f.y; m.m[2][2] = f.z;
  m.m[3][0] = pos.x; m.m[3][1] = pos.y; m.m[3][2] = pos.z;
  return m;
}
mat4 basis(vec3 x, vec3 y, vec3 z, vec3 pos) {
  mat4 m;
  m.m[0][0] = x.x; m.m[0][1] = x.y; m.m[0][2] = x.z;
  m.m[1][0] = y.x; m.m[1][1] = y.y; m.m[1][2] = y.z;
  m.m[2][0] = z.x; m.m[2][1] = z.y; m.m[2][2] = z.z;
  m.m[3][0] = pos.x; m.m[3][1] = pos.y; m.m[3][2] = pos.z;
  return m;
}

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
  MeshBuilder gq; gq.quad({-0.5f, -0.5f, 0}, {0.5f, -0.5f, 0}, {0.5f, 0.5f, 0}, {-0.5f, 0.5f, 0});
  glow_ = gq.upload(); glowTex_ = makeGlowTexture();
  {   // padlock sprite (texGembokBuat): dark disc with an orange rim, shackle arc, body, keyhole
    MeshBuilder d, o, h;
    circle2d(d, {0, 0}, 60.f / 128, 32);
    arc2d(o, {0, 0}, 60.f / 128, 7.f / 128, 0, 2 * PI, 32);
    arc2d(o, px(64, 54), 19.f / 128, 13.f / 128, 0, PI, 12);
    vec2 b0 = px(36, 101), b1 = px(92, 54); rect2d(o, b0.x, b0.y, b1.x, b1.y);
    circle2d(h, px(64, 71), 7.5f / 128, 12); vec2 k0 = px(60.5f, 88), k1 = px(67.5f, 71); rect2d(h, k0.x, k0.y, k1.x, k1.y);
    padDark_ = d.upload(); padOrange_ = o.upload(); padHole_ = h.upload();
  }
  for (int i = 0; i < 4; ++i) {
    bool set = i < 2;
    mat_[i] = unlit(rgbLin(COLOURS[i]), true); mat_[i].baseColor.w = set ? 1.f : (i == 3 ? 0.55f : 0.9f);
    glowMat_[i] = unlit(rgbLin(COLOURS[i]), true); glowMat_[i].baseColor.w = set ? 0.5f : 0.44f;
    curtainMat_[i] = unlit(rgbLin(COLOURS[i]), true); curtainMat_[i].baseColor.w = set ? 0.3f : 0.26f;
  }
  padMat_[0] = unlit(rgbLin(0x12161c), false); padMat_[1] = unlit(rgbLin(0xffb74d), false); padMat_[2] = unlit(rgbLin(0x3a2a10), false);
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
    p.left = left;
    for (int i = 0; i < 2; ++i) {
      p.legSeg[i] = n.legs[i];
      float side = i == dom ? sideDom : -sideDom;
      p.legDir[i] = dir[i]; p.legSide[i] = side;
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
  const vec3 eye = r.eye();
  double now = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
  float pulse = 0.92f + 0.08f * (float)std::sin(now / 240.0);   // denyut
  for (const PointInstance& p : points_) {
    for (int i = 0; i < 2; ++i) {
      bool set = i == p.setting; int state = (set ? 0 : 2) + (p.locked ? 1 : 0);
      if (!frustum || frustum->contains(arrowBounds_.transformed(p.arrow[i]))) r.drawMesh(arrow_, mat_[state], {}, p.arrow[i]);
      else ++r.culled;
      // glow: ground ellipse 12 m along the leg + a curtain facing the camera about the leg axis
      vec3 c = p.pos + p.legDir[i] * 12.f; c.y += CURTAIN_HEIGHT / 2;
      if (length(c - eye) >= GLOW_RANGE) continue;
      vec3 axis = p.legDir[i], side = normalize(cross(axis, vec3{0, 1, 0}));
      r.drawMesh(glow_, glowMat_[state], glowTex_, basis(axis * GLOW_LENGTH, side * GLOW_WIDTH, {0, 1, 0}, {c.x, p.pos.y + 0.45f, c.z}));
      vec3 face = eye - c; face = face - axis * dot(face, axis);
      if (dot(face, face) < 1e-6f) face = vec3{0, 1, 0} - axis * axis.y;
      face = normalize(face); vec3 up = cross(face, axis);
      r.drawMesh(glow_, curtainMat_[state], glowTex_, basis(axis * GLOW_LENGTH, up * CURTAIN_HEIGHT, face, c));
    }
    if (p.locked) {   // padlock over the set leg's arrow side, 3 m sprite, pulsing
      float gs = (ARROW_OFFSET + ARROW_SCALE * 0.25f) * p.legSide[p.setting];
      vec3 pos = p.pos + p.left * gs; pos.y += PADLOCK_Y;
      float size = 3 * pulse;
      for (int k = 0; k < 3; ++k)
        r.drawMesh(k == 0 ? padDark_ : k == 1 ? padOrange_ : padHole_, padMat_[k], {}, billboard(pos, eye, size, 0.02f * (float)k));
    }
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

void PointVisuals::destroy() {
  points_.clear();
  rhi::destroyMesh(arrow_); rhi::destroyMesh(glow_); rhi::destroyMesh(padDark_); rhi::destroyMesh(padOrange_); rhi::destroyMesh(padHole_);
  if (glowTex_.id) { rhi::destroyTexture(glowTex_); glowTex_ = {}; }
}

} // namespace eng

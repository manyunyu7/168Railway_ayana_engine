#include "engine/world/signal_visual.h"
#include <cmath>

namespace eng {

namespace {
// UKUR (uji3dSinyal.ts:38-89)
constexpr float LENS_D = 0.200f, LAMP_PITCH = 0.230f, RING_D = 0.240f, HOOD_L = 0.120f;
constexpr float PLATE_W = 0.400f, PLATE_H = 0.780f, PLATE_T = 0.050f;
constexpr float MAST_D = 0.114f, MAST_TOP = 3.550f, MAST_BOTTOM = -2.500f, STRIPE = 0.90f;
constexpr float Y_RED = 3.60f, PLATE_BOTTOM = 3.48f, LENS_X = -0.070f;

vec4 rgb(uint32_t c) { return {(float)((c >> 16) & 255) / 255.f, (float)((c >> 8) & 255) / 255.f, (float)(c & 255) / 255.f, 1}; }

// Prism/cylinder along an axis: `axis` 0 = X, 1 = Y. Sides only (caps optional).
void cylinder(MeshBuilder& b, vec3 centre, float radius, float length, int axis, int sides, bool caps) {
  auto point = [&](float a, float t) {
    float c = std::cos(a) * radius, s = std::sin(a) * radius;
    return axis == 1 ? vec3{centre.x + c, centre.y + t, centre.z + s} : vec3{centre.x + t, centre.y + s, centre.z + c};
  };
  float h0 = -length / 2, h1 = length / 2;
  for (int i = 0; i < sides; ++i) {
    float a0 = 2 * PI * (float)i / (float)sides, a1 = 2 * PI * (float)(i + 1) / (float)sides;
    vec3 p00 = point(a0, h0), p01 = point(a0, h1), p10 = point(a1, h0), p11 = point(a1, h1);
    b.quad(p10, p00, p01, p11);
  }
  if (caps) {
    for (int end = 0; end < 2; ++end) {
      float t = end ? h1 : h0;
      vec3 c = axis == 1 ? vec3{centre.x, centre.y + t, centre.z} : vec3{centre.x + t, centre.y, centre.z};
      vec3 n = axis == 1 ? vec3{0, end ? 1.f : -1.f, 0} : vec3{end ? 1.f : -1.f, 0, 0};
      uint32_t ci = b.vertex(c, n, {0.5f, 0.5f});
      for (int i = 0; i < sides; ++i) {
        float a0 = 2 * PI * (float)i / (float)sides, a1 = 2 * PI * (float)(i + 1) / (float)sides;
        uint32_t v0 = b.vertex(point(a0, t), n, {0, 0}), v1 = b.vertex(point(a1, t), n, {1, 1});
        if ((axis == 1) == (end == 1)) b.triangle(ci, v0, v1); else b.triangle(ci, v1, v0);
      }
    }
  }
}

void sphere(MeshBuilder& b, float r, int seg) {
  for (int i = 0; i < seg; ++i)
    for (int j = 0; j < seg * 2; ++j) {
      auto p = [&](int ii, int jj) {
        float th = PI * (float)ii / (float)seg, ph = 2 * PI * (float)jj / (float)(seg * 2);
        return vec3{std::sin(th) * std::cos(ph), std::cos(th), std::sin(th) * std::sin(ph)};
      };
      vec3 a = p(i, j), c = p(i + 1, j), d = p(i + 1, j + 1), e = p(i, j + 1);
      uint32_t ia = b.vertex(a * r, a, {}), ic = b.vertex(c * r, c, {}), id = b.vertex(d * r, d, {}), ie = b.vertex(e * r, e, {});
      b.triangle(ia, id, ic); b.triangle(ia, ie, id);
    }
}

void darkBody(MeshBuilder& dark, int lamps) {
  // dark mast bands
  for (float y = MAST_BOTTOM; y < MAST_TOP; y += 2 * STRIPE) {
    float y1 = std::min(MAST_TOP, y + STRIPE);
    cylinder(dark, {0, (y + y1) / 2, 0}, MAST_D / 2, y1 - y, 1, 10, false);
  }
  // head plate (box; the semicircular top is approximated by a box)
  float top = PLATE_BOTTOM + PLATE_H + (lamps == 3 ? LAMP_PITCH : 0);
  dark.box({-PLATE_T / 2, PLATE_BOTTOM, -PLATE_W / 2}, {PLATE_T / 2, top, PLATE_W / 2});
  for (int i = 0; i < lamps; ++i) {
    float y = Y_RED + LAMP_PITCH * (float)i;
    cylinder(dark, {LENS_X / 2 - PLATE_T / 4, y, 0}, RING_D / 2, -LENS_X - PLATE_T / 2 + 0.01f, 0, 16, false);   // lens ring
    // hood: thin curved cap over the upper half of the lens, protruding 0.12 m forward
    const int n = 8;
    for (int k = 0; k < n; ++k) {
      float a0 = PI * (float)k / n, a1 = PI * (float)(k + 1) / n;
      vec3 r0{-PLATE_T / 2, y + std::sin(a0) * RING_D / 2, std::cos(a0) * RING_D / 2};
      vec3 r1{-PLATE_T / 2, y + std::sin(a1) * RING_D / 2, std::cos(a1) * RING_D / 2};
      vec3 f0 = r0, f1 = r1; f0.x -= HOOD_L + 0.02f; f1.x -= HOOD_L + 0.02f;
      dark.quad(r0, r1, f1, f0); dark.quad(r1, r0, f0, f1);
    }
  }
  // number plate below the head (0.235 x 0.138 at y 2.37)
  dark.box({-0.02f, 2.37f - 0.069f, -0.1175f}, {0.02f, 2.37f + 0.069f, 0.1175f});
}
} // namespace

void SignalVisuals::buildMeshes() {
  MeshBuilder yellow, d2, d3, lens, sph;
  for (float y = MAST_BOTTOM + STRIPE; y < MAST_TOP; y += 2 * STRIPE) {
    float y1 = std::min(MAST_TOP, y + STRIPE);
    cylinder(yellow, {0, (y + y1) / 2, 0}, MAST_D / 2, y1 - y, 1, 10, false);
  }
  darkBody(d2, 2); darkBody(d3, 3);
  // lens disc facing -X, centred at the origin (positioned per lens at draw time)
  {
    const int n = 16; vec3 nrm{-1, 0, 0};
    uint32_t c = lens.vertex({0, 0, 0}, nrm, {0.5f, 0.5f});
    for (int i = 0; i < n; ++i) {
      float a0 = 2 * PI * (float)i / n, a1 = 2 * PI * (float)(i + 1) / n;
      uint32_t v0 = lens.vertex({0, std::sin(a0) * LENS_D / 2, std::cos(a0) * LENS_D / 2}, nrm, {0, 0});
      uint32_t v1 = lens.vertex({0, std::sin(a1) * LENS_D / 2, std::cos(a1) * LENS_D / 2}, nrm, {1, 1});
      lens.triangle(c, v0, v1);
    }
  }
  sphere(sph, 0.75f, 8);
  mastYellow_ = yellow.upload(); dark2_ = d2.upload(); dark3_ = d3.upload(); lens_ = lens.upload(); sphere_ = sph.upload();
  bodyBounds_ = d3.bounds; bodyBounds_.expand(yellow.bounds);

  yellowMat_ = {}; yellowMat_.baseColor = rgb(0xf2a516); yellowMat_.metallic = 0; yellowMat_.roughness = 0.7f;
  darkMat_ = {}; darkMat_.baseColor = rgb(0x1c1f24); darkMat_.metallic = 0.2f; darkMat_.roughness = 0.6f;
  unlitMat_ = {}; unlitMat_.baseColor = rgb(0x2a2a2a); unlitMat_.metallic = 0; unlitMat_.roughness = 0.4f;
  const uint32_t colours[3] = {0xff3b30, 0xffcc00, 0x34c759};
  for (int i = 0; i < 3; ++i) {
    litMat_[i] = {}; litMat_[i].baseColor = rgb(colours[i]); litMat_[i].baseColor.x *= 0.2f; litMat_[i].baseColor.y *= 0.2f; litMat_[i].baseColor.z *= 0.2f; litMat_[i].emissive = rgb(colours[i]).xyz(); litMat_[i].metallic = 0; litMat_[i].roughness = 0.4f;
    sphereMat_[i] = litMat_[i]; sphereMat_[i].emissive = rgb(colours[i]).xyz();
  }
}

void SignalVisuals::build(const TrackGraph& g, const RailProfile& profile, const Json& trackside) {
  destroy();
  buildMeshes();
  const WorldOrigin& o = g.origin();
  for (const Json& t : trackside.arr) {
    if (t["kind"].stringOr("") != "signal") continue;
    SignalInstance si;
    si.id = t["id"].stringOr(""); si.name = t["name"].stringOr(""); si.signalType = t["signalType"].stringOr("interlocking");
    si.segId = t["segId"].stringOr(""); si.s = t["s"].numberOr(0); si.dir = t["dir"].intOr(1) == -1 ? -1 : 1;
    si.left = t["sisi"].stringOr("kanan") == "kiri";
    int seg = g.segIndex(si.segId);
    if (seg < 0) continue;
    // Lamps present, bottom -> top (reverse of trackside.ts daftarLampu).
    if (si.signalType == "muka") { si.lamps = 2; si.lensAspect[0] = Aspect::Yellow; si.lensAspect[1] = Aspect::Green; }
    else if (t["lampu"].intOr(3) == 2) { si.lamps = 2; si.lensAspect[0] = Aspect::Red; si.lensAspect[1] = Aspect::Green; }
    else { si.lamps = 3; si.lensAspect[0] = Aspect::Red; si.lensAspect[1] = Aspect::Yellow; si.lensAspect[2] = Aspect::Green; }

    TrackSample sm = g.sampleAt(seg, si.s);
    double tx = sm.tx * si.dir, ty = sm.ty * si.dir;
    double sisi = si.left ? -1 : 1;
    double nx = -ty * sisi, ny = tx * sisi;
    float hy = profile.railHeight(si.segId.c_str(), si.s);
    si.railPos = o.toScene(sm.wx, sm.wy, hy);
    si.pos = o.toScene(sm.wx + nx * 3, sm.wy + ny * 3, hy);
    si.yaw = WorldOrigin::yawFromTangent(tx, ty);
    si.world = mat4::translation(si.pos) * mat4::rotationY(si.yaw);
    for (int i = 0; i < si.lamps; ++i) si.lensWorld[i] = si.world.transformPoint({LENS_X, Y_RED + LAMP_PITCH * (float)i, 0});
    signals_.push_back(std::move(si));
  }
}

int SignalVisuals::indexOf(const std::string& id) const {
  for (size_t i = 0; i < signals_.size(); ++i) if (signals_[i].id == id) return (int)i;
  return -1;
}

void SignalVisuals::setAspect(const std::string& id, Aspect a) {
  int i = indexOf(id);
  if (i >= 0) signals_[(size_t)i].aspect = a;
}

void SignalVisuals::draw(ModelRenderer& r, vec3 eye, const Frustum* frustum) const {
  for (const SignalInstance& s : signals_) {
    float d = length(s.pos - eye);
    if (d > LOD_DISTANCE) {
      float sc = std::max(1.f, d / 320.f);
      mat4 m = mat4::translation(s.railPos + vec3{0, 1.2f, 0}) * mat4::scale({sc, sc, sc});
      r.drawMesh(sphere_, sphereMat_[(int)s.aspect], {}, m);
      continue;
    }
    if (frustum && !frustum->contains(bodyBounds_.transformed(s.world))) { ++r.culled; continue; }
    r.drawMesh(mastYellow_, yellowMat_, {}, s.world);
    r.drawMesh(s.lamps == 2 ? dark2_ : dark3_, darkMat_, {}, s.world);
    // Lit lens: the aspect's lamp if present, else the most restrictive lamp (red, or the bottom one).
    int lit = -1;
    for (int i = 0; i < s.lamps; ++i) if (s.lensAspect[i] == s.aspect) lit = i;
    if (lit < 0) { lit = 0; for (int i = 0; i < s.lamps; ++i) if (s.lensAspect[i] == Aspect::Red) lit = i; }
    for (int i = 0; i < s.lamps; ++i) {
      mat4 m = s.world * mat4::translation({LENS_X - 0.005f, Y_RED + LAMP_PITCH * (float)i, 0});
      r.drawMesh(lens_, i == lit ? litMat_[(int)s.lensAspect[i]] : unlitMat_, {}, m);
    }
  }
}

std::vector<ScreenPoint> SignalVisuals::screenPositions(const mat4& viewProj, int w, int h, vec3 eye) const {
  std::vector<ScreenPoint> out; out.reserve(signals_.size());
  for (const SignalInstance& s : signals_) {
    float d = length(s.pos - eye);
    vec3 p = d > LOD_DISTANCE ? s.railPos + vec3{0, 1.2f, 0} : s.lensWorld[s.lamps - 1];
    vec4 c = viewProj * vec4(p, 1);
    ScreenPoint sp; sp.id = s.id;
    if (c.w > 0) {
      sp.x = (c.x / c.w * 0.5f + 0.5f) * (float)w; sp.y = (1 - (c.y / c.w * 0.5f + 0.5f)) * (float)h;
      sp.visible = sp.x >= 0 && sp.x <= (float)w && sp.y >= 0 && sp.y <= (float)h && c.z / c.w <= 1;
    }
    out.push_back(std::move(sp));
  }
  return out;
}

void SignalVisuals::destroy() {
  signals_.clear();
  rhi::destroyMesh(mastYellow_); rhi::destroyMesh(dark2_); rhi::destroyMesh(dark3_); rhi::destroyMesh(lens_); rhi::destroyMesh(sphere_);
}

} // namespace eng

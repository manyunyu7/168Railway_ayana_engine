#include "engine/world/signal_visual.h"
#include <chrono>
#include <cmath>
#include <regex>

namespace eng {

namespace {
// UKUR (uji3dSinyal.ts:38-89)
constexpr float LENS_D = 0.200f, LAMP_PITCH = 0.230f, RING_D = 0.240f, HOOD_L = 0.120f;
constexpr float PLATE_W = 0.400f, PLATE_H = 0.780f, PLATE_T = 0.050f;
constexpr float MAST_D = 0.114f, MAST_TOP = 3.550f, MAST_BOTTOM = -2.500f, STRIPE = 0.90f;
constexpr float Y_RED = 3.60f, PLATE_BOTTOM = 3.48f, LENS_X = -0.070f;
// UKUR_SEMAFOR (uji3dSemafor.ts:45-91)
constexpr float ARM_LEN = 1.20f, ARM_W = 0.19f, ARM_T = 0.028f, DISC_D = 0.34f, DISC_C = 1.03f, ARM_ROOT = 0.10f;
constexpr float ARM_GAP = 0.85f, PIVOT_OUT = 0.17f, LAT_TOP_W = 0.18f, LAT_BOTTOM_W = 0.40f, LAT_BOTTOM = -2.20f;
constexpr float TOP_ABOVE_PIVOT = 0.55f, CAP_BELOW_PIVOT = 1.55f, CAP_D = 0.52f, SPECT_R = 0.30f, GLASS_D = 0.17f, LAMP_BOX = 0.20f;
constexpr float STRIPE_SEM = 0.65f, SEM_RAISED = PI / 4;
constexpr float PIVOT_Y[3] = {7.0f, 5.5f, 5.0f};   // masuk, keluar, muka
constexpr float SPRING_K = 150, SPRING_C = 15, SPRING_H = 0.05f;

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

// Tapered lattice mast: four corner angles joined by diagonal braces, painted in 0.65 m yellow/black
// bands (the web version is two crossed alpha planes; here thin boxes, no alpha textures).
void latticeMast(MeshBuilder& yellow, MeshBuilder& dark, float top) {
  const float h = top - LAT_BOTTOM; const int bands = (int)std::ceil(h / STRIPE_SEM);
  for (int b = 0; b < bands; ++b) {
    float y0 = LAT_BOTTOM + STRIPE_SEM * (float)b, y1 = std::min(top, y0 + STRIPE_SEM);
    float w0 = LAT_BOTTOM_W + (LAT_TOP_W - LAT_BOTTOM_W) * (y0 - LAT_BOTTOM) / h;
    float w1 = LAT_BOTTOM_W + (LAT_TOP_W - LAT_BOTTOM_W) * (y1 - LAT_BOTTOM) / h;
    MeshBuilder& m = (b % 2 == 0) ? yellow : dark;
    // corner posts (as slightly tapered quads on each side, 0.035 wide)
    for (int c = 0; c < 4; ++c) {
      float sx = c & 1 ? 1.f : -1.f, sz = c & 2 ? 1.f : -1.f;
      vec3 a{sx * w0 / 2, y0, sz * w0 / 2}, e{sx * w1 / 2, y1, sz * w1 / 2};
      const float t = 0.035f;
      vec3 dx{-sx * t, 0, 0}, dz{0, 0, -sz * t};
      m.quad(a, a + dz, e + dz, e); m.quad(a + dz, a, e, e + dz);
      m.quad(a, e, e + dx, a + dx); m.quad(e, a, a + dx, e + dx);
    }
    // one diagonal brace per face per band (X-bracing halves)
    for (int f = 0; f < 4; ++f) {
      float w = 0.02f;
      vec3 p0, p1, n;
      if (f < 2) { float sz = f ? 1.f : -1.f; p0 = {-w0 / 2, y0, sz * w0 / 2}; p1 = {w1 / 2, y1, sz * w1 / 2}; n = {0, 0, sz}; }
      else { float sx = f == 3 ? 1.f : -1.f; p0 = {sx * w0 / 2, y0, -w0 / 2}; p1 = {sx * w1 / 2, y1, w1 / 2}; n = {sx, 0, 0}; }
      vec3 d = normalize(p1 - p0), side = normalize(cross(n, d)) * w;
      m.quad(p0 - side, p0 + side, p1 + side, p1 - side); m.quad(p0 + side, p0 - side, p1 - side, p1 + side);
    }
  }
}

void octagon(MeshBuilder& b, vec3 c, float r, float t, int n = 10) {   // disc normal ±X, thickness t
  for (int side = 0; side < 2; ++side) {
    float x = c.x + (side ? t / 2 : -t / 2); vec3 nrm{side ? 1.f : -1.f, 0, 0};
    uint32_t ci = b.vertex({x, c.y, c.z}, nrm, {0.5f, 0.5f});
    for (int i = 0; i < n; ++i) {
      float a0 = 2 * PI * (float)i / n, a1 = 2 * PI * (float)(i + 1) / n;
      uint32_t v0 = b.vertex({x, c.y + std::sin(a0) * r, c.z + std::cos(a0) * r}, nrm, {0, 0});
      uint32_t v1 = b.vertex({x, c.y + std::sin(a1) * r, c.z + std::cos(a1) * r}, nrm, {1, 1});
      if (side) b.triangle(ci, v1, v0); else b.triangle(ci, v0, v1);
    }
  }
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
  buildSemaphoreMeshes();
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

void SignalVisuals::buildSemaphoreMeshes() {
  for (int role = 0; role < 3; ++role) {
    MeshBuilder yellow, dark;
    float top = PIVOT_Y[role] + TOP_ABOVE_PIVOT;
    latticeMast(yellow, dark, top);
    dark.box({-LAT_TOP_W / 2 - 0.02f, top, -LAT_TOP_W / 2 - 0.02f}, {LAT_TOP_W / 2 + 0.02f, top + 0.04f, LAT_TOP_W / 2 + 0.02f});   // top cap plate
    float yc = PIVOT_Y[role] - CAP_BELOW_PIVOT;   // black rain cap / anti-climb collar
    dark.box({-CAP_D / 2, yc - 0.065f, -CAP_D / 2}, {CAP_D / 2, yc + 0.065f, CAP_D / 2});
    dark.box({-0.02f, 2.37f - 0.069f, -0.1175f}, {0.02f, 2.37f + 0.069f, 0.1175f});   // number plate
    semMast_[role] = yellow.upload(); semMastDark_[role] = dark.upload();
    semBounds_[role] = yellow.bounds; semBounds_[role].expand(dark.bounds);
    semBounds_[role].expand({0, PIVOT_Y[role], ARM_LEN + PIVOT_OUT}); semBounds_[role].expand({0, PIVOT_Y[role] + ARM_LEN, -SPECT_R});
  }
  // Arm (local origin = pivot, arm along +Z, rotates about X): yellow blade + end disc, dark bracket and stripe.
  MeshBuilder ay, ad, sp, gl, lamp;
  ay.box({-ARM_T / 2, -ARM_W / 2, ARM_ROOT}, {ARM_T / 2, ARM_W / 2, DISC_C - DISC_D / 2 + 0.02f});
  octagon(ay, {0, 0, DISC_C}, DISC_D / 2, ARM_T);
  ad.box({-0.04f, -0.065f, -0.065f}, {0.04f, 0.065f, 0.065f});                       // pivot bracket
  ad.box({-ARM_T / 2 - 0.004f, -ARM_W / 2, 0.55f}, {ARM_T / 2 + 0.004f, ARM_W / 2, 0.70f});   // black band
  ad.box({-0.02f, -0.0225f, -0.42f}, {0.02f, 0.0225f, -0.18f});                     // counterweight stalk
  ad.box({-0.03f, -0.07f, -0.53f}, {0.09f, 0.07f, -0.39f});                          // counterweight
  // spectacle plate (pivot -> glass), one per glass angle; glass disc at -SPECT_R
  sp.box({-0.015f, -0.055f, -SPECT_R}, {0.035f, 0.055f, 0});
  octagon(gl, {-0.03f, 0, -SPECT_R}, GLASS_D / 2, 0.01f, 12);
  // lamp house fixed on the mast behind the spectacle (drawn per arm)
  lamp.box({-0.03f, -LAMP_BOX * 0.6f, -LAMP_BOX * 0.45f}, {-0.03f + LAMP_BOX, LAMP_BOX * 0.6f, LAMP_BOX * 0.45f});
  lamp.box({0.0f, LAMP_BOX * 0.6f, -0.045f}, {0.09f, LAMP_BOX * 0.6f + 0.06f, 0.045f});
  armYellow_ = ay.upload(); armDark_ = ad.upload(); spectacle_ = sp.upload(); glass_ = gl.upload(); semLamp_ = lamp.upload();
  steelMat_ = {}; steelMat_.baseColor = rgb(0x8d949c); steelMat_.metallic = 0.6f; steelMat_.roughness = 0.5f;
  const uint32_t gc[3] = {0xff2a1f, 0x2fd45f, 0xffc21f};
  for (int i = 0; i < 3; ++i) { glassMat_[i] = {}; glassMat_[i].baseColor = rgb(gc[i]); glassMat_[i].emissive = rgb(gc[i]).xyz() * 0.6f; glassMat_[i].metallic = 0; glassMat_[i].roughness = 0.3f; }
}

// lenganTarget (trackside.ts:155-162): radians from horizontal per arm, index 0 = top arm.
void SignalVisuals::armTargets(const SignalInstance& s, float out[2]) {
  out[0] = out[1] = 0;
  if (s.signalType == "muka" || s.signalType == "pengulang") { out[0] = s.aspect == Aspect::Green ? SEM_RAISED : 0; return; }
  if (s.arms >= 2) {
    if (s.aspect == Aspect::Red) return;
    out[0] = SEM_RAISED; out[1] = s.aspect == Aspect::Green ? SEM_RAISED : 0; return;
  }
  out[0] = s.aspect == Aspect::Red ? 0 : SEM_RAISED;
}

void SignalVisuals::animate(float dt) {
  double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  if (dt < 0) dt = animClock_ > 0 ? (float)(now - animClock_) : 0;
  animClock_ = now;
  float h = std::min(SPRING_H, std::max(0.f, dt));
  for (SignalInstance& s : signals_) {
    if (!s.mechanical) continue;
    armTargets(s, s.armTarget);
    for (int i = 0; i < s.arms; ++i) {
      float a = SPRING_K * (s.armTarget[i] - s.armAngle[i]) - SPRING_C * s.armVel[i];
      s.armVel[i] += a * h; s.armAngle[i] += s.armVel[i] * h;
    }
  }
}

void SignalVisuals::drawSemaphore(ModelRenderer& r, const SignalInstance& s) const {
  int role = s.pivotY == PIVOT_Y[0] ? 0 : s.pivotY == PIVOT_Y[2] ? 2 : 1;
  r.drawMesh(semMast_[role], yellowMat_, {}, s.world);
  r.drawMesh(semMastDark_[role], darkMat_, {}, s.world);
  bool muka = s.signalType == "muka" || s.signalType == "pengulang";
  for (int i = 0; i < s.arms; ++i) {
    float y = s.pivotY - ARM_GAP * (float)i;
    mat4 pivot = s.world * mat4::translation({0, y, PIVOT_OUT});
    mat4 arm = pivot * mat4::rotationX(-s.armAngle[i]);   // +Z end rises for positive angles (arm on the right)
    r.drawMesh(armYellow_, yellowMat_, {}, arm);
    r.drawMesh(armDark_, darkMat_, {}, arm);
    // spectacle: glass k sits at the angle it represents, so the lamp (fixed at the horizontal
    // position) shines through red when the arm is level and through green when raised.
    const float pos[2] = {0, SEM_RAISED};
    for (int k = 0; k < 2; ++k) {
      mat4 spec = arm * mat4::rotationX(-pos[k]);
      r.drawMesh(spectacle_, darkMat_, {}, spec);
      r.drawMesh(glass_, glassMat_[k == 0 ? (muka ? 2 : 0) : 1], {}, spec);
    }
    r.drawMesh(semLamp_, darkMat_, {}, s.world * mat4::translation({0.02f, y, PIVOT_OUT - SPECT_R}));
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
    si.mechanical = t["bentuk"].stringOr("elektrik") == "mekanik";
    if (si.mechanical) {
      static const std::regex masuk("^(.+?) M[A-Z][0-9]*$");
      bool muka = si.signalType == "muka" || si.signalType == "pengulang";
      si.pivotY = muka ? PIVOT_Y[2] : std::regex_match(si.name, masuk) ? PIVOT_Y[0] : PIVOT_Y[1];
      si.arms = muka ? 1 : std::max(1, std::min(2, t["lengan"].intOr(1)));
    }
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
    si.pivotWorld = si.world.transformPoint({0, si.pivotY, PIVOT_OUT});
    armTargets(si, si.armTarget); si.armAngle[0] = si.armTarget[0]; si.armAngle[1] = si.armTarget[1];
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
    if (s.mechanical) {
      int role = s.pivotY == PIVOT_Y[0] ? 0 : s.pivotY == PIVOT_Y[2] ? 2 : 1;
      if (frustum && !frustum->contains(semBounds_[role].transformed(s.world))) { ++r.culled; continue; }
      drawSemaphore(r, s);
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
    vec3 p = d > LOD_DISTANCE ? s.railPos + vec3{0, 1.2f, 0} : s.mechanical ? s.pivotWorld : s.lensWorld[s.lamps - 1];
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
  for (int i = 0; i < 3; ++i) { rhi::destroyMesh(semMast_[i]); rhi::destroyMesh(semMastDark_[i]); }
  rhi::destroyMesh(semLamp_); rhi::destroyMesh(armYellow_); rhi::destroyMesh(armDark_); rhi::destroyMesh(spectacle_); rhi::destroyMesh(glass_);
}

} // namespace eng

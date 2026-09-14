#include "engine/world/signal_visual.h"
#include "engine/world/pixel_canvas.h"
#include <chrono>
#include <cmath>
#include <regex>

namespace eng {

namespace {
// UKUR (uji3dSinyal.ts:38-89)
constexpr float LENS_D = 0.200f, LAMP_PITCH = 0.230f, RING_D = 0.240f, HOOD_L = 0.120f;
constexpr float PLATE_W = 0.400f, PLATE_H = 0.780f, PLATE_T = 0.050f, ARC_R = 0.200f, LAMP_FROM_BOTTOM = 0.120f;
constexpr float BOARD_SIDE = 0.573f, BOARD_UP = 0.230f, BOARD_BACK = 0.045f, PANEL_W = 0.288f, PANEL_H = 0.470f;
constexpr float MAST_D = 0.114f, MAST_TOP = 3.550f, MAST_BOTTOM = -2.500f;
constexpr float STRIPE = 0.45f;   // pitaTinggi: one band; the yellow/black pair repeats every 0.90 m
constexpr float CAGE_W = 0.495f, CAGE_H = 0.770f, CAGE_FWD = 0.110f, OCT_W = 0.240f, OCT_H = 0.255f, OCT_D = 0.160f, SHUNT_LENS_D = 0.155f;
constexpr float PLATE_NO_W = 0.235f, PLATE_NO_H = 0.138f;
constexpr float Y_RED = 3.60f, PLATE_BOTTOM = Y_RED - LAMP_FROM_BOTTOM, Y_CLAMP = 2.56f, Y_PLATE_NO = 2.37f, Y_SHUNT = 3.19f, Y_CAGE = 3.095f;
constexpr float LENS_FWD = -0.045f, LENS_X = -PLATE_T / 2 + LENS_FWD;   // lens face (titikLampuSinyal)
// corona (uji3dSinyal.ts:685-743, keretaVisual3d.ts pasangSilauSinyal)
constexpr float CORONA_M = 0.20f, CORONA_WIDE = 1.9f, CORONA_PX = 13, CORONA_SIDE = 0.06f, CORONA_FAR = 2500, CORONA_FWD = 0.12f;
// UKUR_PENGULANG (uji3dSinyal.ts:903-925)
constexpr float PG_DISC_D = 0.75f, PG_DISC_T = 0.055f, PG_LED_D = 0.100f, PG_PITCH = 0.125f, PG_HOOD_L = 0.085f, PG_DISC_FWD = 0.075f, PG_Y = 3.55f;
// UKUR_SEMAFOR (uji3dSemafor.ts:45-91)
constexpr float ARM_LEN = 1.20f, ARM_W = 0.19f, ARM_T = 0.028f, DISC_D = 0.34f, DISC_C = 1.03f, ARM_ROOT = 0.10f;
constexpr float ARM_GAP = 0.85f, PIVOT_OUT = 0.17f, LAT_TOP_W = 0.18f, LAT_BOTTOM_W = 0.40f, LAT_BOTTOM = -2.20f;
constexpr float TOP_ABOVE_PIVOT = 0.55f, CAP_BELOW_PIVOT = 1.55f, CAP_D = 0.52f, SPECT_R = 0.30f, GLASS_D = 0.17f, LAMP_BOX = 0.20f;
constexpr float STRIPE_SEM = 0.65f, SEM_RAISED = PI / 4;
constexpr float PIVOT_Y[3] = {7.0f, 5.5f, 5.0f};   // masuk, keluar, muka
constexpr float SPRING_K = 150, SPRING_C = 15, SPRING_H = 0.05f;
constexpr uint32_t WARNA_ASPEK[3] = {0xff3b30, 0xffcc00, 0x34c759};   // red, yellow, green
constexpr uint32_t HITAM = 0x20242a;

vec4 rgb(uint32_t c) { return {(float)((c >> 16) & 255) / 255.f, (float)((c >> 8) & 255) / 255.f, (float)(c & 255) / 255.f, 1}; }
// sRGB hex -> linear, for unlit materials (the shader gamma-encodes its output)
vec4 rgbLin(uint32_t c) { vec4 v = rgb(c); return {std::pow(v.x, 2.2f), std::pow(v.y, 2.2f), std::pow(v.z, 2.2f), 1}; }

// Open cylinder (sides only, optional caps) along axis 0 = X, 1 = Y; `a0..a1` = partial sweep (radians,
// angle measured in the plane perpendicular to the axis: X axis -> (y, z) = (sin a, cos a)).
void cylinder(MeshBuilder& b, vec3 centre, float radius, float length, int axis, int sides, bool caps,
              float a0 = 0, float a1 = 2 * PI, bool twoSided = false) {
  auto point = [&](float a, float t) {
    float c = std::cos(a) * radius, s = std::sin(a) * radius;
    return axis == 1 ? vec3{centre.x + c, centre.y + t, centre.z + s} : vec3{centre.x + t, centre.y + s, centre.z + c};
  };
  float h0 = -length / 2, h1 = length / 2;
  for (int i = 0; i < sides; ++i) {
    float aa = a0 + (a1 - a0) * (float)i / (float)sides, ab = a0 + (a1 - a0) * (float)(i + 1) / (float)sides;
    vec3 p00 = point(aa, h0), p01 = point(aa, h1), p10 = point(ab, h0), p11 = point(ab, h1);
    b.quad(p10, p00, p01, p11);
    if (twoSided) b.quad(p00, p10, p11, p01);
  }
  if (caps) {
    for (int end = 0; end < 2; ++end) {
      float t = end ? h1 : h0;
      vec3 c = axis == 1 ? vec3{centre.x, centre.y + t, centre.z} : vec3{centre.x + t, centre.y, centre.z};
      vec3 n = axis == 1 ? vec3{0, end ? 1.f : -1.f, 0} : vec3{end ? 1.f : -1.f, 0, 0};
      uint32_t ci = b.vertex(c, n, {0.5f, 0.5f});
      for (int i = 0; i < sides; ++i) {
        float aa = a0 + (a1 - a0) * (float)i / (float)sides, ab = a0 + (a1 - a0) * (float)(i + 1) / (float)sides;
        uint32_t v0 = b.vertex(point(aa, t), n, {0, 0}), v1 = b.vertex(point(ab, t), n, {1, 1});
        if ((axis == 1) == (end == 1)) b.triangle(ci, v0, v1); else b.triangle(ci, v1, v0);
      }
    }
  }
}

// Disc facing -X at `c` with UVs spanning the disc (lens texture), optional back face.
void disc(MeshBuilder& b, vec3 c, float r, int n, bool back = false) {
  for (int side = 0; side < (back ? 2 : 1); ++side) {
    vec3 nrm{side ? 1.f : -1.f, 0, 0};
    uint32_t ci = b.vertex(c, nrm, {0.5f, 0.5f});
    for (int i = 0; i < n; ++i) {
      float a0 = 2 * PI * (float)i / n, a1 = 2 * PI * (float)(i + 1) / n;
      uint32_t v0 = b.vertex({c.x, c.y + std::sin(a0) * r, c.z + std::cos(a0) * r}, nrm, {0.5f + 0.5f * std::cos(a0), 0.5f + 0.5f * std::sin(a0)});
      uint32_t v1 = b.vertex({c.x, c.y + std::sin(a1) * r, c.z + std::cos(a1) * r}, nrm, {0.5f + 0.5f * std::cos(a1), 0.5f + 0.5f * std::sin(a1)});
      if (side) b.triangle(ci, v1, v0); else b.triangle(ci, v0, v1);
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

// kotak(): axis-aligned box centred at (x, y, z) with size (w, h, d)
void kotak(MeshBuilder& b, float w, float h, float d, float x, float y, float z) {
  b.box({x - w / 2, y - h / 2, z - d / 2}, {x + w / 2, y + h / 2, z + d / 2});
}

// Head plate (geoPelatBuat): rectangle with a semicircular top, extruded PLATE_T along X and centred on
// x = 0; origin at the plate's bottom centre. Front face normal -X.
void headPlate(MeshBuilder& b, float height, float yBase) {
  const float w = PLATE_W / 2, r = ARC_R; const int arc = 7;
  std::vector<vec3> prof;   // counter-clockwise seen from -X, in (y, z)
  prof.push_back({0, 0, -w}); prof.push_back({0, 0, w}); prof.push_back({0, height - r, w});
  for (int i = 1; i < arc; ++i) { float a = PI * (float)i / arc; prof.push_back({0, height - r + std::sin(a) * r, std::cos(a) * r}); }
  prof.push_back({0, height - r, -w});
  for (vec3& p : prof) p.y += yBase;
  const size_t n = prof.size();
  vec3 cen{0, yBase + height / 2, 0};
  for (int side = 0; side < 2; ++side) {
    float x = side ? PLATE_T / 2 : -PLATE_T / 2; vec3 nrm{side ? 1.f : -1.f, 0, 0};
    // UV 0..1 across the face (u left->right seen from -X, v 0 at the top) for the plate texture
    auto uv = [&](vec3 p) { return vec2{(p.z + w) / PLATE_W, 1 - (p.y - yBase) / height}; };
    uint32_t ci = b.vertex({x, cen.y, cen.z}, nrm, uv(cen));
    for (size_t i = 0; i < n; ++i) {
      vec3 p0 = prof[i], p1 = prof[(i + 1) % n]; p0.x = p1.x = x;
      uint32_t v0 = b.vertex(p0, nrm, uv(p0)), v1 = b.vertex(p1, nrm, uv(p1));
      if (side) b.triangle(ci, v1, v0); else b.triangle(ci, v0, v1);
    }
  }
  for (size_t i = 0; i < n; ++i) {
    vec3 p0 = prof[i], p1 = prof[(i + 1) % n];
    vec3 f0 = p0, f1 = p1, k0 = p0, k1 = p1; f0.x = f1.x = -PLATE_T / 2; k0.x = k1.x = PLATE_T / 2;
    b.quad(f1, f0, k0, k1);
  }
}

// Diamond board: BOARD_SIDE square rotated 45° in the YZ plane, both faces (steel plate seen from behind).
void diamond(MeshBuilder& b, vec3 c) {
  float h = BOARD_SIDE * 0.70710678f;
  vec3 t{c.x, c.y + h, c.z}, r{c.x, c.y, c.z + h}, bt{c.x, c.y - h, c.z}, l{c.x, c.y, c.z - h};
  b.quad(t, l, bt, r); b.quad(t, r, bt, l);
}

// Regular octagon prism along X (shunting head), flats top/bottom, scaled OCT_H/OCT_W vertically.
void octagonPrism(MeshBuilder& b, vec3 c, float w, float h, float depth) {
  const float rr = (w / 2) / std::cos(PI / 8); const int n = 8;
  auto pt = [&](int i, float x) { float a = 2 * PI * (float)i / n + PI / 8; return vec3{c.x + x, c.y + std::sin(a) * rr * (h / w), c.z + std::cos(a) * rr}; };
  for (int side = 0; side < 2; ++side) {
    float x = side ? depth / 2 : -depth / 2; vec3 nrm{side ? 1.f : -1.f, 0, 0};
    uint32_t ci = b.vertex({c.x + x, c.y, c.z}, nrm, {});
    for (int i = 0; i < n; ++i) {
      uint32_t v0 = b.vertex(pt(i, x), nrm, {}), v1 = b.vertex(pt(i + 1, x), nrm, {});
      if (side) b.triangle(ci, v1, v0); else b.triangle(ci, v0, v1);
    }
  }
  for (int i = 0; i < n; ++i) b.quad(pt(i + 1, -depth / 2), pt(i, -depth / 2), pt(i, depth / 2), pt(i + 1, depth / 2));
}

// Everything black on a colour-light head: plate, board, cage, shunting unit, mast clamp, number plate.
void headDark(MeshBuilder& d, int lamps, bool board, bool cage, bool shunting) {
  float plateH = PLATE_H - (float)(3 - lamps) * LAMP_PITCH;   // the plate itself is a textured mesh (headPlate)
  if (board) {
    float yb = PLATE_BOTTOM + plateH + BOARD_UP;
    diamond(d, {BOARD_BACK, yb, 0});
    // number panel (no text): a slightly proud plate on the board's face
    kotak(d, 0.006f, PANEL_H, PANEL_W, BOARD_BACK - 0.012f, yb, 0);
    kotak(d, 0.05f, 0.30f, 0.06f, BOARD_BACK + 0.01f, yb - 0.28f, 0);   // dudukan-papan
  }
  if (cage) {
    float half = (CAGE_W - 0.03f) / 2, top = Y_CAGE + CAGE_H / 2, bottom = Y_CAGE - CAGE_H / 2, xK = -CAGE_FWD - 0.045f;
    kotak(d, 0.035f, CAGE_H, 0.03f, xK, Y_CAGE, -half); kotak(d, 0.035f, CAGE_H, 0.03f, xK, Y_CAGE, half);
    kotak(d, 0.035f, 0.03f, CAGE_W, xK, top, 0); kotak(d, 0.035f, 0.03f, CAGE_W, xK, bottom, 0);
    for (float y : {top, bottom}) for (float z : {-half, half}) kotak(d, CAGE_FWD, 0.03f, 0.03f, xK / 2, y, z);
    kotak(d, 0.16f, 0.10f, CAGE_W * 0.86f, -0.05f, top - 0.05f, 0);   // siku-gantung
    if (shunting) {
      kotak(d, 0.19f, PLATE_BOTTOM - 2.82f, 0.17f, -0.020f, (PLATE_BOTTOM + 2.82f) / 2, 0);   // tulang-punggung
      octagonPrism(d, {-0.05f, Y_SHUNT, 0}, OCT_W, OCT_H, OCT_D);
      kotak(d, 0.15f, 0.21f, 0.155f, -0.03f, 2.955f, 0);   // kotak-mekanik
      kotak(d, 0.17f, 0.11f, 0.235f, -0.02f, 2.84f, 0);    // siku-tiang-langsir
    }
  }
  kotak(d, 0.16f, 0.225f, 0.21f, -0.02f, Y_CLAMP, 0);              // klem-tiang
  kotak(d, 0.05f, PLATE_NO_H, PLATE_NO_W, -0.045f, Y_PLATE_NO, 0);   // plat-nomor (no text)
}

// Open shells per lamp unit: lens ring (0.045 long), hood = upper half tube 0.12 long, lip at its end.
void headShell(MeshBuilder& s, int lamps) {
  for (int i = 0; i < lamps; ++i) {
    float y = Y_RED + LAMP_PITCH * (float)i, x0 = -PLATE_T / 2;
    cylinder(s, {x0 - 0.022f, y, 0}, RING_D / 2, 0.045f, 0, 14, false, 0, 2 * PI, true);
    cylinder(s, {x0 + LENS_FWD - HOOD_L / 2, y, 0}, RING_D / 2 + 0.013f, HOOD_L, 0, 10, false, 0, PI, true);
    cylinder(s, {x0 + LENS_FWD - HOOD_L, y, 0}, RING_D / 2 + 0.020f, 0.012f, 0, 10, false, 0, PI, true);
  }
}

// Quad facing -X centred at the origin, `w` across Z, `h` along Y, UV 0..1 (u left->right seen from -X, v 0 top).
void faceQuad(MeshBuilder& b, float w, float h) {
  vec3 n{-1, 0, 0};
  uint32_t a = b.vertex({0, -h / 2, -w / 2}, n, {0, 1}), c = b.vertex({0, -h / 2, w / 2}, n, {1, 1});
  uint32_t d = b.vertex({0, h / 2, w / 2}, n, {1, 0}), e = b.vertex({0, h / 2, -w / 2}, n, {0, 0});
  b.triangle(a, c, d); b.triangle(a, d, e);
}
rhi::Texture upload(const PixelCanvas& c) { return rhi::createTexture(c.w, c.h, rhi::Format::RGBA8, std::as_bytes(std::span(c.px)), true, true, rhi::Wrap::Clamp, rhi::Wrap::Clamp); }

// texPelatBuat: paint #20242a, platform grime rising from the bottom third, bolt rows (edges, eight around
// every lens, five along the top arc). `ratio` = plate height / width.
rhi::Texture makePlateTexture(float ratio, int lamps) {
  const int W = 256, H = (int)std::lround(256 * ratio);
  PixelCanvas c(W, H);
  c.fill(HITAM);
  c.gradientV((float)H, H * 0.6f, 0x0b0d10, 0.55f, 0);
  const float m = W * 0.03f, rb = W * 0.016f;
  auto bolt = [&](float x, float y) { c.circle(x, y, rb, 0x9ba4ae); };
  for (float x : {m, W - m}) { bolt(x, H - m); bolt(x, H * 0.30f); }
  for (int i = 0; i < lamps; ++i) {
    float yL = H * (1 - (LAMP_FROM_BOTTOM + (float)i * LAMP_PITCH) / (PLATE_W * ratio));
    float rB = W * ((RING_D / 2 + 0.008f) / PLATE_W);
    for (int k = 0; k < 8; ++k) { float a = (float)k / 8 * 2 * PI + 0.39f; bolt(W / 2.f + std::cos(a) * rB, yL + std::sin(a) * rB); }
  }
  for (float a = 0.15f; a <= PI - 0.15f + 1e-4f; a += (PI - 0.3f) / 4) bolt(W / 2.f + std::cos(a) * (W / 2.f - m), H * 0.22f - std::sin(a) * (W / 2.f - m));
  return upload(c);
}

// ANGKA (uji3dSinyal.ts): the digit as polylines in the panel's unit square; strips of lamps are laid across
// the smoothed (Catmull-Rom) stroke every `gap` px (stripAngka).
const std::vector<std::vector<vec2>>* angkaStrokes(char d) {
  static const std::map<char, std::vector<std::vector<vec2>>> ANGKA = {
    {'3', {{{0.16f, 0.13f}, {0.62f, 0.11f}, {0.84f, 0.24f}, {0.80f, 0.42f}, {0.50f, 0.50f}, {0.80f, 0.58f}, {0.84f, 0.76f}, {0.62f, 0.89f}, {0.16f, 0.87f}}}},
    {'2', {{{0.14f, 0.26f}, {0.34f, 0.11f}, {0.68f, 0.12f}, {0.84f, 0.28f}, {0.74f, 0.48f}, {0.16f, 0.88f}, {0.86f, 0.88f}}}},
    {'4', {{{0.70f, 0.10f}, {0.14f, 0.66f}, {0.88f, 0.66f}}, {{0.70f, 0.38f}, {0.70f, 0.92f}}}},
    {'5', {{{0.82f, 0.12f}, {0.24f, 0.12f}, {0.20f, 0.44f}, {0.60f, 0.40f}, {0.82f, 0.56f}, {0.78f, 0.78f}, {0.56f, 0.90f}, {0.20f, 0.86f}}}},
    {'6', {{{0.76f, 0.12f}, {0.40f, 0.16f}, {0.18f, 0.44f}, {0.18f, 0.70f}, {0.36f, 0.89f}, {0.62f, 0.89f}, {0.80f, 0.72f}, {0.72f, 0.54f}, {0.42f, 0.50f}, {0.22f, 0.62f}}}},
    {'8', {{{0.50f, 0.50f}, {0.24f, 0.40f}, {0.26f, 0.20f}, {0.50f, 0.11f}, {0.74f, 0.20f}, {0.76f, 0.40f}, {0.50f, 0.50f}, {0.24f, 0.62f}, {0.26f, 0.80f}, {0.50f, 0.90f}, {0.74f, 0.80f}, {0.76f, 0.62f}, {0.50f, 0.50f}}}},
  };
  auto it = ANGKA.find(d); return it == ANGKA.end() ? nullptr : &it->second;
}
std::vector<vec2> haluskan(const std::vector<vec2>& p, int bagi = 8) {
  if (p.size() < 3) return p;
  auto at = [&](int i) { return p[(size_t)std::max(0, std::min((int)p.size() - 1, i))]; };
  std::vector<vec2> out;
  for (int i = 0; i + 1 < (int)p.size(); ++i) {
    vec2 p0 = at(i - 1), p1 = at(i), p2 = at(i + 1), p3 = at(i + 2);
    for (int k = 0; k < bagi; ++k) {
      float t = (float)k / bagi, t2 = t * t, t3 = t2 * t;
      out.push_back({0.5f * (2 * p1.x + (-p0.x + p2.x) * t + (2 * p0.x - 5 * p1.x + 4 * p2.x - p3.x) * t2 + (-p0.x + 3 * p1.x - 3 * p2.x + p3.x) * t3),
                     0.5f * (2 * p1.y + (-p0.y + p2.y) * t + (2 * p0.y - 5 * p1.y + 4 * p2.y - p3.y) * t2 + (-p0.y + 3 * p1.y - 3 * p2.y + p3.y) * t3)});
    }
  }
  out.push_back(p.back());
  return out;
}
void gambarAngka(PixelCanvas& c, char digit, uint32_t colour, float alpha, float grow = 0) {
  const std::vector<std::vector<vec2>>* strokes = angkaStrokes(digit);
  if (!strokes) return;
  const float W = (float)c.w, H = (float)c.h, x0 = W * 0.20f, y0 = H * 0.15f, w = W * 0.60f, h = H * 0.70f;
  const float panjang = W * 0.088f + grow, tebal = W * 0.046f + grow, jarak = W * 0.125f;
  for (const std::vector<vec2>& raw : *strokes) {
    std::vector<vec2> p; for (vec2 q : haluskan(raw)) p.push_back({x0 + q.x * w, y0 + q.y * h});
    float d = 0, next = 0;
    for (size_t i = 0; i + 1 < p.size(); ++i) {
      float dx = p[i + 1].x - p[i].x, dy = p[i + 1].y - p[i].y, L = std::hypot(dx, dy);
      if (L < 1e-6f) continue;
      float ux = dx / L, uy = dy / L;
      while (next <= d + L) {
        float t = next - d;
        c.capsule(p[i].x + ux * t, p[i].y + uy * t, std::atan2(uy, ux) + PI / 2, panjang, tebal, colour, alpha);
        next += jarak;
      }
      d += L;
    }
  }
}
// texAngkaBuat: dark panel, thin frame, six bright bolts, the digit's strips unlit (#2b333c).
rhi::Texture makeAngkaTexture(char digit) {
  const int W = 256, H = (int)std::lround(256 * (PANEL_H / PANEL_W));
  PixelCanvas c(W, H);
  c.fill(0x101418);
  c.strokeRect(1.5f, 1.5f, W - 3.f, H - 3.f, 3, 0x39414a);
  const float pts[6][2] = {{0.06f, 0.04f}, {0.94f, 0.04f}, {0.06f, 0.96f}, {0.94f, 0.96f}, {0.06f, 0.5f}, {0.94f, 0.5f}};
  for (const float* q : pts) c.circle(q[0] * W, q[1] * H, W * 0.013f, 0xe6ecf2);
  gambarAngka(c, digit, 0x2b333c, 1);
  return upload(c);
}
// texAngkaNyalaBuat: white strips with a soft halo on a transparent panel (blended over the dark one).
rhi::Texture makeAngkaLitTexture(char digit) {
  const int W = 256, H = (int)std::lround(256 * (PANEL_H / PANEL_W));
  PixelCanvas c(W, H);
  gambarAngka(c, digit, 0xffffff, 0.35f, W * 0.02f * 2);   // halo (shadowBlur ~ 5 px)
  gambarAngka(c, digit, 0xffffff, 1);
  return upload(c);
}
// texPlatBuat: white text on a #15181c plate, a rivet above and below; names >= 5 chars break at the space.
rhi::Texture makeNameTexture(const BitmapFont& font, const std::string& teks) {
  const int W = 256, H = (int)std::lround(256 * (PLATE_NO_H / PLATE_NO_W));
  PixelCanvas c(W, H);
  c.fill(0x15181c);
  c.circle(W / 2.f, H * 0.13f, W * 0.012f, 0xeef0f2); c.circle(W / 2.f, H * 0.87f, W * 0.012f, 0xeef0f2);
  size_t cut = teks.size() >= 5 ? teks.find(' ') : std::string::npos;
  if (cut != std::string::npos && cut > 0) {
    c.text(font, teks.substr(0, cut), W / 2.f, H * 0.34f, std::round(H * 0.34f), 0xeef0f2);
    c.text(font, teks.substr(cut + 1, 7), W / 2.f, H * 0.68f, std::round(H * 0.34f), 0xeef0f2);
  } else c.text(font, teks.substr(0, 7), W / 2.f, H * 0.52f, std::round(H * 0.46f), 0xeef0f2);
  return upload(c);
}

// texLensaBuat: LED dot matrix on a dark disc (white dots; the material colour multiplies it).
rhi::Texture makeLensTexture() {
  const int S = 128; std::vector<uint8_t> px((size_t)S * S * 4);
  const float cx = S / 2.f, cy = S / 2.f, R = S / 2.f, rDot = R * 0.026f + 0.6f;
  struct Ring { float rad; int n; };
  const Ring rings[] = {{0.11f, 5}, {0.21f, 9}, {0.31f, 13}, {0.41f, 17}, {0.51f, 21}, {0.61f, 25}, {0.70f, 29}, {0.79f, 33}, {0.87f, 36}};
  std::vector<vec2> dots{{cx, cy}};
  for (const Ring& r : rings) for (int i = 0; i < r.n; ++i) { float a = (float)i / (float)r.n * 2 * PI + r.rad * 3; dots.push_back({cx + std::cos(a) * R * r.rad, cy + std::sin(a) * R * r.rad}); }
  for (int y = 0; y < S; ++y) for (int x = 0; x < S; ++x) {
    float fx = (float)x + 0.5f, fy = (float)y + 0.5f, d = std::hypot(fx - cx, fy - cy) / R;
    float r = 13, g = 17, b = 16;   // #0d1110
    if (d > 0.925f) { r = 5; g = 7; b = 6; }   // dark rim
    float glow = std::max(0.f, 1 - std::hypot(fx - cx, fy - (cy - R * 0.42f)) / (R * 0.42f)) * 0.30f;
    r += (255 - r) * glow; g += (255 - g) * glow; b += (255 - b) * glow;
    float cov = 0;
    for (const vec2& p : dots) { float dd = std::hypot(fx - p.x, fy - p.y); cov = std::max(cov, std::min(1.f, rDot + 0.5f - dd)); }
    r += (255 - r) * cov; g += (255 - g) * cov; b += (255 - b) * cov;
    uint8_t* o = &px[((size_t)y * S + x) * 4]; o[0] = (uint8_t)r; o[1] = (uint8_t)g; o[2] = (uint8_t)b; o[3] = 255;
  }
  return rhi::createTexture(S, S, rhi::Format::RGBA8, std::as_bytes(std::span(px)), true, true);
}

// texKabut: white radial gradient in alpha (0.95 centre, 0.38 at 40 %, 0 at the edge).
rhi::Texture makeCoronaTexture() {
  const int S = 64; std::vector<uint8_t> px((size_t)S * S * 4);
  for (int y = 0; y < S; ++y) for (int x = 0; x < S; ++x) {
    float d = std::hypot((float)x + 0.5f - S / 2.f, (float)y + 0.5f - S / 2.f) / (S / 2.f);
    float a = d < 0.4f ? 0.95f + (0.38f - 0.95f) * (d / 0.4f) : d < 1 ? 0.38f * (1 - (d - 0.4f) / 0.6f) : 0;
    uint8_t* o = &px[((size_t)y * S + x) * 4]; o[0] = o[1] = o[2] = 255; o[3] = (uint8_t)(a * 255);
  }
  return rhi::createTexture(S, S, rhi::Format::RGBA8, std::as_bytes(std::span(px)), true, false);
}

// Camera-facing quad basis at `pos`, `size` metres wide/high.
mat4 billboard(vec3 pos, vec3 eye, float size) {
  vec3 f = eye - pos; float l = length(f); f = l > 1e-6f ? f / l : vec3{0, 0, 1};
  vec3 r = cross({0, 1, 0}, f); float rl = length(r); r = rl > 1e-6f ? r / rl : vec3{1, 0, 0};
  vec3 u = cross(f, r);
  mat4 m;
  m.m[0][0] = r.x * size; m.m[0][1] = r.y * size; m.m[0][2] = r.z * size;
  m.m[1][0] = u.x * size; m.m[1][1] = u.y * size; m.m[1][2] = u.z * size;
  m.m[2][0] = f.x; m.m[2][1] = f.y; m.m[2][2] = f.z;
  m.m[3][0] = pos.x; m.m[3][1] = pos.y; m.m[3][2] = pos.z;
  return m;
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


int SignalVisuals::headFor(int flags) {
  for (size_t i = 0; i < heads_.size(); ++i) if (heads_[i].flags == flags) return (int)i;
  int lamps = (flags & 1) ? 3 : 2;
  MeshBuilder d, s, pl;
  headDark(d, lamps, flags & 2, flags & 4, flags & 8);
  headShell(s, lamps);
  headPlate(pl, PLATE_H - (float)(3 - lamps) * LAMP_PITCH, PLATE_BOTTOM);
  HeadMesh h; h.flags = flags; h.dark = d.upload(); h.shell = s.upload(); h.plate = pl.upload(); h.bounds = d.bounds; h.bounds.expand(s.bounds); h.bounds.expand(pl.bounds);
  h.bounds.expand({0, MAST_BOTTOM, 0});
  heads_.push_back(h);
  return (int)heads_.size() - 1;
}

// Pengulang 9C: disc + top hood + 14 LED units (titikLensaPengulang) + support frame.
void SignalVisuals::buildPengulang() {
  const float R = PG_DISC_D / 2;
  MeshBuilder d, s, led;
  cylinder(d, {-PG_DISC_FWD, PG_Y, 0}, R, PG_DISC_T, 0, 28, true);
  kotak(d, PG_DISC_FWD + 0.06f, 0.09f, 0.10f, -PG_DISC_FWD / 2, PG_Y, 0);   // siku-cakram
  float yLow = PG_Y - R;
  for (float z : {-R * 0.62f, R * 0.62f}) kotak(d, 0.04f, R * 1.15f, 0.04f, 0.06f, yLow + R * 0.5f, z);
  kotak(d, 0.04f, 0.04f, R * 1.24f, 0.06f, yLow + R * 1.05f, 0);
  kotak(d, 0.17f, 0.20f, 0.21f, -0.01f, yLow - 0.10f, 0);
  kotak(d, 0.16f, 0.225f, 0.21f, -0.02f, Y_CLAMP, 0);
  kotak(d, 0.05f, PLATE_NO_H, PLATE_NO_W, -0.045f, Y_PLATE_NO, 0);
  // top hood: upper arc leaving the "shoulders" free (θ from 0.55 to π-0.55 in our (y,z) = (sin,cos) frame)
  cylinder(s, {-PG_DISC_FWD - PG_DISC_T / 2 - PG_HOOD_L / 2, PG_Y, 0}, R + 0.004f, PG_HOOD_L, 0, 22, false, 0.55f, PI - 0.55f, true);
  const float xUnit = -PG_DISC_FWD - PG_DISC_T / 2, y0 = PG_Y - PG_PITCH / 2, K = 0.70710678f;
  pengLedPos_.clear(); pengLedLine_.clear();
  pengLedPos_.push_back({xUnit + LENS_FWD, y0, 0}); pengLedLine_.push_back(3);
  auto arm = [&](int line, float uy, float uz, int from, int to) {
    for (int k = from; k <= to; ++k) { if (k == 0) continue; pengLedPos_.push_back({xUnit + LENS_FWD, y0 + uy * PG_PITCH * (float)k, uz * PG_PITCH * (float)k}); pengLedLine_.push_back(line); }
  };
  arm(2, 1, 0, -2, 3); arm(1, K, K, -2, 2); arm(0, 0, 1, -2, 2);
  for (const vec3& p : pengLedPos_) cylinder(s, {xUnit - 0.0225f, p.y, p.z}, PG_LED_D / 2 + 0.008f, 0.045f, 0, 8, false, 0, PI, true);
  disc(led, {0, 0, 0}, PG_LED_D / 2, 10);
  pengDark_ = d.upload(); pengShell_ = s.upload(); pengLed_ = led.upload();
}

void SignalVisuals::buildMeshes() {
  MeshBuilder yellow, dark, lens, sph, bb;
  // mast bands: 0.45 m each, black at the foot (texTiangBuat, V = 0 at the foot reads the black half first)
  int k = 0;
  for (float y = MAST_BOTTOM; y < MAST_TOP - 1e-4f; y += STRIPE, ++k) {
    float y1 = std::min(MAST_TOP, y + STRIPE);
    cylinder(k % 2 ? yellow : dark, {0, (y + y1) / 2, 0}, MAST_D / 2, y1 - y, 1, 10, false);
  }
  disc(lens, {0, 0, 0}, LENS_D / 2, 14);
  sphere(sph, 0.75f, 8);
  bb.quad({-0.5f, -0.5f, 0}, {0.5f, -0.5f, 0}, {0.5f, 0.5f, 0}, {-0.5f, 0.5f, 0});
  { MeshBuilder q; faceQuad(q, PANEL_W, PANEL_H); panel_ = q.upload(); }
  { MeshBuilder q; faceQuad(q, PLATE_NO_W, PLATE_NO_H); plateNo_ = q.upload(); }
  for (int lamps = 2; lamps <= 3; ++lamps) plateTex_[lamps - 2] = makePlateTexture((PLATE_H - (float)(3 - lamps) * LAMP_PITCH) / PLATE_W, lamps);
  buildPengulang();
  buildSemaphoreMeshes();
  mastYellow_ = yellow.upload(); mastDark_ = dark.upload(); lens_ = lens.upload(); sphere_ = sph.upload(); billboard_ = bb.upload();
  lensTex_ = makeLensTexture(); coronaTex_ = makeCoronaTexture();

  yellowMat_ = {}; yellowMat_.baseColor = rgb(0xf2a516); yellowMat_.metallic = 0; yellowMat_.roughness = 0.7f;
  darkMat_ = {}; darkMat_.baseColor = rgb(HITAM); darkMat_.metallic = 0.2f; darkMat_.roughness = 0.75f; darkMat_.emissive = rgb(0x0b0d11).xyz();
  plateMat_ = darkMat_; plateMat_.baseColor = {1, 1, 1, 1};   // the texture carries the paint colour
  angkaMat_ = plateMat_; angkaMat_.emissive = {};
  angkaLitMat_ = {}; angkaLitMat_.baseColor = {1, 1, 1, 1}; angkaLitMat_.unlit = true; angkaLitMat_.alphaMode = AlphaMode::Blend;
  shellMat_ = {}; shellMat_.baseColor = rgb(0x2e343d); shellMat_.metallic = 0.2f; shellMat_.roughness = 0.7f; shellMat_.emissive = rgb(0x0b0d11).xyz(); shellMat_.doubleSided = true;
  unlitMat_ = {}; unlitMat_.baseColor = rgbLin(0x2a2a2a); unlitMat_.unlit = true;
  whiteMat_ = {}; whiteMat_.baseColor = {1, 1, 1, 1}; whiteMat_.unlit = true;
  for (int i = 0; i < 3; ++i) {
    litMat_[i] = {}; litMat_[i].baseColor = rgbLin(WARNA_ASPEK[i]); litMat_[i].unlit = true;
    sphereMat_[i] = {}; sphereMat_[i].baseColor = rgbLin(WARNA_ASPEK[i]); sphereMat_[i].unlit = true;
  }
  coronaMat_ = {}; coronaMat_.unlit = true; coronaMat_.alphaMode = AlphaMode::Blend; coronaMat_.additive = true; coronaMat_.doubleSided = true;
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

void SignalVisuals::build(const TrackGraph& g, const RailProfile& profile, const Json& trackside, const std::string& fontPath) {
  destroy();
  buildMeshes();
  BitmapFont font; if (!fontPath.empty()) font.load(fontPath);
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
    bool muka = si.signalType == "muka" || si.signalType == "pengulang";
    if (si.mechanical) {
      static const std::regex masuk("^(.+?) M[A-Z][0-9]*$");
      si.pivotY = muka ? PIVOT_Y[2] : std::regex_match(si.name, masuk) ? PIVOT_Y[0] : PIVOT_Y[1];
      si.arms = muka ? 1 : std::max(1, std::min(2, t["lengan"].intOr(1)));
    } else {
      si.pengulang = si.signalType == "pengulang";
      bool utama = !muka && si.signalType != "bersama";   // isSinyalUtama
      si.cage = utama;                                     // kandang
      si.shunting = si.signalType == "interlocking";       // langsir
      si.board = si.signalType == "interlocking" && t["papanAngka"].intOr(3) != 0;   // papanAngkaSinyal
      si.angka = (char)('0' + std::abs(t["papanAngka"].intOr(3)) % 10);
      if (si.board && !angkaTex_.count(si.angka)) { angkaTex_[si.angka] = makeAngkaTexture(si.angka); angkaLitTex_[si.angka] = makeAngkaLitTexture(si.angka); }
      auto it = nameTex_.find(si.name);
      if (it == nameTex_.end()) it = nameTex_.emplace(si.name, makeNameTexture(font, si.name)).first;
      si.nameTex = it->second;
    }
    // Lamps present, bottom -> top (reverse of trackside.ts daftarLampu).
    if (si.signalType == "muka") { si.lamps = 2; si.lensAspect[0] = Aspect::Yellow; si.lensAspect[1] = Aspect::Green; }
    else if (t["lampu"].intOr(3) == 2) { si.lamps = 2; si.lensAspect[0] = Aspect::Red; si.lensAspect[1] = Aspect::Green; }
    else { si.lamps = 3; si.lensAspect[0] = Aspect::Red; si.lensAspect[1] = Aspect::Yellow; si.lensAspect[2] = Aspect::Green; }
    if (!si.mechanical && !si.pengulang)
      si.headVariant = headFor((si.lamps == 3 ? 1 : 0) | (si.board ? 2 : 0) | (si.cage ? 4 : 0) | (si.shunting ? 8 : 0));

    TrackSample sm = g.sampleAt(seg, si.s);
    double tx = sm.tx * si.dir, ty = sm.ty * si.dir;
    double sisi = si.left ? -1 : 1;
    double nx = -ty * sisi, ny = tx * sisi;
    float hy = profile.railHeight(si.segId.c_str(), si.s);
    si.railPos = o.toScene(sm.wx, sm.wy, hy);
    si.pos = o.toScene(sm.wx + nx * 3, sm.wy + ny * 3, hy);
    si.yaw = WorldOrigin::yawFromTangent(tx, ty);
    si.world = mat4::translation(si.pos) * mat4::rotationY(si.yaw);
    for (int i = 0; i < si.lamps; ++i)
      si.lensWorld[i] = si.world.transformPoint(si.pengulang ? pengLedPos_[0] : vec3{LENS_X, Y_RED + LAMP_PITCH * (float)i, 0});
    si.pivotWorld = si.world.transformPoint({0, si.pivotY, PIVOT_OUT});
    armTargets(si, si.armTarget); si.armAngle[0] = si.armTarget[0]; si.armAngle[1] = si.armTarget[1];
    signals_.push_back(std::move(si));
  }
  coronaPool_.reserve(signals_.size() * 2);
}

int SignalVisuals::indexOf(const std::string& id) const {
  for (size_t i = 0; i < signals_.size(); ++i) if (signals_[i].id == id) return (int)i;
  return -1;
}

void SignalVisuals::setAspect(const std::string& id, Aspect a) {
  int i = indexOf(id);
  if (i >= 0) signals_[(size_t)i].aspect = a;
}

// silauSinyal + pasangSilauSinyal: two additive billboards (aspect colour, then a smaller white core).
void SignalVisuals::drawCorona(ModelRenderer& r, const SignalInstance& s, vec3 lens, vec3 eye, vec4 colour) const {
  vec3 toCam = eye - lens; float d = length(toCam); if (d < 1e-3f) d = 1e-3f;
  if (d > CORONA_FAR) return;
  float fade = std::min(1.f, (CORONA_FAR - d) / (CORONA_FAR * 0.25f));
  float perPx = 2 * std::tan(fovY_ / 2) / (float)std::max(1, viewportH_);
  vec3 face = s.world.transformDir({-1, 0, 0});
  float al = std::max(0.f, std::min(1.f, dot(toCam, face) / d));
  float scale = std::max(CORONA_M * CORONA_WIDE, CORONA_PX * d * perPx);
  float opacity = (night_ ? 1.f : 0.7f) * (CORONA_SIDE + (1 - CORONA_SIDE) * al * al) * fade;
  vec3 pos = lens + toCam * (std::min(CORONA_FWD, d * 0.5f) / d);
  const float width[2] = {1, 0.34f}, strength[2] = {1, 0.85f};
  for (int k = 0; k < 2; ++k) {
    vec4 c = k == 0 ? colour : vec4{1, 1, 1, 1};
    // The renderer queues blended draws by material pointer, so every corona needs its own Material
    // that outlives draw(): a pool sized at build() (never reallocates during a frame).
    if (coronaPool_.size() == coronaPool_.capacity()) return;
    coronaPool_.push_back(coronaMat_);
    coronaPool_.back().baseColor = {c.x, c.y, c.z, opacity * strength[k]};
    r.drawMesh(billboard_, coronaPool_.back(), coronaTex_, billboard(pos, eye, scale * width[k]));
  }
}

void SignalVisuals::drawHead(ModelRenderer& r, const SignalInstance& s, int lit) const {
  const HeadMesh& h = heads_[(size_t)s.headVariant];
  r.drawMesh(mastYellow_, yellowMat_, {}, s.world);
  r.drawMesh(mastDark_, darkMat_, {}, s.world);
  r.drawMesh(h.dark, darkMat_, {}, s.world);
  r.drawMesh(h.plate, plateMat_, plateTex_[s.lamps - 2], s.world);
  r.drawMesh(h.shell, shellMat_, {}, s.world);
  r.drawMesh(plateNo_, plateMat_, s.nameTex, s.world * mat4::translation({-0.045f - 0.026f, Y_PLATE_NO, 0}));
  if (s.board) {
    float yb = PLATE_BOTTOM + PLATE_H - (float)(3 - s.lamps) * LAMP_PITCH + BOARD_UP;
    auto at = angkaTex_.find(s.angka);
    if (at != angkaTex_.end()) {
      r.drawMesh(panel_, angkaMat_, at->second, s.world * mat4::translation({BOARD_BACK - 0.012f - 0.004f, yb, 0}));
      if (s.angkaLit) if (auto lt = angkaLitTex_.find(s.angka); lt != angkaLitTex_.end()) r.drawMesh(panel_, angkaLitMat_, lt->second, s.world * mat4::translation({BOARD_BACK - 0.016f - 0.004f, yb, 0}));
    }
  }
  for (int i = 0; i < s.lamps; ++i) {
    mat4 m = s.world * mat4::translation({LENS_X, Y_RED + LAMP_PITCH * (float)i, 0});
    r.drawMesh(lens_, i == lit ? litMat_[(int)s.lensAspect[i]] : unlitMat_, lensTex_, m);
  }
  if (s.shunting) {   // shunting lens in the octagon head, always dark
    mat4 m = s.world * mat4::translation({-0.05f - 0.085f, Y_SHUNT, 0}) * mat4::scale({1, SHUNT_LENS_D / LENS_D, SHUNT_LENS_D / LENS_D});
    r.drawMesh(lens_, unlitMat_, lensTex_, m);
  }
}

void SignalVisuals::drawPengulang(ModelRenderer& r, const SignalInstance& s) const {
  r.drawMesh(mastYellow_, yellowMat_, {}, s.world);
  r.drawMesh(mastDark_, darkMat_, {}, s.world);
  r.drawMesh(pengDark_, darkMat_, {}, s.world);
  r.drawMesh(pengShell_, shellMat_, {}, s.world);
  int line = s.aspect == Aspect::Green ? 2 : s.aspect == Aspect::Yellow ? 1 : 0;   // KEDUDUKAN_ASPEK
  for (size_t i = 0; i < pengLedPos_.size(); ++i) {
    bool on = pengLedLine_[i] == 3 || pengLedLine_[i] == line;
    r.drawMesh(pengLed_, on ? whiteMat_ : unlitMat_, lensTex_, s.world * mat4::translation(pengLedPos_[i]));
  }
}

void SignalVisuals::draw(ModelRenderer& r, vec3 eye, const Frustum* frustum) const {
  coronaPool_.clear();
  for (const SignalInstance& s : signals_) {
    float d = length(s.pos - eye);
    bool utama = s.signalType != "muka" && s.signalType != "pengulang" && s.signalType != "bersama";
    vec4 colour = rgbLin(WARNA_ASPEK[(int)s.aspect]);
    // Lit lens: the aspect's lamp if present, else the most restrictive lamp (red, or the bottom one).
    int lit = -1;
    for (int i = 0; i < s.lamps; ++i) if (s.lensAspect[i] == s.aspect) lit = i;
    if (lit < 0) { lit = 0; for (int i = 0; i < s.lamps; ++i) if (s.lensAspect[i] == Aspect::Red) lit = i; }
    if (d > LOD_DISTANCE) {
      if (utama) {
        float sc = std::max(1.f, d / 320.f);
        mat4 m = mat4::translation(s.railPos + vec3{0, 1.2f, 0}) * mat4::scale({sc, sc, sc});
        r.drawMesh(sphere_, sphereMat_[(int)s.aspect], {}, m);
      }
      if (!s.mechanical) drawCorona(r, s, s.lensWorld[lit], eye, s.pengulang ? vec4{1, 1, 1, 1} : colour);
      continue;
    }
    if (s.mechanical) {
      int role = s.pivotY == PIVOT_Y[0] ? 0 : s.pivotY == PIVOT_Y[2] ? 2 : 1;
      if (frustum && !frustum->contains(semBounds_[role].transformed(s.world))) { ++r.culled; continue; }
      drawSemaphore(r, s);
      continue;
    }
    // coronas are placed at every distance (they are what reads from afar), the head only when visible
    drawCorona(r, s, s.lensWorld[lit], eye, s.pengulang ? vec4{1, 1, 1, 1} : colour);
    if (s.pengulang) {
      AABB b{{-0.3f, MAST_BOTTOM, -0.4f}, {0.15f, PG_Y + PG_DISC_D / 2, 0.4f}};
      if (frustum && !frustum->contains(b.transformed(s.world))) { ++r.culled; continue; }
      drawPengulang(r, s);
      continue;
    }
    if (frustum && !frustum->contains(heads_[(size_t)s.headVariant].bounds.transformed(s.world))) { ++r.culled; continue; }
    drawHead(r, s, lit);
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
  for (HeadMesh& h : heads_) { rhi::destroyMesh(h.dark); rhi::destroyMesh(h.shell); rhi::destroyMesh(h.plate); }
  heads_.clear();
  rhi::destroyMesh(panel_); rhi::destroyMesh(plateNo_);
  for (rhi::Texture& t : plateTex_) if (t.id) { rhi::destroyTexture(t); t = {}; }
  for (auto& [k, t] : angkaTex_) rhi::destroyTexture(t); for (auto& [k, t] : angkaLitTex_) rhi::destroyTexture(t); for (auto& [k, t] : nameTex_) rhi::destroyTexture(t);
  angkaTex_.clear(); angkaLitTex_.clear(); nameTex_.clear();
  rhi::destroyMesh(mastYellow_); rhi::destroyMesh(mastDark_); rhi::destroyMesh(lens_); rhi::destroyMesh(sphere_); rhi::destroyMesh(billboard_);
  rhi::destroyMesh(pengDark_); rhi::destroyMesh(pengShell_); rhi::destroyMesh(pengLed_);
  if (lensTex_.id) { rhi::destroyTexture(lensTex_); lensTex_ = {}; }
  if (coronaTex_.id) { rhi::destroyTexture(coronaTex_); coronaTex_ = {}; }
  for (int i = 0; i < 3; ++i) { rhi::destroyMesh(semMast_[i]); rhi::destroyMesh(semMastDark_[i]); }
  rhi::destroyMesh(semLamp_); rhi::destroyMesh(armYellow_); rhi::destroyMesh(armDark_); rhi::destroyMesh(spectacle_); rhi::destroyMesh(glass_);
}

} // namespace eng

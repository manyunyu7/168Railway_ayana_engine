#include "engine/world/krl_station.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>

namespace eng {
using namespace krl;

namespace {

inline vec3 rgbf(unsigned hex) { return {((hex >> 16) & 255) / 255.f, ((hex >> 8) & 255) / 255.f, (hex & 255) / 255.f}; }
inline mat4 rotZ(float a) { mat4 r; float c = std::cos(a), s = std::sin(a); r.m[0][0] = c; r.m[0][1] = s; r.m[1][0] = -s; r.m[1][1] = c; return r; }

// Material palette (WARNA_STASIUN, bahanGedung/bahanPeron; textures replaced by flat colours).
struct Mat { unsigned color; float rough, metal; unsigned emissive = 0; float emissiveK = 0; };
const std::map<std::string, Mat> PALETTE = {
  {"acp", {0xb9bfc4, 0.55f, 0.25f}}, {"beton", {0xa9a49b, 0.95f, 0}}, {"beton-gelap", {0x8d8880, 0.95f, 0}},
  {"baja-putih", {0xe9e9e4, 0.45f, 0.2f}}, {"seng", {0xc4cbd0, 0.6f, 0.15f}}, {"fasia", {0x4b5158, 0.7f, 0}},
  {"kaca", {0x6a7c88, 0.25f, 0.2f}}, {"pagar", {0x7d858c, 0.5f, 0.5f}}, {"huruf", {0xd8281f, 0.6f, 0, 0xd8281f, 0.12f}},
  {"lambang", {0xd8a52a, 0.5f, 0}}, {"aksen", {0x1c4f9c, 0.5f, 0}},
  {"lantai", {0xd6d2ca, 0.92f, 0}}, {"dinding", {0xa8a49c, 0.95f, 0}}, {"talud", {0x9a958c, 0.96f, 0}},
  {"tactile", {0xe0ac10, 0.7f, 0}}, {"putih", {0xe8e6e0, 0.8f, 0}}, {"baja", {0xb2b8bd, 0.55f, 0.2f}},
  {"baja-gelap", {0x76808a, 0.6f, 0.3f}}, {"kursi", {0xd4271f, 0.45f, 0}}, {"oranye", {0xe0761c, 0.5f, 0}},
  {"papan-biru", {0x14459b, 0.55f, 0}}, {"lampu", {0xf6f7f2, 0.4f, 0, 0xfff6dc, 0.55f}}, {"kawat", {0x6e5a48, 0.5f, 0.6f}},
};

// Per-material geometry accumulator (class Rakit in the reference).
struct Rakit {
  std::map<std::string, MeshBuilder> parts;
  MeshBuilder& at(const std::string& k) { return parts[k]; }

  void kotak(const std::string& k, float sx, float sy, float sz, float x, float y, float z, float rx = 0, float ry = 0, float rz = 0) {
    MeshBuilder b; b.box({-sx / 2, -sy / 2, -sz / 2}, {sx / 2, sy / 2, sz / 2});
    mat4 m = mat4::translation({x, y, z});
    if (rx != 0) m = m * mat4::rotationX(rx);
    if (ry != 0) m = m * mat4::rotationY(ry);
    if (rz != 0) m = m * rotZ(rz);
    at(k).append(b, m);
  }
  void silinder(const std::string& k, float d, float h, float x, float y, float z, int sides = 10) {
    MeshBuilder& b = at(k);
    float r = d / 2;
    uint32_t top = b.vertex({x, y + h / 2, z}, {0, 1, 0}, {0.5f, 0.5f}), bot = b.vertex({x, y - h / 2, z}, {0, -1, 0}, {0.5f, 0.5f});
    uint32_t base = (uint32_t)b.vertices.size();
    for (int i = 0; i <= sides; ++i) {
      float a = (float)i / sides * 2 * PI, c = std::cos(a), s = std::sin(a);
      vec3 n{c, 0, s};
      b.vertex({x + r * c, y - h / 2, z + r * s}, n, {(float)i / sides, 0});
      b.vertex({x + r * c, y + h / 2, z + r * s}, n, {(float)i / sides, 1});
    }
    for (int i = 0; i < sides; ++i) {
      uint32_t a = base + (uint32_t)i * 2;
      b.quad(a, a + 1, a + 3, a + 2);
      b.triangle(top, a + 3, a + 1); b.triangle(bot, a, a + 2);
    }
  }
  // Bar of square section t between two points (truss-style box).
  void batang(const std::string& k, vec3 a, vec3 c, float t) {
    vec3 d = c - a; float L = length(d); if (L < 1e-4f) return;
    vec3 u = d / L, bantu = std::fabs(u.y) > 0.9f ? vec3{1, 0, 0} : vec3{0, 1, 0};
    vec3 p = normalize(cross(u, bantu)), q = cross(u, p);
    float h = t / 2; vec3 v[8];
    const float sp[4] = {-1, 1, 1, -1}, sq[4] = {-1, -1, 1, 1};
    for (int e = 0; e < 2; ++e) for (int i = 0; i < 4; ++i) v[e * 4 + i] = (e ? c : a) + (p * sp[i] + q * sq[i]) * h;
    MeshBuilder& b = at(k);
    b.quad(v[0], v[1], v[5], v[4]); b.quad(v[1], v[2], v[6], v[5]); b.quad(v[2], v[3], v[7], v[6]);
    b.quad(v[3], v[0], v[4], v[7]); b.quad(v[4], v[5], v[6], v[7]); b.quad(v[3], v[2], v[1], v[0]);
  }
  // Strip prism: closed band between `top` and `bottom` polylines (same count, planar (a, y)), extruded
  // along an axis from `from` to `to`. axis 2: a = x, extruded along z; axis 0: a = z, extruded along x.
  // Covers every ExtrudeGeometry the reference uses (curved roofs, canopy sheets, bow arches).
  void strip(const std::string& k, const std::vector<vec2>& top, const std::vector<vec2>& bottom, int axis, float from, float to, const std::string* capKey = nullptr) {
    auto P = [&](vec2 v, float w) { return axis == 2 ? vec3{v.x, v.y, w} : vec3{w, v.y, v.x}; };
    MeshBuilder &b = at(k), &cap = at(capKey ? *capKey : k);
    size_t n = top.size();
    for (size_t i = 0; i + 1 < n; ++i) {
      // side surfaces along the extrusion: top face, bottom face
      vec3 t0 = P(top[i], from), t1 = P(top[i + 1], from), t2 = P(top[i + 1], to), t3 = P(top[i], to);
      vec3 b0 = P(bottom[i], from), b1 = P(bottom[i + 1], from), b2 = P(bottom[i + 1], to), b3 = P(bottom[i], to);
      if (axis == 2) { b.quad(t0, t3, t2, t1); b.quad(b0, b1, b2, b3); }
      else { b.quad(t0, t1, t2, t3); b.quad(b0, b3, b2, b1); }
      // caps: quads between top and bottom at each end
      if (axis == 2) { cap.quad(t0, t1, b1, b0); cap.quad(t3, b3, b2, t2); }
      else { cap.quad(t0, b0, b1, t1); cap.quad(t3, t2, b2, b3); }
    }
    // end walls at the first and last profile points
    vec3 e0 = P(top[0], from), e1 = P(bottom[0], from), e2 = P(bottom[0], to), e3 = P(top[0], to);
    vec3 f0 = P(top[n - 1], from), f1 = P(bottom[n - 1], from), f2 = P(bottom[n - 1], to), f3 = P(top[n - 1], to);
    if (axis == 2) { b.quad(e0, e1, e2, e3); b.quad(f0, f3, f2, f1); }
    else { b.quad(e0, e3, e2, e1); b.quad(f0, f1, f2, f3); }
  }
};

float stairLength(float h, bool landing) {
  int n = std::max(2, (int)std::lround(h / RISER));
  return (float)n * TREAD + (landing ? LANDING : 0);
}

struct StairResult { float xEnd, length, slope, y0; };

// Straight stair (+ optional landing) with railings; climbs `h` from (x0, yBase) toward dir·X.
StairResult stair(Rakit& r, float x0, float z, float h, float w, int dir, bool landing, float yBase = 0) {
  int n = std::max(2, (int)std::lround(h / RISER));
  float rise = h / n; int nLanding = landing ? n / 2 : -1;
  float x = x0;
  for (int i = 0; i < n; ++i) {
    float y = yBase + (i + 1) * rise;
    r.kotak("beton", TREAD, rise, w, x + dir * TREAD / 2, y - rise / 2, z);
    x += dir * TREAD;
    if (i == nLanding) { r.kotak("beton", LANDING, 0.22f, w, x + dir * LANDING / 2, y - 0.11f, z); x += dir * LANDING; }
  }
  float len = std::fabs(x - x0), xm = (x0 + x) / 2, slope = (float)dir * std::atan2(h, len);
  r.kotak("beton", std::hypot(len, h), 0.35f, w, xm, yBase + h / 2 - 0.28f, z, 0, 0, slope);
  for (int s = -1; s <= 1; s += 2) {
    float zz = z + s * (w / 2 - 0.06f);
    r.kotak("pagar", std::hypot(len, h), 0.07f, 0.07f, xm, yBase + h / 2 + RAIL_H, zz, 0, 0, slope);
    int nPost = std::max(2, (int)std::lround(len / 1.5f));
    for (int i = 0; i <= nPost; ++i) { float t = (float)i / nPost; r.kotak("pagar", 0.06f, RAIL_H, 0.06f, x0 + dir * len * t, yBase + h * t + RAIL_H / 2, zz); }
  }
  return {x, len, slope, yBase};
}

// Canopy cross-section: height above the platform floor at lateral z (sine, flat at the ridge).
float canopyCurve(float z, float half) {
  float t = std::clamp(z / half, -1.f, 1.f);
  return CANOPY_H - CANOPY_CURVE * (1 - std::cos(t * PI / 2));
}

// Curved sheet spanning the canopy (lembaranLengkung): thickness may vary with |z|/half; along +X from x0.
void curvedSheet(Rakit& r, const std::string& k, float x0, float len, float half, float yBase, float thick0, float thickTaper = 0, float raise = 0) {
  const int n = 14; std::vector<vec2> top, bot;
  for (int i = 0; i <= n; ++i) {
    float z = -half + half * 2 * i / n, y = yBase + canopyCurve(z, half) + raise;
    float t = std::fabs(z) / half;
    top.push_back({z, y}); bot.push_back({z, y - (thick0 - thickTaper * t * t)});
  }
  r.strip(k, top, bot, 0, x0, x0 + len);
}

} // namespace

KrlLayout krlLayout(int tracks, float platformWidth) {
  KrlLayout L; L.tracks = std::clamp(tracks, 1, 12);
  float half = platformWidth / 2 + TRACK_FROM_PLATFORM, z = 0;
  for (int i = 0; i < (L.tracks + 1) / 2; ++i) {
    KrlLayout::Platform p; p.z = z; int a = i * 2 + 1;
    L.trackZ.push_back({z - half, a}); p.trackNumbers.push_back(a);
    if (a + 1 <= L.tracks) { L.trackZ.push_back({z + half, a + 1}); p.trackNumbers.push_back(a + 1); }
    L.platforms.push_back(p);
    z += half * 2 + TRACK_GAP;
  }
  float zMin = 1e9f, zMax = -1e9f;
  for (auto& t : L.trackZ) { zMin = std::min(zMin, t.first); zMax = std::max(zMax, t.first); }
  float shift = -(zMin + zMax) / 2;
  for (auto& t : L.trackZ) t.first += shift;
  for (auto& p : L.platforms) p.z += shift;
  L.halfWidth = (zMax - zMin) / 2;
  return L;
}

void KrlStation::build(const KrlOptions& opt) {
  auto t0 = std::chrono::steady_clock::now();
  destroy();
  Rakit r;
  const float L = opt.boxLength, D = opt.boxDepth, pw = opt.platformWidth;
  layout_ = krlLayout(opt.tracks, pw);
  const float zAcross = layout_.halfWidth + pw / 2 + 3;   // concourse leaves the yard on both ends
  const float yFloor = DECK, wallH = ROOM_H;
  const float zA = -(zAcross + D / 2); hallZ_ = zA;
  auto zc = [&](float dz) { return zA + dz; };

  // ---- receiving hall: columns, floor, walls, glass band ----
  int nCol = std::max(2, (int)std::lround(L / COLUMN_GAP));
  for (int i = 0; i <= nCol; ++i) {
    float x = -L / 2 + L * i / nCol;
    for (float dz : {-D / 2 + 1.2f, D / 2 - 1.2f}) r.silinder("beton", COLUMN_D, yFloor, x, yFloor / 2, zc(dz), 12);
  }
  for (float dz : {-D / 2 + 1.2f, D / 2 - 1.2f}) r.kotak("beton", L + 1, 0.8f, 1, 0, yFloor - 0.4f, zc(dz));
  float yWall = yFloor + wallH / 2;
  r.kotak("acp", L, wallH, 0.28f, 0, yWall, zc(-D / 2)); r.kotak("acp", L, wallH, 0.28f, 0, yWall, zc(D / 2));
  r.kotak("acp", 0.28f, wallH, D, -L / 2, yWall, zc(0)); r.kotak("acp", 0.28f, wallH, D, L / 2, yWall, zc(0));
  r.kotak("beton", L, 0.4f, D, 0, yFloor - 0.2f, zc(0));
  for (int s = -1; s <= 1; s += 2) r.kotak("kaca", L * 0.82f, 1.5f, 0.1f, 0, yFloor + 3.6f, zc(s * (D / 2 + 0.06f)));

  // ---- sweeping roof: quadratic rise from the lip (12.1) to the ridge (14.3), extruded across ----
  {
    float x0 = -L / 2 - ROOF_OVERHANG, x1 = L / 2 + ROOF_OVERHANG, ctrl = ROOF_LIP + (ROOF_RIDGE - ROOF_LIP) * 0.92f;
    auto rise = [&](float x) { float t = std::clamp((x - x0) / (x1 - x0), 0.f, 1.f); return (1 - t) * (1 - t) * ROOF_LIP + 2 * (1 - t) * t * ctrl + t * t * ROOF_RIDGE; };
    const int n = 16; std::vector<vec2> top, bot;
    for (int i = 0; i <= n; ++i) { float x = x0 + (x1 - x0) * i / n; top.push_back({x, rise(x)}); bot.push_back({x, rise(x) - ROOF_THICK}); }
    float depth = D + ROOF_OVERHANG * 2; std::string cap = "fasia";
    r.strip("seng", top, bot, 2, zc(-depth / 2), zc(depth / 2), &cap);
    int nRib = std::max(3, (int)std::lround(L / 3.2f));
    for (int i = 0; i <= nRib; ++i) { float x = x0 + (x1 - x0) * i / nRib; r.kotak("fasia", 0.16f, 0.26f, depth + 0.5f, x, rise(x) - ROOF_THICK - 0.12f, zc(0)); }
  }
  // ---- name plate + logo on the plaza face (-Z) ----
  {
    float zFace = zc(-D / 2) - 0.22f, plateW = std::min(L * 0.62f, 15.f), plateH = plateW / 2;
    r.kotak("huruf", plateW * 0.9f, 0.55f, 0.06f, 0, yFloor + wallH * 0.62f - plateH * 0.1f, zFace);
    r.kotak("lambang", 1.10f, 1.10f, 0.06f, 0, yFloor + wallH * 0.62f + plateH * 0.42f, zFace - 0.02f);
  }
  // ---- ground floor: recessed glass walls between the columns ----
  {
    float in = 1.6f;
    for (int s = -1; s <= 1; s += 2) {
      float z = zc(s * (D / 2 - in));
      r.kotak("kaca", L - 4, 3.4f, 0.2f, 0, 1.75f, z);
      r.kotak("beton", L - 4, 0.5f, 0.35f, 0, 3.65f, z);
      for (float x = -(L - 4) / 2; x <= (L - 4) / 2 + 0.01f; x += 3) r.kotak("beton", 0.18f, 3.4f, 0.3f, x, 1.75f, z);
    }
  }
  // ---- curved front canopy over the drop-off ----
  if (opt.frontCanopy) {
    float zOut = zc(-D / 2) - 7.0f, zIn = zc(-D / 2) - 0.4f, yC = 5.0f, w = std::min(L * 0.55f, 15.f);
    const int n = 10; std::vector<vec2> top, bot;
    for (int i = 0; i <= n; ++i) {
      float t = (float)i / n, z = zIn + (zOut - zIn) * t;
      float yt = (1 - t) * (1 - t) * yC + 2 * (1 - t) * t * (yC + 1.0f) + t * t * (yC - 0.9f);
      top.push_back({z, yt}); bot.push_back({z, yt - 0.22f});
    }
    r.strip("baja-putih", top, bot, 0, -w / 2, w / 2);
    for (int i = -1; i <= 1; ++i) r.kotak("baja-putih", 0.26f, yC - 0.5f, 0.26f, i * w * 0.38f, (yC - 0.5f) / 2, zOut + 0.7f, 7 * PI / 180);
  }
  // ---- outer stairs plaza -> hall with a bowstring arch roof ----
  if (opt.stairs != 0) {
    int dir = opt.stairs > 0 ? -1 : 1;
    float zT = zc(-D / 2) - 3.0f, x0 = dir < 0 ? L / 2 + 12.5f : -L / 2 - 12.5f;
    StairResult t = stair(r, x0, zT, yFloor, STAIR_W_OUT, dir, true);
    r.kotak("beton", 3.4f, 0.32f, 3.6f, t.xEnd + dir * 1.2f, yFloor - 0.16f, zc(-D / 2) - 1.4f);
    float xFoot = x0 - dir * 2.0f, xTop = t.xEnd + dir * 2.0f, yFoot = 0, yMid = ROOF_LIP * 1.02f, yTop = ROOF_LIP - 1.2f;
    auto arch = [&](float x) { float tt = std::clamp((x - xFoot) / (xTop - xFoot), 0.f, 1.f); return (1 - tt) * (1 - tt) * yFoot + 2 * (1 - tt) * tt * yMid + tt * tt * yTop; };
    auto stairY = [&](float x) { float tt = std::clamp((x - x0) / (t.xEnd - x0), 0.f, 1.f); return tt * yFloor; };
    const float DZ = 1.55f; const int nA = 24;
    for (float dz : {-DZ, DZ})
      for (int i = 0; i < nA; ++i) {
        float xa = xFoot + (xTop - xFoot) * i / nA, xb = xFoot + (xTop - xFoot) * (i + 1) / nA;
        r.batang("baja-putih", {xa, arch(xa), zT + dz}, {xb, arch(xb), zT + dz}, 0.34f);
      }
    for (int i = 1; i < 6; ++i) {
      float x = xFoot + (xTop - xFoot) * i / 6, yA = arch(x), yB = stairY(x) + RAIL_H;
      if (yA - yB > 0.6f) for (float dz : {-DZ, DZ}) r.kotak("pagar", 0.09f, yA - yB, 0.09f, x, (yA + yB) / 2, zT + dz);
      r.kotak("pagar", 0.10f, 0.10f, DZ * 2, x, yA - 0.30f, zT);
    }
    std::vector<vec2> top, bot;
    for (int i = 0; i <= 12; ++i) { float x = xFoot + (xTop - xFoot) * i / 12; top.push_back({x, arch(x)}); bot.push_back({x, arch(x) - 0.22f}); }
    r.strip("baja-putih", top, bot, 2, zT - DZ, zT + DZ);
  }
  // ---- concourse: walled waiting hall spanning the yard, columns only on platform axes ----
  const float cw = std::min(L * 0.42f, 11.f);
  if (opt.concourse) {
    float z0 = zc(D / 2), z1 = zAcross, len = z1 - z0, zm = (z0 + z1) / 2;
    r.kotak("beton", cw, 0.5f, len, 0, yFloor - 0.25f, zm);
    for (int s = -1; s <= 1; s += 2) {
      float x = s * cw / 2;
      r.kotak("kaca", 0.14f, 2.0f, len, x, yFloor + 1.4f, zm);
      r.kotak("acp", 0.22f, 1.5f, len, x, yFloor + 3.2f, zm);
      r.kotak("pagar", 0.2f, 0.16f, len, x, yFloor + 0.45f, zm);
    }
    {
      float sh = (cw + 0.9f) / 2, yBase = yFloor + 3.9f;
      auto curve = [&](float x) { return yBase + 0.75f * std::cos(std::clamp(x / sh, -1.f, 1.f) * PI / 2); };
      std::vector<vec2> top, bot;
      for (int i = 0; i <= 12; ++i) { float x = -sh + sh * 2 * i / 12; top.push_back({x, curve(x)}); bot.push_back({x, curve(x) - 0.14f}); }
      r.strip("seng", top, bot, 2, z0, z1);
      int nRib = std::max(2, (int)std::lround(len / 3));
      for (int i = 0; i <= nRib; ++i) { float z = z0 + len * i / nRib; for (float t : {-0.8f, -0.4f, 0.f, 0.4f, 0.8f}) r.kotak("pagar", 0.16f, 0.16f, 0.34f, t * sh, curve(t * sh) - 0.24f, z); }
    }
    for (const KrlLayout::Platform& p : layout_.platforms) {
      for (int dx = -1; dx <= 1; dx += 2) r.silinder("beton", 0.7f, yFloor, dx * (cw / 2 - 0.6f), yFloor / 2, p.z, 12);
      r.kotak("beton", cw, 0.7f, 1.1f, 0, yFloor - 0.6f, p.z);
    }
    for (int dx = -1; dx <= 1; dx += 2) r.silinder("beton", 0.7f, yFloor, dx * (cw / 2 - 0.6f), yFloor / 2, z1 - 1.2f, 12);
    if (opt.platformStairs)
      for (const KrlLayout::Platform& p : layout_.platforms) {
        float h = yFloor - PLATFORM_FLOOR, xTop = cw / 2 + 0.6f, xFoot = xTop + stairLength(h, true);
        StairResult t = stair(r, xFoot, p.z, h, STAIR_W_PLAT, -1, true, PLATFORM_FLOOR);
        r.kotak("beton", 2.6f, 0.35f, 3.0f, xTop - 1.0f, yFloor - 0.17f, p.z);
        r.kotak("seng", std::hypot(t.length, h) + 1.4f, 0.12f, STAIR_W_PLAT + 1.0f, (xTop + xFoot) / 2, PLATFORM_FLOOR + h / 2 + 2.6f, p.z, 0, 0, t.slope);
        openings_.push_back({p.z, xTop - 2.5f, xFoot + 2.0f});
      }
  }

  // ---- island platforms (peron-krl: body, floor, tactile strips, canopy, portals, ends, furniture) ----
  if (opt.platformLength > 0) {
    const float PL = opt.platformLength, yL = PLATFORM_FLOOR, bodyH = yL + PLATFORM_SKIRT, edge = 0.45f, half = (pw + CANOPY_OVERHANG * 2) / 2;
    for (const KrlLayout::Platform& p : layout_.platforms) {
      const float z = p.z;
      r.kotak("talud", PL, bodyH, pw, 0, yL - 0.01f - bodyH / 2, z);
      for (int s = -1; s <= 1; s += 2) r.kotak("dinding", PL, 0.16f, 0.12f, 0, yL - 0.12f, z + s * (pw / 2 + 0.05f));
      r.kotak("lantai", PL, 0.06f, pw - edge * 2, 0, yL - 0.03f, z);
      for (int s = -1; s <= 1; s += 2) {
        r.kotak("dinding", PL, 0.06f, edge, 0, yL - 0.03f, z + s * (pw / 2 - edge / 2));
        r.kotak("tactile", PL, 0.03f, TACTILE_W, 0, yL, z + s * (pw / 2 - TACTILE_FROM_EDGE));
        r.kotak("putih", PL, 0.025f, 0.10f, 0, yL + 0.005f, z + s * (pw / 2 - 0.12f));
      }
      // ramps at both ends (buatUjungPeronKRL), tapering wedge + tactile strips
      for (int e = -1; e <= 1; e += 2) {
        const float P = 7.0f, xb = e * PL / 2; const int n = 10;
        for (int i = 0; i < n; ++i) {
          float t0 = (float)i / n, t1 = (float)(i + 1) / n, ym = yL * (1 - (t0 + t1) / 2), wHere = pw * (1 - 0.45f * (t0 + t1) / 2);
          float xm = xb + e * P * (t0 + t1) / 2;
          r.kotak("lantai", P / n + 0.02f, 0.06f, wHere, xm, ym - 0.03f, z);
          r.kotak("dinding", P / n + 0.02f, std::max(0.08f, ym), wHere + 0.02f, xm, ym / 2 - 0.02f, z);
        }
        float slope = std::atan2(yL, P);
        for (int s = -1; s <= 1; s += 2) r.kotak("tactile", std::hypot(P, yL), 0.03f, TACTILE_W, xb + e * P / 2, yL / 2, z + s * (pw / 2 - TACTILE_FROM_EDGE) * 0.75f, 0, 0, -e * slope);
        r.kotak("beton-gelap", 0.3f, 0.5f, pw * 0.5f, xb + e * (P - 0.15f), 0.25f, z);
      }
      // canopy sheet in runs between the openings; gutters with the blue accent plate
      auto inOpening = [&](float x0, float x1) { for (const Opening& o : openings_) if (std::fabs(o.z - z) < 0.5f && x1 > o.x0 && x0 < o.x1) return true; return false; };
      std::vector<std::pair<float, float>> runs; float xr = -PL / 2;
      while (xr < PL / 2 - 0.01f) {
        float xe = std::min(PL / 2, xr + 6.f);
        if (!inOpening(xr, xe)) { if (!runs.empty() && std::fabs(runs.back().second - xr) < 0.01f) runs.back().second = xe; else runs.push_back({xr, xe}); }
        xr = xe;
      }
      for (auto [a, b] : runs) {
        Rakit local; curvedSheet(local, "seng", a, b - a, half, yL, 0.08f);
        r.at("seng").append(local.parts["seng"], mat4::translation({0, 0, z}));
        for (float t : {-0.82f, -0.45f, 0.f, 0.45f, 0.82f}) r.kotak("baja", b - a, 0.1f, 0.1f, (a + b) / 2, yL + canopyCurve(t * half, half) - 0.14f, z + t * half);
        float yEdge = yL + canopyCurve(half, half);
        for (int s = -1; s <= 1; s += 2) {
          r.kotak("aksen", b - a, 0.28f, 0.05f, (a + b) / 2, yEdge - 0.06f, z + s * (half + 0.12f));
          r.kotak("baja", b - a, 0.05f, 0.26f, (a + b) / 2, yEdge - 0.19f, z + s * (half + 0.02f));
        }
      }
      // portals every 6 m: base, collar, post, tapered cantilever arm, head plate, lamp, downpipe
      for (float x = -PL / 2 + 3; x < PL / 2 - 2; x += POST_GAP) {
        if (inOpening(x - 0.3f, x + 0.3f)) continue;
        float peak = yL + CANOPY_H;
        r.kotak("beton-gelap", POST_BASE, POST_BASE_H, POST_BASE, x, yL + POST_BASE_H / 2, z);
        r.kotak("baja-gelap", 0.48f, 0.035f, 0.48f, x, yL + POST_BASE_H + 0.017f, z);
        r.silinder("aksen", POST_D + 0.16f, 0.34f, x, yL + POST_BASE_H + 0.19f, z, 12);
        r.silinder("baja", POST_D, CANOPY_H, x, yL + CANOPY_H / 2, z, 12);
        Rakit arm; curvedSheet(arm, "baja", -0.11f, 0.22f, half, yL, 0.62f, 0.46f, -0.10f);
        r.at("baja").append(arm.parts["baja"], mat4::translation({x, 0, z}));
        r.kotak("baja", 0.34f, 0.12f, 1.5f, x, peak - 0.06f, z);
        r.kotak("lampu", 0.12f, 0.09f, 1.35f, x, peak - 0.5f, z);
        r.kotak("baja-gelap", 0.16f, 0.05f, 1.45f, x, peak - 0.42f, z);
        r.silinder("baja", 0.09f, CANOPY_H - 0.5f, x + 0.24f, yL + (CANOPY_H - 0.5f) / 2, z, 8);
      }
      // furniture every 18 m: back-to-back red seats, bin, hanging track-number board, name board
      for (float x = -PL / 2 + 9; x < PL / 2 - 6; x += FURNITURE_GAP) {
        if (inOpening(x - 1, x + 6)) continue;
        const float G = 3.2f;
        for (int s = -1; s <= 1; s += 2) {
          float zz = z + s * 0.40f;
          for (float xs : {-0.93f, -0.31f, 0.31f, 0.93f}) {
            float xk = x + xs + G;
            r.kotak("kursi", 0.54f, 0.07f, 0.48f, xk, yL + 0.45f, zz);
            r.kotak("kursi", 0.54f, 0.11f, 0.05f, xk, yL + 0.40f, zz - s * 0.24f);
            r.kotak("kursi", 0.54f, 0.40f, 0.06f, xk, yL + 0.64f, z + s * 0.655f, s * 10 * PI / 180);
            for (int d = -1; d <= 1; d += 2) { r.kotak("baja-gelap", 0.05f, 0.05f, 0.44f, xk + d * 0.30f, yL + 0.62f, zz); r.kotak("baja-gelap", 0.05f, 0.17f, 0.05f, xk + d * 0.30f, yL + 0.53f, zz - s * 0.21f); }
          }
          r.kotak("baja-gelap", 2.4f, 0.07f, 0.07f, x + G, yL + 0.38f, zz);
          for (float xs : {-1.05f, 1.05f}) r.kotak("baja-gelap", 0.07f, 0.42f, 0.07f, x + xs + G, yL + 0.21f, zz);
        }
        r.silinder("oranye", 0.52f, 0.78f, x + G + 1.9f, yL + 0.39f, z - 0.9f, 12);
        r.silinder("baja-gelap", 0.56f, 0.07f, x + G + 1.9f, yL + 0.8f, z - 0.9f, 12);
        float side = 0.95f, yB = yL + 2.55f + side / 2;
        r.kotak("papan-biru", side, side, 0.22f, x, yB, z + 1.9f);
        for (int s = -1; s <= 1; s += 2) r.kotak("baja-gelap", 0.05f, CANOPY_H - 2.55f - side, 0.05f, x + s * side * 0.35f, yB + side / 2 + 0.4f, z + 1.9f);
        r.kotak("putih", 0.30f, 1.60f, 0.03f, x, yL + 2.5f, z + 0.22f);
      }
    }
    // ---- LAA 1500 V DC: contact wire 5.0 m + messenger 6.2 m per track, portal gantries every 48 m ----
    if (opt.laa) {
      const float wl = PL + 40, zMast = layout_.halfWidth + 3.2f, yBeam = 7.2f;
      for (const auto& t : layout_.trackZ) {
        r.kotak("kawat", wl, 0.014f, 0.014f, 0, LAA_WIRE, t.first);
        r.kotak("kawat", wl, 0.012f, 0.012f, 0, 6.2f, t.first);
      }
      for (float x = -wl / 2 + 8; x < wl / 2 - 4; x += 48) {
        for (int s = -1; s <= 1; s += 2) r.kotak("baja-gelap", 0.30f, yBeam + 0.4f, 0.30f, x, (yBeam + 0.4f) / 2, s * zMast);
        r.kotak("baja-gelap", 0.25f, 0.35f, zMast * 2 + 0.3f, x, yBeam, 0);
        for (const auto& t : layout_.trackZ) r.kotak("baja-gelap", 0.04f, yBeam - 6.2f, 0.04f, x, (yBeam + 6.2f) / 2, t.first);
      }
    }
  }

  // ---- upload per material ----
  bounds = {};
  for (auto& [key, mb] : r.parts) {
    if (mb.empty()) continue;
    Part p; p.mat = {}; p.mat.name = key;
    auto it = PALETTE.find(key); const Mat m = it != PALETTE.end() ? it->second : Mat{0xffffff, 0.8f, 0};
    vec3 c = rgbf(m.color); p.mat.baseColor = {c.x, c.y, c.z, 1}; p.mat.roughness = m.rough; p.mat.metallic = m.metal;
    if (m.emissiveK > 0) p.mat.emissive = rgbf(m.emissive) * m.emissiveK;
    p.mesh = mb.upload(); p.bounds = mb.bounds; bounds.expand(mb.bounds);
    stats.tris += (uint32_t)(mb.indices.size() / 3);
    parts_.push_back(p);
  }
  stats.parts = (int)parts_.size();
  stats.buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void KrlStation::draw(ModelRenderer& r, const mat4& xf, const Frustum* frustum) const {
  if (frustum && !frustum->contains(bounds.transformed(xf))) { ++r.culled; return; }
  for (const Part& p : parts_) r.drawMesh(p.mesh, p.mat, {}, xf);
}

void KrlStation::destroy() {
  for (Part& p : parts_) rhi::destroyMesh(p.mesh);
  parts_.clear(); openings_.clear(); stats = {}; bounds = {};
}

} // namespace eng

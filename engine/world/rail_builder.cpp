#include "engine/world/rail_builder.h"
#include "engine/asset/emod.h"
#include "engine/core/json.h"
#include "engine/render/mesh_builder.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <queue>
#include <sstream>

namespace eng {

namespace {

// Cross-section constants (dunia3dKonst.ts:296-322, uji3dJembatan.ts:28-40).
constexpr float REL_TAPAK = -0.176f, REL_L_DALAM = 0.534f, REL_L_LUAR = 0.618f, BALAS_KAKI = -0.580f;
constexpr float U_REL_BADAN = 0.775f, U_REL_KEPALA0 = 0.800f, U_REL_KEPALA1 = 0.822f;
constexpr float DEK_TEPI = 3.4f, DEK_TEBAL = 1.4f, PARAPET_T = 1.05f, PARAPET_TEBAL = 0.18f;
constexpr float PILAR_JARAK = 28, PILAR_SISI = 1.9f, DEK_BAWAH = BALAS_KAKI - DEK_TEBAL;
// Bridge shape presets + Warren truss (uji3dJembatan.ts:246-330).
constexpr float RANGKA_BENTANG_MIN = 50, RANGKA_TINGGI_MIN = 8, VIADUK_TINGGI_MIN = 25;
constexpr float RANGKA_T = 7.0f, RANGKA_PANEL = 8, BATANG_T = 0.34f, BATANG_D = 0.24f;
constexpr float TRW_TEPI = 2.45f, TRW_SPRING = 2.10f, TRW_RAMP = 45, JBT_RAMP = 14;
constexpr float JARAK_BANTALAN = 0.6f;
constexpr int TEX_N = 512;
constexpr float uLat(float lat) { return 0.36f + lat * 0.1042f; }
constexpr float uLatBalas(float lat) { return uLat(std::max(-1.795f, std::min(1.795f, lat))); }   // stay inside the stone band
// Ballast section (engine additions over PROFIL_BALAS): the shoulder / foot wobble with world-keyed noise,
// a buried "skirt" carries the stone out to ~2.5 m and under the carved plateau so the ballast-terrain seam
// is a wavy, dark, buried edge instead of a straight step, and parallel tracks within BADAN_MAX share one
// bed (inner slopes cut at the shoulder, a flat crib strip between the shoulders — KAI double track sits at
// 4.0–4.5 m centres, where two 3.59 m trapezoids would otherwise interpenetrate).
constexpr float BAHU_LAT = 1.117f, BAHU_H = -0.243f, KAKI_LAT = 1.795f;
constexpr float SKIRT_LAT = 2.45f, SKIRT_H = BALAS_KAKI - 0.16f;   // plateau is BALAS_KAKI - 0.04: the skirt edge is buried
constexpr float SKIRT_SHADE = 0.50f, KAKI_SHADE = 0.82f;           // dirt-stained edge
constexpr float BADAN_MIN = 2.3f, BADAN_MAX = 6.0f;                // neighbour axis spacing counted as one bed (m)
constexpr float TRW_GELAP = 40, TRW_SHADE = 0.08f;                 // full interior darkness this far past a portal
constexpr size_t BALAS_N = 8;                                      // ring points: skirt foot shoulder rail | rail shoulder foot skirt

float smoothstep01(float t) { t = std::max(0.f, std::min(1.f, t)); return t * t * (3 - 2 * t); }
// Value noise in [-1, 1] keyed on scene x/z so neighbouring segments, chunk runs and parallel chains agree.
float hash2(int x, int z) {
  uint32_t h = (uint32_t)x * 374761393u + (uint32_t)z * 668265263u;
  h = (h ^ (h >> 13)) * 1274126177u; h ^= h >> 16;
  return (float)h / 4294967295.f * 2 - 1;
}
float noise2(float x, float z) {
  float fx = std::floor(x), fz = std::floor(z), tx = smoothstep01(x - fx), tz = smoothstep01(z - fz);
  int ix = (int)fx, iz = (int)fz;
  float a = hash2(ix, iz), b = hash2(ix + 1, iz), c = hash2(ix, iz + 1), d = hash2(ix + 1, iz + 1);
  return (a + (b - a) * tx) * (1 - tz) + (c + (d - c) * tx) * tz;
}

struct PP { float lat, h, u; };
struct Pt { float x, y, z, tx, tz, s; };

const PP PROFIL_REL_KIRI[] = {
  {-REL_L_LUAR, REL_TAPAK, U_REL_BADAN}, {-REL_L_LUAR, 0, U_REL_BADAN}, {-REL_L_LUAR, 0, U_REL_KEPALA0},
  {-REL_L_DALAM, 0, U_REL_KEPALA1}, {-REL_L_DALAM, 0, U_REL_BADAN}, {-REL_L_DALAM, REL_TAPAK, U_REL_BADAN},
};
const PP PROFIL_REL_KANAN[] = {
  {+REL_L_DALAM, REL_TAPAK, U_REL_BADAN}, {+REL_L_DALAM, 0, U_REL_BADAN}, {+REL_L_DALAM, 0, U_REL_KEPALA1},
  {+REL_L_LUAR, 0, U_REL_KEPALA0}, {+REL_L_LUAR, 0, U_REL_BADAN}, {+REL_L_LUAR, REL_TAPAK, U_REL_BADAN},
};

std::vector<PP> profilDek(float latMin, float latMax, float kaki) {
  float bawah = kaki - DEK_TEBAL;
  return {{latMin, bawah, 0}, {latMin, kaki - 0.02f, 0.1f}, {latMax, kaki - 0.02f, 0.9f}, {latMax, bawah, 1}};
}
std::vector<PP> profilDekBawah(float latMin, float latMax, float kaki) {
  float bawah = kaki - DEK_TEBAL;
  return {{latMax, bawah, 1}, {latMin, bawah, 0}};
}
std::vector<PP> profilParapet(float lat, int sisi, float kaki) {
  float dalam = lat - (float)sisi * PARAPET_TEBAL;
  if (sisi < 0) return {{lat, kaki, 0}, {lat, PARAPET_T, 0.3f}, {dalam, PARAPET_T, 0.7f}, {dalam, kaki, 1}};
  return {{dalam, kaki, 1}, {dalam, PARAPET_T, 0.7f}, {lat, PARAPET_T, 0.3f}, {lat, kaki, 0}};
}
std::vector<PP> profilTerowongan(float latMin, float latMax, float kaki, bool toCentre, float skala = 1) {
  float pusat = (latMin + latMax) / 2, r = ((latMax - latMin) / 2 - (DEK_TEPI - TRW_TEPI)) * skala;
  float lantai = kaki - 0.3f, spring = TRW_SPRING * skala;
  std::vector<PP> t{{pusat - r, lantai, 0}};
  const int N = 14;
  for (int i = 0; i <= N; ++i) {
    float th = PI * (1 - (float)i / N);
    t.push_back({pusat + r * std::cos(th), spring + r * std::sin(th), 0.1f + 0.8f * (float)i / N});
  }
  t.push_back({pusat + r, lantai, 1});
  if (toCentre) std::reverse(t.begin(), t.end());
  return t;
}
std::vector<PP> profilTerowonganLantai(float latMin, float latMax, float kaki) {
  float pusat = (latMin + latMax) / 2, r = (latMax - latMin) / 2 - (DEK_TEPI - TRW_TEPI);
  return {{pusat - r, kaki - 0.3f, 0}, {pusat + r, kaki - 0.3f, 1}};
}


BridgeShape bentukJembatan(float bentang, float tinggi) {
  if (tinggi >= VIADUK_TINGGI_MIN) return BridgeShape::Viaduct;
  if (bentang >= RANGKA_BENTANG_MIN && tinggi >= RANGKA_TINGGI_MIN) return BridgeShape::Truss;
  return BridgeShape::Deck;
}

// Truss member in (lateral, height rel. rail head, chainage) space.
struct Batang { float aLat, aY, aS, bLat, bY, bS, t; };

// Warren through-truss: bottom/top chords, alternating diagonals, verticals, top bracing + portals
// (uji3dJembatan.ts rangkaBatang). The train runs inside; the top chord sits at RANGKA_T = 7 m.
std::vector<Batang> rangkaBatang(float panjang, float latMin, float latMax, float kaki) {
  std::vector<Batang> out;
  int n = std::max(2, (int)std::lround(panjang / RANGKA_PANEL));
  float p = panjang / (float)n;
  const float sisi[2] = {latMin + 0.35f, latMax - 0.35f};
  const float bawah = kaki + 0.1f, atas = RANGKA_T;
  for (float lat : sisi) {
    for (int i = 0; i < n; ++i) {
      float s0 = (float)i * p, s1 = (float)(i + 1) * p;
      out.push_back({lat, bawah, s0, lat, bawah, s1, BATANG_T});
      out.push_back({lat, atas, s0, lat, atas, s1, BATANG_T});
      if (i % 2 == 0) out.push_back({lat, bawah, s0, lat, atas, s1, BATANG_D});
      else out.push_back({lat, atas, s0, lat, bawah, s1, BATANG_D});
    }
    for (int i = 0; i <= n; ++i) out.push_back({lat, bawah, (float)i * p, lat, atas, (float)i * p, BATANG_D});
  }
  for (int i = 0; i <= n; ++i) out.push_back({sisi[0], atas, (float)i * p, sisi[1], atas, (float)i * p, BATANG_D});
  for (int i = 0; i < n; i += 2) {
    float s0 = (float)i * p, s1 = std::min(panjang, (float)(i + 1) * p);
    out.push_back({sisi[0], atas, s0, sisi[1], atas, s1, 0.14f});
  }
  return out;
}

// Sample interpolated at chainage s (bangun3d.ts titikDiS).
Pt titikDiS(const std::vector<Pt>& pts, float s) {
  float L = pts.back().s, t = std::max(0.f, std::min(L, s));
  size_t i = 1;
  while (i < pts.size() - 1 && pts[i].s < t) ++i;
  const Pt &a = pts[i - 1], &b = pts[i];
  float f = b.s > a.s ? (t - a.s) / (b.s - a.s) : 0;
  return {a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f, a.z + (b.z - a.z) * f, a.tx, a.tz, t};
}

// Square-section bar between two points (bangun3d.ts batangKotak): 8 vertices, 6 quads.
void batangKotak(MeshBuilder& b, vec3 a, vec3 c, float t) {
  vec3 d = c - a; float L = length(d);
  if (L < 1e-4f) return;
  vec3 u = d / L;
  vec3 bantu = std::fabs(u.y) > 0.9f ? vec3{1, 0, 0} : vec3{0, 1, 0};
  vec3 p = normalize(cross(u, bantu)), q = cross(u, p);
  float h = t / 2;
  vec3 v[8];
  const float sp[4] = {-1, 1, 1, -1}, sq[4] = {-1, -1, 1, 1};
  for (int e = 0; e < 2; ++e)
    for (int k = 0; k < 4; ++k) v[e * 4 + k] = (e ? c : a) + (p * sp[k] + q * sq[k]) * h;
  b.quad(v[0], v[1], v[5], v[4]); b.quad(v[1], v[2], v[6], v[5]);
  b.quad(v[2], v[3], v[7], v[6]); b.quad(v[3], v[0], v[4], v[7]);
  b.quad(v[4], v[5], v[6], v[7]); b.quad(v[3], v[2], v[1], v[0]);
}

// Extrude a (lat, height, u) profile along rings; analytic normals from the profile edges. The profile may
// vary per ring (`perRing`: prof holds n*P points) and every vertex may carry a brightness (`shade`, n*P,
// or n values per ring when `shadePerRing`).
void extrudeVar(MeshBuilder& b, const Pt* pts, size_t n, const PP* prof, size_t P, float texLen, bool perRing,
                const float* shade, bool shadePerRing) {
  if (n < 2 || P < 2) return;
  uint32_t base = (uint32_t)b.vertices.size();
  for (size_t i = 0; i < n; ++i) {
    const Pt& p = pts[i];
    const PP* pr = perRing ? prof + i * P : prof;
    float nx = -p.tz, nz = p.tx, v = p.s / texLen;
    for (size_t j = 0; j < P; ++j) {
      const PP &q0 = pr[j > 0 ? j - 1 : 0], &q1 = pr[j + 1 < P ? j + 1 : P - 1];
      float dl = q1.lat - q0.lat, dh = q1.h - q0.h, l = std::hypot(dl, dh); if (l <= 0) l = 1;
      float nl = -dh / l, nh = dl / l;
      vec3 pos{p.x + nx * pr[j].lat, p.y + pr[j].h, p.z + nz * pr[j].lat}, nrm{nx * nl, nh, nz * nl}; vec2 uv{pr[j].u, v};
      if (shade) b.vertex(pos, nrm, uv, shadePerRing ? shade[i] : shade[i * P + j]);
      else b.vertex(pos, nrm, uv);
    }
  }
  for (size_t i = 0; i + 1 < n; ++i)
    for (size_t j = 0; j + 1 < P; ++j) {
      uint32_t a = base + (uint32_t)(i * P + j), c = a + (uint32_t)P;
      b.triangle(a, a + 1, c); b.triangle(a + 1, c + 1, c);
    }
}
void extrude(MeshBuilder& b, const Pt* pts, size_t n, const PP* prof, size_t P, float texLen, const float* ringShade = nullptr) {
  extrudeVar(b, pts, n, prof, P, texLen, false, ringShade, true);
}

// Nearest parallel track on one side of a ring (lateral axis spacing, rail-head height difference).
struct Neighbour { bool has = false; float lat = 0, dy = 0; size_t seg = 0; };
struct RingCtx { Neighbour left, right; float shade = 1; bool jitter = true, skirt = true; };

// One ring of the ballast section (BALAS_N points, increasing lat) with its per-point shade.
void profilBalasRing(const Pt& p, const RingCtx& c, PP* out, float* shade) {
  float nx = -p.tz, nz = p.tx;
  auto side = [&](float sgn, const Neighbour& nb, PP& skirt, PP& foot, PP& sh, float& sSkirt, float& sFoot, float& sSh) {
    sSh = c.shade;
    if (nb.has) {   // shared bed: cut at the shoulder (the crib strip continues from here)
      sh = {sgn * BAHU_LAT, BAHU_H, uLatBalas(sgn * BAHU_LAT)};
      foot = {sgn * (BAHU_LAT + 0.02f), BAHU_H, sh.u}; skirt = {sgn * (BAHU_LAT + 0.04f), BAHU_H, sh.u};
      sFoot = sSkirt = c.shade;
      return;
    }
    float n1 = 0, n2 = 0;
    if (c.jitter) {
      float wx = p.x + nx * sgn * KAKI_LAT, wz = p.z + nz * sgn * KAKI_LAT;
      n1 = noise2(wx * 0.31f, wz * 0.31f); n2 = noise2(wx * 0.23f + 37.1f, wz * 0.23f - 91.7f);
    }
    float shLat = sgn * (BAHU_LAT + 0.10f * n1), shH = BAHU_H + 0.03f * n2;
    float ftLat = sgn * (KAKI_LAT + 0.22f * n2), ftH = BALAS_KAKI + 0.04f * n1;
    // foot / skirt keep the unclamped u (0.55..0.68, still stone in both atlases) so the edge is not streaked
    sh = {shLat, shH, uLatBalas(shLat)}; foot = {ftLat, ftH, uLat(ftLat)};
    sFoot = c.shade * (c.skirt ? KAKI_SHADE : 1.f);
    if (c.skirt) { float skLat = sgn * (SKIRT_LAT + 0.45f * n1); skirt = {skLat, SKIRT_H, uLat(skLat)}; sSkirt = c.shade * SKIRT_SHADE; }
    else { skirt = {ftLat + sgn * 0.02f, ftH, foot.u}; sSkirt = sFoot; }
  };
  out[3] = {-REL_L_LUAR, REL_TAPAK, uLat(-REL_L_LUAR)}; out[4] = {+REL_L_LUAR, REL_TAPAK, uLat(+REL_L_LUAR)};
  shade[3] = shade[4] = c.shade;
  side(-1, c.left, out[0], out[1], out[2], shade[0], shade[1], shade[2]);
  side(+1, c.right, out[7], out[6], out[5], shade[7], shade[6], shade[5]);
}
// Crib strip between two shoulders (3 points, increasing lat). `nb` is the neighbour on side sgn.
void profilStrip(float sgn, const Neighbour& nb, PP* out) {
  float mine = sgn * BAHU_LAT, theirs = nb.lat - sgn * BAHU_LAT, mid = (mine + theirs) / 2;
  PP a{mine, BAHU_H, uLat(BAHU_LAT)}, m{mid, BAHU_H / 2 + (nb.dy + BAHU_H) / 2 - 0.02f, uLat(1.55f)}, b2{theirs, nb.dy + BAHU_H, uLat(BAHU_LAT)};
  if (sgn > 0) { out[0] = a; out[1] = m; out[2] = b2; } else { out[0] = b2; out[1] = m; out[2] = a; }
}

// Rail distance from every node to the nearest tunnel mouth, capped (uji3dJembatan.ts jarakKeMulut).
// inside = false walks the open track away from the mouths; true walks the tunnel segments into the hill.
std::map<int, double> mouthDistances(const TrackGraph& g, double maxD, bool inside = false) {
  std::map<int, double> out;
  std::priority_queue<std::pair<double, int>, std::vector<std::pair<double, int>>, std::greater<>> pq;
  for (size_t ni = 0; ni < g.nodes.size(); ++ni) {
    bool tun = false, ground = false;
    for (int si : g.nodes[ni].segs) (g.segments[(size_t)si].kind == RailKind::Tunnel ? tun : ground) = true;
    if (tun && ground) { out[(int)ni] = 0; pq.push({0, (int)ni}); }
  }
  while (!pq.empty()) {
    auto [d, n] = pq.top(); pq.pop();
    if (d > out[n]) continue;
    for (int si : g.nodes[(size_t)n].segs) {
      const TrackSegment& s = g.segments[(size_t)si];
      if ((s.kind == RailKind::Tunnel) != inside) continue;
      int o = g.otherNode(si, n); double nd = d + s.length;
      if (nd > maxD) continue;
      auto it = out.find(o);
      if (it == out.end() || nd < it->second) { out[o] = nd; pq.push({nd, o}); }
    }
  }
  return out;
}

} // namespace

void RailBuilder::paintTexture() {
  std::vector<uint8_t> px((size_t)TEX_N * TEX_N * 4);
  auto fill = [&](int x0, int y0, int w, int h, uint32_t rgb) {
    x0 = std::max(0, x0); y0 = std::max(0, y0);
    int x1 = std::min(TEX_N, x0 + w), y1 = std::min(TEX_N, y0 + h);
    for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) {
      uint8_t* p = &px[((size_t)y * TEX_N + (size_t)x) * 4];
      p[0] = (uint8_t)(rgb >> 16); p[1] = (uint8_t)(rgb >> 8); p[2] = (uint8_t)rgb; p[3] = 255;
    }
  };
  fill(0, 0, TEX_N, TEX_N, 0x4b463f);
  uint32_t seed = 0x9e3779b9u;
  auto rnd = [&]() { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; };
  for (int i = 0; i < 26000; ++i) fill((int)(rnd() % TEX_N), (int)(rnd() % TEX_N), 2, 2, i % 2 ? 0x5a544a : 0x3d3933);
  const float PX_PER_M = (float)TEX_N / TEX_LENGTH;
  int x0 = (int)(uLat(-0.75f) * TEX_N), lebar = (int)((uLat(0.75f) - uLat(-0.75f)) * TEX_N);
  int tebal = (int)std::lround(0.26f * PX_PER_M);
  for (float m = 0; m < TEX_LENGTH; m += JARAK_BANTALAN) {
    int y = (int)(m * PX_PER_M);
    fill(x0, y, lebar, tebal, 0x3a3027);
    fill(x0, y, lebar, 2, 0x463a2e);
  }
  fill((int)(0.75f * TEX_N), 0, (int)(0.19f * TEX_N), TEX_N, 0x4a3527);
  int gx0 = (int)(U_REL_KEPALA0 * TEX_N), gx1 = (int)(U_REL_KEPALA1 * TEX_N);
  const uint32_t stops[3] = {0x6b6560, 0xb8b2ab, 0x7a736c};
  for (int x = gx0; x < gx1; ++x) {
    float t = (float)(x - gx0) / (float)std::max(1, gx1 - gx0 - 1) * 2;
    int k = t < 1 ? 0 : 1; float f = t - (float)k;
    uint32_t c = 0;
    for (int ch = 0; ch < 3; ++ch) {
      float a = (float)((stops[k] >> (16 - 8 * ch)) & 255), b2 = (float)((stops[k + 1] >> (16 - 8 * ch)) & 255);
      c |= (uint32_t)std::lround(a + (b2 - a) * f) << (16 - 8 * ch);
    }
    fill(x, 0, 1, TEX_N, c);
  }
  texture = rhi::createTexture(TEX_N, TEX_N, rhi::Format::RGBA8, std::as_bytes(std::span(px)), true, true);
}

void RailBuilder::build(const TrackGraph& g, const RailProfile& profile, const HeightSource* ground, float demBase) {
  auto t0 = std::chrono::steady_clock::now();
  rhi::Texture keep = texture; texture = {};   // an atlas set before build() (web: eng_rail_atlas_rgba) survives
  destroy();
  if (keep.id) texture = keep; else paintTexture();
  ballastMat = {}; ballastMat.name = "ballast"; ballastMat.metallic = 0; ballastMat.roughness = 1;
  railMat = {}; railMat.name = "rail"; railMat.metallic = 0.3f; railMat.roughness = 0.42f;
  concreteMat = {}; concreteMat.name = "concrete"; concreteMat.baseColor = {0x9a / 255.f, 0xa0 / 255.f, 0xa6 / 255.f, 1}; concreteMat.roughness = 0.92f; concreteMat.metallic = 0.02f;
  steelMat = {}; steelMat.name = "truss"; steelMat.baseColor = {0x5c / 255.f, 0x6b / 255.f, 0x5a / 255.f, 1}; steelMat.roughness = 0.68f; steelMat.metallic = 0.35f;
  tunnelMat = {}; tunnelMat.name = "tunnel"; tunnelMat.baseColor = {0x4a / 255.f, 0x4d / 255.f, 0x52 / 255.f, 1}; tunnelMat.roughness = 0.98f; tunnelMat.metallic = 0; tunnelMat.doubleSided = true;

  const WorldOrigin& o = g.origin();
  struct Builders { MeshBuilder ballast, rails, bridge, tunnel, truss; };
  std::map<std::pair<int, int>, Builders> cells;
  auto cellOf = [](const Pt& p) { return std::pair<int, int>{(int)std::floor(p.x / CHUNK), (int)std::floor(p.z / CHUNK)}; };
  std::map<int, double> mouth = mouthDistances(g, TRW_RAMP);
  stats_ = {};

  // Parallel double-track structures share one deck/tube: partner within 9 m laterally and
  // |cos| >= 0.985 at the segment midpoints; the lowest-index member is the host and builds it.
  struct Pair { bool host = true; float latMin = -DEK_TEPI, latMax = DEK_TEPI; };
  std::vector<Pair> pairs(g.segments.size());
  {
    struct Mid { size_t si; double x, z, tx, tz; };
    std::vector<Mid> mids;
    for (size_t si = 0; si < g.segments.size(); ++si) {
      const TrackSegment& seg = g.segments[si];
      if (seg.kind == RailKind::Ground || seg.length <= 0) continue;
      TrackSample sm = g.sampleAt((int)si, seg.length / 2);
      mids.push_back({si, sm.wx - o.ox, sm.wy - o.oz, sm.tx, sm.ty});
    }
    for (const Mid& a : mids)
      for (const Mid& b : mids) {
        if (a.si == b.si || g.segments[a.si].kind != g.segments[b.si].kind) continue;
        if (std::fabs(a.tx * b.tx + a.tz * b.tz) < 0.985) continue;
        double dx = b.x - a.x, dz = b.z - a.z;
        double lat = dx * (-a.tz) + dz * a.tx, along = dx * a.tx + dz * a.tz;
        if (std::fabs(lat) > 9 || std::fabs(along) > (g.segments[a.si].length + g.segments[b.si].length) / 2) continue;
        Pair& p = pairs[a.si];
        p.latMin = std::min(p.latMin, (float)lat - DEK_TEPI); p.latMax = std::max(p.latMax, (float)lat + DEK_TEPI);
        if (b.si < a.si) p.host = false;
      }
  }

  // Span of the whole structure chain (same kind, connected through nodes) — the shape preset judges
  // the real bridge, not one OSM piece of it (bangun3d.ts bentangStruktur).
  auto chainSpan = [&](size_t s0) {
    RailKind kind = g.segments[s0].kind; std::vector<char> seen(g.segments.size(), 0); std::vector<size_t> st{s0}; seen[s0] = 1;
    double total = 0;
    while (!st.empty()) {
      size_t si = st.back(); st.pop_back(); total += g.segments[si].length;
      for (int node : {g.segments[si].a, g.segments[si].b})
        for (int o : g.nodes[(size_t)node].segs)
          if (!seen[(size_t)o] && g.segments[(size_t)o].kind == kind) { seen[(size_t)o] = 1; st.push_back((size_t)o); }
    }
    return (float)total;
  };
  auto groundAt = [&](const Pt& p) { double wx, wy; o.toWorld({p.x, p.y, p.z}, wx, wy); return ground ? ground->rawHeight(wx, wy) - demBase : -1e9f; };

  // Pass 1: rings of every segment (4 m), a grid of them for the parallel-track search, and the interior
  // mouth distances for the tunnel darkness.
  std::vector<std::vector<Pt>> allPts(g.segments.size());
  struct RingRef { size_t seg, ring; };
  std::map<std::pair<int, int>, std::vector<RingRef>> grid;   // 8 m cells
  auto gridKey = [](float x, float z) { return std::pair<int, int>{(int)std::floor(x / 8), (int)std::floor(z / 8)}; };
  for (size_t si = 0; si < g.segments.size(); ++si) {
    const TrackSegment& seg = g.segments[si];
    const double L = seg.length;
    if (L <= 0) continue;
    const int n = std::max(1, (int)std::ceil(L / SAMPLE_STEP));
    std::vector<Pt>& pts = allPts[si]; pts.reserve((size_t)n + 1);
    for (int i = 0; i <= n; ++i) {
      double s = L * i / n;
      TrackSample sm = g.sampleAt((int)si, s);
      float y = profile.railHeight(seg.id.c_str(), s);
      vec3 p = o.toScene(sm.wx, sm.wy, y);
      pts.push_back({p.x, p.y, p.z, (float)sm.tx, (float)sm.ty, (float)s});
      grid[gridKey(p.x, p.z)].push_back({si, (size_t)i});
    }
  }
  std::map<int, double> inside = mouthDistances(g, TRW_GELAP + 60, true);
  // Nearest parallel track on each side of a ring: another segment's ring within BADAN_MAX laterally,
  // |cos| >= 0.985 and within one ring spacing along the track (the along test also rejects this track's
  // own later rings on a tight curve).
  auto neighbours = [&](size_t si, const Pt& p, Neighbour& left, Neighbour& right) {
    left = right = {};
    float nx = -p.tz, nz = p.tx;
    auto k = gridKey(p.x, p.z);
    for (int dx = -1; dx <= 1; ++dx)
      for (int dz = -1; dz <= 1; ++dz) {
        auto it = grid.find({k.first + dx, k.second + dz});
        if (it == grid.end()) continue;
        for (const RingRef& r : it->second) {
          if (r.seg == si) continue;
          const Pt& q = allPts[r.seg][r.ring];
          if (std::fabs(q.tx * p.tx + q.tz * p.tz) < 0.985f) continue;
          float ox = q.x - p.x, oz = q.z - p.z;
          float lat = ox * nx + oz * nz, along = ox * p.tx + oz * p.tz;
          if (std::fabs(along) > SAMPLE_STEP * 0.75f) continue;
          float a = std::fabs(lat);
          if (a < BADAN_MIN || a > BADAN_MAX) continue;
          Neighbour& nb = lat < 0 ? left : right;
          if (!nb.has || a < std::fabs(nb.lat)) nb = {true, lat, q.y - p.y, r.seg};
        }
      }
  };

  const bool bedDebug = std::getenv("ENG_BED_DEBUG") != nullptr;
  std::vector<RingCtx> ctx;
  std::vector<PP> ringProf; std::vector<float> ringShade, segShade;
  for (size_t si = 0; si < g.segments.size(); ++si) {
    const TrackSegment& seg = g.segments[si];
    const double L = seg.length;
    if (L <= 0) continue;
    const std::vector<Pt>& pts = allPts[si];
    // Per-ring context: neighbours, tunnel darkness (interior distance to the nearest portal), and whether
    // the section wobbles / spills (open ground only: on a deck or tunnel floor the stone is contained).
    ctx.assign(pts.size(), {});
    {
      auto insideAt = [&](int node) { auto it = inside.find(node); return it == inside.end() ? 1e9 : it->second; };
      double iA = insideAt(seg.a), iB = insideAt(seg.b);
      for (size_t i = 0; i < pts.size(); ++i) {
        RingCtx& c = ctx[i];
        neighbours(si, pts[i], c.left, c.right);
        if (c.left.has || c.right.has) ++stats_.bedRings;
        if (bedDebug && i == pts.size() / 2) std::fprintf(stderr, "bed %s L %d %.2f %s dy %.2f R %d %.2f %s dy %.2f\n", seg.id.c_str(), c.left.has, c.left.lat, c.left.has ? g.segments[c.left.seg].id.c_str() : "-", c.left.dy, c.right.has, c.right.lat, c.right.has ? g.segments[c.right.seg].id.c_str() : "-", c.right.dy);
        if ((c.left.has && si < c.left.seg) || (c.right.has && si < c.right.seg)) ++stats_.stripRings;
        c.jitter = c.skirt = seg.kind == RailKind::Ground;
        if (seg.kind == RailKind::Tunnel) {
          double d = std::min(iA + pts[i].s, iB + (L - pts[i].s));
          c.shade = 1 + (TRW_SHADE - 1) * smoothstep01((float)(d / TRW_GELAP));
        }
      }
    }
    { std::vector<vec3> line; line.reserve(pts.size()); for (const Pt& p : pts) line.push_back({p.x, p.y + 0.5f, p.z}); centre_.push_back(std::move(line)); }
    const bool atGrade = seg.kind == RailKind::Ground;
    if (seg.kind == RailKind::Bridge) ++stats_.bridgeSegs;
    if (seg.kind == RailKind::Tunnel) ++stats_.tunnelSegs;

    // Terrain samples every 12 m (3 samples), with the tunnel-mouth carve weight. Bridge samples carry
    // the trough weight, ramping to 0 over JBT_RAMP only at ends that meet the ground (bangun3d.ts:366-384).
    auto mouthAt = [&](int node) { auto it = mouth.find(node); return it == mouth.end() ? -1.0 : it->second; };
    double dA = mouthAt(seg.a), dB = mouthAt(seg.b);
    bool free[2] = {true, true};
    if (seg.kind == RailKind::Bridge)
      for (int e = 0; e < 2; ++e)
        for (int x : g.nodes[(size_t)(e ? seg.b : seg.a)].segs) if (x != (int)si && g.segments[(size_t)x].kind == RailKind::Bridge) free[e] = false;
    for (size_t i = 0; i < pts.size(); i += 3) {
      const Pt& p = pts[i];
      float w = 1;
      if (dA >= 0 || dB >= 0) {
        double d = 1e30;
        if (dA >= 0) d = std::min(d, dA + p.s);
        if (dB >= 0) d = std::min(d, dB + (L - p.s));
        w = smoothstep01((float)(d / TRW_RAMP));
      }
      float jb = -1;
      if (seg.kind == RailKind::Bridge) {
        float t = 1;
        if (free[0]) t = std::min(t, p.s / JBT_RAMP);
        if (free[1]) t = std::min(t, ((float)L - p.s) / JBT_RAMP);
        jb = smoothstep01(t);
      }
      double wx, wy; o.toWorld({p.x, p.y, p.z}, wx, wy);
      samples_.push_back({wx, wy, p.y, atGrade, w, jb});
    }
    // Tunnel mouths: the first 12 m inside each portal are registered with weight 0 so the terrain's
    // narrow mouth corridor (Terrain::groundHeight) opens a short cutting through the hill face
    // instead of burying the portal ring (engine addition, not in the reference).
    if (seg.kind == RailKind::Tunnel)
      for (int e = 0; e < 2; ++e) {
        int node = e ? seg.b : seg.a; bool more = false;
        for (int x : g.nodes[(size_t)node].segs) if (x != (int)si && g.segments[(size_t)x].kind == RailKind::Tunnel) more = true;
        if (more) continue;
        for (int i = 0; i <= 3 && i < (int)pts.size(); ++i) {
          const Pt& p = pts[e ? pts.size() - 1 - (size_t)i : (size_t)i];
          double wx, wy; o.toWorld({p.x, p.y, p.z}, wx, wy);
          samples_.push_back({wx, wy, p.y, true, 0.f, -1.f});
        }
        samples_.push_back({0, 0, 0, false, 0, -1});   // chain break
      }

    BridgeShape shape = BridgeShape::Deck;
    if (seg.kind == RailKind::Bridge && pairs[si].host) {
      const Pt& m = pts[pts.size() / 2];
      float deckH = ground ? m.y + DEK_BAWAH - groundAt(m) : 0;
      float span = chainSpan(si);
      shape = seg.bridge != BridgeShape::Auto ? seg.bridge : bentukJembatan(span, deckH);
      stats_.bridges.push_back({seg.id, shape, span, deckH});
      if (shape == BridgeShape::Truss) ++stats_.trussSegs;
      if (shape == BridgeShape::Viaduct) ++stats_.viaductSegs;
    }

    // Split the ring list into runs of constant chunk cell (the boundary ring is shared).
    size_t start = 0;
    while (start + 1 < pts.size()) {
      auto key = cellOf(pts[start]);
      size_t end = start + 1;
      while (end < pts.size() && cellOf(pts[end]) == key) ++end;
      if (end >= pts.size()) end = pts.size() - 1;
      const Pt* run = &pts[start]; size_t rn = end - start + 1;
      Builders& b = cells[key];
      // ballast: one section per ring (skirt / wobble / shared-bed cut), rails with the ring shade
      ringProf.resize(rn * BALAS_N); ringShade.resize(rn * BALAS_N); segShade.resize(rn);
      for (size_t i = 0; i < rn; ++i) {
        profilBalasRing(run[i], ctx[start + i], &ringProf[i * BALAS_N], &ringShade[i * BALAS_N]);
        segShade[i] = ctx[start + i].shade;
      }
      extrudeVar(b.ballast, run, rn, ringProf.data(), BALAS_N, TEX_LENGTH, true, ringShade.data(), false);
      extrude(b.rails, run, rn, PROFIL_REL_KIRI, std::size(PROFIL_REL_KIRI), TEX_LENGTH, segShade.data());
      extrude(b.rails, run, rn, PROFIL_REL_KANAN, std::size(PROFIL_REL_KANAN), TEX_LENGTH, segShade.data());
      // crib strips to a parallel neighbour, built by the lower-index segment, in runs of consecutive rings
      for (float sgn : {-1.f, 1.f}) {
        size_t i = 0;
        while (i < rn) {
          auto owns = [&](size_t j) { const Neighbour& nb = sgn < 0 ? ctx[start + j].left : ctx[start + j].right; return nb.has && si < nb.seg; };
          if (!owns(i)) { ++i; continue; }
          size_t j = i; while (j < rn && owns(j)) ++j;
          if (j - i >= 2) {
            ringProf.resize((j - i) * 3);
            for (size_t k = i; k < j; ++k) profilStrip(sgn, sgn < 0 ? ctx[start + k].left : ctx[start + k].right, &ringProf[(k - i) * 3]);
            extrudeVar(b.ballast, run + i, j - i, ringProf.data(), 3, TEX_LENGTH, true, segShade.data() + i, true);
          }
          i = j;
        }
      }
      const Pair& pg = pairs[si];
      if (seg.kind == RailKind::Bridge && pg.host) {
        std::vector<PP> dek = profilDek(pg.latMin, pg.latMax, BALAS_KAKI), bawah = profilDekBawah(pg.latMin, pg.latMax, BALAS_KAKI);
        std::vector<PP> pl = profilParapet(pg.latMin, -1, BALAS_KAKI), pr = profilParapet(pg.latMax, 1, BALAS_KAKI);
        extrude(b.bridge, run, rn, dek.data(), dek.size(), TEX_LENGTH);
        extrude(b.bridge, run, rn, bawah.data(), bawah.size(), TEX_LENGTH);
        if (shape != BridgeShape::Truss) {   // the truss replaces the parapets
          extrude(b.bridge, run, rn, pl.data(), pl.size(), TEX_LENGTH);
          extrude(b.bridge, run, rn, pr.data(), pr.size(), TEX_LENGTH);
        }
      } else if (seg.kind == RailKind::Tunnel && pg.host) {
        std::vector<PP> arch = profilTerowongan(pg.latMin, pg.latMax, BALAS_KAKI, true), floor = profilTerowonganLantai(pg.latMin, pg.latMax, BALAS_KAKI);
        extrude(b.tunnel, run, rn, arch.data(), arch.size(), TEX_LENGTH, segShade.data());
        extrude(b.tunnel, run, rn, floor.data(), floor.size(), TEX_LENGTH, segShade.data());
      }
      start = end;
    }

    // Warren through-truss on the deck (member list in (lat, y, s), mapped onto the curve).
    if (shape == BridgeShape::Truss) {
      const Pair& pg = pairs[si];
      for (const Batang& bt : rangkaBatang(pts.back().s, pg.latMin, pg.latMax, BALAS_KAKI)) {
        auto dunia = [&](float lat, float y, float sv) { Pt p = titikDiS(pts, sv); return vec3{p.x - p.tz * lat, p.y + y, p.z + p.tx * lat}; };
        batangKotak(cells[cellOf(titikDiS(pts, bt.aS))].truss, dunia(bt.aLat, bt.aY, bt.aS), dunia(bt.bLat, bt.bY, bt.bS), bt.t);
      }
    }
    // Piers (bangun3d.ts taruhPilar): at the structure centre every 28 m (84 m under a truss, which spans
    // freely), twin columns 1.3 m in from each edge for a viaduct; deck bottom -> ground - 1, skipped
    // under 2.5 m clearance.
    if (seg.kind == RailKind::Bridge && ground && pairs[si].host) {
      const Pair& pg = pairs[si];
      float jarak = shape == BridgeShape::Truss ? PILAR_JARAK * 3 : PILAR_JARAK;
      std::vector<float> lats = shape == BridgeShape::Viaduct ? std::vector<float>{pg.latMin + 1.3f, pg.latMax - 1.3f}
                                                              : std::vector<float>{(pg.latMin + pg.latMax) / 2};
      for (const Pt& p : pts) {
        if (std::floor(p.s / jarak) == std::floor((p.s - SAMPLE_STEP) / jarak)) continue;
        float tanah = groundAt(p), atas = p.y + DEK_BAWAH;
        if (atas - tanah < 2.5f) continue;
        MeshBuilder& b = cells[cellOf(p)].bridge;
        float h = PILAR_SISI / 2;
        for (float l : lats) {
          float cx = p.x - p.tz * l, cz = p.z + p.tx * l;
          b.box({cx - h, tanah - 1, cz - h}, {cx + h, atas, cz + h});
        }
        ++stats_.piers;
      }
    }
    // Tunnel portal rings (1.3 m long, scale 1.15) at mouths.
    if (seg.kind == RailKind::Tunnel && pairs[si].host) {
      for (int end = 0; end < 2; ++end) {
        int node = end ? seg.b : seg.a; bool more = false;
        for (int x : g.nodes[(size_t)node].segs) if (x != (int)si && g.segments[(size_t)x].kind == RailKind::Tunnel) more = true;
        if (more) continue;
        Pt p = pts[end ? pts.size() - 1 : 0]; float dir = end ? 1.f : -1.f;
        Pt ring[2] = {p, p};
        ring[0].s = 0; ring[1].x += p.tx * 1.3f * dir; ring[1].z += p.tz * 1.3f * dir; ring[1].s = 1.3f;
        std::vector<PP> portal = profilTerowongan(pairs[si].latMin, pairs[si].latMax, BALAS_KAKI, false, 1.15f);
        extrude(cells[cellOf(p)].tunnel, ring, 2, portal.data(), portal.size(), TEX_LENGTH);
      }
    }
  }

  for (auto& [key, b] : cells) {
    RailChunk c; c.cx = key.first; c.cz = key.second;
    c.bounds.expand(b.ballast.bounds); c.bounds.expand(b.rails.bounds); c.bounds.expand(b.bridge.bounds); c.bounds.expand(b.tunnel.bounds); c.bounds.expand(b.truss.bounds);
    if (!b.ballast.empty()) c.ballast = b.ballast.upload();
    if (!b.rails.empty()) c.rails = b.rails.upload();
    if (!b.bridge.empty()) c.bridge = b.bridge.upload();
    if (!b.tunnel.empty()) c.tunnel = b.tunnel.upload();
    if (!b.truss.empty()) c.truss = b.truss.upload();
    c.tris = (uint32_t)((b.ballast.indices.size() + b.rails.indices.size() + b.bridge.indices.size() + b.tunnel.indices.size() + b.truss.indices.size()) / 3);
    stats_.tris += c.tris;
    chunks_.push_back(c);
  }
  stats_.chunks = (int)chunks_.size();
  stats_.buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void RailBuilder::draw(ModelRenderer& r, const Frustum* frustum, float refDist, bool dark, bool always) const {
  const mat4 I = mat4::identity();
  for (const RailChunk& c : chunks_) {
    if (frustum && !frustum->contains(c.bounds)) { ++r.culled; continue; }
    if (c.ballast.indexCount) r.drawMesh(c.ballast, ballastMat, texture, I);
    if (c.rails.indexCount) r.drawMesh(c.rails, railMat, texture, I);
    if (c.bridge.indexCount) r.drawMesh(c.bridge, concreteMat, {}, I);
    if (c.tunnel.indexCount) r.drawMesh(c.tunnel, tunnelMat, {}, I);
    if (c.truss.indexCount) r.drawMesh(c.truss, steelMat, {}, I);
  }
  // Iconic centreline with the BENANG_MUNCUL / BENANG_HILANG hysteresis (dunia3d.ts:4057-4063).
  iconicOn_ = always || refDist > (iconicOn_ ? 500.f : 700.f);
  if (!iconicOn_ || centre_.empty()) return;
  // width bucket: ~1.6 px at the reference distance (52° fov, 1080 px ≈ 0.0009 m/px per metre), steps of ×1.5
  float want = std::max(0.6f, refDist * 0.0015f);
  if (!iconic_.indexCount || want > iconicWidth_ * 1.5f || want < iconicWidth_ / 1.5f) {
    rhi::destroyMesh(iconic_);
    iconicWidth_ = want;
    MeshBuilder b;
    for (const std::vector<vec3>& line : centre_) {
      if (line.size() < 2) continue;
      uint32_t first = (uint32_t)b.vertices.size();
      for (size_t i = 0; i < line.size(); ++i) {
        vec3 a = line[i > 0 ? i - 1 : 0], c = line[std::min(i + 1, line.size() - 1)];
        vec3 t{c.x - a.x, 0, c.z - a.z}; float l = length(t); t = l > 1e-6f ? t / l : vec3{1, 0, 0};
        vec3 side{-t.z * want / 2, 0, t.x * want / 2};
        b.vertex(line[i] + side, {0, 1, 0}, {0, 0}); b.vertex(line[i] - side, {0, 1, 0}, {1, 0});
      }
      for (size_t i = 0; i + 1 < line.size(); ++i) {
        uint32_t v = first + (uint32_t)i * 2;
        b.triangle(v, v + 2, v + 1); b.triangle(v + 1, v + 2, v + 3);
      }
    }
    iconic_ = b.empty() ? rhi::Mesh{} : b.upload();
  }
  if (!iconic_.indexCount) return;
  Material m; m.name = "rel-ikonik"; m.unlit = true; m.doubleSided = true; m.metallic = 0; m.roughness = 1;
  uint32_t c = dark ? 0xd6a04a : 0xb3702f;   // TEMA.gelap / terang .garis
  m.baseColor = {std::pow((float)((c >> 16) & 255) / 255.f, 2.2f), std::pow((float)((c >> 8) & 255) / 255.f, 2.2f), std::pow((float)(c & 255) / 255.f, 2.2f), 1};
  r.drawMesh(iconic_, m, {}, I);
}

void RailBuilder::setAtlas(rhi::Texture atlas) {
  if (!atlas.id) return;
  if (texture.id) rhi::destroyTexture(texture);
  texture = atlas;
}

bool RailBuilder::loadAtlas(const std::string& eimgPath) {
  std::ifstream f(eimgPath, std::ios::binary);
  if (!f) return false;
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
  Image im; std::string err;
  if (!loadImageFile(bytes, im, err)) { std::fprintf(stderr, "rail atlas %s: %s\n", eimgPath.c_str(), err.c_str()); return false; }
  rhi::Texture t = uploadImage(im);
  if (!t.id) return false;
  setAtlas(t);
  return true;
}

std::string RailBuilder::prepareAtlas(const std::string& ppkaRoot, const std::string& cacheDir, const std::string& toolDir) {
  namespace fs = std::filesystem;
  std::string berkas = "pilot-rel-1067.jpg";
  {   // catalog entry `tekstur.rel1067.berkas` (model.json) names the picture; the default is the known pilot file
    std::ifstream f(ppkaRoot + "/public/model3d/model.json");
    if (f) { std::stringstream ss; ss << f.rdbuf(); std::string err; Json j = Json::parse(ss.str(), &err); if (err.empty()) berkas = j["tekstur"]["rel1067"]["berkas"].stringOr(berkas); }
  }
  std::error_code ec;
  std::string src = ppkaRoot + "/public/model3d/" + berkas;
  std::string out = cacheDir + "/" + fs::path(berkas).stem().string() + ".eimg";
  if (fs::exists(out, ec) && (!fs::exists(src, ec) || fs::last_write_time(out, ec) >= fs::last_write_time(src, ec))) return out;
  std::string tool = toolDir + "/imgconv";
  if (!fs::exists(src, ec) || !fs::exists(tool, ec)) return "";
  fs::create_directories(cacheDir, ec);
  auto q = [](const std::string& s) { std::string o = "'"; for (char c : s) o += c == '\'' ? std::string("'\\''") : std::string(1, c); return o + "'"; };
  std::string cmd = q(tool) + " " + q(src) + " " + q(out) + " --max 512 --wrap-s clamp --wrap-t repeat --flip";
  std::fprintf(stderr, "rail atlas: %s\n", cmd.c_str());
  if (std::system(cmd.c_str()) != 0) return "";
  return fs::exists(out, ec) ? out : "";
}

void RailBuilder::destroy() {
  for (RailChunk& c : chunks_) { rhi::destroyMesh(c.ballast); rhi::destroyMesh(c.rails); rhi::destroyMesh(c.bridge); rhi::destroyMesh(c.tunnel); rhi::destroyMesh(c.truss); }
  chunks_.clear(); samples_.clear(); centre_.clear();
  rhi::destroyMesh(iconic_); iconic_ = {}; iconicWidth_ = 0; iconicOn_ = false;
  if (texture.id) { rhi::destroyTexture(texture); texture = {}; }
}

} // namespace eng

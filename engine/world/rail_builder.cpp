#include "engine/world/rail_builder.h"
#include "engine/render/mesh_builder.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <queue>

namespace eng {

namespace {

// Cross-section constants (dunia3dKonst.ts:296-322, uji3dJembatan.ts:28-40).
constexpr float REL_TAPAK = -0.176f, REL_L_DALAM = 0.534f, REL_L_LUAR = 0.618f, BALAS_KAKI = -0.580f;
constexpr float U_REL_BADAN = 0.775f, U_REL_KEPALA0 = 0.800f, U_REL_KEPALA1 = 0.822f;
constexpr float DEK_TEPI = 3.4f, DEK_TEBAL = 1.4f, PARAPET_T = 1.05f, PARAPET_TEBAL = 0.18f;
constexpr float PILAR_JARAK = 28, PILAR_SISI = 1.9f;
constexpr float TRW_TEPI = 2.45f, TRW_SPRING = 2.10f, TRW_RAMP = 45;
constexpr float JARAK_BANTALAN = 0.6f;
constexpr int TEX_N = 512;
constexpr float uLat(float lat) { return 0.36f + lat * 0.1042f; }

struct PP { float lat, h, u; };
struct Pt { float x, y, z, tx, tz, s; };

const PP PROFIL_BALAS[] = {
  {-1.795f, BALAS_KAKI, uLat(-1.795f)}, {-1.117f, -0.243f, uLat(-1.117f)}, {-REL_L_LUAR, REL_TAPAK, uLat(-REL_L_LUAR)},
  {+REL_L_LUAR, REL_TAPAK, uLat(+REL_L_LUAR)}, {+1.117f, -0.243f, uLat(+1.117f)}, {+1.795f, BALAS_KAKI, uLat(+1.795f)},
};
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

// Extrude a (lat, height, u) profile along rings; analytic normals from the profile edges.
void extrude(MeshBuilder& b, const Pt* pts, size_t n, const PP* prof, size_t P, float texLen) {
  if (n < 2 || P < 2) return;
  uint32_t base = (uint32_t)b.vertices.size();
  for (size_t i = 0; i < n; ++i) {
    const Pt& p = pts[i];
    float nx = -p.tz, nz = p.tx, v = p.s / texLen;
    for (size_t j = 0; j < P; ++j) {
      const PP &q0 = prof[j > 0 ? j - 1 : 0], &q1 = prof[j + 1 < P ? j + 1 : P - 1];
      float dl = q1.lat - q0.lat, dh = q1.h - q0.h, l = std::hypot(dl, dh); if (l <= 0) l = 1;
      float nl = -dh / l, nh = dl / l;
      b.vertex({p.x + nx * prof[j].lat, p.y + prof[j].h, p.z + nz * prof[j].lat}, {nx * nl, nh, nz * nl}, {prof[j].u, v});
    }
  }
  for (size_t i = 0; i + 1 < n; ++i)
    for (size_t j = 0; j + 1 < P; ++j) {
      uint32_t a = base + (uint32_t)(i * P + j), c = a + (uint32_t)P;
      b.triangle(a, a + 1, c); b.triangle(a + 1, c + 1, c);
    }
}

float smoothstep01(float t) { t = std::max(0.f, std::min(1.f, t)); return t * t * (3 - 2 * t); }

// Rail distance from every node to the nearest tunnel mouth, capped (uji3dJembatan.ts jarakKeMulut).
std::map<int, double> mouthDistances(const TrackGraph& g, double maxD) {
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
      if (s.kind == RailKind::Tunnel) continue;
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
  destroy();
  paintTexture();
  ballastMat = {}; ballastMat.name = "ballast"; ballastMat.metallic = 0; ballastMat.roughness = 1;
  railMat = {}; railMat.name = "rail"; railMat.metallic = 0.3f; railMat.roughness = 0.42f;
  concreteMat = {}; concreteMat.name = "concrete"; concreteMat.baseColor = {0x9a / 255.f, 0xa0 / 255.f, 0xa6 / 255.f, 1}; concreteMat.roughness = 0.92f; concreteMat.metallic = 0.02f;
  tunnelMat = {}; tunnelMat.name = "tunnel"; tunnelMat.baseColor = {0x4a / 255.f, 0x4d / 255.f, 0x52 / 255.f, 1}; tunnelMat.roughness = 0.98f; tunnelMat.metallic = 0;

  const WorldOrigin& o = g.origin();
  struct Builders { MeshBuilder ballast, rails, bridge, tunnel; };
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

  std::vector<Pt> pts;
  for (size_t si = 0; si < g.segments.size(); ++si) {
    const TrackSegment& seg = g.segments[si];
    const double L = seg.length;
    if (L <= 0) continue;
    const int n = std::max(1, (int)std::ceil(L / SAMPLE_STEP));
    pts.clear(); pts.reserve((size_t)n + 1);
    for (int i = 0; i <= n; ++i) {
      double s = L * i / n;
      TrackSample sm = g.sampleAt((int)si, s);
      float y = profile.railHeight(seg.id.c_str(), s);
      vec3 p = o.toScene(sm.wx, sm.wy, y);
      pts.push_back({p.x, p.y, p.z, (float)sm.tx, (float)sm.ty, (float)s});
    }
    const bool atGrade = seg.kind == RailKind::Ground;
    if (seg.kind == RailKind::Bridge) ++stats_.bridgeSegs;
    if (seg.kind == RailKind::Tunnel) ++stats_.tunnelSegs;

    // Terrain samples every 12 m (3 samples), with the tunnel-mouth carve weight.
    auto mouthAt = [&](int node) { auto it = mouth.find(node); return it == mouth.end() ? -1.0 : it->second; };
    double dA = mouthAt(seg.a), dB = mouthAt(seg.b);
    for (size_t i = 0; i < pts.size(); i += 3) {
      const Pt& p = pts[i];
      float w = 1;
      if (dA >= 0 || dB >= 0) {
        double d = 1e30;
        if (dA >= 0) d = std::min(d, dA + p.s);
        if (dB >= 0) d = std::min(d, dB + (L - p.s));
        w = smoothstep01((float)(d / TRW_RAMP));
      }
      double wx, wy; o.toWorld({p.x, p.y, p.z}, wx, wy);
      samples_.push_back({wx, wy, p.y, atGrade, w});
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
      extrude(b.ballast, run, rn, PROFIL_BALAS, std::size(PROFIL_BALAS), TEX_LENGTH);
      extrude(b.rails, run, rn, PROFIL_REL_KIRI, std::size(PROFIL_REL_KIRI), TEX_LENGTH);
      extrude(b.rails, run, rn, PROFIL_REL_KANAN, std::size(PROFIL_REL_KANAN), TEX_LENGTH);
      const Pair& pg = pairs[si];
      if (seg.kind == RailKind::Bridge && pg.host) {
        std::vector<PP> dek = profilDek(pg.latMin, pg.latMax, BALAS_KAKI), bawah = profilDekBawah(pg.latMin, pg.latMax, BALAS_KAKI);
        std::vector<PP> pl = profilParapet(pg.latMin, -1, BALAS_KAKI), pr = profilParapet(pg.latMax, 1, BALAS_KAKI);
        extrude(b.bridge, run, rn, dek.data(), dek.size(), TEX_LENGTH);
        extrude(b.bridge, run, rn, bawah.data(), bawah.size(), TEX_LENGTH);
        extrude(b.bridge, run, rn, pl.data(), pl.size(), TEX_LENGTH);
        extrude(b.bridge, run, rn, pr.data(), pr.size(), TEX_LENGTH);
      } else if (seg.kind == RailKind::Tunnel && pg.host) {
        std::vector<PP> arch = profilTerowongan(pg.latMin, pg.latMax, BALAS_KAKI, true), floor = profilTerowonganLantai(pg.latMin, pg.latMax, BALAS_KAKI);
        extrude(b.tunnel, run, rn, arch.data(), arch.size(), TEX_LENGTH);
        extrude(b.tunnel, run, rn, floor.data(), floor.size(), TEX_LENGTH);
      }
      start = end;
    }

    // Piers every 28 m (deck bottom -> ground - 1), skipped under 2.5 m clearance.
    if (seg.kind == RailKind::Bridge && ground && pairs[si].host) {
      for (float s = PILAR_JARAK / 2; s < (float)L; s += PILAR_JARAK) {
        size_t i = std::min(pts.size() - 1, (size_t)std::lround(s / (float)L * (double)n));
        const Pt& p = pts[i];
        double wx, wy; o.toWorld({p.x, p.y, p.z}, wx, wy);
        float tanah = ground->rawHeight(wx, wy) - demBase;
        float atas = p.y + BALAS_KAKI - DEK_TEBAL;
        if (atas - tanah < 2.5f) continue;
        MeshBuilder& b = cells[cellOf(p)].bridge;
        float h = PILAR_SISI / 2, mid = (pairs[si].latMin + pairs[si].latMax) / 2;
        float cx = p.x - p.tz * mid, cz = p.z + p.tx * mid;   // pier at the structure centre
        b.box({cx - h, tanah - 1, cz - h}, {cx + h, atas, cz + h});
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
    c.bounds.expand(b.ballast.bounds); c.bounds.expand(b.rails.bounds); c.bounds.expand(b.bridge.bounds); c.bounds.expand(b.tunnel.bounds);
    if (!b.ballast.empty()) c.ballast = b.ballast.upload();
    if (!b.rails.empty()) c.rails = b.rails.upload();
    if (!b.bridge.empty()) c.bridge = b.bridge.upload();
    if (!b.tunnel.empty()) c.tunnel = b.tunnel.upload();
    c.tris = (uint32_t)((b.ballast.indices.size() + b.rails.indices.size() + b.bridge.indices.size() + b.tunnel.indices.size()) / 3);
    stats_.tris += c.tris;
    chunks_.push_back(c);
  }
  stats_.chunks = (int)chunks_.size();
  stats_.buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void RailBuilder::draw(ModelRenderer& r, const Frustum* frustum) const {
  const mat4 I = mat4::identity();
  for (const RailChunk& c : chunks_) {
    if (frustum && !frustum->contains(c.bounds)) { ++r.culled; continue; }
    if (c.ballast.indexCount) r.drawMesh(c.ballast, ballastMat, texture, I);
    if (c.rails.indexCount) r.drawMesh(c.rails, railMat, texture, I);
    if (c.bridge.indexCount) r.drawMesh(c.bridge, concreteMat, {}, I);
    if (c.tunnel.indexCount) r.drawMesh(c.tunnel, tunnelMat, {}, I);
  }
}

void RailBuilder::destroy() {
  for (RailChunk& c : chunks_) { rhi::destroyMesh(c.ballast); rhi::destroyMesh(c.rails); rhi::destroyMesh(c.bridge); rhi::destroyMesh(c.tunnel); }
  chunks_.clear(); samples_.clear();
  if (texture.id) { rhi::destroyTexture(texture); texture = {}; }
}

} // namespace eng

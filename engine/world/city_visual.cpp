#include "engine/world/city_visual.h"
#include "engine/render/mesh_builder.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>

namespace eng {

namespace city {

bool flatRoofType(const std::string& k) {
  static const std::set<std::string> FLAT = {"apartments", "commercial", "retail", "warehouse", "industrial", "school", "hospital", "office",
                                            "train_station", "public", "civic", "university", "hotel", "mall", "supermarket", "construction"};
  return FLAT.count(k) > 0;
}

bool minRect(const std::vector<vec2>& ring, MinRect& out) {
  bool have = false; float bestArea = 0, bux = 1, buz = 0, bu0 = 0, bu1 = 0, bv0 = 0, bv1 = 0;
  for (size_t i = 0; i < ring.size(); ++i) {
    vec2 a = ring[i], b = ring[(i + 1) % ring.size()];
    float dx = b.x - a.x, dz = b.y - a.y, L = std::hypot(dx, dz);
    if (L < 1e-6f) continue;
    float ux = dx / L, uz = dz / L, u0 = 1e30f, u1 = -1e30f, v0 = 1e30f, v1 = -1e30f;
    for (vec2 p : ring) { float u = p.x * ux + p.y * uz, v = -p.x * uz + p.y * ux; u0 = std::fmin(u0, u); u1 = std::fmax(u1, u); v0 = std::fmin(v0, v); v1 = std::fmax(v1, v); }
    float area = (u1 - u0) * (v1 - v0);
    if (!have || area < bestArea) { have = true; bestArea = area; bux = ux; buz = uz; bu0 = u0; bu1 = u1; bv0 = v0; bv1 = v1; }
  }
  if (!have) return false;
  float cu = (bu0 + bu1) / 2, cv = (bv0 + bv1) / 2, A = (bu1 - bu0) / 2, B = (bv1 - bv0) / 2;
  out.cx = cu * bux - cv * buz; out.cz = cu * buz + cv * bux; out.ux = bux; out.uz = buz;
  if (A < B) { float t = out.ux; out.ux = -out.uz; out.uz = t; std::swap(A, B); }
  out.A = A; out.B = B;
  return true;
}

void triangulate(const std::vector<vec2>& ring, std::vector<int>& out) {
  int n = (int)ring.size(); if (n < 3) return;
  double area = 0; for (int i = 0; i < n; ++i) { vec2 a = ring[(size_t)i], b = ring[(size_t)((i + 1) % n)]; area += (double)a.x * b.y - (double)b.x * a.y; }
  float sign = area >= 0 ? 1.f : -1.f;
  std::vector<int> idx(static_cast<size_t>(n)); for (int i = 0; i < n; ++i) idx[(size_t)i] = i;
  auto cross2 = [](vec2 a, vec2 b, vec2 c) { return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x); };
  auto inside = [&](vec2 p, vec2 a, vec2 b, vec2 c) { return cross2(a, b, p) * sign >= 0 && cross2(b, c, p) * sign >= 0 && cross2(c, a, p) * sign >= 0; };
  int guard = 0;
  while (idx.size() > 3 && guard++ < n * n) {
    bool clipped = false;
    for (size_t i = 0; i < idx.size(); ++i) {
      int i0 = idx[(i + idx.size() - 1) % idx.size()], i1 = idx[i], i2 = idx[(i + 1) % idx.size()];
      vec2 a = ring[(size_t)i0], b = ring[(size_t)i1], c = ring[(size_t)i2];
      if (cross2(a, b, c) * sign <= 1e-9f) continue;   // reflex or degenerate
      bool ear = true;
      for (int j : idx) { if (j == i0 || j == i1 || j == i2) continue; if (inside(ring[(size_t)j], a, b, c)) { ear = false; break; } }
      if (!ear) continue;
      out.push_back(i0); out.push_back(i1); out.push_back(i2);
      idx.erase(idx.begin() + (long)i); clipped = true; break;
    }
    if (!clipped) { for (size_t i = 1; i + 1 < idx.size(); ++i) { out.push_back(idx[0]); out.push_back(idx[i]); out.push_back(idx[i + 1]); } return; }
  }
  if (idx.size() == 3) { out.push_back(idx[0]); out.push_back(idx[1]); out.push_back(idx[2]); }
}

} // namespace city

namespace {
using namespace city;

vec4 rgb(uint32_t c) { return {(float)((c >> 16) & 255) / 255.f, (float)((c >> 8) & 255) / 255.f, (float)(c & 255) / 255.f, 1}; }

// EPSG:3857 with y flipped (spec §2.1).
void lonLatToWorld(double lon, double lat, double& x, double& y) {
  const double R = 6378137.0;
  x = R * lon * PI / 180.0; y = -R * std::log(std::tan(PI / 4 + lat * PI / 180.0 / 2));
}

struct Rng { uint32_t s = 12345; float next() { s = s * 1664525u + 1013904223u; return (float)(s >> 8) / 16777216.f; } };
void put(std::vector<uint8_t>& px, int W, int x, int y, uint32_t c, float a = 1) {
  if (x < 0 || y < 0 || x >= W || y >= W) return;
  uint8_t* o = &px[((size_t)y * W + x) * 4];
  float r = (float)((c >> 16) & 255), g = (float)((c >> 8) & 255), b = (float)(c & 255);
  o[0] = (uint8_t)(o[0] * (1 - a) + r * a); o[1] = (uint8_t)(o[1] * (1 - a) + g * a); o[2] = (uint8_t)(o[2] * (1 - a) + b * a); o[3] = 255;
}
void fill(std::vector<uint8_t>& px, int W, int x0, int y0, int w, int h, uint32_t c, float a = 1) { for (int y = y0; y < y0 + h; ++y) for (int x = x0; x < x0 + w; ++x) put(px, W, x, y, c, a); }

// texturDinding: plaster noise, one window (dark glass, light frame, diagonal reflection), dirt gradient at the foot.
rhi::Texture makeWallTexture() {
  const int n = 256; std::vector<uint8_t> px((size_t)n * n * 4);
  fill(px, n, 0, 0, n, n, 0xefeae2);
  Rng rng;
  for (int i = 0; i < 2600; ++i) { float t = rng.next(); uint32_t c = t > 0.6f ? 0xe4ded4 : t > 0.3f ? 0xf5f1ea : 0xdcd6cc; int x = (int)(rng.next() * n), y = (int)(rng.next() * n); fill(px, n, x, y, 2, 2, c); }
  int jx = (int)(n * 0.28f), jy = (int)(n * 0.2f), jw = (int)(n * 0.44f), jh = (int)(n * 0.4f);
  fill(px, n, jx - 4, jy - 4, jw + 8, jh + 8, 0xf7f4ee);
  fill(px, n, jx, jy, jw, jh, 0x55636e);
  for (int y = 0; y < jh; ++y) for (int x = 0; x < jw; ++x) if ((float)x / jw > 1 - (float)y / jh) put(px, n, jx + x, jy + y, 0x93a4b0, 0.35f);   // reflection triangle
  fill(px, n, jx + jw / 2 - 2, jy, 4, jh, 0xf7f4ee);
  for (int y = (int)(n * 0.86f); y < n; ++y) { float t = ((float)y - n * 0.86f) / (n * 0.14f); for (int x = 0; x < n; ++x) put(px, n, x, y, 0x6e6a5e, 0.5f * t); }
  return rhi::createTexture(n, n, rhi::Format::RGBA8, std::as_bytes(std::span(px)), true, true);
}

// texturGenteng: 8 tile rows with a dark shadow line, a light top edge and staggered vertical joints.
rhi::Texture makeRoofTexture() {
  const int n = 128, rows = 8, rh = n / rows; std::vector<uint8_t> px((size_t)n * n * 4);
  fill(px, n, 0, 0, n, n, 0xf2ece5);
  for (int b = 0; b < rows; ++b) {
    int y = b * rh;
    fill(px, n, 0, y + rh - 3, n, 3, 0x5a4a40, 0.42f);
    fill(px, n, 0, y, n, 2, 0xfffcf6, 0.5f);
    for (int x = 0; x < n; x += 16) fill(px, n, x + (b % 2 ? 8 : 0), y, 2, rh - 3, 0x5a4a40, 0.22f);
  }
  return rhi::createTexture(n, n, rhi::Format::RGBA8, std::as_bytes(std::span(px)), true, true);
}

// Rail chords (scene xz) in a 64 m hash grid for the clearance test — every segment, bridges included.
struct RailGrid {
  static constexpr float CELL = 64;
  struct Chord { float x, z, x2, z2; };
  std::unordered_map<int64_t, std::vector<Chord>> cells;
  static int64_t key(int i, int j) { return ((int64_t)i << 32) ^ (uint32_t)j; }
  void add(Chord c) {
    int i0 = (int)std::floor(std::fmin(c.x, c.x2) / CELL), i1 = (int)std::floor(std::fmax(c.x, c.x2) / CELL);
    int j0 = (int)std::floor(std::fmin(c.z, c.z2) / CELL), j1 = (int)std::floor(std::fmax(c.z, c.z2) / CELL);
    for (int i = i0; i <= i1; ++i) for (int j = j0; j <= j1; ++j) cells[key(i, j)].push_back(c);
  }
  float distance(float x, float z, float maxD) const {
    int r = (int)std::ceil(maxD / CELL), ci = (int)std::floor(x / CELL), cj = (int)std::floor(z / CELL); float best = 1e30f;
    for (int i = -r; i <= r; ++i) for (int j = -r; j <= r; ++j) {
      auto it = cells.find(key(ci + i, cj + j)); if (it == cells.end()) continue;
      for (const Chord& c : it->second) {
        float dx = c.x2 - c.x, dz = c.z2 - c.z, L2 = dx * dx + dz * dz;
        float t = L2 > 0 ? std::fmax(0.f, std::fmin(1.f, ((x - c.x) * dx + (z - c.z) * dz) / L2)) : 0;
        best = std::fmin(best, std::hypot(c.x + dx * t - x, c.z + dz * t - z));
      }
    }
    return best;
  }
};

struct Bins { MeshBuilder wall[PALETTE], roof[PALETTE]; };
} // namespace

bool CityVisuals::build(const std::string& jsonPath, const WorldOrigin& origin, const TrackGraph& graph, const GroundFn& ground) {
  destroy(); stats = {};
  std::ifstream f(jsonPath, std::ios::binary);
  if (!f) return false;   // corridor not baked: no city, not an error
  auto t0 = std::chrono::steady_clock::now();
  std::stringstream ss; ss << f.rdbuf();
  std::string err; Json kota = Json::parse(ss.str(), &err);
  if (!kota["bangunan"].isArray()) { std::fprintf(stderr, "city: %s: %s\n", jsonPath.c_str(), err.empty() ? "no `bangunan`" : err.c_str()); return false; }
  stats.loaded = true;
  const Json& asal = kota["asal"];
  bool haveAsal = asal.isArray() && asal.size() >= 2;
  float clearance = (float)kota["bebas"].numberOr(CLEARANCE);

  RailGrid rails;
  for (const TrackSegment& seg : graph.segments)
    for (size_t i = 0; i + 1 < seg.lut.size(); ++i) {
      vec3 a = origin.toScene(seg.lut[i].px, seg.lut[i].py, 0), b = origin.toScene(seg.lut[i + 1].px, seg.lut[i + 1].py, 0);
      rails.add({a.x, a.z, b.x, b.z});
    }

  std::map<std::pair<int, int>, Bins> chunks;
  struct Pt { float x, z; double wx, wy; };
  std::vector<Pt> ring; std::vector<vec2> ring2; std::vector<int> tris;
  size_t used = 0;
  for (const Json& b : kota["bangunan"].arr) {
    const Json& r = b["r"];
    if (r.size() < 8 || used >= (size_t)MAX_BUILDINGS) continue;
    ring.clear(); ring2.clear();
    double ex = 0, ey = 0;   // encoded ring: first pair absolute (1e-6°), then deltas
    for (size_t i = 0; i + 1 < r.size(); i += 2) {
      double lon, lat;
      if (haveAsal) { ex = i == 0 ? r[i].num : ex + r[i].num; ey = i == 0 ? r[i + 1].num : ey + r[i + 1].num; lon = asal[0].num + ex / 1e6; lat = asal[1].num + ey / 1e6; }
      else { lon = r[i].num; lat = r[i + 1].num; }
      double wx, wy; lonLatToWorld(lon, lat, wx, wy);
      vec3 p = origin.toScene(wx, wy, 0);
      ring.push_back({p.x, p.z, wx, wy}); ring2.push_back({p.x, p.z});
    }
    if (ring.size() < 3) continue;
    float cxm = 0, czm = 0; for (const Pt& p : ring) { cxm += p.x; czm += p.z; } cxm /= (float)ring.size(); czm /= (float)ring.size();
    // clearance tested per corner: a long building may be centred far away and still touch the line
    float dMin = 1e30f; for (const Pt& p : ring) dMin = std::fmin(dMin, rails.distance(p.x, p.z, clearance + 4));
    if (dMin < clearance) { ++stats.skippedNearTrack; continue; }
    MinRect rect; if (!minRect(ring2, rect)) continue;
    ++used;
    Bins& bins = chunks[{(int)std::floor(cxm / CHUNK), (int)std::floor(czm / CHUNK)}];   // whole building in its centroid's chunk

    float base = 1e30f; for (const Pt& p : ring) base = std::fmin(base, ground(p.wx, p.wy));
    base -= 0.5f;
    std::string k = b["k"].stringOr(""); float h = (float)b["h"].numberOr(0);
    bool flat = flatRoofType(k) || h > 8;
    float tD = h > 0 ? h : flat ? 8.f : 4.2f, he = base + tD;
    int wallIdx = (int)std::floor(hash01(cxm, czm, 1) * PALETTE) % PALETTE, roofIdx = (int)std::floor(hash01(cxm, czm, 2) * PALETTE) % PALETTE;
    MeshBuilder& wall = bins.wall[wallIdx]; MeshBuilder& roof = bins.roof[roofIdx];

    // walls: one quad per edge, u along the wall / 3.2, v = floors; normal outward
    double area2 = 0; for (size_t i = 0; i < ring.size(); ++i) { const Pt &a = ring[i], &c = ring[(i + 1) % ring.size()]; area2 += (double)a.x * c.z - (double)c.x * a.z; }
    float outward = area2 >= 0 ? 1.f : -1.f;
    float floors = std::fmax(1.f, std::round(tD / 3.4f)), along = 0;
    for (size_t i = 0; i < ring.size(); ++i) {
      const Pt &a = ring[i], &c = ring[(i + 1) % ring.size()];
      float L = std::hypot(c.x - a.x, c.z - a.z); if (L < 1e-4f) continue;
      float u0 = along / 3.2f, u1 = (along + L) / 3.2f; along += L;
      vec3 n = normalize(vec3{(c.z - a.z) * outward, 0, -(c.x - a.x) * outward});
      uint32_t p0 = wall.vertex({a.x, base, a.z}, n, {u0, floors}), p1 = wall.vertex({a.x, he, a.z}, n, {u0, 0});
      uint32_t p2 = wall.vertex({c.x, base, c.z}, n, {u1, floors}), p3 = wall.vertex({c.x, he, c.z}, n, {u1, 0});
      if (outward > 0) { wall.triangle(p0, p2, p1); wall.triangle(p1, p2, p3); } else { wall.triangle(p0, p1, p2); wall.triangle(p1, p3, p2); }
    }

    if (flat) {
      tris.clear(); triangulate(ring2, tris);
      uint32_t first = (uint32_t)roof.vertices.size();
      for (const Pt& p : ring) roof.vertex({p.x, he, p.z}, {0, 1, 0}, {p.x / 2.4f, p.z / 2.4f});
      for (size_t i = 0; i + 2 < tris.size(); i += 3) {
        uint32_t a = first + (uint32_t)tris[i], bb = first + (uint32_t)tris[i + 1], c = first + (uint32_t)tris[i + 2];
        if (outward > 0) roof.triangle(a, c, bb); else roof.triangle(a, bb, c);   // seen from above (+y)
      }
    } else {
      float a = rect.A + OVERHANG, bl = rect.B + OVERHANG, vx = -rect.uz, vz = rect.ux;
      float rise = std::fmin(std::fmax(bl * 0.7f, 0.9f), 2.8f), hr = he + rise, slope = std::hypot(bl, rise);
      auto E = [&](float su, float sv) { return vec3{rect.cx + su * a * rect.ux + sv * bl * vx, he, rect.cz + su * a * rect.uz + sv * bl * vz}; };
      auto R = [&](float su) { return vec3{rect.cx + su * a * rect.ux, hr, rect.cz + su * a * rect.uz}; };
      vec3 e00 = E(-1, -1), e01 = E(-1, 1), e10 = E(1, -1), e11 = E(1, 1), r0 = R(-1), r1 = R(1);
      auto tri = [&](vec3 p, vec3 q, vec3 s, vec2 uvp, vec2 uvq, vec2 uvs) {
        vec3 n = cross(q - p, s - p);
        if (n.y < 0) { std::swap(q, s); std::swap(uvq, uvs); n = n * -1.f; }   // face up
        if (length(n) < 1e-6f) return;
        n = normalize(n);
        uint32_t i = roof.vertex(p, n, uvp); roof.vertex(q, n, uvq); roof.vertex(s, n, uvs); roof.triangle(i, i + 1, i + 2);
      };
      vec2 uE0{-a / 2.4f, slope / 2.4f}, uE1{a / 2.4f, slope / 2.4f}, uR0{-a / 2.4f, 0}, uR1{a / 2.4f, 0};
      tri(e00, e10, r1, uE0, uE1, uR1); tri(e00, r1, r0, uE0, uR1, uR0);   // slope -v
      tri(e11, e01, r0, uE1, uE0, uR0); tri(e11, r0, r1, uE1, uR0, uR1);   // slope +v
      // gable ends: vertical triangles, normal along the ridge axis
      auto gable = [&](vec3 p, vec3 q, vec3 s, float dir) {
        vec3 n{rect.ux * dir, 0, rect.uz * dir};
        if (dot(cross(q - p, s - p), n) < 0) std::swap(q, s);
        uint32_t i = roof.vertex(p, n, {0, slope / 2.4f}); roof.vertex(q, n, {bl / 1.2f, slope / 2.4f}); roof.vertex(s, n, {bl / 2.4f, 0}); roof.triangle(i, i + 1, i + 2);
      };
      gable(e00, e01, r0, -1); gable(e11, e10, r1, 1);
    }
  }

  wallTex_ = makeWallTexture(); roofTex_ = makeRoofTexture();
  for (int i = 0; i < PALETTE; ++i) {
    wallMat_[i] = {}; wallMat_[i].baseColor = rgb(WALL_COLOURS[i]); wallMat_[i].metallic = 0; wallMat_[i].roughness = 0.94f; wallMat_[i].doubleSided = true;
    roofMat_[i] = {}; roofMat_[i].baseColor = rgb(ROOF_COLOURS[i]); roofMat_[i].metallic = 0; roofMat_[i].roughness = 0.88f; roofMat_[i].doubleSided = true;
  }
  for (auto& [key, bins] : chunks) {
    Chunk ch;
    for (int i = 0; i < PALETTE; ++i) for (int roof = 0; roof < 2; ++roof) {
      MeshBuilder& mb = roof ? bins.roof[i] : bins.wall[i];
      if (mb.empty()) continue;
      ch.parts.push_back({mb.upload(), mb.bounds, i, roof == 1}); ch.bounds.expand(mb.bounds);
      stats.tris += (unsigned)(mb.indices.size() / 3); ++stats.meshes;
    }
    if (!ch.parts.empty()) chunks_.push_back(std::move(ch));
  }
  stats.buildings = used; stats.chunks = (int)chunks_.size();
  stats.buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  return true;
}

void CityVisuals::draw(ModelRenderer& r, const Frustum* frustum) {
  stats.drawn = 0;
  for (const Chunk& ch : chunks_) {
    if (frustum && !frustum->contains(ch.bounds)) continue;
    for (const Part& p : ch.parts) {
      if (frustum && !frustum->contains(p.bounds)) continue;
      r.drawMesh(p.mesh, p.roof ? roofMat_[p.palette] : wallMat_[p.palette], p.roof ? roofTex_ : wallTex_, mat4::identity());
      ++stats.drawn;
    }
  }
}

void CityVisuals::destroy() {
  for (Chunk& ch : chunks_) for (Part& p : ch.parts) rhi::destroyMesh(p.mesh);
  chunks_.clear();
  if (wallTex_.id) { rhi::destroyTexture(wallTex_); wallTex_ = {}; }
  if (roofTex_.id) { rhi::destroyTexture(roofTex_); roofTex_ = {}; }
}

} // namespace eng

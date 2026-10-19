#include "engine/world/meja_putar_visual.h"
#include "engine/render/mesh_builder.h"
#include <cmath>

namespace eng {

namespace {
using namespace mejaputar;

vec4 rgb(uint32_t c) { return {(float)((c >> 16) & 255) / 255.f, (float)((c >> 8) & 255) / 255.f, (float)(c & 255) / 255.f, 1}; }

struct Bridge { int seg = -1; double cx = 0, cy = 0, r = 0; std::string id; };

// The bridge of a turntable: a segment with BOTH end nodes on the rim (± RIM_TOL) and its midpoint at the pivot
// (MejaPutar.jembatan in the sim — the same rule, so both sides agree on which segment turns).
std::vector<Bridge> findBridges(const TrackGraph& g, const Json& world) {
  std::vector<Bridge> out;
  for (const Json& o : world["scenery"].arr) {
    if (o["kind"].stringOr("") != "mejaPutar") continue;
    double px = o["pos"]["x"].numberOr(0), py = o["pos"]["y"].numberOr(0), r = o["w"].numberOr(20) / 2;
    for (size_t si = 0; si < g.segments.size(); ++si) {
      const TrackSegment& seg = g.segments[si];
      if (seg.a < 0 || seg.b < 0) continue;
      const TrackNode &a = g.nodes[(size_t)seg.a], &b = g.nodes[(size_t)seg.b];
      if (std::fabs(std::hypot(a.wx - px, a.wy - py) - r) > RIM_TOL || std::fabs(std::hypot(b.wx - px, b.wy - py) - r) > RIM_TOL) continue;
      TrackSample m = g.sampleAt((int)si, seg.length / 2);
      if (std::hypot(m.wx - px, m.wy - py) > RIM_TOL) continue;
      out.push_back({(int)si, m.wx, m.wy, r, o["id"].stringOr("")});
      break;
    }
  }
  return out;
}

// Inward-facing ring wall (the pit's side), radius r, y0..y1, n sides.
void ringWall(MeshBuilder& b, float r, float y0, float y1, int n) {
  uint32_t base = (uint32_t)b.vertices.size();
  for (int i = 0; i <= n; ++i) {
    float a = 2 * PI * (float)i / (float)n; vec3 nrm{-std::cos(a), 0, -std::sin(a)};
    b.vertex({std::cos(a) * r, y0, std::sin(a) * r}, nrm, {(float)i / (float)n, 0});
    b.vertex({std::cos(a) * r, y1, std::sin(a) * r}, nrm, {(float)i / (float)n, 1});
  }
  for (int i = 0; i < n; ++i) { uint32_t k = base + (uint32_t)i * 2; b.quad(k, k + 1, k + 3, k + 2); }
}

// Flat annulus (r0..r1) at height y facing up; r0 = 0 gives a disc.
void annulus(MeshBuilder& b, float r0, float r1, float y, int n) {
  uint32_t base = (uint32_t)b.vertices.size();
  for (int i = 0; i <= n; ++i) {
    float a = 2 * PI * (float)i / (float)n, c = std::cos(a), s = std::sin(a);
    b.vertex({c * r0, y, s * r0}, {0, 1, 0}, {}); b.vertex({c * r1, y, s * r1}, {0, 1, 0}, {});
  }
  for (int i = 0; i < n; ++i) { uint32_t k = base + (uint32_t)i * 2; b.quad(k, k + 2, k + 3, k + 1); }
}
} // namespace

std::set<std::string> MejaPutarVisuals::bridgeSegments(const TrackGraph& g, const Json& world) {
  std::set<std::string> out;
  for (const Bridge& br : findBridges(g, world)) out.insert(g.segments[(size_t)br.seg].id);
  return out;
}

void MejaPutarVisuals::build(const TrackGraph& g, const RailProfile& profile, const Json& world) {
  destroy();
  spots_.clear();
  const WorldOrigin& o = g.origin();
  MeshBuilder pit, coping;
  for (const Bridge& br : findBridges(g, world)) {
    const TrackSegment& seg = g.segments[(size_t)br.seg];
    const TrackNode &na = g.nodes[(size_t)seg.a], &nb = g.nodes[(size_t)seg.b];
    MejaPutarSpot s;
    s.id = br.id; s.segId = seg.id; s.radius = (float)br.r;
    s.pivot = o.toScene(br.cx, br.cy, profile.railHeight(seg.id.c_str(), seg.length / 2));
    s.yaw0 = std::atan2(-(float)(nb.wy - na.wy), (float)(nb.wx - na.wx));
    const float R = s.radius, h = s.pivot.y;
    // static pit about the pivot: floor, wall up to the deck top, concrete coping ring at rail level
    MeshBuilder one;
    annulus(one, 0, R, h - PIT_DEPTH, 40);
    ringWall(one, R, h - PIT_DEPTH, h - DECK_DOWN + 0.02f, 40);
    pit.append(one, mat4::translation({s.pivot.x, 0, s.pivot.z}));
    one = {};
    annulus(one, R, R + COPING_W, h - DECK_DOWN + 0.02f, 40);
    coping.append(one, mat4::translation({s.pivot.x, 0, s.pivot.z}));
    // turning parts, local to the pivot (x = along the bridge)
    const float L = 2 * R - 0.25f;
    MeshBuilder deck; deck.box({-L / 2, -DECK_DOWN - DECK_H, -DECK_W / 2}, {L / 2, -DECK_DOWN, DECK_W / 2});
    MeshBuilder rails;
    for (float side : {-1.f, 1.f})
      rails.box({-R, -RAIL_H, side * RAIL_HALF - RAIL_W / 2}, {R, 0, side * RAIL_HALF + RAIL_W / 2});
    Part d; d.mesh = deck.upload(); d.bounds = deck.bounds; d.used = true; deck_.push_back(d);
    Part rr; rr.mesh = rails.upload(); rr.bounds = rails.bounds; rr.used = true; rails_.push_back(rr);
    spots_.push_back(std::move(s));
  }
  if (spots_.empty()) return;
  pit_.mesh = pit.upload(); pit_.bounds = pit.bounds; pit_.used = true;
  coping_.mesh = coping.upload(); coping_.bounds = coping.bounds; coping_.used = true;
  pitMat_ = {}; pitMat_.baseColor = rgb(0x6f6a62); pitMat_.roughness = 0.97f; pitMat_.metallic = 0; pitMat_.doubleSided = true;
  copingMat_ = {}; copingMat_.baseColor = rgb(0x9aa0a6); copingMat_.roughness = 0.92f; copingMat_.metallic = 0.02f;
  deckMat_ = {}; deckMat_.baseColor = rgb(0x3d4247); deckMat_.roughness = 0.7f; deckMat_.metallic = 0.45f;
  railMat_ = {}; railMat_.baseColor = rgb(0xa9adb1); railMat_.roughness = 0.42f; railMat_.metallic = 0.6f;
}

void MejaPutarVisuals::setState(const std::vector<SimMeja>& state) {
  for (const SimMeja& m : state) for (MejaPutarSpot& s : spots_) if (s.id == m.id) { s.angle = m.angle; break; }
}

void MejaPutarVisuals::draw(ModelRenderer& r, const Frustum* frustum) const {
  if (spots_.empty()) return;
  if (pit_.used && !(frustum && !frustum->contains(pit_.bounds))) r.drawMesh(pit_.mesh, pitMat_, {}, mat4::identity());
  if (coping_.used && !(frustum && !frustum->contains(coping_.bounds))) r.drawMesh(coping_.mesh, copingMat_, {}, mat4::identity());
  for (size_t i = 0; i < spots_.size(); ++i) {
    const MejaPutarSpot& s = spots_[i];
    // the sim turns the vehicles by +angle in world XY (= scene XZ): yaw = yaw0 - angle
    mat4 xf = mat4::translation(s.pivot) * mat4::rotationY(s.yaw0 - s.angle);
    if (frustum && !frustum->contains(deck_[i].bounds.transformed(xf))) continue;
    r.drawMesh(deck_[i].mesh, deckMat_, {}, xf);
    r.drawMesh(rails_[i].mesh, railMat_, {}, xf);
  }
}

void MejaPutarVisuals::destroy() {
  for (Part* p : {&pit_, &coping_}) if (p->used) { rhi::destroyMesh(p->mesh); p->used = false; }
  for (Part& p : deck_) if (p.used) rhi::destroyMesh(p.mesh);
  for (Part& p : rails_) if (p.used) rhi::destroyMesh(p.mesh);
  deck_.clear(); rails_.clear();
}

} // namespace eng

#include "engine/world/jpl_visual.h"
#include "engine/render/mesh_builder.h"
#include <chrono>
#include <cmath>

namespace eng {

namespace {
using namespace jpl;

vec4 rgb(uint32_t c) { return {(float)((c >> 16) & 255) / 255.f, (float)((c >> 8) & 255) / 255.f, (float)(c & 255) / 255.f, 1}; }
vec4 rgbLin(uint32_t c) { vec4 v = rgb(c); return {std::pow(v.x, 2.2f), std::pow(v.y, 2.2f), std::pow(v.z, 2.2f), 1}; }

// Axis-aligned box centred at c with full size s (local space).
void box(MeshBuilder& b, vec3 c, vec3 s) { b.box(c - s * 0.5f, c + s * 0.5f); }

// Y-axis cylinder (radius r, from y0 to y1), n sides, smooth normals, capped.
void cylinder(MeshBuilder& b, float r, float y0, float y1, int n) {
  uint32_t base = (uint32_t)b.vertices.size();
  for (int i = 0; i <= n; ++i) {
    float a = 2 * PI * (float)i / (float)n; vec3 nrm{std::cos(a), 0, std::sin(a)};
    b.vertex({nrm.x * r, y0, nrm.z * r}, nrm, {(float)i / (float)n, 0}); b.vertex({nrm.x * r, y1, nrm.z * r}, nrm, {(float)i / (float)n, 1});
  }
  for (int i = 0; i < n; ++i) { uint32_t k = base + (uint32_t)i * 2; b.quad(k, k + 2, k + 3, k + 1); }
  for (float y : {y0, y1}) {
    vec3 nrm{0, y == y1 ? 1.f : -1.f, 0}; uint32_t c = b.vertex({0, y, 0}, nrm, {0.5f, 0.5f});
    for (int i = 0; i <= n; ++i) { float a = 2 * PI * (float)i / (float)n; b.vertex({std::cos(a) * r, y, std::sin(a) * r}, nrm, {}); }
    for (int i = 0; i < n; ++i) { if (y == y1) b.triangle(c, c + 1 + (uint32_t)i, c + 2 + (uint32_t)i); else b.triangle(c, c + 2 + (uint32_t)i, c + 1 + (uint32_t)i); }
  }
}

// UV sphere, radius r, at the origin.
void sphere(MeshBuilder& b, float r, int seg, int rings) {
  uint32_t base = (uint32_t)b.vertices.size();
  for (int j = 0; j <= rings; ++j) {
    float v = (float)j / (float)rings, phi = v * PI;
    for (int i = 0; i <= seg; ++i) {
      float u = (float)i / (float)seg, th = u * 2 * PI;
      vec3 n{std::sin(phi) * std::cos(th), std::cos(phi), std::sin(phi) * std::sin(th)};
      b.vertex(n * r, n, {u, v});
    }
  }
  for (int j = 0; j < rings; ++j) for (int i = 0; i < seg; ++i) {
    uint32_t a = base + (uint32_t)(j * (seg + 1) + i), c = a + (uint32_t)seg + 1;
    b.triangle(a, a + 1, c + 1); b.triangle(a, c + 1, c);
  }
}

// texturPalang: 128x16, left half red, right half off-white, thin dark seam; repeated ARM_STRIPES times.
rhi::Texture makeArmTexture() {
  const int W = 128, H = 16; std::vector<uint8_t> px((size_t)W * H * 4);
  for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
    uint8_t* o = &px[((size_t)y * W + x) * 4];
    bool red = x < W / 2; o[0] = red ? 0xd1 : 0xf2; o[1] = red ? 0x37 : 0xf2; o[2] = red ? 0x2b : 0xef; o[3] = 255;
    if (x == W / 2 - 1 || x == W / 2) for (int k = 0; k < 3; ++k) o[k] = (uint8_t)(o[k] * 0.82f);
  }
  return rhi::createTexture(W, H, rhi::Format::RGBA8, std::as_bytes(std::span(px)), true, true, rhi::Wrap::Repeat, rhi::Wrap::Clamp);
}

mat4 baseMatrix(vec3 p, float rotY) { return mat4::translation(p) * mat4::rotationY(rotY); }
} // namespace

void JplVisuals::gatesFor(float x, float z, float roadYaw, float span, JplGate out[2]) {
  float c = std::cos(roadYaw), s = std::sin(roadYaw);
  auto put = [&](float lx, float lz, float turn) { JplGate g; g.pos = {x + lx * c + lz * s, 0, z - lx * s + lz * c}; g.rotY = roadYaw + turn; return g; };
  float a = postDistance(span), b = FROM_EDGE;
  out[0] = put(+a, +b, PI);   // across track #1, road edge #1, sweeps toward -Z
  out[1] = put(-a, -b, 0);    // across track #2, road edge #2, sweeps toward +Z
}

float JplVisuals::trackSpan(const std::vector<std::pair<double, double>>& samples, double px, double py, double rot) {
  double ux = std::cos(rot), uy = std::sin(rot), best = 0;
  for (const auto& [x, y] : samples) {
    double dx = x - px, dy = y - py;
    if (std::fabs(-dx * uy + dy * ux) > ROAD_HALF) continue;   // not actually crossed by the road
    best = std::fmax(best, std::fabs(dx * ux + dy * uy));
  }
  return (float)best;
}

void JplVisuals::build(const TrackGraph& g, const Json& world, const WorldOrigin& origin, const GroundFn& ground) {
  destroy();
  crossings_.clear(); gates_.clear(); stats = {};
  for (const Json& o : world["scenery"].arr) {
    if (o["kind"].stringOr("") != "jpl") continue;
    double px = o["pos"]["x"].numberOr(0), py = o["pos"]["y"].numberOr(0), rot = o["rot"].numberOr(0);
    // nearestOnTrack(pos, 60): a crossing away from every track is skipped; sampelSekitar(pos, 45) for the span
    double nearest = 1e30; std::vector<std::pair<double, double>> around;
    for (const TrackSegment& seg : g.segments) {
      if (seg.bx0 - SNAP_RADIUS > px || seg.bx1 + SNAP_RADIUS < px || seg.by0 - SNAP_RADIUS > py || seg.by1 + SNAP_RADIUS < py) continue;
      for (const TrackSegment::Lut& l : seg.lut) {
        double d = std::hypot(l.px - px, l.py - py);
        nearest = std::fmin(nearest, d);
        if (d <= SPAN_RADIUS) around.emplace_back(l.px, l.py);
      }
    }
    if (nearest > SNAP_RADIUS) continue;
    JplCrossing c; c.id = o["id"].stringOr(""); c.label = o["label"].stringOr(""); c.gate0 = (int)gates_.size();
    vec3 sp = origin.toScene(px, py, 0); c.pos = sp;
    JplGate gs[2]; gatesFor(sp.x, sp.z, (float)-rot, trackSpan(around, px, py, rot), gs);
    for (JplGate& gt : gs) {
      double wx, wy; origin.toWorld(gt.pos, wx, wy);
      gt.pos.y = ground ? ground(wx, wy) : 0;
      gt.base = baseMatrix(gt.pos, gt.rotY);
      // lamps hang from the head on the OUTER side (-X local = away from the crossing), ±LAMP_GAP/2 along Z
      for (int k = 0; k < 2; ++k) gt.lamp[k] = gt.base.transformPoint({-LAMP_OUT, LAMP_Y, (k ? 1.f : -1.f) * LAMP_GAP / 2});
      gates_.push_back(gt);
    }
    c.pos.y = (gs[0].pos.y + gs[1].pos.y) / 2;
    crossings_.push_back(std::move(c));
  }
  if (gates_.empty()) return;

  // static parts baked per colour (the reference bakes one vertex-coloured mesh; we have no vertex colours)
  MeshBuilder foundation, post, head, cross, one;
  one.clear(); box(one, {0, FOUNDATION_H / 2, 0}, {FOUNDATION, FOUNDATION_H, FOUNDATION}); MeshBuilder fnd = one;
  one.clear(); cylinder(one, POST_D / 2, FOUNDATION_H, FOUNDATION_H + POST_H, 8); MeshBuilder pst = one;
  one.clear(); box(one, {0, LAMP_Y, 0}, {0.10f, 0.12f, HEAD_L}); MeshBuilder hd = one;
  one.clear();
  for (float ang : {PI / 4, -PI / 4}) { MeshBuilder blade; box(blade, {}, {0.05f, CROSS_W, CROSS_L}); one.append(blade, mat4::translation({0, CROSS_Y, 0}) * mat4::rotationX(ang)); }
  MeshBuilder crs = one;
  for (const JplGate& gt : gates_) { foundation.append(fnd, gt.base); post.append(pst, gt.base); head.append(hd, gt.base); cross.append(crs, gt.base); }
  auto upload = [&](MeshBuilder& mb, Batch& out) { out.mesh = mb.upload(); out.bounds = mb.bounds; out.used = true; stats.staticTris += (unsigned)(mb.indices.size() / 3); };
  upload(foundation, foundation_); upload(post, post_); upload(head, head_); upload(cross, cross_);

  // arm: 6 m box, pivot at one end, extends along local +Z (BoxGeometry translated +X then rotated -90° about Y)
  MeshBuilder arm;
  {
    vec3 mn{-ARM_T / 2, -ARM_H / 2, 0}, mx{ARM_T / 2, ARM_H / 2, ARM_L};
    vec3 a{mn.x, mn.y, mn.z}, b{mx.x, mn.y, mn.z}, c{mx.x, mx.y, mn.z}, d{mn.x, mx.y, mn.z}, e{mn.x, mn.y, mx.z}, f{mx.x, mn.y, mx.z}, gg{mx.x, mx.y, mx.z}, h{mn.x, mx.y, mx.z};
    const float S = (float)ARM_STRIPES;
    arm.quad(f, e, h, gg, {0, 0}, {1, 1}); arm.quad(a, b, c, d, {0, 0}, {1, 1});                 // ends
    arm.quad(b, f, gg, c, {0, 0}, {S, 1}); arm.quad(e, a, d, h, {S, 0}, {0, 1});                  // sides (+X, -X)
    arm.quad(d, c, gg, h, {0, 0}, {1, S}); arm.quad(e, f, b, a, {0, 0}, {1, S});                  // top, bottom
    // stripes must run along Z on every long face: rewrite UVs so u follows z / (ARM_L / S)
    for (Vertex& v : arm.vertices) v.uv = {v.pos.z / (ARM_L / S), v.uv.y};
    arm_ = arm.upload(); armBounds_ = arm.bounds; stats.armTris = (unsigned)(arm.indices.size() / 3);
  }
  MeshBuilder lp; sphere(lp, LAMP_R, 8, 6); lamp_ = lp.upload(); stats.lampTris = (unsigned)(lp.indices.size() / 3);
  armTex_ = makeArmTexture();
  built_ = true;
  foundationMat_ = {}; foundationMat_.baseColor = rgb(0x9d9a92); foundationMat_.metallic = 0; foundationMat_.roughness = 0.95f;
  postMat_ = {}; postMat_.baseColor = rgb(0xe6e6e2); postMat_.metallic = 0.2f; postMat_.roughness = 0.6f;
  headMat_ = {}; headMat_.baseColor = rgb(0x3c4046); headMat_.metallic = 0.3f; headMat_.roughness = 0.7f;
  crossMat_ = {}; crossMat_.baseColor = rgb(0xf4f4f0); crossMat_.metallic = 0; crossMat_.roughness = 0.8f;
  armMat_ = {}; armMat_.baseColor = {1, 1, 1, 1}; armMat_.metallic = 0; armMat_.roughness = 0.7f;
  lampOff_ = {}; lampOff_.baseColor = rgbLin(0x4a1512); lampOff_.unlit = true;
  lampOn_ = {}; lampOn_.baseColor = rgbLin(0xff3b2a); lampOn_.unlit = true;
}

void JplVisuals::setState(const std::vector<SimJpl>& state) {
  for (const SimJpl& s : state) for (JplCrossing& c : crossings_) if (c.id == s.id) { c.closed = s.closed; break; }
}

void JplVisuals::animate(float dtSim) {
  float step = std::fmin(1.f, dtSim / CLOSE_SECONDS);
  for (JplCrossing& c : crossings_) {
    float target = c.closed ? 0.f : 1.f;
    if (c.open == target) continue;
    c.open = target > c.open ? std::fmin(target, c.open + step) : std::fmax(target, c.open - step);
  }
}

void JplVisuals::draw(ModelRenderer& r, const Frustum* frustum) const {
  if (!built_) return;
  auto drawBatch = [&](const Batch& b, const Material& m) { if (b.used && !(frustum && !frustum->contains(b.bounds))) r.drawMesh(b.mesh, m, {}, mat4::identity()); };
  drawBatch(foundation_, foundationMat_); drawBatch(post_, postMat_); drawBatch(head_, headMat_); drawBatch(cross_, crossMat_);
  auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
  int blink = (int)std::floor(ms / BLINK_MS) % 2;
  for (const JplCrossing& c : crossings_) {
    for (int k = 0; k < 2; ++k) {
      const JplGate& gt = gates_[(size_t)(c.gate0 + k)];
      mat4 xf = gt.base * mat4::translation({0, ARM_Y, 0}) * mat4::rotationX(-c.open * PI / 2);
      if (!frustum || frustum->contains(armBounds_.transformed(xf))) r.drawMesh(arm_, armMat_, armTex_, xf);
      for (int l = 0; l < 2; ++l) {
        AABB lb{gt.lamp[l] - vec3{LAMP_R, LAMP_R, LAMP_R}, gt.lamp[l] + vec3{LAMP_R, LAMP_R, LAMP_R}};
        if (frustum && !frustum->contains(lb)) continue;
        r.drawMesh(lamp_, c.closed && blink == l ? lampOn_ : lampOff_, {}, mat4::translation(gt.lamp[l]));
      }
    }
  }
}

void JplVisuals::destroy() {
  for (Batch* b : {&foundation_, &post_, &head_, &cross_}) if (b->used) { rhi::destroyMesh(b->mesh); b->used = false; }
  if (built_) { rhi::destroyMesh(arm_); rhi::destroyMesh(lamp_); rhi::destroyTexture(armTex_); armTex_ = {}; built_ = false; }
}

} // namespace eng

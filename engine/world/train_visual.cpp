#include "engine/world/train_visual.h"
#include "engine/world/sway.h"
#include <algorithm>
#include <cmath>
#include <utility>

namespace eng {

namespace {

// keretaVisual3d.ts / dunia3dKonst.ts light constants
constexpr float KORONA_BAKU = 0.22f, KORONA_LEBAR = 1.9f, KORONA_PX = 7, KORONA_MAKS = 0.24f, KORONA_MAJU = 0.12f;
constexpr unsigned WARNA_LAMPU_BAKU = 0xffe9b0, WARNA_AKHIRAN = 0xff2f1c;
// uji3dSemboyan21.ts UKUR_S21
constexpr float S21_TINGGI = 1.54f, S21_MASUK = 0.38f, S21_LENGAN = 0.09f, S21_PELAT = 0.34f, S21_TEBAL = 0.012f, S21_KACA = 0.155f;
constexpr float S21_RUMAH_X = 0.24f, S21_RUMAH_Y = 0.30f, S21_RUMAH_Z = 0.22f;

mat4 rotationZ(float a) { return quat::axisAngle({0, 0, 1}, a).toMat4(); }

AABB transformedBounds(const AABB& b, const mat4& m) {
  AABB r;
  for (int c = 0; c < 8; ++c)
    r.expand(m.transformPoint({c & 1 ? b.max.x : b.min.x, c & 2 ? b.max.y : b.min.y, c & 4 ? b.max.z : b.min.z}));
  return r;
}

vec3 hex(unsigned v) { return {((v >> 16) & 255) / 255.f, ((v >> 8) & 255) / 255.f, (v & 255) / 255.f}; }

// texKabut: white radial gradient in alpha (0.95 centre, 0.38 at 40 %, 0 at the edge) — as signal_visual.cpp
rhi::Texture makeCoronaTexture() {
  const int S = 64; std::vector<uint8_t> px((size_t)S * S * 4);
  for (int y = 0; y < S; ++y) for (int x = 0; x < S; ++x) {
    float d = std::hypot((float)x + 0.5f - S / 2.f, (float)y + 0.5f - S / 2.f) / (S / 2.f);
    float a = d < 0.4f ? 0.95f + (0.38f - 0.95f) * (d / 0.4f) : d < 1 ? 0.38f * (1 - (d - 0.4f) / 0.6f) : 0;
    uint8_t* o = &px[((size_t)y * S + x) * 4]; o[0] = o[1] = o[2] = 255; o[3] = (uint8_t)(a * 255);
  }
  return rhi::createTexture(S, S, rhi::Format::RGBA8, std::as_bytes(std::span(px)), true, false);
}

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

// Disc/cylinder along X: radius r, from x0 to x1, centred at (·, cy, cz).
void cylinderX(MeshBuilder& mb, float r, float x0, float x1, float cy, float cz, int n = 24) {
  uint32_t base = (uint32_t)mb.vertices.size();
  for (int i = 0; i < n; ++i) {
    float a = (float)i / n * 2 * PI, y = cy + r * std::cos(a), z = cz + r * std::sin(a);
    vec3 nrm{0, std::cos(a), std::sin(a)};
    mb.vertex({x0, y, z}, nrm, {0, 0}); mb.vertex({x1, y, z}, nrm, {1, 0});   // side ring: 2i, 2i+1
  }
  for (int i = 0; i < n; ++i) { uint32_t j = (uint32_t)((i + 1) % n); mb.quad(base + 2u * (uint32_t)i, base + 2u * j, base + 2u * j + 1, base + 2u * (uint32_t)i + 1); }
  for (int cap = 0; cap < 2; ++cap) {   // caps as fans around a centre vertex
    float x = cap ? x1 : x0; vec3 nrm{cap ? 1.f : -1.f, 0, 0};
    uint32_t c = (uint32_t)mb.vertices.size(); mb.vertex({x, cy, cz}, nrm, {0.5f, 0.5f});
    for (int i = 0; i < n; ++i) { float a = (float)i / n * 2 * PI; mb.vertex({x, cy + r * std::cos(a), cz + r * std::sin(a)}, nrm, {0, 0}); }
    for (int i = 0; i < n; ++i) { uint32_t j = (uint32_t)((i + 1) % n); if (cap) mb.triangle(c, c + 1 + (uint32_t)i, c + 1 + j); else mb.triangle(c, c + 1 + j, c + 1 + (uint32_t)i); }
  }
}

} // namespace

// §7.1 `warna` per sarana; fallback 0x9aa3ac (loco) / 0x6f7a85 (car).
vec3 TrainVisuals::boxColor(const std::string& s, bool loco) {
  if (s == "cc203" || s == "cc206") return hex(0xe8e8e8);
  if (s == "cc201") return hex(0xe8b23b);
  if (s == "k1" || s == "m1") return hex(0x3b6fd6);
  if (s == "k3") return hex(0xd67f3b);
  if (s == "p" || s == "mp") return hex(0x4a5058);
  if (s == "gd") return hex(0x4a4136);
  if (s == "gk") return hex(0x3c3c40);
  if (s.rfind("krl", 0) == 0) return hex(0xd63b3b);
  return hex(loco ? 0x9aa3ac : 0x6f7a85);
}

void TrainVisuals::init(RollingStock& stock) {
  stock_ = &stock;
  MeshBuilder mb; mb.box({-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f});
  box_ = mb.upload();
  MeshBuilder bb; bb.quad({-0.5f, -0.5f, 0}, {0.5f, -0.5f, 0}, {0.5f, 0.5f, 0}, {-0.5f, 0.5f, 0});
  billboard_ = bb.upload();
  coronaTex_ = makeCoronaTexture();
  coronaMat_ = {}; coronaMat_.unlit = true; coronaMat_.alphaMode = AlphaMode::Blend; coronaMat_.additive = true; coronaMat_.doubleSided = true;
  // Semboyan 21, LEFT unit (extends to -Z from the mount point; uji3dSemboyan21.ts buatSemboyan21 sisi = -1)
  const float sisi = -1;
  { MeshBuilder m; cylinderX(m, S21_PELAT / 2, -S21_TEBAL / 2, S21_TEBAL / 2, 0, sisi * (S21_LENGAN + S21_TEBAL)); s21Plate_ = m.upload(); }
  { MeshBuilder m; m.box({-0.0225f, -0.0225f, sisi < 0 ? -S21_LENGAN : 0}, {0.0225f, 0.0225f, sisi < 0 ? 0 : S21_LENGAN}); s21Iron_ = m.upload(); }
  { MeshBuilder m; float z = sisi * (S21_LENGAN + S21_RUMAH_Z / 2);
    m.box({-S21_RUMAH_X / 2, -S21_RUMAH_Y / 2, z - S21_RUMAH_Z / 2}, {S21_RUMAH_X / 2, S21_RUMAH_Y / 2, z + S21_RUMAH_Z / 2}); s21Housing_ = m.upload(); }
  for (int k = 0; k < 2; ++k) {   // glass discs protruding 11 mm from the housing faces: red to the rear (-X), green forward
    float arah = k == 0 ? -1.f : 1.f, x = arah * (S21_RUMAH_X / 2 + 0.011f);
    MeshBuilder m; cylinderX(m, S21_KACA / 2, x - 0.01f, x + 0.01f, 0, sisi * (S21_LENGAN + S21_RUMAH_Z / 2), 20);
    (k == 0 ? s21Red_ : s21Green_) = m.upload();
  }
}

void TrainVisuals::shutdown() {
  rhi::destroyMesh(box_); rhi::destroyMesh(billboard_); rhi::destroyTexture(coronaTex_);
  rhi::destroyMesh(s21Plate_); rhi::destroyMesh(s21Iron_); rhi::destroyMesh(s21Housing_); rhi::destroyMesh(s21Red_); rhi::destroyMesh(s21Green_);
}

void TrainVisuals::update(const SimState& st, const WorldOrigin& origin, const RailProfile* rail, double timeScale) {
  vehicles_.clear(); labels_.clear(); lights_.clear(); markers_.clear(); poses_.clear();
  // Doors advance in SIM time (a fast clock = fast doors); a clock jump backwards (set clock) resets them.
  float dtSim = 0;
  if (lastClock_ >= 0 && st.clock >= lastClock_ && st.clock - lastClock_ < 3600) dtSim = (float)(st.clock - lastClock_);
  else doors_.clear();
  lastClock_ = st.clock;
  const float skalaKA = sway::skalaLaju(timeScale);
  std::map<std::string, float> doors;

  for (const SimTrain& t : st.trains) {
    // keretaVisual3d.ts: open while dwelling (unless the consist is resting), close otherwise; clamped to the clip
    const bool rehat = t.istirahat;
    float tPintu = doorTime(t.id) + (t.state == "dwell" && !rehat ? dtSim : -dtSim);
    tPintu = std::max(0.f, std::min(doorDuration_, tPintu));
    doors[t.id] = tPintu;
    std::vector<std::pair<const VehicleProto*, int>> poseCache;   // per train: proto -> scrubbed pose index
    const size_t nCars = t.vehicles.size();
    for (size_t vi = 0; vi < nCars; ++vi) {
      const SimVehicle& v = t.vehicles[vi];
      // §7.4: a = front coupler, b = rear coupler (scene XZ), heights from the rail profile.
      vec3 a0 = origin.toScene(v.x1, v.y1, 0), b0 = origin.toScene(v.x2, v.y2, 0);
      float hA0 = rail ? rail->railHeight(v.seg.c_str(), v.s) : 0.f;
      float hB0 = rail ? rail->railHeight(v.seg2.c_str(), v.s2) : 0.f;
      float dx0 = a0.x - b0.x, dz0 = a0.z - b0.z;
      float L0 = std::sqrt(dx0 * dx0 + dz0 * dz0);
      if (L0 < 1e-3f) { dx0 = std::cos(v.heading); dz0 = -std::sin(v.heading); L0 = 1; }
      // §7.5: the noise field moves the two RAIL POINTS (lift + lateral), so shared couplers stay joined
      float kx = -dz0 / L0, kz = dx0 / L0;
      sway::Medan mA = sway::medan(a0.x, a0.z, t.speed, skalaKA), mB = sway::medan(b0.x, b0.z, t.speed, skalaKA);
      float ax = a0.x + kx * mA.geser, az = a0.z + kz * mA.geser, hA = hA0 + mA.naik;
      float bx = b0.x + kx * mB.geser, bz = b0.z + kz * mB.geser, hB = hB0 + mB.naik;
      float dx = ax - bx, dz = az - bz;
      float L = std::sqrt(dx * dx + dz * dz); if (L < 1e-3f) L = v.length > 0 ? v.length : 1;
      float ux = dx / L, uz = dz / L;
      float hy = (hA + hB) / 2;
      float yaw = std::atan2(-dz, dx), pitch = std::atan2(hA - hB, L);
      float cx = (ax + bx) / 2, cz = (az + bz) / 2;
      float kurva = sway::kurvaRel(sway::yawTiga(std::cos(v.t1), std::sin(v.t1)), sway::yawTiga(std::cos(v.t2), std::sin(v.t2)), L);
      float roll = sway::hitungGoyang({cx, cz, t.speed, kurva, 0, skalaKA}).roll;
      // §7.4: the trailing KRL cab car faces backwards (cab at the rear of the consist)
      const bool balik = v.model == "nryJr205KuhaBadan" && vi + 1 == nCars;
      float yawM = yaw + (balik ? PI : 0), pitchM = balik ? -pitch : pitch, rollM = balik ? -roll : roll;
      VehicleInstance inst;
      inst.len = v.length; inst.loco = v.kind == "loco";
      inst.centre = {cx, hy, cz};
      inst.proto = stock_ ? stock_->proto(v.model) : nullptr;
      inst.color = boxColor(v.sarana, inst.loco);
      mat4 rot = mat4::rotationY(yawM) * rotationZ(pitchM) * mat4::rotationX(rollM);   // Euler 'YZX'
      const bool kepala = vi == 0 && !rehat, ekor = vi + 1 == nCars && !rehat;
      float k = 1, bodyL = v.length, bodyW = 3.0f;   // for the light points / tail markers
      vec3 lightCentre;                               // `mo.position` of pasangLampuKA
      const std::vector<LightPoint>* lightPts = nullptr; std::vector<LightPoint> boxLights;
      if (inst.proto) {
        const VehicleProto& p = *inst.proto;
        k = inst.scale = v.length / p.panjang;
        inst.place = mat4::translation({cx, hy + 0.02f, cz}) * rot;
        inst.bounds = transformedBounds(p.bounds, inst.place * mat4::scale({k, k, k}));
        if (!p.restWorld.empty() && !p.doorClips.empty() && tPintu > 0) {
          if (!p.doorClips.empty()) doorDuration_ = std::max(doorDuration_, p.doorDuration);
          int idx = -1;
          for (const auto& [pp, pi] : poseCache) if (pp == &p) idx = pi;
          if (idx < 0) {   // both door clips scrubbed to the same time (sarana3d.ts setelPintu)
            std::vector<mat4> local = p.restLocal;
            for (int c : p.doorClips) scrubAnimation(p.body->nodes, p.body->animations[(size_t)c], tPintu, local);
            poses_.emplace_back(); computeWorld(p.body->nodes, p.body->roots, local, poses_.back());
            idx = (int)poses_.size() - 1; poseCache.push_back({&p, idx});
          }
          inst.pose = idx;
        } else if (!p.doorClips.empty()) doorDuration_ = std::max(doorDuration_, p.doorDuration);
        bodyL = p.size.x * k; bodyW = p.size.z * k;
        lightCentre = {cx, hy + 0.02f, cz}; lightPts = &p.lights;
      } else {
        float h = inst.loco ? 4.0f : 3.7f;
        inst.place = mat4::translation({cx, hy + 0.55f + h / 2, cz}) * rot;
        inst.bounds = transformedBounds({{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}}, inst.place * mat4::scale({v.length, h, 3.0f}));
        lightCentre = {cx, hy + 0.55f, cz}; boxLights = fallbackLights({v.length, h, 3.0f}); lightPts = &boxLights;
      }
      // Head / tail lamps (pasangLampuKA): no roll in the lamp frame, flipped ends for the rear cab
      if (kepala || ekor) {
        mat4 lf = mat4::translation(lightCentre) * mat4::rotationY(yawM) * rotationZ(pitchM);
        for (const LightPoint& lp : *lightPts) {
          int ujung = balik ? -lp.end : lp.end;
          bool tail = lp.role == LightRole::Akhiran;
          bool nyala = tail ? (ekor && ujung == -1) : (kepala && ujung == 1);
          if (!nyala) continue;
          lights_.push_back({lf.transformPoint(lp.pos * k), {ux * (float)ujung, 0, uz * (float)ujung}, tail});
        }
      }
      // Semboyan 21 on the last vehicle: mount points from the BODY size (titikSemboyan21), unflipped frame
      if (ekor) {
        mat4 f = mat4::translation({cx, hy + (inst.proto ? 0.02f : 0.f), cz}) * mat4::rotationY(yaw) * rotationZ(pitch) * mat4::rotationX(roll);
        float x = -(bodyL / 2 - S21_MASUK), z = bodyW / 2;
        markers_.push_back({f * mat4::translation({x, S21_TINGGI, -z}), false});
        markers_.push_back({f * mat4::translation({x, S21_TINGGI, z}), true});
      }
      if (vi == 0) labels_.push_back({t.id, t.no, t.name, t.state, t.speed, inst.centre + vec3{0, 4.5f, 0}, {ux, 0, uz}});
      vehicles_.push_back(inst);
    }
  }
  doors_.swap(doors);
}

void TrainVisuals::drawLights(ModelRenderer& r) {
  if (!night_ || lights_.empty()) return;
  vec3 eye = r.eye();
  float perPx = 2 * std::tan(fovY_ / 2) / (float)std::max(1, viewportH_);
  coronaPool_.clear(); coronaPool_.reserve(lights_.size());   // blended draws keep a Material pointer until flush
  for (const TrainLight& l : lights_) {
    vec3 toCam = eye - l.pos; float d = length(toCam); if (d < 1e-3f) d = 1e-3f;
    float al = std::max(0.f, dot(l.aim, toCam) / d);
    float lebih = l.tail ? 1.f : 1.3f;
    float scale = std::max(std::min(KORONA_BAKU, KORONA_MAKS) * KORONA_LEBAR * lebih, KORONA_PX * lebih * d * perPx);
    vec3 c = hex(l.tail ? WARNA_AKHIRAN : WARNA_LAMPU_BAKU);
    coronaPool_.push_back(coronaMat_);
    coronaPool_.back().baseColor = {c.x, c.y, c.z, 0.16f + 0.84f * al * al};
    vec3 pos = l.pos + toCam * (std::min(KORONA_MAJU, d * 0.5f) / d);
    r.drawMesh(billboard_, coronaPool_.back(), coronaTex_, billboard(pos, eye, scale));
  }
}

void TrainVisuals::draw(ModelRenderer& r, const Frustum* frustum) {
  drawn = culledVehicles = 0;
  Material boxMat; boxMat.metallic = 0; boxMat.roughness = 0.8f;
  for (const VehicleInstance& v : vehicles_) {
    if (frustum && !frustum->contains(v.bounds)) { ++culledVehicles; continue; }
    ++drawn;
    if (v.proto) {
      mat4 base = v.place * mat4::scale({v.scale, v.scale, v.scale});
      const std::vector<mat4>* pose = v.pose >= 0 ? &poses_[(size_t)v.pose] : v.proto->restWorld.empty() ? nullptr : &v.proto->restWorld;
      r.draw(*v.proto->body, base * v.proto->normalize, frustum, pose);
      for (const Attachment& p : v.proto->parts) r.draw(*p.model, base * p.local, frustum);
    } else {
      float h = v.loco ? 4.0f : 3.7f;
      boxMat.baseColor = {v.color.x, v.color.y, v.color.z, 1};
      boxMat.emissive = v.loco ? hex(0x1a1206) : vec3{};
      r.drawMesh(box_, boxMat, {}, v.place * mat4::scale({v.len, h, 3.0f}));
    }
  }
  // Semboyan 21: day = red plate, night = lantern (red glass to the rear, green forward); materials of
  // uji3dSemboyan21.ts. The right-hand unit is the left mesh mirrored in Z (the renderer flips winding).
  static Material iron, red, glassRed, glassGreen; static bool matsInit = false;
  if (!matsInit) {
    matsInit = true;
    iron.baseColor = {hex(0x2b2f33).x, hex(0x2b2f33).y, hex(0x2b2f33).z, 1}; iron.roughness = 0.72f; iron.metallic = 0.55f;
    red.baseColor = {hex(0xc41f1f).x, hex(0xc41f1f).y, hex(0xc41f1f).z, 1}; red.roughness = 0.55f; red.metallic = 0.15f; red.emissive = hex(0x1a0000);
    glassRed.baseColor = {hex(0x2a0000).x, hex(0x2a0000).y, hex(0x2a0000).z, 1}; glassRed.roughness = 0.25f; glassRed.metallic = 0; glassRed.emissive = hex(0xd81616);
    glassGreen.baseColor = {hex(0x002a10).x, hex(0x002a10).y, hex(0x002a10).z, 1}; glassGreen.roughness = 0.25f; glassGreen.metallic = 0; glassGreen.emissive = hex(0x18c04a);
  }
  for (const TailMarker& m : markers_) {
    { vec3 c = m.frame.transformPoint({0, 0, 0}); if (frustum && !frustum->contains(AABB{c - vec3{0.5f, 0.5f, 0.5f}, c + vec3{0.5f, 0.5f, 0.5f}})) continue; }
    mat4 f = m.right ? m.frame * mat4::scale({1, 1, -1}) : m.frame;
    r.drawMesh(s21Iron_, iron, {}, f);
    if (night_) { r.drawMesh(s21Housing_, iron, {}, f); r.drawMesh(s21Red_, glassRed, {}, f); r.drawMesh(s21Green_, glassGreen, {}, f); }
    else r.drawMesh(s21Plate_, red, {}, f);
  }
  drawLights(r);
}

} // namespace eng

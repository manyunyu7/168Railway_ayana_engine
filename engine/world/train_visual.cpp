#include "engine/world/train_visual.h"
#include <cmath>

namespace eng {

namespace {

mat4 rotationZ(float a) { return quat::axisAngle({0, 0, 1}, a).toMat4(); }

AABB transformedBounds(const AABB& b, const mat4& m) {
  AABB r;
  for (int c = 0; c < 8; ++c)
    r.expand(m.transformPoint({c & 1 ? b.max.x : b.min.x, c & 2 ? b.max.y : b.min.y, c & 4 ? b.max.z : b.min.z}));
  return r;
}

vec3 hex(unsigned v) { return {((v >> 16) & 255) / 255.f, ((v >> 8) & 255) / 255.f, (v & 255) / 255.f}; }

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
}

void TrainVisuals::shutdown() { rhi::destroyMesh(box_); }

void TrainVisuals::update(const SimState& st, const WorldOrigin& origin, const RailProfile* rail) {
  vehicles_.clear(); labels_.clear();
  for (const SimTrain& t : st.trains) {
    bool first = true;
    for (size_t vi = 0; vi < t.vehicles.size(); ++vi) {
      const SimVehicle& v = t.vehicles[vi];
      // §7.4: a = front coupler, b = rear coupler (scene XZ), heights from the rail profile.
      vec3 a = origin.toScene(v.x1, v.y1, 0), b = origin.toScene(v.x2, v.y2, 0);
      float hA = rail ? rail->railHeight(v.seg.c_str(), v.s) : 0.f;
      float hB = rail ? rail->railHeight(v.seg2.c_str(), v.s2) : 0.f;
      float dx = a.x - b.x, dz = a.z - b.z;
      float L = std::sqrt(dx * dx + dz * dz);
      if (L < 1e-3f) { dx = std::cos(v.heading); dz = -std::sin(v.heading); L = 1; }
      float yaw = std::atan2(-dz, dx), pitch = std::atan2(hA - hB, L), roll = 0;
      // §7.4: the trailing KRL cab car faces backwards (cab at the rear of the consist)
      if (v.model == "nryJr205KuhaBadan" && vi + 1 == t.vehicles.size()) { yaw += PI; pitch = -pitch; roll = -roll; }
      VehicleInstance inst;
      inst.len = v.length; inst.loco = v.kind == "loco";
      inst.centre = {(a.x + b.x) * 0.5f, (hA + hB) * 0.5f, (a.z + b.z) * 0.5f};
      inst.proto = stock_ ? stock_->proto(v.model) : nullptr;
      inst.color = boxColor(v.sarana, inst.loco);
      mat4 rot = mat4::rotationY(yaw) * rotationZ(pitch) * mat4::rotationX(roll);   // Euler 'YZX'
      if (inst.proto) {
        inst.scale = v.length / inst.proto->panjang;
        inst.place = mat4::translation({inst.centre.x, inst.centre.y + 0.02f, inst.centre.z}) * rot;
        inst.bounds = transformedBounds(inst.proto->bounds, inst.place * mat4::scale({inst.scale, inst.scale, inst.scale}));
      } else {
        float h = inst.loco ? 4.0f : 3.7f;
        inst.place = mat4::translation({inst.centre.x, inst.centre.y + 0.55f + h / 2, inst.centre.z}) * rot;
        inst.bounds = transformedBounds({{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}}, inst.place * mat4::scale({v.length, h, 3.0f}));
      }
      if (first) {
        labels_.push_back({t.id, t.no, t.name, t.state, t.speed, inst.centre + vec3{0, 4.5f, 0}});
        first = false;
      }
      vehicles_.push_back(inst);
    }
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
      r.draw(*v.proto->body, base * v.proto->normalize, frustum);
      for (const Attachment& p : v.proto->parts) r.draw(*p.model, base * p.local, frustum);
    } else {
      float h = v.loco ? 4.0f : 3.7f;
      boxMat.baseColor = {v.color.x, v.color.y, v.color.z, 1};
      boxMat.emissive = v.loco ? hex(0x1a1206) : vec3{};
      r.drawMesh(box_, boxMat, {}, v.place * mat4::scale({v.len, h, 3.0f}));
    }
  }
}

} // namespace eng

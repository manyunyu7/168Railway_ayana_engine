#include "engine/world/avatar_visual.h"
#include "engine/render/mesh_builder.h"
#include <algorithm>
#include <cmath>

namespace eng {

namespace {
// Body measurements (metres, 1.70 m figure): feet at y = 0.
constexpr float KAKI = 0.8f, BADAN = 0.6f, KEPALA = 0.25f, SEPATU = 0.1f;
constexpr float Y_BADAN = KAKI, Y_KEPALA = KAKI + BADAN, Y_TOPI = KAKI + BADAN + KEPALA;
constexpr float BAHU = 0.29f, LENGAN = 0.5f;
const vec3 KULIT[6] = {{0.96f, 0.82f, 0.70f}, {0.90f, 0.72f, 0.56f}, {0.80f, 0.60f, 0.44f},
                       {0.64f, 0.45f, 0.31f}, {0.45f, 0.30f, 0.21f}, {0.29f, 0.19f, 0.13f}};
vec3 rgb24(int v) { return {(float)((v >> 16) & 255) / 255.f, (float)((v >> 8) & 255) / 255.f, (float)(v & 255) / 255.f}; }

// Right-arm pitch (radians about the lateral axis, + = raised forward/up) of a gesture at time `t`.
float lenganGesture(AvatarGesture g, float t) {
  float d = std::max(0.001f, avatarGestureDuration(g));
  float u = std::clamp(t / d, 0.f, 1.f);
  float naik = std::sin(std::min(1.f, u * 3.f) * PI * 0.5f);   // ~1/3 of the clip to reach the pose
  switch (g) {
    case AvatarGesture::S40: case AvatarGesture::S3: return naik * PI;              // straight up (S40 with the baton)
    case AvatarGesture::S1: case AvatarGesture::Tunjuk: return naik * (PI * 0.5f);   // straight ahead
    case AvatarGesture::Hormat: return naik * (PI * 0.72f);                          // hand to the cap
    case AvatarGesture::Lambai: return naik * (PI * 0.82f) + std::sin(u * PI * 8) * 0.25f * naik;
    default: return 0;
  }
}
} // namespace

vec3 AvatarVisuals::skinColor(int i) { return KULIT[std::clamp(i, 0, 5)]; }

vec3 AvatarVisuals::shirtColor(const Pakaian& p) {
  if (p.warnaBaju >= 0) return rgb24(p.warnaBaju);
  if (p.baju.rfind("baju-ppka-", 0) == 0) return {0.95f, 0.95f, 0.96f};   // PPKA whites
  return {0.55f, 0.62f, 0.72f};
}

vec3 AvatarVisuals::trouserColor(const Pakaian& p) {
  if (p.celana.find("jeans") != std::string::npos) return {0.20f, 0.28f, 0.45f};
  return {0.12f, 0.12f, 0.14f};
}

void AvatarVisuals::build() {
  if (built_) return;
  MeshBuilder mb; mb.box({-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f});
  box_ = mb.upload();
  built_ = true;
}

void AvatarVisuals::destroy() { if (built_) { rhi::destroyMesh(box_); built_ = false; } }

void AvatarVisuals::part(ModelRenderer& r, const mat4& root, vec3 centre, vec3 size, vec3 color) {
  Material m; m.baseColor = {color.x, color.y, color.z, 1}; m.metallic = 0; m.roughness = 0.85f;
  r.drawMesh(box_, m, rhi::Texture{}, root * mat4::translation(centre) * mat4::scale(size));
}

void AvatarVisuals::drawAvatar(ModelRenderer& r, const Avatar& a, float t) {
  if (!built_) return;
  const Pakaian& p = a.state.pakaian;
  const vec3 kulit = skinColor(p.kulit), baju = shirtColor(p), celana = trouserColor(p);
  const mat4 root = mat4::translation(a.pos) * mat4::rotationY(a.bodyYaw);

  // limb swing: walking / running move the legs and the free arm, standing still does not
  float laju = a.anim == AvatarAnim::Lari ? 4.5f : a.anim == AvatarAnim::Jalan ? 1.4f : 0;
  float ayun = laju > 0 ? std::sin(t * (laju * 2.6f)) * (laju > 2 ? 0.75f : 0.45f) : 0;

  // legs (trousers) + shoes: two boxes swinging in opposite phase about the hips
  for (int s = 0; s < 2; ++s) {
    float sign = s == 0 ? 1.f : -1.f;
    mat4 hip = root * mat4::translation({0, KAKI, sign * 0.11f}) * quat::axisAngle({0, 0, 1}, ayun * sign).toMat4();
    part(r, hip, {0, -KAKI * 0.5f, 0}, {0.17f, KAKI - SEPATU, 0.16f}, celana);
    part(r, hip, {0.03f, -KAKI + SEPATU * 0.5f, 0}, {0.24f, SEPATU, 0.16f}, {0.08f, 0.08f, 0.09f});
  }
  // torso + head
  part(r, root, {0, Y_BADAN + BADAN * 0.5f, 0}, {0.28f, BADAN, 0.46f}, baju);
  part(r, root, {0, Y_KEPALA + KEPALA * 0.5f, 0}, {KEPALA, KEPALA, KEPALA}, kulit);
  if (!p.topi.empty()) {
    vec3 warna = p.topi == "topi-ppka-merah" ? vec3{0.72f, 0.10f, 0.12f} : vec3{0.20f, 0.22f, 0.26f};
    part(r, root, {0, Y_TOPI + 0.04f, 0}, {0.27f, 0.08f, 0.28f}, warna);
    part(r, root, {0.15f, Y_TOPI + 0.01f, 0}, {0.10f, 0.03f, 0.25f}, warna);   // peak, facing forward
  }
  // arms: the RIGHT one (local -Z) carries the gesture, the left one swings with the legs
  float pitchKanan = a.state.gesture != AvatarGesture::None ? lenganGesture(a.state.gesture, a.state.gestureT) : -ayun;
  for (int s = 0; s < 2; ++s) {
    float sign = s == 0 ? -1.f : 1.f;   // -1 = right
    float pitch = sign < 0 ? pitchKanan : ayun;
    mat4 bahu = root * mat4::translation({0, Y_KEPALA - 0.05f, sign * BAHU}) * quat::axisAngle({0, 0, 1}, pitch).toMat4();
    part(r, bahu, {0, -LENGAN * 0.5f, 0}, {0.12f, LENGAN, 0.12f}, sign < 0 ? baju : baju);
    part(r, bahu, {0, -LENGAN - 0.06f, 0}, {0.12f, 0.12f, 0.12f}, kulit);   // hand
  }
  // the S40 baton: a white stick in the right hand while that gesture runs
  if (a.state.gesture == AvatarGesture::S40) {
    mat4 bahu = root * mat4::translation({0, Y_KEPALA - 0.05f, -BAHU}) * quat::axisAngle({0, 0, 1}, pitchKanan).toMat4();
    part(r, bahu, {0, -LENGAN - 0.36f, 0}, {0.05f, 0.55f, 0.05f}, {0.95f, 0.95f, 0.95f});
  }
}

void AvatarVisuals::draw(ModelRenderer& r, const AvatarSet& set, float t) {
  if (!built_) return;
  drawAvatar(r, set.local, t);
  for (const auto& [id, a] : set.remote) { (void)id; drawAvatar(r, a, t); }
}

} // namespace eng

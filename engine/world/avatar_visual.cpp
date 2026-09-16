#include "engine/world/avatar_visual.h"
#include "engine/render/mesh_builder.h"
#include <algorithm>
#include <cmath>

namespace eng {

namespace {
// Placeholder body measurements (metres, 1.70 m figure): feet at y = 0.
constexpr float KAKI = 0.8f, BADAN = 0.6f, KEPALA = 0.25f, SEPATU = 0.1f;
constexpr float Y_BADAN = KAKI, Y_KEPALA = KAKI + BADAN, Y_TOPI = KAKI + BADAN + KEPALA;
constexpr float BAHU = 0.29f, LENGAN = 0.5f;
const vec3 KULIT[6] = {{0.96f, 0.82f, 0.70f}, {0.90f, 0.72f, 0.56f}, {0.80f, 0.60f, 0.44f},
                       {0.64f, 0.45f, 0.31f}, {0.45f, 0.30f, 0.21f}, {0.29f, 0.19f, 0.13f}};
vec3 rgb24(int v) { return {(float)((v >> 16) & 255) / 255.f, (float)((v >> 8) & 255) / 255.f, (float)(v & 255) / 255.f}; }

// Locomotion set of the PPKA rig (docs/avatar-aset.md §4): the clip speeds the walk/run cycles were authored for.
constexpr float V_JALAN = 1.4f, V_LARI = 4.5f;

// The pieces carry vertex colours (COLOR_0) the runtime does not read, and a white baseColorFactor, so the
// colour a piece is drawn with is decided here. `tint: true` pieces are painted neutral and take the player's
// colour; the others get the dominant colour of their own mesh. Unknown ids fall back per slot.
vec3 warnaPotongan(const Pakaian& p, const std::string& slot, const std::string& id) {
  if (slot == "tubuh") return AvatarVisuals::skinColor(p.kulit);
  if (id == "baju-ppka-putih") return {0.888f, 0.896f, 0.913f};
  if (id == "baju-kaos") return AvatarVisuals::shirtColor(p);
  if (id == "celana-hitam") return {0.027f, 0.031f, 0.037f};
  if (id == "celana-jeans") return {0.050f, 0.109f, 0.242f};
  if (id == "sepatu-hitam") return {0.012f, 0.012f, 0.014f};
  if (id == "topi-ppka-merah") return {0.527f, 0.020f, 0.013f};
  if (id == "topi-kaos") return AvatarVisuals::shirtColor(p);
  if (id == "atribut-tongkat-s40") return {0.888f, 0.896f, 0.913f};
  if (id == "atribut-ht") return {0.027f, 0.031f, 0.037f};
  if (slot == "baju" || slot == "topi") return AvatarVisuals::shirtColor(p);
  if (slot == "celana") return AvatarVisuals::trouserColor(p);
  if (slot == "sepatu") return {0.012f, 0.012f, 0.014f};
  return {0.6f, 0.6f, 0.62f};
}

// Right-arm pitch (radians about the lateral axis, + = raised forward/up) of a gesture at time `t` — the
// PLACEHOLDER's stand-in for the real clips.
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

const std::vector<std::string>& AvatarVisuals::gestureJoints() {
  static const std::vector<std::string> j{"Shoulder.R", "Head"};   // + their sub-trees (LowerArm.R, Hand.R, Prop.R)
  return j;
}

const char* AvatarVisuals::gestureClip(AvatarGesture g) { return avatarGestureName(g); }

float AvatarVisuals::animSpeed(AvatarAnim a) {
  return a == AvatarAnim::Lari ? V_LARI : a == AvatarAnim::Jalan ? V_JALAN : 0.f;
}

std::vector<AvatarVisuals::Piece> AvatarVisuals::pieces(const Pakaian& p) {
  std::vector<Piece> out;
  auto add = [&](const char* slot, const std::string& id) {
    if (id.empty()) return;
    out.push_back({slot, id, warnaPotongan(p, slot, id)});
  };
  add("tubuh", p.tubuh);
  add("baju", p.baju);
  add("celana", p.celana);
  add("sepatu", p.sepatu);
  add("topi", p.topi);
  for (const std::string& a : p.atribut) add("atribut", a);
  return out;
}

void AvatarVisuals::build() {
  if (built_) return;
  MeshBuilder mb; mb.box({-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f});
  box_ = mb.upload();
  built_ = true;
}

void AvatarVisuals::destroy() {
  if (built_) { rhi::destroyMesh(box_); built_ = false; }
  figs_.clear(); binds_.clear();
  cat_ = nullptr;   // the scene that owns it may be going away; eng_avatar_draw re-attaches it
  rig_ = nullptr; rigSkin_ = -1; rigNodeByName_.clear(); gestureMask_.clear();
}

// ---- skinned path --------------------------------------------------------------------------------------

const GpuModel* AvatarVisuals::rig() {
  if (!cat_) return nullptr;
  const GpuModel* m = cat_->model(slotId(RIG));   // nullptr while the host still owes us the file
  if (m == rig_) return rig_ && rigSkin_ >= 0 ? rig_ : nullptr;
  rig_ = m; rigSkin_ = -1; rigNodeByName_.clear(); gestureMask_.clear(); binds_.clear();
  for (auto& [id, f] : figs_) { (void)id; f.sourced = false; }
  if (!m || m->skins.empty() || m->animations.empty()) return nullptr;
  rigSkin_ = 0;
  for (size_t i = 0; i < m->nodes.size(); ++i) rigNodeByName_[m->nodes[i].name] = (int)i;
  gestureMask_ = jointMask(m->nodes, gestureJoints());
  return rig_;
}

// A piece's own skin lists node indices of ITS file; the pose lives on the rig's nodes. Match them by joint
// name once and keep the mapping (22 ints). When the order is the rig's own, the rig palette is reused.
const AvatarVisuals::Bind* AvatarVisuals::bind(const std::string& id, const GpuModel& m) {
  auto it = binds_.find(id);
  if (it != binds_.end()) return &it->second;
  Bind b;
  if (m.skins.empty()) { binds_[id] = b; return &binds_[id]; }
  const Skin& sk = m.skins[0];
  const Skin& rs = rig_->skins[(size_t)rigSkin_];
  b.rigNode.assign(sk.joints.size(), -1);
  bool same = sk.joints.size() == rs.joints.size();
  for (size_t j = 0; j < sk.joints.size(); ++j) {
    int n = sk.joints[j];
    if (n < 0 || n >= (int)m.nodes.size()) { same = false; continue; }
    auto f = rigNodeByName_.find(m.nodes[(size_t)n].name);
    if (f == rigNodeByName_.end()) { same = false; continue; }
    b.rigNode[j] = f->second;
    if (same && (j >= rs.joints.size() || rs.joints[j] != f->second)) same = false;
  }
  if (same)   // identical joint order AND identical bind pose -> one palette drives every piece
    for (size_t j = 0; j < sk.inverseBind.size() && same; ++j) {
      if (j >= rs.inverseBind.size()) { same = false; break; }
      for (int c = 0; c < 4 && same; ++c)
        for (int r = 0; r < 4 && same; ++r)
          if (std::fabs(sk.inverseBind[j].m[c][r] - rs.inverseBind[j].m[c][r]) > 1e-4f) same = false;
    }
  b.sameAsRig = same;
  binds_[id] = std::move(b);
  return &binds_[id];
}

AvatarVisuals::Figure& AvatarVisuals::figure(const Avatar& a, float t) {
  Figure& f = figs_[a.id];
  float dt = f.lastT < 0 ? 0.f : std::clamp(t - f.lastT, 0.f, 0.2f);
  f.lastT = t;
  if (!rig_) return f;
  if (!f.sourced) {
    f.player.setSource(&rig_->nodes, &rig_->animations);
    f.player.setLocomotion({{f.player.clipIndex("diam"), 0, 0},
                            {f.player.clipIndex("jalan"), V_JALAN, V_JALAN},
                            {f.player.clipIndex("lari"), V_LARI, V_LARI}});
    f.sourced = true; f.anim = AvatarAnim::Diam; f.gesture = AvatarGesture::None; f.gestureT = -1;
    f.param = animSpeed(a.anim);
  }
  // locomotion parameter: the anim is all a remote figure reports, so the speed is its nominal one, eased so
  // a walk->run switch blends instead of snapping
  float target = animSpeed(a.anim);
  f.param += (target - f.param) * std::min(1.f, dt * 6.f);
  f.player.setParam(f.param);
  // jump / sit are base clips over the locomotion; sitting holds the last frame until the anim changes
  if (a.anim != f.anim) {
    if (a.anim == AvatarAnim::Lompat) f.player.play(f.player.clipIndex("lompat"), false);
    else if (a.anim == AvatarAnim::Duduk) f.player.playHold(f.player.clipIndex("duduk"));
    else if (f.anim == AvatarAnim::Duduk || f.anim == AvatarAnim::Lompat) f.player.stopBase();
    f.anim = a.anim;
  }
  // a gesture starts when the name changes, or when the same one is restarted (gestureT jumps back)
  AvatarGesture g = a.state.gesture;
  if (g != AvatarGesture::None && (g != f.gesture || a.state.gestureT < f.gestureT)) {
    int c = f.player.clipIndex(gestureClip(g));
    if (c >= 0) f.player.gesture(c, gestureMask_, 0.25f);
  }
  f.gesture = g; f.gestureT = g == AvatarGesture::None ? -1 : a.state.gestureT;
  f.player.update(dt);
  return f;
}

void AvatarVisuals::drawAvatar(ModelRenderer& r, const Avatar& a, float t) {
  // figures nobody drew for a while (a remote that left) let go of their player
  if (figs_.size() > 8)
    for (auto it = figs_.begin(); it != figs_.end();) it = (it->second.lastT >= 0 && t - it->second.lastT > 5.f) ? figs_.erase(it) : ++it;

  const GpuModel* rg = rig();
  if (!rg) { drawPlaceholder(r, a, t); return; }
  Figure& f = figure(a, t);
  const mat4 root = mat4::translation(a.pos) * mat4::rotationY(a.bodyYaw + YAW_MODEL);

  f.player.localMatrices(local_);
  computeWorld(rg->nodes, rg->roots, local_, world_);
  jointPalette(rg->skins[(size_t)rigSkin_], world_, palRig_);

  bool any = false;
  for (const Piece& p : pieces(a.state.pakaian)) {
    GpuModel* m = cat_->model(slotId(p.id));
    if (!m || m->skins.empty() || !m->textured()) continue;
    const Bind* b = bind(p.id, *m);
    const std::vector<mat4>* pal = &palRig_;
    if (!b->sameAsRig) {
      const Skin& sk = m->skins[0];
      palPiece_.assign(sk.joints.size(), mat4::identity());
      for (size_t j = 0; j < sk.joints.size(); ++j) {
        int n = b->rigNode[j];
        if (n < 0 || n >= (int)world_.size()) continue;
        palPiece_[j] = j < sk.inverseBind.size() ? world_[(size_t)n] * sk.inverseBind[j] : world_[(size_t)n];
      }
      pal = &palPiece_;
    }
    // the tint IS the piece's colour here (the models are white with vertex colours the runtime drops), so it
    // is written into the material for the duration of the call — every avatar piece is opaque, i.e. drawn
    // immediately by submit(), so no other figure can see the borrowed colour
    std::vector<Material> saved = m->materials;
    for (Material& mt : m->materials) { mt.baseColor = {p.tint.x, p.tint.y, p.tint.z, 1}; mt.metallic = 0; mt.roughness = 0.85f; }
    r.drawSkinned(*m, root, *pal, nullptr, 1.0f);
    m->materials = std::move(saved);
    any = true;
  }
  if (!any) drawPlaceholder(r, a, t);   // outfit still streaming: boxes rather than an invisible player
}

// ---- placeholder ---------------------------------------------------------------------------------------

void AvatarVisuals::part(ModelRenderer& r, const mat4& root, vec3 centre, vec3 size, vec3 color) {
  Material m; m.baseColor = {color.x, color.y, color.z, 1}; m.metallic = 0; m.roughness = 0.85f;
  r.drawMesh(box_, m, rhi::Texture{}, root * mat4::translation(centre) * mat4::scale(size));
}

void AvatarVisuals::drawPlaceholder(ModelRenderer& r, const Avatar& a, float t) {
  if (!built_) return;
  const Pakaian& p = a.state.pakaian;
  const vec3 kulit = skinColor(p.kulit), baju = shirtColor(p), celana = trouserColor(p);
  const mat4 root = mat4::translation(a.pos) * mat4::rotationY(a.bodyYaw);

  // limb swing: walking / running move the legs and the free arm, standing still does not
  float laju = animSpeed(a.anim);
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
    part(r, bahu, {0, -LENGAN * 0.5f, 0}, {0.12f, LENGAN, 0.12f}, baju);
    part(r, bahu, {0, -LENGAN - 0.06f, 0}, {0.12f, 0.12f, 0.12f}, kulit);   // hand
  }
  // the S40 baton: a white stick in the right hand while that gesture runs
  if (a.state.gesture == AvatarGesture::S40) {
    mat4 bahu = root * mat4::translation({0, Y_KEPALA - 0.05f, -BAHU}) * quat::axisAngle({0, 0, 1}, pitchKanan).toMat4();
    part(r, bahu, {0, -LENGAN - 0.36f, 0}, {0.05f, 0.55f, 0.05f}, {0.95f, 0.95f, 0.95f});
  }
}

void AvatarVisuals::draw(ModelRenderer& r, const AvatarSet& set, float t) {
  drawAvatar(r, set.local, t);
  for (const auto& [id, a] : set.remote) { (void)id; drawAvatar(r, a, t); }
}

} // namespace eng

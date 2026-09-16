// Skeletal animation: pose sampling, blending, gesture layers and the GPU joint palette.
//
// Everything works on the node array of a Model (`Model::nodes` / `GpuModel::nodes`) — a skeleton is
// just the subset of nodes a `Skin` lists as joints, so no separate bone structure is needed.
//
//   Pose              one TRS per node (local space), the unit every operation produces and consumes
//   samplePose        a clip at time t (STEP / LINEAR; CUBICSPLINE arrives as LINEAR from the parser)
//   blendPose         linear blend of two poses (translation/scale lerp, rotation slerp)
//   layerPose         gesture layer: the joints in a mask take the layer's pose at a weight
//   poseToLocal       Pose -> local matrices; `computeWorld` (model.h) then chains parents
//   jointPalette      world[joint] * inverseBind -> the mat4 array `ModelRenderer::drawSkinned` wants
//
// Header-only on purpose: the root CMakeLists lists the engine sources one by one and is off-limits
// to module agents, so a new .cpp in engine/render would not be compiled.
#pragma once
#include "engine/asset/model.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

namespace eng {

// Hard limit of the skinned shader's uniform array (64 * 64 B = 4 KB, safe on GL 4.1 and GLES 3.0,
// whose minimum is 256 vec4 = 4 KB for the vertex stage).
constexpr int MAX_JOINTS = 64;

// A local pose: translation / rotation / scale per node of the model's node array.
struct Pose {
  std::vector<vec3> t;
  std::vector<quat> r;
  std::vector<vec3> s;
  size_t size() const { return t.size(); }
  void resize(size_t n) { t.assign(n, vec3{}); r.assign(n, quat{}); s.assign(n, vec3{1, 1, 1}); }
  bool empty() const { return t.empty(); }
};

inline quat slerp(quat a, quat b, float f) {
  float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
  if (d < 0) { b = {-b.x, -b.y, -b.z, -b.w}; d = -d; }
  float w0 = 1 - f, w1 = f;
  if (d < 0.9995f) {
    float th = std::acos(std::fmin(1.f, d)), sn = std::sin(th);
    if (sn > 1e-6f) { w0 = std::sin((1 - f) * th) / sn; w1 = std::sin(f * th) / sn; }
  }
  quat q{a.x * w0 + b.x * w1, a.y * w0 + b.y * w1, a.z * w0 + b.z * w1, a.w * w0 + b.w * w1};
  float l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  if (l > 1e-9f) { q.x /= l; q.y /= l; q.z /= l; q.w /= l; }
  return q;
}

// The model's rest TRS, the starting point of every sampled pose (channels replace one component).
inline void restPose(const std::vector<Node>& nodes, Pose& p) {
  p.resize(nodes.size());
  for (size_t i = 0; i < nodes.size(); ++i) { p.t[i] = nodes[i].translation; p.r[i] = nodes[i].rotation; p.s[i] = nodes[i].scale; }
}

// One clip at time `t` seconds. loop = wrap into [0, duration), else clamp. Nodes the clip does not
// target keep their rest TRS.
inline void samplePose(const std::vector<Node>& nodes, const Animation& a, float t, bool loop, Pose& out) {
  restPose(nodes, out);
  if (a.duration > 1e-6f) t = loop ? t - a.duration * std::floor(t / a.duration) : std::fmin(std::fmax(t, 0.f), a.duration);
  else t = 0;
  for (const AnimChannel& ch : a.channels) {
    if (ch.sampler < 0 || ch.sampler >= (int)a.samplers.size() || ch.node < 0 || ch.node >= (int)nodes.size()) continue;
    const AnimSampler& sm = a.samplers[(size_t)ch.sampler];
    if (sm.times.empty()) continue;
    size_t n = sm.times.size(), k = 0;
    while (k + 1 < n && sm.times[k + 1] <= t) ++k;
    size_t k1 = std::min(k + 1, n - 1);
    float f = 0;
    if (!sm.step && k1 != k) { float dt = sm.times[k1] - sm.times[k]; f = dt > 1e-9f ? std::fmin(1.f, std::fmax(0.f, (t - sm.times[k]) / dt)) : 0; }
    const float* v0 = &sm.values[k * sm.comps];
    const float* v1 = &sm.values[k1 * sm.comps];
    size_t i = (size_t)ch.node;
    if (ch.path == AnimPath::Rotation && sm.comps == 4) out.r[i] = slerp({v0[0], v0[1], v0[2], v0[3]}, {v1[0], v1[1], v1[2], v1[3]}, f);
    else if (sm.comps >= 3) {
      vec3 v{v0[0] + (v1[0] - v0[0]) * f, v0[1] + (v1[1] - v0[1]) * f, v0[2] + (v1[2] - v0[2]) * f};
      if (ch.path == AnimPath::Translation) out.t[i] = v; else if (ch.path == AnimPath::Scale) out.s[i] = v;
    }
  }
}

// out = a at w = 0, b at w = 1. `out` may alias `a` or `b`. Sizes must match (a wins otherwise).
inline void blendPose(const Pose& a, const Pose& b, float w, Pose& out) {
  if (a.size() != b.size()) { out = a; return; }
  w = std::fmin(std::fmax(w, 0.f), 1.f);
  if (&out != &a) out.resize(a.size());
  for (size_t i = 0; i < a.size(); ++i) {
    out.t[i] = a.t[i] + (b.t[i] - a.t[i]) * w;
    out.s[i] = a.s[i] + (b.s[i] - a.s[i]) * w;
    out.r[i] = slerp(a.r[i], b.r[i], w);
  }
}

// Gesture layer: only the nodes listed in `mask` move towards `layer`, at weight `w` (fade in/out).
// This is an override layer on the masked joints — what the avatar spec calls "additive": the base
// pose keeps driving everything the gesture does not claim (legs keep walking while an arm salutes).
inline void layerPose(Pose& base, const Pose& layer, const std::vector<int>& mask, float w) {
  if (base.size() != layer.size() || w <= 0) return;
  w = std::fmin(w, 1.f);
  for (int i : mask) {
    if (i < 0 || i >= (int)base.size()) continue;
    size_t j = (size_t)i;
    base.t[j] = base.t[j] + (layer.t[j] - base.t[j]) * w;
    base.s[j] = base.s[j] + (layer.s[j] - base.s[j]) * w;
    base.r[j] = slerp(base.r[j], layer.r[j], w);
  }
}

// Node indices of the named joints; with `withDescendants` their whole sub-tree comes along
// (mask {"UpperArm.R", "Head"} then covers the lower arm, the hand and the prop anchor).
inline std::vector<int> jointMask(const std::vector<Node>& nodes, const std::vector<std::string>& names, bool withDescendants = true) {
  std::vector<uint8_t> in(nodes.size(), 0);
  for (const std::string& nm : names)
    for (size_t i = 0; i < nodes.size(); ++i)
      if (nodes[i].name == nm) {
        if (!withDescendants) { in[i] = 1; continue; }
        std::vector<int> stack{(int)i};
        while (!stack.empty()) {
          int n = stack.back(); stack.pop_back();
          if (in[(size_t)n]) continue;
          in[(size_t)n] = 1;
          for (int c : nodes[(size_t)n].children) stack.push_back(c);
        }
      }
  std::vector<int> out;
  for (size_t i = 0; i < in.size(); ++i) if (in[i]) out.push_back((int)i);
  return out;
}

inline void poseToLocal(const Pose& p, std::vector<mat4>& local) {
  local.resize(p.size());
  for (size_t i = 0; i < p.size(); ++i) local[i] = trs(p.t[i], p.r[i], p.s[i]);
}

// The matrices the skinned shader wants: palette[j] = world[skin.joints[j]] * inverseBind[j].
// `world` comes from computeWorld(nodes, roots, local, world). The skinned mesh node's own transform
// is deliberately ignored (glTF: joints are already in scene space); the instance transform given to
// drawSkinned places the character.
inline void jointPalette(const Skin& skin, const std::vector<mat4>& world, std::vector<mat4>& palette) {
  palette.assign(skin.joints.size(), mat4::identity());
  for (size_t j = 0; j < skin.joints.size(); ++j) {
    int n = skin.joints[j];
    if (n < 0 || n >= (int)world.size()) continue;
    palette[j] = j < skin.inverseBind.size() ? world[(size_t)n] * skin.inverseBind[j] : world[(size_t)n];
  }
}

// One entry of a locomotion set: a looping clip authored for `param` (m/s for walk/run; 0 = idle).
// `clipSpeed` > 0 time-scales the clip so the feet match the real speed (stride length stays honest).
struct LocoClip { int clip = -1; float param = 0; float clipSpeed = 0; };

// Plays clips over a node array: a locomotion set blended by one parameter (speed), an optional
// one-shot / looping base clip on top (jump, sit), and a masked gesture layer that fades in and out.
// Angles and speeds come from the caller — the player has no idea what a metre is.
class AnimationPlayer {
public:
  void setSource(const std::vector<Node>* nodes, const std::vector<Animation>* clips) {
    nodes_ = nodes; clips_ = clips;
    if (nodes_) restPose(*nodes_, pose_);
  }
  int clipIndex(std::string_view name) const {
    if (!clips_) return -1;
    for (size_t i = 0; i < clips_->size(); ++i) if ((*clips_)[i].name == name) return (int)i;
    return -1;
  }
  // Blend set, sorted by `param` ascending (idle 0, walk 1.4, run 4.5 for the PPKA avatar).
  void setLocomotion(std::vector<LocoClip> set) {
    std::sort(set.begin(), set.end(), [](const LocoClip& a, const LocoClip& b) { return a.param < b.param; });
    loco_ = std::move(set);
  }
  void setParam(float v) { param_ = v; }
  float param() const { return param_; }

  // Base clip that replaces locomotion while it runs (`loop` = until stopBase()).
  void play(int clip, bool loop, float fade = 0.2f) {
    if (clip < 0 || !clips_ || clip >= (int)clips_->size()) return;
    base_ = clip; baseLoop_ = loop; baseTime_ = 0; baseFade_ = std::fmax(fade, 1e-3f); baseW_ = 0; baseOut_ = false;
  }
  void stopBase() { baseOut_ = true; }
  bool baseActive() const { return base_ >= 0; }

  // Gesture layer on the masked joints; plays once, then fades out over `fade` seconds.
  void gesture(int clip, std::vector<int> mask, float fade = 0.25f) {
    if (clip < 0 || !clips_ || clip >= (int)clips_->size()) return;
    gest_ = clip; gestMask_ = std::move(mask); gestTime_ = 0; gestFade_ = std::fmax(fade, 1e-3f); gestW_ = 0; gestOut_ = false;
  }
  void stopGesture() { gestOut_ = true; }
  bool gestureActive() const { return gest_ >= 0; }
  float gestureWeight() const { return gestW_; }

  void update(float dt) {
    if (!nodes_ || !clips_) return;
    if (dt < 0) dt = 0;
    sampleLocomotion(dt);
    if (base_ >= 0) {
      const Animation& a = (*clips_)[(size_t)base_];
      baseTime_ += dt;
      bool done = !baseLoop_ && baseTime_ >= a.duration - baseFade_;
      baseW_ = std::fmin(1.f, std::fmax(0.f, baseW_ + dt / baseFade_ * ((baseOut_ || done) ? -1.f : 1.f)));
      samplePose(*nodes_, a, baseTime_, baseLoop_, tmp_);
      blendPose(pose_, tmp_, baseW_, pose_);
      if (baseW_ <= 0 && (baseOut_ || done)) base_ = -1;
    }
    if (gest_ >= 0) {
      const Animation& a = (*clips_)[(size_t)gest_];
      gestTime_ += dt;
      bool done = gestTime_ >= a.duration;
      gestW_ = std::fmin(1.f, std::fmax(0.f, gestW_ + dt / gestFade_ * ((gestOut_ || done) ? -1.f : 1.f)));
      samplePose(*nodes_, a, gestTime_, false, tmp_);
      layerPose(pose_, tmp_, gestMask_, gestW_);
      if (gestW_ <= 0 && (gestOut_ || done)) gest_ = -1;
    }
  }

  const Pose& pose() const { return pose_; }
  void localMatrices(std::vector<mat4>& local) const { poseToLocal(pose_, local); }
  // Convenience: pose -> local -> world -> palette for one skin, in one call. `nodes`/`roots` are the
  // model's (a GpuModel keeps its own copies, so it works for both).
  void palette(const std::vector<Node>& nodes, const std::vector<int>& roots, const Skin& skin, std::vector<mat4>& out) {
    poseToLocal(pose_, local_);
    computeWorld(nodes, roots, local_, world_);
    jointPalette(skin, world_, out);
  }
  float phase() const { return phase_; }   // 0..1 within the locomotion loop (foot sync across a blend)

private:
  void sampleLocomotion(float dt) {
    if (loco_.empty()) { if (pose_.empty()) restPose(*nodes_, pose_); return; }
    size_t i = 0;
    while (i + 1 < loco_.size() && loco_[i + 1].param <= param_) ++i;
    size_t j = std::min(i + 1, loco_.size() - 1);
    float w = 0;
    if (j != i) { float d = loco_[j].param - loco_[i].param; w = d > 1e-6f ? std::fmin(1.f, std::fmax(0.f, (param_ - loco_[i].param) / d)) : 0; }
    auto dur = [&](size_t k) {
      const LocoClip& lc = loco_[k];
      if (lc.clip < 0 || lc.clip >= (int)clips_->size()) return 1.f;
      float d = (*clips_)[(size_t)lc.clip].duration;
      // a clip authored for clipSpeed played at `param` covers the same ground in less time
      if (lc.clipSpeed > 1e-3f && param_ > 1e-3f) d *= lc.clipSpeed / param_;
      return d > 1e-4f ? d : 1.f;
    };
    float period = dur(i) + (dur(j) - dur(i)) * w;
    phase_ += dt / period;
    phase_ -= std::floor(phase_);
    auto sample = [&](size_t k, Pose& out) {
      const LocoClip& lc = loco_[k];
      if (lc.clip < 0 || lc.clip >= (int)clips_->size()) { restPose(*nodes_, out); return; }
      const Animation& a = (*clips_)[(size_t)lc.clip];
      samplePose(*nodes_, a, phase_ * a.duration, true, out);
    };
    sample(i, pose_);
    if (j != i && w > 0) { sample(j, tmp_); blendPose(pose_, tmp_, w, pose_); }
  }

  const std::vector<Node>* nodes_ = nullptr;
  const std::vector<Animation>* clips_ = nullptr;
  std::vector<LocoClip> loco_;
  Pose pose_, tmp_;
  std::vector<mat4> local_, world_;
  std::vector<int> gestMask_;
  float param_ = 0, phase_ = 0;
  int base_ = -1, gest_ = -1;
  float baseTime_ = 0, baseW_ = 0, baseFade_ = 0.2f;
  float gestTime_ = 0, gestW_ = 0, gestFade_ = 0.25f;
  bool baseLoop_ = false, baseOut_ = false, gestOut_ = false;
};

} // namespace eng

// AvatarVisuals — PLACEHOLDER body for the multiplayer avatars: a handful of coloured boxes per figure
// (legs / shoes, torso, head, cap, arms) built once from MeshBuilder and drawn with per-part materials.
// The skinned models of docs/multiplayer.md §6 are not in the engine yet; this keeps the ABI, the camera
// and the networking honest in the meantime.
//
// Replacing it with real models: everything a figure draws goes through drawAvatar(renderer, avatar, t).
// A skinned implementation swaps that ONE function (look the outfit ids up in the catalog, pose the shared
// skeleton from `avatar.anim` / `state.gesture` / `gestureT`, draw the pieces); nothing else — not the ABI,
// not CameraRig, not AvatarSet — knows how a body is drawn.
//
// Local space of a figure: feet at the origin, +X = the direction it faces, +Y up, +Z its LEFT
// (mat4::rotationY(bodyYaw) maps +X onto the scene heading, see engine/world/coords.h).
#pragma once
#include "engine/render/model_renderer.h"
#include "engine/world/avatar.h"

namespace eng {

class AvatarVisuals {
public:
  void build();                       // GPU box mesh (needs a GL context); safe to call twice
  void destroy();
  bool built() const { return built_; }
  // Every avatar of the set: the local one plus the smoothed remotes. `t` = seconds (limb swing phase).
  void draw(ModelRenderer& r, const AvatarSet& set, float t);
  // One figure — the seam a skinned renderer replaces.
  void drawAvatar(ModelRenderer& r, const Avatar& a, float t);

  static vec3 skinColor(int index);   // six skin tones (§4 `kulit`)
  static vec3 shirtColor(const Pakaian& p);
  static vec3 trouserColor(const Pakaian& p);

private:
  rhi::Mesh box_;   // unit cube centred on the origin
  bool built_ = false;
  void part(ModelRenderer& r, const mat4& root, vec3 centre, vec3 size, vec3 color);
};

} // namespace eng

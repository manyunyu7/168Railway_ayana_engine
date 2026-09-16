// AvatarVisuals — how a multiplayer figure is drawn. Two implementations behind ONE seam:
//
//   • the SKINNED body (the real thing): the outfit ids of `AvatarState::pakaian` are resolved against the
//     catalog (`model.json` `avatar[]`, reachable as `avatar:<id>`), the shared 22-joint skeleton of
//     `avatar:rangka` is posed by an AnimationPlayer per figure (locomotion blend by speed, jump / sit as a
//     base clip, gesture as a masked layer on the right arm + head) and every piece is drawn with
//     ModelRenderer::drawSkinned using that one palette. At most one draw call per piece (<= 8 per avatar).
//   • the PLACEHOLDER body (coloured boxes), used while the models are still streaming in, or when there is
//     no catalog at all. Same pose information, no assets needed.
//
// Everything a figure draws goes through drawAvatar(renderer, avatar, t); nothing else — not the ABI, not
// CameraRig, not AvatarSet — knows how a body is drawn. `t` is a monotone seconds clock: the animation dt is
// its delta, so the caller does not have to carry one.
//
// Local space of a figure: feet at the origin, +X = the direction it faces, +Y up, +Z its LEFT
// (mat4::rotationY(bodyYaw) maps +X onto the scene heading, see engine/world/coords.h). The glTF models
// face -Z, so they are drawn with the extra yaw YAW_MODEL = -PI/2.
#pragma once
#include "engine/render/animator.h"
#include "engine/render/model_renderer.h"
#include "engine/world/asset_catalog.h"
#include "engine/world/avatar.h"
#include <map>
#include <string>
#include <vector>

namespace eng {

class AvatarVisuals {
public:
  void build();                       // GPU box mesh for the placeholder (needs a GL context); safe to call twice
  void destroy();
  bool built() const { return built_; }
  // Where the skinned models come from. Without it (or until `avatar:rangka` is resident) the placeholder draws.
  void setCatalog(AssetCatalog* c) { cat_ = c; }
  AssetCatalog* catalog() const { return cat_; }
  // Every avatar of the set: the local one plus the smoothed remotes. `t` = seconds (monotone).
  void draw(ModelRenderer& r, const AvatarSet& set, float t);
  // One figure — the seam.
  void drawAvatar(ModelRenderer& r, const Avatar& a, float t);

  // ---- outfit resolution (pure, tested) ----
  struct Piece { std::string slot, id; vec3 tint{1, 1, 1}; };
  // Draw order: tubuh, baju, celana, sepatu, topi, atribut[]. Empty ids are skipped; the tint is what the
  // piece is drawn with (the models carry vertex colours the runtime does not read, so the colour is ours).
  static std::vector<Piece> pieces(const Pakaian& p);
  static std::string slotId(const std::string& id) { return "avatar:" + id; }   // catalog id of a piece
  static constexpr const char* RIG = "rangka";                                  // skeleton + clips
  // Joints the gesture layer claims: the right arm (Shoulder.R and its sub-tree, which carries Prop.R) + the head.
  static const std::vector<std::string>& gestureJoints();
  static const char* gestureClip(AvatarGesture g);   // clip name ("" for None)
  static float animSpeed(AvatarAnim a);              // locomotion parameter in m/s

  static vec3 skinColor(int index);   // six skin tones (§4 `kulit`)
  static vec3 shirtColor(const Pakaian& p);
  static vec3 trouserColor(const Pakaian& p);
  static constexpr float YAW_MODEL = -1.57079632679f;   // glTF -Z -> local +X

private:
  // One figure's animation state, kept between frames and keyed by avatar id (0 = local).
  struct Figure {
    AnimationPlayer player;
    float lastT = -1, param = 0;
    AvatarAnim anim = AvatarAnim::Diam;
    AvatarGesture gesture = AvatarGesture::None;
    float gestureT = -1;
    bool sourced = false;
  };
  // Per outfit piece: how its own skin's joints map onto the rig's nodes (the pieces are exported separately,
  // so their node arrays differ even though the joint NAMES are the same).
  struct Bind { bool sameAsRig = false; std::vector<int> rigNode; };

  rhi::Mesh box_;   // unit cube centred on the origin (placeholder)
  bool built_ = false;
  AssetCatalog* cat_ = nullptr;

  const GpuModel* rig_ = nullptr;     // avatar:rangka once resident
  int rigSkin_ = -1;
  std::map<std::string, int> rigNodeByName_;
  std::vector<int> gestureMask_;
  std::map<int, Figure> figs_;
  std::map<std::string, Bind> binds_;
  std::vector<mat4> local_, world_, palRig_, palPiece_;   // reused every draw: no per-frame allocation

  const GpuModel* rig();                                   // resolve + cache the skeleton
  Figure& figure(const Avatar& a, float t);                // per-avatar player, advanced to `t`
  const Bind* bind(const std::string& id, const GpuModel& m);
  void drawPlaceholder(ModelRenderer& r, const Avatar& a, float t);
  void part(ModelRenderer& r, const mat4& root, vec3 centre, vec3 size, vec3 color);
};

} // namespace eng

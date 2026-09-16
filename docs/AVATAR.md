# Avatars & the third-person camera

Contract: `../ppka-wannabe-2/docs/multiplayer.md` §4 (`AvatarState` / `Pakaian` JSON), §5 (the `eng_avatar_*` ABI
and camera mode "orang"), §6 (the asset set). This document is the engine side of it.

Files
- `engine/world/avatar.h/.cpp` — `Pakaian`, `AvatarState` (JSON), `Avatar` (one figure + its smoothing buffer),
  `AvatarSet` (local id 0 + remotes by user id). Headless, no GL.
- `engine/world/avatar_visual.h/.cpp` — `AvatarVisuals`, the **placeholder** body (coloured boxes).
- `engine/app/camera_rig.*` — `CamMode::Orang` (index 6, name `"orang"`).
- `engine/api/engine_api_avatar.cpp` — the C ABI + the per-frame hooks called from `engine_api.cpp`.
- `tests/test_avatar.cpp`, `examples/avatartest/` (`ENG_CAPTURE=/tmp/x.ppm ./build/<dir>/avatartest`).

## Coordinates

`AvatarState` is in **sim world space** like every other host coordinate: `x` / `y` are Web-Mercator metres with
y south-positive, `z` is height in metres, `yaw` is radians with 0 = +x. The engine converts once
(`Avatar::place` / `Avatar::store`, the only two places that know the rule):

```
scene    = origin.toScene(x, y, z)      // world y -> scene z, world z -> scene y (engine/world/coords.h)
sceneYaw = -yaw                         // mat4::rotationY maps +X to (cos, -sin) in (x, z)
```

A figure's local space: feet at the origin, **+X is the direction it faces**, +Y up, +Z its left.

## Local avatar

The local avatar **is** the walker of the camera rig. In `CamMode::Orang` the existing walking machine
(`langkahPejalan`: wall slide, floors / stairs / platform kerbs, jump, gravity — shared with `jalan`) moves the
FEET instead of the eye, at 1.4 m/s walking and 4.5 m/s running (Shift). The body yaw follows the direction
actually travelled (it turns with τ 0.12 s, so sliding along a wall turns the figure too) — not the camera's yaw.
`eng_avatar_update` reads `rig.walkerPos() / walkerBodyYaw() / walkerSpeed() / walkerOnGround()` and picks
`anim`: airborne = `lompat`, > 2.2 m/s = `lari`, > 0.15 = `jalan`, else `diam`. `duduk` is host-set only.

`eng_avatar_set_local` therefore takes the **outfit / anim / gesture** and ignores the position; the host reads
back `eng_avatar_local_json()` at 15 Hz and relays that.

## Third-person camera ("orang", mode 6)

Target = the feet + 1.8 m. Boom 4.5 m, scroll 2..9 m (`CameraRig::jarakOrang`, also `eng_set_rig_param`), pitch
−12° when the mode is entered, drag orbits yaw/pitch, WASD is relative to the camera's yaw, damping like the
other rigs (`redam` 12 with the 0.4 s blend-in). The boom is shortened by whatever stands behind the avatar:
a horizontal `WalkCollider::wallRay` for scenery walls plus a segment/AABB test against the vehicle `WalkBox`es,
minus 0.25 m clearance, and the eye is never below the ground + 0.4 m. Entering the mode plants the walker where
the previous view was aimed (`enterOrang`, the `enterWalk` rule).

## ABI (engine/api/engine_api.h)

```c
void        eng_avatar_set_local(const char* json);        // AvatarState; position ignored (the engine drives it)
const char* eng_avatar_local_json(void);                   // static buffer, valid until the next call
void        eng_avatar_upsert(int id, const char* json);   // remote avatar (id = userId); 0 = local -> outfit only
void        eng_avatar_remove(int id);
void        eng_avatar_gesture(const char* nama);          // "s40|s3|s1|hormat|lambai|tunjuk"; "" cancels
void        eng_avatar_outfit(int id, const char* pakaianJson);   // 0 = local
int         eng_avatar_screen(int id, float* out);         // out[2] = css px of the head top; 1 = in front of the camera
```

Gestures run for the clip length of §6 (s40 2.0 s, s3 1.5, s1 1.5, hormat 1.5, lambai 3.2, tunjuk 1.2) and then
disappear by themselves (`gestureT` counts up, the field is dropped from the JSON when it is over).

Remote smoothing happens inside the engine: every `eng_avatar_upsert` pushes a timestamped sample, the drawn
pose is what the avatar was `Avatar::LAG` = 100 ms ago, interpolated between the last two samples, extrapolated
for at most `Avatar::EKSTRA` = 200 ms past the newest one and then frozen. Yaw takes the short way round.

## Replacing the placeholder with skinned models

Everything a figure draws goes through ONE function:

```cpp
void AvatarVisuals::drawAvatar(ModelRenderer& r, const Avatar& a, float t);
```

A skinned implementation resolves `a.state.pakaian` against the catalog (`model.json` `avatar[]`), poses the
shared skeleton from `a.anim` (locomotion blend by speed) and `a.state.gesture` / `gestureT` (additive layer on
the arm + head, fade 0.25 s) and draws the pieces with `ModelRenderer::drawSkinned` + `AnimationPlayer`
(`engine/render/animator.h`, `docs/SKINNING.md`). Nothing else changes: not the ABI, not `AvatarSet`, not
`CameraRig` — none of them knows how a body is drawn. The only two hints the placeholder leaves behind are the
tint rules (`AvatarVisuals::skinColor / shirtColor / trouserColor`), which a skinned version keeps as per-slot
uniform tints, and the figure's local axes (above), which the loaded models must match (glTF faces −Z: the model
transform gets the extra yaw).

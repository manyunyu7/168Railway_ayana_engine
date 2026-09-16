# Avatars & the third-person camera

Contract: `../ppka-wannabe-2/docs/multiplayer.md` §4 (`AvatarState` / `Pakaian` JSON), §5 (the `eng_avatar_*` ABI
and camera mode "orang"), §6 (the asset set). This document is the engine side of it.

Files
- `engine/world/avatar.h/.cpp` — `Pakaian`, `AvatarState` (JSON), `Avatar` (one figure + its smoothing buffer),
  `AvatarSet` (local id 0 + remotes by user id). Headless, no GL.
- `engine/world/avatar_visual.h/.cpp` — `AvatarVisuals`: the **skinned** body (§ *Skinned bodies* below), with the
  coloured-box placeholder kept as the fallback while the models stream in.
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

## Skinned bodies

Everything a figure draws still goes through ONE function:

```cpp
void AvatarVisuals::drawAvatar(ModelRenderer& r, const Avatar& a, float t);   // t = a monotone seconds clock
```

`t` is not a phase any more: its delta is the animation `dt`, so a caller that already ticks a clock needs to
pass nothing else. Per avatar id the class keeps one `AnimationPlayer`; figures nobody drew for 5 s are dropped.

**Assets.** `ppka-wannabe-2/public/model3d/nry-avatar-*.glb` (11 files, ±222 kB, in git — they are the project's
own, see `ppka-wannabe-2/docs/avatar-aset.md`), catalogued in `model.json` `avatar[] {id, slot, berkas, tint, tri}`.
`AssetCatalog` registers each one under the synthetic id **`avatar:<id>`**, so both loading paths already work:

* native — `AssetCatalog::model("avatar:rangka")` converts the GLB with `tools/convert` and caches the `.emod`;
* web — the catalog reports the slot once, the host fetches the file and answers with
  `eng_model_begin_glb("avatar:rangka", …)` (`src/tiga-ayana/avatarAyana.ts` + `duniaAyana.ts`).

`avatar:rangka` carries the 22-joint skeleton and all 11 clips; the outfit pieces are meshes rigged to the same
skeleton (`tests/test_avatar` checks that the joint names, their order and the inverse bind matrices are
identical, which is what lets ONE palette per frame drive every piece — otherwise the palette is rebuilt per
piece by matching joint names, `AvatarVisuals::Bind`).

**Pose.** `AvatarVisuals::pieces(pakaian)` resolves the outfit to the draw list (tubuh, baju, celana, sepatu,
topi, atribut[]) — pure and tested. The player runs the locomotion set `{diam 0, jalan 1.4, lari 4.5}` against a
speed eased from `Avatar::anim`; `lompat` is a one-shot base clip and `duduk` a held one (`AnimationPlayer::playHold`);
a gesture plays its clip as a layer masked to `jointMask(nodes, {"Shoulder.R", "Head"})` — the right arm with its
whole sub-tree (so `Prop.R`, the baton anchor, comes along) plus the head — fading in over 0.25 s and ending by
itself with the clip.

**Tint.** The models carry vertex colours (`COLOR_0`) that the runtime does not read and a white baseColorFactor,
so the colour of a piece is decided here: `kulit` (the six-tone table) for the body, `warnaBaju` for the pieces
the catalog marks `tint: true`, and the dominant colour of its own mesh for everything else. It is written into
the model's materials for the duration of the call — every avatar piece is opaque, so `submit()` draws it
immediately and no other figure sees the borrowed colour.

**Budget.** One draw call per piece, ≤ 8 per avatar (the default PPKA outfit is 6). No per-frame allocation: the
pose / world / palette vectors are members that are reused.

**Orientation.** The figure's local axes are the ones above (+X = facing); the glTF models face −Z, so they are
drawn with the extra yaw `AvatarVisuals::YAW_MODEL = −π/2`.

**Fallback.** No catalog, the rig not resident yet, or no piece resolved → the placeholder boxes draw instead,
with the same pose information. A player is never invisible.

## Host wiring (web)

```c
const char* eng_avatar_assets_json(void);   // ["avatar:rangka","avatar:tubuh-baku",...] for the outfits in play
```

The engine requests every slot it draws by itself, so this is only for prefetching. `src/tiga-ayana/avatarAyana.ts`
(`siapkanAvatarAyana(api)`, `pakaianBaku()`, `slotPakaian()`, `berkasAvatar()`) does that from `duniaAyana`'s
`pramuatModel()`; `duniaAyana.berkasTwin()` resolves `avatar:<id>` to its GLB and `layaniModel()` skips the `.emod`
attempt for avatar slots (they are never baked).

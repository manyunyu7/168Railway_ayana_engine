# Skinning & animation

GPU skeletal animation for the multiplayer avatars (`../ppka-wannabe-2/docs/multiplayer.md` §5–6).
Everything here is engine-side and generic: the avatar module owns the joint names, the clip names and
the metres per second; this layer only blends poses and deforms vertices.

Files: `engine/asset/model.h` (`Skin`, `VertexSkin`), `engine/asset/gltf.cpp` (parse),
`engine/asset/emod.*` (v8), `engine/render/animator.h` (poses, player, palette),
`engine/render/model_renderer.*` + `engine/render/pbr_shader.h` (`drawSkinned`, `SKINNED` variant),
`tests/test_skin.cpp`, `examples/skintest`, `tools/testdata/make_rigged.py`.

## 1. Asset side

glTF `skins[]` and the `JOINTS_0` / `WEIGHTS_0` attributes are parsed:

```cpp
struct VertexSkin { uint8_t joints[4]; float weights[4]; };   // parallel to Primitive::vertices
struct Skin { std::string name; int skeleton; std::vector<int> joints; std::vector<mat4> inverseBind; };
```

* `Primitive::skin` is empty for a static primitive, otherwise `vertices.size()` entries.
* `joints[c]` indexes `Skin::joints` (which holds node indices), **not** the node array directly.
* Weights are normalised to sum 1 at parse time; a vertex with no influence falls back to `w0 = 1`.
* `Node::skin` = the skin of a skinned mesh node (glTF `node.skin`).
* Joint indices above 255 are refused at load; the runtime limit is lower still (see §2).
* Animation clips were already parsed (`Model::animations`, `Animation`/`AnimSampler`/`AnimChannel`).
  CUBICSPLINE samplers arrive as LINEAR (tangents dropped) — good enough for character clips.

`.emod` is at **version 8**: primitives carry `u32 nSkinVerts + VertexSkin[]`, nodes carry `i32 skin`,
and a `u32 nSkins { name, i32 skeleton, joints[], inverseBind[] }` block follows the animations.
Versions 1–7 still load (the new fields stay empty), and `tools/convert` needs no new flag — converting
a rigged GLB keeps its skin and clips.

Test data: `assets/test/rigged.glb` (37 KB, committed), a two-bone cylinder with the clips `diam` and
`jalan`, rebuilt with

```
/Applications/Blender.app/Contents/MacOS/Blender --background --python tools/testdata/make_rigged.py
```

## 2. Vertex format and the 64-joint limit

Skinned primitives upload a 52-byte vertex (`eng::SkinnedVertex`, built at `GpuModel::upload`):

| offset | attribute | location | type |
|---|---|---|---|
| 0  | position | 0 | f32 x3 |
| 12 | normal   | 1 | f32 x3 |
| 24 | uv       | 2 | f32 x2 |
| 32 | joints   | `rhi::ATTR_JOINTS` = 8 | u8 x4, **unnormalized** (reaches the shader as 0..255 floats) |
| 36 | weights  | `rhi::ATTR_WEIGHTS` = 9 | f32 x4 |

Locations 3..6 are the instancing matrix and 7 is `ATTR_SHADE`, so the skinning pair sits above them and
a skinned mesh never collides with the instanced path.

The palette is a plain uniform array `uniform mat4 uJoints[64]` in the `SKINNED` variant of the PBR
vertex shader (`shaders::SKINNING_DEFINE` prepended to `PBR_VS`; `ModelRenderer` keeps both programs).
`eng::MAX_JOINTS = 64` → 64 × 64 B = **4 KB**, inside the guaranteed minimum for the vertex stage on
GL 4.1 and GLES 3.0 / WebGL2 (256 vec4). One avatar skeleton is 22 joints, so a whole outfit fits in one
palette — every clothing piece is skinned to the *same* skeleton and shares the palette of the frame.
Skins with more joints are drawn with the first 64 and log a warning.

The shader does linear blend skinning with all four influences and rotates the normal by the same
matrix (uniform joint scale assumed — rigs do not squash bones).

## 3. Animation API (`engine/render/animator.h`)

Header-only: the root `CMakeLists.txt` lists engine sources one by one and is off-limits to module
agents, so a new `.cpp` under `engine/render/` would never be compiled.

```cpp
constexpr int MAX_JOINTS = 64;

struct Pose { std::vector<vec3> t; std::vector<quat> r; std::vector<vec3> s; };   // one TRS per NODE

quat slerp(quat a, quat b, float f);
void restPose (const std::vector<Node>& nodes, Pose& out);
void samplePose(const std::vector<Node>& nodes, const Animation& clip, float t, bool loop, Pose& out);
void blendPose (const Pose& a, const Pose& b, float w, Pose& out);          // w=0 -> a, w=1 -> b; may alias
void layerPose (Pose& base, const Pose& layer, const std::vector<int>& mask, float w);
std::vector<int> jointMask(const std::vector<Node>& nodes, const std::vector<std::string>& names,
                           bool withDescendants = true);
void poseToLocal (const Pose& p, std::vector<mat4>& local);
void jointPalette(const Skin& skin, const std::vector<mat4>& world, std::vector<mat4>& palette);
```

`palette[j] = world[skin.joints[j]] * inverseBind[j]`, where `world` comes from
`computeWorld(nodes, roots, local, world)` (`engine/asset/model.h`). The skinned mesh node's own
transform is deliberately ignored, as the glTF spec requires — the instance transform passed to
`drawSkinned` is what places the character in the world.

`layerPose` is the gesture layer: only the joints in the mask move towards the gesture pose, at a
weight the caller fades. `jointMask(nodes, {"UpperArm.R", "Head"})` covers those joints *and their
sub-trees* (lower arm, hand, `Prop.R`), which is exactly the S40 / salute mask of the avatar spec.

### AnimationPlayer

```cpp
struct LocoClip { int clip; float param; float clipSpeed; };   // clipSpeed = m/s the clip was authored for

class AnimationPlayer {
  void  setSource(const std::vector<Node>* nodes, const std::vector<Animation>* clips);
  int   clipIndex(std::string_view name) const;               // -1 when absent
  void  setLocomotion(std::vector<LocoClip> set);             // sorted by param internally
  void  setParam(float v);  float param() const;              // e.g. speed in m/s
  void  play(int clip, bool loop, float fade = 0.2f);         // base clip over locomotion (jump, sit)
  void  stopBase();  bool baseActive() const;
  void  gesture(int clip, std::vector<int> mask, float fade = 0.25f);
  void  stopGesture(); bool gestureActive() const; float gestureWeight() const;
  void  update(float dt);
  const Pose& pose() const;
  void  localMatrices(std::vector<mat4>& local) const;
  void  palette(const std::vector<Node>& nodes, const std::vector<int>& roots, const Skin& skin,
                std::vector<mat4>& out);                      // pose -> local -> world -> palette
  float phase() const;                                        // 0..1 inside the locomotion loop
};
```

* **Locomotion blend**: the two clips bracketing `param` are sampled at a *shared normalised phase* and
  blended, so feet stay in sync across the blend; the loop period is the blend of the two durations,
  each time-scaled by `clipSpeed / param` when `clipSpeed > 0` (a 1.4 m/s walk clip played at 2 m/s runs
  proportionally faster instead of sliding). The PPKA avatar passes
  `{{diam, 0, 0}, {jalan, 1.4f, 1.4f}, {lari, 4.5f, 4.5f}}` — the numbers live in the caller.
* **Base clip** (`play`): replaces locomotion while it runs, fading in and back out; a non-looping clip
  releases itself `fade` seconds before its end. `playHold` instead freezes on the last frame until
  `stopBase()` — the sit-down transition ends *in* the pose, and staying there is the caller's decision.
* **Gesture** (`gesture`): plays once on the masked joints, fading in over `fade` and out again at the
  end of the clip — the 0.25 s of the spec is the default.
* `update(dt)` does locomotion → base → gesture in that order into a single `Pose`.

## 4. Drawing

```cpp
void ModelRenderer::drawSkinned(const GpuModel& model, const mat4& transform,
                                const std::vector<mat4>& palette, const Frustum* frustum = nullptr,
                                float boundsPad = 1.0f);
```

Skinned primitives go through the `SKINNED` program with `palette` in `uJoints[]`; non-skinned
primitives of the same model are drawn normally (a rig may carry static props). Culling uses the
rest-pose bounds grown by `boundsPad` metres, because a posed skeleton leaves them.

`GpuModel` now also keeps `skins` (and already kept `nodes`, `roots`, `animations`), so the CPU `Model`
can be dropped after upload — the player and the palette only need the GPU model.

## 5. How the avatar module uses it

```cpp
// once, per outfit piece (all pieces share one skeleton = the same joint names)
GpuModel body; body.upload(bodyModel);              // .emod / .glb with a skin
AnimationPlayer player; player.setSource(&rig.nodes, &rig.animations);   // rig = cc0-avatar-rangka
player.setLocomotion({{player.clipIndex("diam"), 0, 0},
                      {player.clipIndex("jalan"), 1.4f, 1.4f},
                      {player.clipIndex("lari"),  4.5f, 4.5f}});
std::vector<int> armMask = jointMask(rig.nodes, {"UpperArm.R", "Head"});

// per frame
player.setParam(speedMetresPerSecond);
if (startedS40) player.gesture(player.clipIndex("s40"), armMask);
player.update(dt);
player.palette(rig.nodes, rig.roots, rig.skins[0], palette);
renderer.drawSkinned(body,    avatarTransform, palette, &frustum);
renderer.drawSkinned(shirt,   avatarTransform, palette, &frustum);
renderer.drawSkinned(trousers,avatarTransform, palette, &frustum);
```

The rig's node array is the one the palette is built from; each clothing piece is a model whose skin
lists *the same joint names*, so as long as the pieces were exported against the standard skeleton the
palette computed once per avatar drives all of them. If a piece's own skin has a different joint order,
build its palette from its own `Skin` against the same posed `world` array (`jointPalette` takes the
skin and the world matrices separately for exactly that reason).

Per-slot tint (`warnaBaju`, skin colour table) is a material concern, not a skinning one: draw the piece
with its own `Material::baseColor`.

## 6. Tests

* `tests/test_skin` (ctest, no GPU): palette maths (a 90° joint rotation moves the weight-1 vertex to
  the expected place while the weight-0 vertex stays put, 0.5/0.5 lands in the middle), `blendPose` at
  0 / 0.5 / 1, `jointMask` + `layerPose`, EMOD v8 round trip (skins, inverse bind matrices, per-vertex
  influences, and identical palettes after a reload), the real Blender GLB (normalised weights, the
  `jalan` clip bending the top of the cylinder while the rooted bottom stays put), and the player
  (locomotion actually moves joints, the gesture fades in to 1 and ends by itself).
* `examples/skintest` — three copies side by side: rest pose, clip, clip + gesture layer.
  `ENG_CAPTURE=/tmp/x.ppm ./build/mac-debug/skintest` captures frame 30 with a fixed 1/60 s step.

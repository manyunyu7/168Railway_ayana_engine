// GPU skinning maths on the CPU: joint palettes, pose sampling, blending, gesture layers, the EMOD
// round trip of a skin, and the real Blender GLB in assets/test/rigged.glb (tools/testdata/make_rigged.py).
#include "engine/asset/emod.h"
#include "engine/asset/gltf.h"
#include "engine/render/animator.h"
#include "tests/check.h"
#include <filesystem>
#include <vector>

using namespace eng;

namespace {

// What the skinned vertex shader does, in C++: sum(w_i * palette[j_i]) applied to the rest position.
vec3 skinPoint(const std::vector<mat4>& palette, const VertexSkin& s, vec3 p) {
  vec3 out{};
  for (int c = 0; c < 4; ++c) {
    if (s.weights[c] <= 0) continue;
    const mat4& m = palette[s.joints[c]];
    out += m.transformPoint(p) * s.weights[c];
  }
  return out;
}

// Two joints in a chain: root at the origin, "Upper" one metre above it. Two vertices, each bound to
// one joint with weight 1 — the classic "does the skin follow the bone" rig.
Model twoBoneModel() {
  Model m;
  m.nodes.resize(3);
  m.nodes[0].name = "Lower"; m.nodes[0].translation = {0, 0, 0}; m.nodes[0].local = trs(m.nodes[0].translation, {}, {1, 1, 1});
  m.nodes[0].children = {1};
  m.nodes[1].name = "Upper"; m.nodes[1].parent = 0; m.nodes[1].translation = {0, 1, 0}; m.nodes[1].local = trs(m.nodes[1].translation, {}, {1, 1, 1});
  m.nodes[2].name = "Body"; m.nodes[2].mesh = 0; m.nodes[2].skin = 0;
  m.roots = {0, 2};

  Skin sk; sk.name = "Rangka"; sk.skeleton = 0; sk.joints = {0, 1};
  sk.inverseBind = {mat4::identity(), mat4::translation({0, -1, 0})};
  m.skins.push_back(sk);

  Primitive p;
  p.vertices = {{{0, 0, 0}, {0, 0, 1}, {0, 0}}, {{0, 2, 0}, {0, 0, 1}, {0, 0}}};
  p.skin.resize(2);
  p.skin[0].joints[0] = 0; p.skin[0].weights[0] = 1;
  p.skin[1].joints[0] = 1; p.skin[1].weights[0] = 1;
  p.indices = {0, 1, 0};
  p.boundsMin = {0, 0, 0}; p.boundsMax = {0, 2, 0};
  Mesh me; me.name = "Body"; me.primitives.push_back(std::move(p));
  m.meshes.push_back(std::move(me));
  m.computeBounds();
  return m;
}

void paletteFor(const Model& m, const Pose& pose, std::vector<mat4>& palette) {
  std::vector<mat4> local, world;
  poseToLocal(pose, local);
  computeWorld(m.nodes, m.roots, local, world);
  jointPalette(m.skins[0], world, palette);
}

void testPalette() {
  Model m = twoBoneModel();
  const Primitive& p = m.meshes[0].primitives[0];
  Pose pose; restPose(m.nodes, pose);
  std::vector<mat4> palette;

  // rest pose: both palette entries identity, nothing moves
  paletteFor(m, pose, palette);
  CHECK_EQ(palette.size(), 2u);
  vec3 a = skinPoint(palette, p.skin[0], p.vertices[0].pos), b = skinPoint(palette, p.skin[1], p.vertices[1].pos);
  CHECK_NEAR(length(a - p.vertices[0].pos), 0, 1e-5);
  CHECK_NEAR(length(b - p.vertices[1].pos), 0, 1e-5);

  // rotate "Upper" 90 degrees about X: its own vertex swings from (0,2,0) to (0,1,1), the root vertex stays put
  pose.r[1] = quat::axisAngle({1, 0, 0}, 3.14159265f / 2);
  paletteFor(m, pose, palette);
  a = skinPoint(palette, p.skin[0], p.vertices[0].pos);
  b = skinPoint(palette, p.skin[1], p.vertices[1].pos);
  CHECK_NEAR(a.x, 0, 1e-5); CHECK_NEAR(a.y, 0, 1e-5); CHECK_NEAR(a.z, 0, 1e-5);
  CHECK_NEAR(b.x, 0, 1e-5); CHECK_NEAR(b.y, 1, 1e-5); CHECK_NEAR(b.z, 1, 1e-5);

  // half and half: a vertex weighted 0.5/0.5 lands between the two results (linear blend skinning)
  VertexSkin half; half.joints[0] = 0; half.joints[1] = 1; half.weights[0] = 0.5f; half.weights[1] = 0.5f;
  vec3 mid = skinPoint(palette, half, {0, 2, 0});
  CHECK_NEAR(mid.y, (2 + 1) * 0.5f, 1e-5);
  CHECK_NEAR(mid.z, (0 + 1) * 0.5f, 1e-5);
}

void testBlendAndLayer() {
  Model m = twoBoneModel();
  Pose rest, bent, mixed;
  restPose(m.nodes, rest);
  bent = rest;
  bent.r[1] = quat::axisAngle({1, 0, 0}, 3.14159265f / 2);
  bent.t[1] = {0, 3, 0};

  // blend 0.5 = halfway: 45 degrees and the midpoint translation
  blendPose(rest, bent, 0.5f, mixed);
  std::vector<mat4> palette;
  paletteFor(m, mixed, palette);
  CHECK_NEAR(mixed.t[1].y, 2, 1e-5);
  quat q45 = quat::axisAngle({1, 0, 0}, 3.14159265f / 4);
  CHECK_NEAR(mixed.r[1].x, q45.x, 1e-5);
  CHECK_NEAR(mixed.r[1].w, q45.w, 1e-5);
  // the ends of the blend are the inputs themselves
  blendPose(rest, bent, 0, mixed); CHECK_NEAR(mixed.t[1].y, 1, 1e-6);
  blendPose(rest, bent, 1, mixed); CHECK_NEAR(mixed.t[1].y, 3, 1e-6);

  // gesture layer: only the masked joints follow, at the layer weight
  std::vector<int> mask = jointMask(m.nodes, {"Upper"});
  CHECK_EQ(mask.size(), 1u);
  CHECK_EQ(mask[0], 1);
  Pose base = rest;
  layerPose(base, bent, mask, 0.5f);
  CHECK_NEAR(base.t[1].y, 2, 1e-5);       // masked joint moved halfway
  CHECK_NEAR(base.t[0].y, 0, 1e-6);       // unmasked joint untouched
  layerPose(base, bent, mask, 0);         // weight 0 changes nothing
  CHECK_NEAR(base.t[1].y, 2, 1e-5);
  // the whole sub-tree comes along when asked for
  CHECK_EQ(jointMask(m.nodes, {"Lower"}).size(), 2u);
  CHECK_EQ(jointMask(m.nodes, {"Lower"}, false).size(), 1u);
}

void testEmodRoundTrip() {
  Model m = twoBoneModel();
  std::filesystem::path out = std::filesystem::temp_directory_path() / "test_skin.emod";
  std::string err;
  CHECK_MSG(saveEmod(m, out.string(), err), err);
  Model back;
  CHECK_MSG(loadEmod(out.string(), back, err), err);
  CHECK_EQ(back.skins.size(), 1u);
  if (back.skins.size() == 1) {
    const Skin& a = m.skins[0]; const Skin& b = back.skins[0];
    CHECK(a.name == b.name);
    CHECK_EQ(a.skeleton, b.skeleton);
    CHECK(a.joints == b.joints);
    CHECK_EQ(b.inverseBind.size(), 2u);
    for (size_t j = 0; j < a.inverseBind.size(); ++j)
      for (int c = 0; c < 16; ++c) CHECK_NEAR(a.inverseBind[j].data()[c], b.inverseBind[j].data()[c], 1e-6);
  }
  CHECK_EQ(back.nodes[2].skin, 0);
  const Primitive& p0 = m.meshes[0].primitives[0];
  const Primitive& p1 = back.meshes[0].primitives[0];
  CHECK_EQ(p1.skin.size(), p0.skin.size());
  for (size_t i = 0; i < p0.skin.size() && i < p1.skin.size(); ++i)
    for (int c = 0; c < 4; ++c) {
      CHECK_EQ(p1.skin[i].joints[c], p0.skin[i].joints[c]);
      CHECK_NEAR(p1.skin[i].weights[c], p0.skin[i].weights[c], 1e-6);
    }
  // posing the reloaded model gives the same vertices as posing the original
  Pose pose; restPose(back.nodes, pose);
  pose.r[1] = quat::axisAngle({1, 0, 0}, 3.14159265f / 2);
  std::vector<mat4> pa, pb;
  paletteFor(m, pose, pa);
  { std::vector<mat4> local, world; poseToLocal(pose, local); computeWorld(back.nodes, back.roots, local, world); jointPalette(back.skins[0], world, pb); }
  for (size_t j = 0; j < pa.size(); ++j) for (int c = 0; c < 16; ++c) CHECK_NEAR(pa[j].data()[c], pb[j].data()[c], 1e-5);
  std::error_code ec; std::filesystem::remove(out, ec);
}

void testPlayer(const Model& m) {
  AnimationPlayer player;
  player.setSource(&m.nodes, &m.animations);
  int idle = player.clipIndex("diam"), walk = player.clipIndex("jalan");
  CHECK(idle >= 0); CHECK(walk >= 0);
  if (idle < 0 || walk < 0) return;
  player.setLocomotion({{idle, 0, 0}, {walk, 1.4f, 1.4f}});

  // standing still: the pose stays the idle clip's (the rig's rest, here)
  player.setParam(0);
  player.update(0.1f);
  Pose still = player.pose();
  // at full walking speed the rig bends somewhere during the cycle
  player.setParam(1.4f);
  float maxDelta = 0;
  for (int i = 0; i < 30; ++i) {
    player.update(1.f / 30);
    const Pose& p = player.pose();
    for (size_t n = 0; n < p.size(); ++n) maxDelta = std::fmax(maxDelta, std::fabs(p.r[n].x - still.r[n].x));
  }
  CHECK_MSG(maxDelta > 0.05f, "the walk clip never moves a joint");
  CHECK(player.phase() >= 0 && player.phase() < 1);

  // gesture layer fades in and back out on its own
  std::vector<int> mask = jointMask(m.nodes, {"Upper"});
  player.gesture(walk, mask, 0.25f);
  CHECK(player.gestureActive());
  player.update(0.25f);
  CHECK_NEAR(player.gestureWeight(), 1, 1e-3);
  for (int i = 0; i < 200 && player.gestureActive(); ++i) player.update(1.f / 30);
  CHECK_MSG(!player.gestureActive(), "the gesture never ended");
  CHECK_NEAR(player.gestureWeight(), 0, 1e-6);
}

void testRiggedGlb() {
  const char* path = "assets/test/rigged.glb";
  if (!std::filesystem::exists(path)) { std::fprintf(stderr, "%s missing (tools/testdata/make_rigged.py)\n", path); ++test::failures; return; }
  Model m; std::string err;
  CHECK_MSG(loadGlbFile(path, m, err), err);
  CHECK_EQ(m.skins.size(), 1u);
  if (m.skins.empty()) return;
  const Skin& sk = m.skins[0];
  CHECK_EQ(sk.joints.size(), 2u);
  CHECK_EQ(sk.inverseBind.size(), 2u);
  CHECK_EQ(m.animations.size(), 2u);

  // the skinned mesh node points at the skin, and every vertex carries normalised influences
  int skinnedNode = -1;
  for (size_t i = 0; i < m.nodes.size(); ++i) if (m.nodes[i].skin >= 0) skinnedNode = (int)i;
  CHECK(skinnedNode >= 0);
  const Primitive& p = m.meshes[0].primitives[0];
  CHECK_EQ(p.skin.size(), p.vertices.size());
  CHECK(!p.skin.empty());
  for (const VertexSkin& s : p.skin) {
    float sum = s.weights[0] + s.weights[1] + s.weights[2] + s.weights[3];
    CHECK_NEAR(sum, 1, 1e-4);
    for (int c = 0; c < 4; ++c) CHECK(s.joints[c] < sk.joints.size());
  }

  // the "jalan" clip bends the cylinder: the top vertices move, the bottom ones do not
  int walk = -1;
  for (size_t i = 0; i < m.animations.size(); ++i) if (m.animations[i].name == "jalan") walk = (int)i;
  CHECK(walk >= 0);
  if (walk < 0) return;
  Pose pose;
  samplePose(m.nodes, m.animations[(size_t)walk], m.animations[(size_t)walk].duration * 0.5f, true, pose);
  std::vector<mat4> local, world, palette;
  poseToLocal(pose, local);
  computeWorld(m.nodes, m.roots, local, world);
  jointPalette(sk, world, palette);
  CHECK(palette.size() <= (size_t)MAX_JOINTS);
  float topMove = 0, bottomMove = 0;
  for (size_t i = 0; i < p.vertices.size(); ++i) {
    vec3 rest = p.vertices[i].pos;
    float d = length(skinPoint(palette, p.skin[i], rest) - rest);
    // the exporter writes Y up: the cylinder stands from y = 0 to y = 2
    if (rest.y > 1.8f) topMove = std::fmax(topMove, d);
    if (rest.y < 0.2f) bottomMove = std::fmax(bottomMove, d);
  }
  CHECK_MSG(topMove > 0.3f, "the bent top of the cylinder barely moved");
  CHECK_MSG(bottomMove < 0.02f, "the rooted bottom of the cylinder moved");

  // the GLB survives a trip through EMOD unchanged
  std::filesystem::path out = std::filesystem::temp_directory_path() / "test_skin_glb.emod";
  CHECK_MSG(saveEmod(m, out.string(), err), err);
  Model back;
  CHECK_MSG(loadEmod(out.string(), back, err), err);
  CHECK_EQ(back.skins.size(), 1u);
  CHECK_EQ(back.animations.size(), m.animations.size());
  if (!back.skins.empty()) CHECK(back.skins[0].joints == sk.joints);
  if (!back.meshes.empty() && !back.meshes[0].primitives.empty())
    CHECK_EQ(back.meshes[0].primitives[0].skin.size(), p.skin.size());
  std::error_code ec; std::filesystem::remove(out, ec);

  testPlayer(m);
}

} // namespace

TEST_MAIN(testPalette(); testBlendAndLayer(); testEmodRoundTrip(); testRiggedGlb())

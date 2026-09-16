// Avatars (docs/AVATAR.md, ppka-wannabe-2/docs/multiplayer.md §4/§5): AvatarState JSON round-trip, the world <->
// scene conversion, the outfit -> pieces resolution and the gesture joint mask, the remote smoothing buffer (100 ms behind, extrapolation capped), the gesture clock, and the
// third-person camera following the local avatar.
#include "engine/app/camera_rig.h"
#include "engine/asset/gltf.h"
#include "engine/render/animator.h"
#include "engine/world/avatar.h"
#include "engine/world/avatar_visual.h"
#include "tests/check.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

#ifndef ENG_SOURCE_DIR
#define ENG_SOURCE_DIR "."
#endif

using namespace eng;

TEST_MAIN({
  // ---- JSON round-trip ----
  const char* src = "{\"x\":12345.5,\"y\":-987.25,\"z\":3.5,\"yaw\":1.25,\"anim\":\"lari\",\"gesture\":\"s40\",\"gestureT\":0.5,"
                    "\"pakaian\":{\"tubuh\":\"tubuh-baku\",\"baju\":\"baju-kaos\",\"celana\":\"celana-jeans\",\"sepatu\":\"sepatu-hitam\","
                    "\"topi\":\"topi-kaos\",\"atribut\":[\"atribut-ht\"],\"kulit\":4,\"warnaBaju\":16711680}}";
  bool ok = false;
  AvatarState s = AvatarState::parse(src, &ok);
  CHECK(ok);
  CHECK_NEAR(s.x, 12345.5, 1e-6); CHECK_NEAR(s.y, -987.25, 1e-6); CHECK_NEAR(s.z, 3.5, 1e-6);
  CHECK_NEAR(s.yaw, 1.25, 1e-4);
  CHECK(s.anim == AvatarAnim::Lari); CHECK(s.gesture == AvatarGesture::S40); CHECK_NEAR(s.gestureT, 0.5, 1e-4);
  CHECK(s.pakaian.baju == "baju-kaos" && s.pakaian.topi == "topi-kaos" && s.pakaian.kulit == 4);
  CHECK(s.pakaian.atribut.size() == 1 && s.pakaian.atribut[0] == "atribut-ht");
  CHECK_EQ(s.pakaian.warnaBaju, 16711680);
  AvatarState s2 = AvatarState::parse(s.toJson(), &ok);   // serialize -> parse is a fixed point
  CHECK(ok);
  CHECK_NEAR(s2.x, s.x, 1e-3); CHECK_NEAR(s2.y, s.y, 1e-3); CHECK_NEAR(s2.z, s.z, 1e-3); CHECK_NEAR(s2.yaw, s.yaw, 1e-3);
  CHECK(s2.anim == s.anim && s2.gesture == s.gesture);
  CHECK(s2.pakaian.toJson() == s.pakaian.toJson());
  CHECK(AvatarState::parse("{}", &ok).pakaian.baju == "baju-ppka-putih");   // PPKA defaults
  AvatarState bad = AvatarState::parse("not json", &ok); CHECK(!ok); (void)bad;
  // a gesture that finished is not serialised
  AvatarState plain; CHECK(plain.toJson().find("gesture") == std::string::npos);

  // ---- world <-> scene ----
  WorldOrigin origin; origin.ox = 10000; origin.oz = -5000;
  Avatar a; a.state = s; a.place(origin);
  CHECK_NEAR(a.pos.x, 2345.5, 1e-3); CHECK_NEAR(a.pos.z, 4012.75, 1e-3); CHECK_NEAR(a.pos.y, 3.5, 1e-3);
  CHECK_NEAR(a.bodyYaw, -1.25, 1e-4);   // scene yaw = -world yaw
  a.store(origin);
  CHECK_NEAR(a.state.x, s.x, 1e-6); CHECK_NEAR(a.state.y, s.y, 1e-6); CHECK_NEAR(a.state.yaw, s.yaw, 1e-5);

  // ---- remote smoothing: two samples 1 s apart, the midpoint is drawn halfway ----
  AvatarSet set;
  AvatarState r; r.x = origin.ox; r.y = origin.oz; r.z = 0; r.yaw = 0; r.anim = AvatarAnim::Jalan;
  set.upsert(7, r, origin, 0.0);
  r.x = origin.ox + 10; r.yaw = 1.0f;
  set.upsert(7, r, origin, 1.0);
  Avatar& rem = set.remote[7];
  rem.sample(1.0 + Avatar::LAG);           // drawn time 1.0 = the newest sample
  CHECK_NEAR(rem.pos.x, 10, 1e-3);
  rem.sample(0.5 + Avatar::LAG);           // drawn time 0.5 = halfway between the two
  CHECK_NEAR(rem.pos.x, 5, 1e-3);
  CHECK_NEAR(rem.bodyYaw, -0.5, 1e-3);
  rem.sample(0.0 + Avatar::LAG);
  CHECK_NEAR(rem.pos.x, 0, 1e-3);
  rem.sample(1.1 + Avatar::LAG);           // 100 ms past the newest sample: extrapolated
  CHECK_NEAR(rem.pos.x, 11, 1e-3);
  rem.sample(5.0 + Avatar::LAG);           // far past it: frozen at the EKSTRA cap (200 ms)
  CHECK_NEAR(rem.pos.x, 12, 1e-3);
  set.remove(7); CHECK(set.remote.empty());

  // ---- gesture clock: runs for the clip length, then it is gone ----
  set.startGesture(AvatarGesture::Lambai);
  CHECK_NEAR(avatarGestureDuration(AvatarGesture::Lambai), 3.2, 1e-5);
  CHECK_NEAR(avatarGestureDuration(AvatarGesture::S40), 2.0, 1e-5);
  double t = 0;
  for (int i = 0; i < 30; ++i) { t += 0.1; set.tick(0.1f, t); }    // 3.0 s
  CHECK(set.local.state.gesture == AvatarGesture::Lambai);
  CHECK_NEAR(set.local.state.gestureT, 3.0, 1e-3);
  for (int i = 0; i < 4; ++i) { t += 0.1; set.tick(0.1f, t); }     // past 3.2 s
  CHECK(set.local.state.gesture == AvatarGesture::None);
  CHECK_NEAR(set.local.state.gestureT, 0, 1e-6);

  // ---- third-person camera follows the local avatar ----
  auto flat = [](float, float) { return 0.f; };
  CameraRig rig;
  rig.setMode(CamMode::Orang, {0, 12, 20}, {0, 0, 0});
  rig.enterOrang({0, 12, 20}, {0, 0, 0}, flat);
  CHECK(std::string(camModeName(CamMode::Orang)) == "orang");
  CamMode parsed; CHECK(parseCamMode("orang", parsed) && parsed == CamMode::Orang);
  WalkInput in;
  for (int i = 0; i < 240; ++i) rig.step(1.f / 60, nullptr, flat, in);   // standing still: the boom settles
  vec3 target = rig.walkerPos() + vec3{0, 1.8f, 0};
  CHECK_NEAR(rig.look().x, target.x, 0.02f); CHECK_NEAR(rig.look().y, target.y, 0.02f); CHECK_NEAR(rig.look().z, target.z, 0.02f);
  CHECK_NEAR(length(rig.eye() - target), rig.jarakOrang, 0.05f);        // eye = target - dir * distance
  CHECK_NEAR(rig.jarakOrang, 4.5f, 1e-4);
  CHECK(rig.eye().y > target.y);                                        // pitched down 12 deg: the camera is higher
  // walking: 1.4 m/s over the ground, the camera keeps its distance, the body faces the way it travels
  vec3 before = rig.walkerPos();
  in.forward = 1;
  for (int i = 0; i < 120; ++i) rig.step(1.f / 60, nullptr, flat, in);  // 2 s
  float jauh = length(rig.walkerPos() - before);
  CHECK_NEAR(jauh, 2.8f, 0.15f);
  CHECK_NEAR(rig.walkerSpeed(), 1.4f, 0.05f);
  in.run = true;
  for (int i = 0; i < 60; ++i) rig.step(1.f / 60, nullptr, flat, in);
  CHECK_NEAR(rig.walkerSpeed(), 4.5f, 0.05f);
  target = rig.walkerPos() + vec3{0, 1.8f, 0};
  CHECK_NEAR(length(rig.eye() - target), rig.jarakOrang, 0.35f);        // the boom trails a little while running
  {   // the avatar's body yaw points where it moved (scene: rotationY(bodyYaw) maps +X onto the heading)
    vec3 d = rig.walkerPos() - before; float mau = std::atan2(-d.z, d.x);
    float diff = std::fmod(rig.walkerBodyYaw() - mau + 3 * (float)PI, 2 * (float)PI) - (float)PI;
    CHECK_NEAR(diff, 0, 0.05f);
  }
  // the local avatar takes that pose straight from the rig
  set.driveLocal(rig.walkerPos(), rig.walkerBodyYaw(), rig.walkerSpeed(), rig.walkerOnGround(), origin);
  CHECK(set.local.anim == AvatarAnim::Lari);
  CHECK_NEAR(set.local.state.x, origin.ox + rig.walkerPos().x, 1e-3);
  CHECK_NEAR(set.local.state.y, origin.oz + rig.walkerPos().z, 1e-3);
  set.driveLocal(rig.walkerPos(), rig.walkerBodyYaw(), 0.f, true, origin);
  CHECK(set.local.anim == AvatarAnim::Diam);
  set.driveLocal(rig.walkerPos(), rig.walkerBodyYaw(), 1.4f, false, origin);
  CHECK(set.local.anim == AvatarAnim::Lompat);
  // the boom is pulled in by a wall behind the avatar
  std::vector<WalkBox> boxes;
  vec3 mata = rig.eye();
  boxes.push_back({AABB{{mata.x - 6, 0, mata.z - 1}, {mata.x + 6, 6, mata.z + 1}}, true});
  in.forward = 0; in.run = false; in.boxes = &boxes;
  for (int i = 0; i < 120; ++i) rig.step(1.f / 60, nullptr, flat, in);
  target = rig.walkerPos() + vec3{0, 1.8f, 0};
  CHECK(length(rig.eye() - target) < rig.jarakOrang - 0.2f);

  // ---- outfit resolution: pakaian -> the pieces that get drawn ----
  {
    using AV = AvatarVisuals;
    Pakaian p;                                     // PPKA defaults
    std::vector<AV::Piece> ps = AV::pieces(p);
    CHECK_EQ((int)ps.size(), 6);                   // tubuh, baju, celana, sepatu, topi, tongkat
    const char* order[] = {"tubuh", "baju", "celana", "sepatu", "topi", "atribut"};
    for (size_t i = 0; i < ps.size(); ++i) CHECK(ps[i].slot == order[i]);
    CHECK(ps[0].id == "tubuh-baku" && ps[4].id == "topi-ppka-merah" && ps[5].id == "atribut-tongkat-s40");
    CHECK(AV::slotId(ps[0].id) == "avatar:tubuh-baku");
    vec3 kulit = AV::skinColor(p.kulit);           // the body piece is painted neutral: it takes the skin tone
    CHECK_NEAR(ps[0].tint.x, kulit.x, 1e-5); CHECK_NEAR(ps[0].tint.z, kulit.z, 1e-5);
    CHECK(ps.size() <= 8);                         // <= 8 draw calls per avatar (one primitive per piece)

    p.topi = "";                                   // bare-headed: the slot disappears, nothing else moves
    ps = AV::pieces(p);
    CHECK_EQ((int)ps.size(), 5);
    for (const AV::Piece& q : ps) CHECK(q.slot != "topi");

    p.topi = "topi-kaos"; p.baju = "baju-kaos"; p.warnaBaju = 0xff0000; p.atribut = {"atribut-ht", "atribut-tongkat-s40"};
    ps = AV::pieces(p);
    CHECK_EQ((int)ps.size(), 7);
    CHECK(ps[5].id == "atribut-ht" && ps[6].id == "atribut-tongkat-s40");
    CHECK_NEAR(ps[1].tint.x, 1.0, 1e-5); CHECK_NEAR(ps[1].tint.y, 0.0, 1e-5);   // tinted shirt takes warnaBaju
    CHECK_NEAR(ps[4].tint.x, 1.0, 1e-5);                                        // ... and so does the tinted cap
    // gestures map onto the clip names of the rig
    CHECK(std::string(AV::gestureClip(AvatarGesture::S40)) == "s40");
    CHECK(std::string(AV::gestureClip(AvatarGesture::None)).empty());
    CHECK_NEAR(AV::animSpeed(AvatarAnim::Jalan), 1.4, 1e-5);
    CHECK_NEAR(AV::animSpeed(AvatarAnim::Lari), 4.5, 1e-5);
    CHECK_NEAR(AV::animSpeed(AvatarAnim::Diam), 0, 1e-5);
  }

  // ---- the real rig: clips, gesture mask, and every piece sharing the skeleton ----
  {
    const char* env = std::getenv("PPKA_ROOT");
    std::string dir = (env && *env ? std::string(env) : std::string(ENG_SOURCE_DIR) + "/../ppka-wannabe-2") + "/public/model3d/";
    Model rig; std::string err;
    if (loadGlbFile(dir + "nry-avatar-rangka.glb", rig, err)) {
      CHECK(!rig.skins.empty() && rig.skins[0].joints.size() == 22);
      for (const char* c : {"diam", "jalan", "lari", "lompat", "duduk", "s40", "s3", "s1", "hormat", "lambai", "tunjuk"}) {
        auto it = std::find_if(rig.animations.begin(), rig.animations.end(), [&](const Animation& a) { return a.name == c; });
        CHECK(it != rig.animations.end());
      }
      // every gesture's clip is as long as the ABI promises (docs/multiplayer.md §6)
      for (int g = 1; g <= 6; ++g) {
        AvatarGesture ge = (AvatarGesture)g;
        auto it = std::find_if(rig.animations.begin(), rig.animations.end(),
                               [&](const Animation& a) { return a.name == AvatarVisuals::gestureClip(ge); });
        CHECK(it != rig.animations.end());
        CHECK_NEAR(it->duration, avatarGestureDuration(ge), 0.02);
      }
      // the gesture layer claims the right arm (with its sub-tree, i.e. the baton anchor) and the head — nothing else
      std::vector<int> mask = jointMask(rig.nodes, AvatarVisuals::gestureJoints());
      auto masked = [&](const char* n) {
        for (int i : mask) if (rig.nodes[(size_t)i].name == n) return true;
        return false;
      };
      for (const char* n : {"Shoulder.R", "UpperArm.R", "LowerArm.R", "Hand.R", "Prop.R", "Head"}) CHECK(masked(n));
      for (const char* n : {"Shoulder.L", "UpperArm.L", "Hand.L", "Hips", "Spine", "Chest", "UpperLeg.R", "Foot.R"}) CHECK(!masked(n));

      // every outfit piece is rigged to the SAME skeleton: same joint names in the same order, same bind pose.
      // That is what lets one palette per frame drive all of them (docs/SKINNING.md §5).
      auto names = [](const Model& m) {
        std::vector<std::string> out;
        for (int j : m.skins[0].joints) out.push_back(m.nodes[(size_t)j].name);
        return out;
      };
      std::vector<std::string> rigNames = names(rig);
      for (const char* berkas : {"nry-avatar-tubuh-baku.glb", "nry-avatar-baju-ppka-putih.glb", "nry-avatar-celana-hitam.glb",
                                 "nry-avatar-sepatu-hitam.glb", "nry-avatar-topi-ppka-merah.glb", "nry-avatar-atribut-tongkat-s40.glb"}) {
        Model m;
        if (!loadGlbFile(dir + berkas, m, err)) { CHECK(false); continue; }
        CHECK(!m.skins.empty());
        CHECK(names(m) == rigNames);
        CHECK_EQ((int)m.skins[0].inverseBind.size(), (int)rig.skins[0].inverseBind.size());
        for (size_t j = 0; j < m.skins[0].inverseBind.size(); ++j)
          for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r) CHECK_NEAR(m.skins[0].inverseBind[j].m[c][r], rig.skins[0].inverseBind[j].m[c][r], 1e-4);
      }
    } else {
      std::printf("[test_avatar] skipping the asset checks: %s\n", err.c_str());
    }
  }
})
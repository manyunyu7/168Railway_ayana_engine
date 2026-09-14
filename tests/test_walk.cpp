// Walk mode (uji3dJalanKaki.ts with AABBs): wall stop + slide, jump height, landing on a platform box, step-up.
#include "engine/app/camera_rig.h"
#include "tests/check.h"
#include <cmath>

using namespace eng;

TEST_MAIN({
  auto flat = [](float, float) { return 0.f; };
  CameraRig rig; rig.mode = CamMode::Jalan;
  rig.enterWalk({0, 1.62f, 5}, {0, 1.62f, 0}, flat);   // standing at the origin looking toward -Z
  std::vector<WalkBox> boxes;
  boxes.push_back({AABB{{-5, 0, -12}, {5, 3, -10}}, true});      // wall 10 m ahead (x -5..5, z -12..-10)
  WalkInput in; in.forward = 1; in.boxes = &boxes;
  for (int i = 0; i < 200; ++i) rig.step(1.f / 60, nullptr, flat, in);   // 3.3 s at 4.5 m/s = 15 m: blocked
  vec3 e = rig.eye();
  CHECK_NEAR(e.z, -10 + 0.38f, 0.02f); CHECK_NEAR(e.y, 1.62f, 0.04f);
  // sliding: walking diagonally into the wall keeps the sideways component
  in.side = 1; for (int i = 0; i < 60; ++i) rig.step(1.f / 60, nullptr, flat, in);
  CHECK(rig.eye().x > 1); CHECK_NEAR(rig.eye().z, -10 + 0.38f, 0.02f);
  // a canopy above the head and a kerb below the knee are not walls
  boxes.clear(); boxes.push_back({AABB{{-5, 2.5f, -30}, {5, 3, -20}}, true}); boxes.push_back({AABB{{-5, 0, -30}, {5, 0.2f, -20}}, true});
  in.side = 0; rig.enterWalk({0, 1.62f, 5}, {0, 1.62f, 0}, flat);
  for (int i = 0; i < 340; ++i) rig.step(1.f / 60, nullptr, flat, in);   // 25.5 m: on the kerb (z -20..-30)
  CHECK(rig.eye().z < -24); CHECK_NEAR(rig.eye().y, 0.2f + 1.62f, 0.04f);   // walked on, stepped up the kerb
  // jump: peak v²/2g ≈ 1.08 m, back on the ground after ~0.94 s; holding the key does not re-jump
  boxes.clear(); rig.enterWalk({0, 1.62f, 5}, {0, 1.62f, 0}, flat);
  in.forward = 0; in.jump = true; float peak = 0; bool airborne = false;
  for (int i = 0; i < 120; ++i) { rig.step(1.f / 60, nullptr, flat, in); peak = std::fmax(peak, rig.eye().y - 1.62f); if (i == 30) airborne = !rig.walkerOnGround(); }
  CHECK(airborne); CHECK_NEAR(peak, 1.08f, 0.06f); CHECK(rig.walkerOnGround());
  // double jump: a second rising edge in the air reaches ~2.1 m
  in.jump = false; rig.step(1.f / 60, nullptr, flat, in);
  peak = 0; in.jump = true;
  for (int i = 0; i < 150; ++i) { if (i == 10) in.jump = false; if (i == 20) in.jump = true; rig.step(1.f / 60, nullptr, flat, in); peak = std::fmax(peak, rig.eye().y - 1.62f); }
  CHECK(peak > 1.6f);
  // platform 1 m high right ahead: walking bumps into it, jumping while walking lands on top
  boxes.push_back({AABB{{-5, 0, -30}, {5, 1.0f, -1}}, false});   // floor-only box (a slab): still a step too high
  in.jump = false; rig.enterWalk({0, 1.62f, 5}, {0, 1.62f, 0}, flat); in.forward = 1;
  for (int i = 0; i < 90; ++i) { in.jump = i == 20; rig.step(1.f / 60, nullptr, flat, in); }
  CHECK_NEAR(rig.eye().y, 1.0f + 1.62f, 0.05f); CHECK(rig.walkerOnGround());
})

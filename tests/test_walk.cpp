// Walk mode (uji3dJalanKaki.ts): with boxes (vehicles) — wall stop + slide, jump height, landing on a platform
// box, step-up; with the triangle collider (scenery) — wall, ramp, stairs, platform kerb, box too high, ceiling.
#include "engine/app/camera_rig.h"
#include "engine/world/walk_collision.h"
#include "tests/check.h"
#include <chrono>
#include <cmath>
#include <cstdio>

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

  // ---- triangle collider ---------------------------------------------------------------------------------
  {
    WalkCollider mesh;
    mesh.addBox(AABB{{-5, 0, -12}, {5, 3, -10}});                // a wall 10 m ahead
    mesh.finish();
    CHECK_EQ(mesh.triangles(), (size_t)12);
    WalkCollider::WallHit wh;
    CHECK(mesh.wallRay({0, 1.05f, 0}, 0, -1, 20, wh)); CHECK_NEAR(wh.dist, 10, 1e-3); CHECK_NEAR(std::fabs(wh.nz), 1, 1e-3); CHECK_NEAR(wh.top, 3, 1e-3);
    CHECK(!mesh.wallRay({0, 1.05f, 0}, 0, 1, 20, wh));            // nothing behind
    WalkCollider::FloorHit fh;
    CHECK(mesh.floorBelow({0, 5, -11}, 10, fh)); CHECK_NEAR(fh.y, 3, 1e-3);   // the box top from above
    CHECK(!mesh.floorBelow({0, 5, 0}, 10, fh));
    float cy; CHECK(mesh.ceilingAbove({0, 1, -11}, 5, cy)); CHECK_NEAR(cy, 3, 1e-3);   // its underside from inside
    // the walker stops RADIUS short of the wall and slides along it
    CameraRig r; r.mode = CamMode::Jalan; r.enterWalk({0, 1.62f, 5}, {0, 1.62f, 0}, flat);
    WalkInput wi; wi.forward = 1; wi.mesh = &mesh;
    for (int i = 0; i < 200; ++i) r.step(1.f / 60, nullptr, flat, wi);
    CHECK_NEAR(r.eye().z, -10 + 0.38f, 0.02f); CHECK_NEAR(r.eye().y, 1.62f, 0.04f);
    wi.side = 1; for (int i = 0; i < 60; ++i) r.step(1.f / 60, nullptr, flat, wi);
    CHECK(r.eye().x > 1); CHECK_NEAR(r.eye().z, -10 + 0.38f, 0.02f);
    // running (12 m/s) does not tunnel through it either
    wi.side = 0; wi.run = true; r.enterWalk({0, 1.62f, 5}, {0, 1.62f, 0}, flat);
    for (int i = 0; i < 120; ++i) r.step(1.f / 30, nullptr, flat, wi);
    CHECK_NEAR(r.eye().z, -10 + 0.38f, 0.03f);
  }
  {   // a ramp (1:4, 8 m long, 2 m rise) is a floor: walked up; a stair (18 cm risers) too
    WalkCollider mesh;
    mesh.addQuad({-2, 0, -10}, {2, 0, -10}, {2, 2, -18}, {-2, 2, -18});   // ramp surface rising toward -Z
    mesh.addBox(AABB{{-2, 0, -30}, {2, 2, -18}});                          // the landing it leads to
    for (int i = 0; i < 6; ++i) mesh.addBox(AABB{{6, 0, -10 - 0.28f * (i + 1)}, {10, 0.18f * (i + 1), -10 - 0.28f * i}});   // stairs at x 6..10
    mesh.finish();
    CameraRig r; r.mode = CamMode::Jalan; r.enterWalk({0, 1.62f, 0}, {0, 1.62f, -5}, flat);
    WalkInput wi; wi.forward = 1; wi.mesh = &mesh;
    for (int i = 0; i < 240; ++i) r.step(1.f / 60, nullptr, flat, wi);   // 18 m: past the ramp, on the landing
    CHECK(r.eye().z < -18.5f); CHECK_NEAR(r.eye().y, 2 + 1.62f, 0.06f); CHECK(r.walkerOnGround());
    r.enterWalk({8, 1.62f, 0}, {8, 1.62f, -5}, flat);
    for (int i = 0; i < 90; ++i) r.step(1.f / 60, nullptr, flat, wi);    // stairs: 6 x 0.18 = 1.08 m up over 1.7 m
    CHECK(r.eye().z < -11.7f); CHECK_NEAR(r.eye().y, 1.08f + 1.62f, 0.06f);
  }
  {   // platform kerb: a 1.0 m `peron` floor is stepped onto at its edge; a 1.5 m box (or a 1.0 m non-platform one) is not
    WalkCollider mesh;
    mesh.addBox(AABB{{-5, -1.8f, -30}, {5, 1.0f, -10}}, true, true);   // platform body (floor only, flagged)
    mesh.addBox(AABB{{10, 0, -30}, {20, 1.5f, -10}});                    // a 1.5 m block
    mesh.addBox(AABB{{-20, 0, -30}, {-10, 1.0f, -10}});                  // a 1.0 m block that is not a platform
    mesh.finish();
    CameraRig r; r.mode = CamMode::Jalan; r.enterWalk({0, 1.62f, 0}, {0, 1.62f, -5}, flat);
    WalkInput wi; wi.forward = 1; wi.mesh = &mesh;
    for (int i = 0; i < 200; ++i) r.step(1.f / 60, nullptr, flat, wi);
    CHECK(r.eye().z < -12); CHECK_NEAR(r.eye().y, 1.0f + 1.62f, 0.05f); CHECK(r.walkerOnGround());
    for (int i = 0; i < 300; ++i) r.step(1.f / 60, nullptr, flat, wi);   // off the far end: back on the ground
    CHECK(r.eye().z < -31); CHECK_NEAR(r.eye().y, 1.62f, 0.05f);
    r.enterWalk({15, 1.62f, 0}, {15, 1.62f, -5}, flat);
    for (int i = 0; i < 200; ++i) r.step(1.f / 60, nullptr, flat, wi);
    CHECK_NEAR(r.eye().z, -10 + 0.38f, 0.02f); CHECK_NEAR(r.eye().y, 1.62f, 0.04f);   // 1.5 m: a wall
    r.enterWalk({-15, 1.62f, 0}, {-15, 1.62f, -5}, flat);
    for (int i = 0; i < 200; ++i) r.step(1.f / 60, nullptr, flat, wi);
    CHECK_NEAR(r.eye().z, -10 + 0.38f, 0.02f); CHECK_NEAR(r.eye().y, 1.62f, 0.04f);   // 1.0 m but not a platform: a wall
    // ... jumping onto it still works (double jump for the 1.5 m one)
    wi.forward = 1; r.enterWalk({15, 1.62f, 0}, {15, 1.62f, -5}, flat);
    for (int i = 0; i < 150; ++i) { wi.jump = i == 100 || i == 112; r.step(1.f / 60, nullptr, flat, wi); }
    CHECK_NEAR(r.eye().y, 1.5f + 1.62f, 0.05f); CHECK(r.walkerOnGround());
  }
  {   // a ceiling 2.2 m up stops a jump at head room (eye 1.62 + 0.12)
    WalkCollider mesh;
    mesh.addBox(AABB{{-5, 2.2f, -5}, {5, 2.5f, 5}});
    mesh.finish();
    CameraRig r; r.mode = CamMode::Jalan; r.enterWalk({0, 1.62f, 0}, {0, 1.62f, -5}, flat);
    WalkInput wi; wi.mesh = &mesh; wi.jump = true; float peak = 0;
    for (int i = 0; i < 120; ++i) { r.step(1.f / 60, nullptr, flat, wi); peak = std::fmax(peak, r.eye().y); }
    CHECK(peak <= 2.2f - 0.12f + 0.04f); CHECK(peak > 1.9f); CHECK(r.walkerOnGround());
  }
  {   // build cost: 100k triangles hashed well under 50 ms; queries stay local
    WalkCollider mesh;
    for (int i = 0; i < 100000 / 12; ++i) { float x = (float)(i % 100) * 3, z = (float)(i / 100) * 3; mesh.addBox(AABB{{x, 0, z}, {x + 2, 4, z + 2}}); }
    auto t0 = std::chrono::steady_clock::now();
    mesh.finish();
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("walk collider: %zu tris hashed in %.1f ms (%zu cells)\n", mesh.triangles(), ms, mesh.stats.cells);
    CHECK(ms < 50 * 8);   // ASan/UBSan debug build: generous; release is far under
    WalkCollider::WallHit wh; CHECK(mesh.wallRay({2.5f, 1, 1}, 1, 0, 10, wh)); CHECK_NEAR(wh.dist, 0.5f, 1e-3);
  }
})

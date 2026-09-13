// Port of ppka-wannabe-2/tests/kompas3d.ts against the pure functions in examples/ppka/compass.cpp.
#include "examples/ppka/compass.h"
#include "tests/check.h"
#include <cmath>

using namespace eng;

// putarY from kompas3d.ts: rotate a ground vector about +Y by theta (positive = turn left).
static vec2 putarY(vec2 v, float th) { float c = std::cos(th), s = std::sin(th); return {v.x * c + v.y * s, -v.x * s + v.y * c}; }

TEST_MAIN({
  CHECK_NEAR(COMPASS_DEAD_ZONE, 0.06, 1e-7); CHECK(COMPASS_JUMP_LIMIT == 4); CHECK_NEAR(COMPASS_JUMP_SECONDS, 0.55, 1e-7); CHECK_NEAR(COMPASS_FADE_SECONDS, 1.4, 1e-7);

  // ---- speed curve
  CHECK(glideSpeed(0, 200) == 0);                                   // screen centre = still
  CHECK(glideSpeed(COMPASS_DEAD_ZONE, 200) == 0);                   // exactly on the dead-zone edge still
  CHECK(glideSpeed(COMPASS_DEAD_ZONE + 0.01f, 200) > 0);            // just outside moves
  { float prev = -1; for (float r = 0; r <= 1.41f; r += 0.01f) { float v = glideSpeed(r, 200); CHECK_MSG(v >= prev, "monotonic at r=" + std::to_string(r)); prev = v; } }
  CHECK(glideSpeed(1, 400) > glideSpeed(1, 200) * 1.9f);             // scales with camera distance
  CHECK_NEAR(glideSpeed(1, 200, 2), glideSpeed(1, 200) * 2, 1e-4);   // Compass speed is a linear multiplier
  CHECK(glideSpeed(0.3f, 200) < glideSpeed(1, 200) * 0.3f);          // slower than linear near the centre (power > 1)
  CHECK_NEAR(glideSpeed(1, 200), 320, 1e-3);                         // u = 1 -> distance * 1.6
  CHECK(glideSpeed(1.41f, 200) == glideSpeed(1, 200));               // saturates at u = 1

  // ---- screen -> ground direction. Camera at (0,0,+100) looking at the origin: forward = -z.
  { vec2 a = glideDirection(0, -1, 0, 100); CHECK_NEAR(a.x, 0, 1e-9); CHECK(a.y < 0); }          // cursor up = away from the camera
  { vec2 b = glideDirection(1, 0, 0, 100); CHECK(b.x > 0); CHECK_NEAR(b.y, 0, 1e-9); }           // cursor right = +x
  { vec2 c = glideDirection(0, 0, 0, 100); CHECK_NEAR(c.x, 0, 1e-9); CHECK_NEAR(c.y, 0, 1e-9); }  // centre = zero
  { vec2 d = glideDirection(1, 0, 100, 0); CHECK_NEAR(d.x, 0, 1e-9); CHECK(d.y < 0); }           // camera moved to +x: right = (0,0,-1)
  { vec2 e = glideDirection(0, -1, 0, 0); CHECK(std::isfinite(e.x) && std::isfinite(e.y)); }     // degenerate offset is safe

  // ---- rotation
  { vec2 L{0, -1}; vec2 left = putarY(L, PI / 2); CHECK_NEAR(left.x, -1, 1e-6); CHECK_NEAR(left.y, 0, 1e-6); }   // theta > 0 = turn left (north -> west)
  float th = rotationRate(1, 1);
  CHECK(th < 0);                                                     // cursor right = look right (negative theta)
  { vec2 L2 = putarY({0, -1}, th); CHECK(L2.x > 0); }                // north view turns east
  CHECK(rotationRate(0, 1) == 0);                                    // cursor on the vertical axis: straight glide
  CHECK(rotationRate(1, COMPASS_DEAD_ZONE) == 0);                    // dead zone: no rotation
  CHECK(std::fabs(rotationRate(0.3f, 0.3f)) < std::fabs(rotationRate(1, 1)));
  CHECK_NEAR(rotationRate(1, 1), -1.1, 1e-6);                        // saturated
  CHECK(rotationRate(-1, 1) > 0);

  // ---- jump clamp
  { vec2 d = clampJump({3000, 4000}, 200); float l = std::hypot(d.x, d.y);
    CHECK_NEAR(l, COMPASS_JUMP_LIMIT * 200, 1e-3); CHECK_NEAR(d.x / d.y, 3.0 / 4, 1e-6); }
  { vec2 e = clampJump({30, 40}, 200); CHECK(e.x == 30 && e.y == 40); }
  { vec2 n = clampJump({0, 0}, 200); CHECK(n.x == 0 && n.y == 0); }
  { vec2 x = clampJump({800, 0}, 200); CHECK(x.x == 800 && x.y == 0); }   // exactly at the limit: unchanged

  // ---- azimuth: +x east, -z north
  CHECK(azimuthDegrees(0, -1) == 0); CHECK(azimuthDegrees(1, 0) == 90); CHECK(azimuthDegrees(0, 1) == 180); CHECK(azimuthDegrees(-1, 0) == 270);
  CHECK(azimuthDegrees(1, -1) == 45); CHECK(azimuthDegrees(-1, -1) == 315);
  CHECK(azimuthDegrees(-0.0001f, -1) == 0);   // wraps to [0, 360)
})

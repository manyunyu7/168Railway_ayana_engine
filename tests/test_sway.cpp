// Sway (§7.5, uji3dGoyang.ts): sign conventions and limits — the numbers tests/goyang3d.ts guards.
#include "engine/world/sway.h"
#include "tests/check.h"
#include <cmath>

using namespace eng::sway;

TEST_MAIN({
  // response: standing still and slow shunting barely move; full at 22 m/s
  CHECK(respons(0) == 0); CHECK(respons(1.4f) < 0.02f); CHECK_NEAR(respons(22), 1, 1e-6); CHECK_NEAR(respons(40), 1, 1e-6);
  // a standing train has no noise at all
  Goyang still = hitungGoyang({100, 200, 0, 0.01f, 0, 1});
  CHECK(still.roll == 0 && still.naik == 0 && still.geser == 0 && still.yaw == 0);
  CHECK(medan(3, 4, 0, 1).naik == 0); CHECK(medan(3, 4, 30, 0).geser == 0);
  // noise is bounded by its amplitude (weights sum to 1) and is a function of position only
  float maxRoll = 0, maxNaik = 0, maxGeser = 0;
  for (int i = 0; i < 2000; ++i) {
    float x = i * 1.7f, z = i * 0.31f;
    Goyang g = hitungGoyang({x, z, 30, 0, 0, 1});
    maxRoll = std::fmax(maxRoll, std::fabs(g.roll)); maxNaik = std::fmax(maxNaik, std::fabs(g.naik)); maxGeser = std::fmax(maxGeser, std::fabs(g.geser));
    Goyang g2 = hitungGoyang({x, z, 30, 0, 0, 1});
    CHECK(g.roll == g2.roll && g.naik == g2.naik);
  }
  CHECK(maxRoll <= AMP_ROLL + 1e-6f && maxRoll > AMP_ROLL * 0.5f);
  CHECK(maxNaik <= AMP_NAIK + 1e-6f && maxGeser <= AMP_GESER + 1e-6f);
  // cant: a left curve (κ > 0) leans the body LEFT (roll < 0); half the balanced angle, clamped at 3° (7° hard)
  float v = 25, kappa = 1.f / 1000;   // R = 1000 m: balanced 3.6°, shown 1.8° (under the 3° clamp)
  float seimbang = -std::atan(v * v * kappa / G) * 0.5f;
  float roll = hitungGoyang({0, 0, v, kappa, 0, 1}).roll;
  CHECK(roll < 0); CHECK_NEAR(roll - AMP_ROLL * derau(0, 0, D_ROLL), seimbang, 1e-6);
  float tight = hitungGoyang({0, 0, 40, 1.f / 100, 0, 1}).roll - AMP_ROLL * derau(0, 0, D_ROLL);
  CHECK_NEAR(tight, -3 * DEG, 1e-5);
  float slider = hitungGoyang({0, 0, 40, 1.f / 100, 0, 4}).roll - 4 * AMP_ROLL * derau(0, 0, D_ROLL);
  CHECK_NEAR(slider, -7 * DEG, 1e-5);   // 3° × 4 hits the 7° hard limit
  // braking pitches the nose down (angguk < 0), pulling lifts it; ±1.2° cap
  CHECK(hitungGoyang({0, 0, 0.01f, 0, -1, 1}).angguk < 0); CHECK(hitungGoyang({0, 0, 0.01f, 0, 1, 1}).angguk > 0);
  CHECK_NEAR(hitungGoyang({0, 0, 0, 0, 10, 1}).angguk, 1.2f * DEG, 1e-6);
  // curvature helper: front yaw minus rear yaw per metre, wrapped
  CHECK_NEAR(kurvaRel(0.1f, 0.0f, 20), 0.005f, 1e-6); CHECK_NEAR(kurvaRel(-3.1f, 3.1f, 10), (2 * 3.14159265f - 6.2f) / 10, 1e-5);
  CHECK(kurvaRel(1, 0, 0) == 0);
  CHECK_NEAR(yawTiga(1, 0), 0, 1e-6); CHECK_NEAR(yawTiga(0, 1), -3.14159265f / 2, 1e-6);   // world +y -> scene +z = yaw -90°
  // time-scale damping
  CHECK(skalaLaju(1) == 1 && skalaLaju(2) == 1); CHECK_NEAR(skalaLaju(8), 0.25, 1e-6); CHECK(skalaLaju(0.5) == 1);
})

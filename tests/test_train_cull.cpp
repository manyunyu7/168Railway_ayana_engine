// Per-train culling tiers (engine/world/train_cull.h, the old tests/cullka3d.ts): a pure function of the
// head-of-train distance + last tier, with hysteresis (no flicker on a threshold), the camera subject never
// culled, and the hide threshold following the fog without ever cutting below the LOD line.
#include "engine/world/train_cull.h"
#include "tests/check.h"
#include <cmath>

using namespace eng;
using T = TingkatKA;

TEST_MAIN({
  const AmbangCullKA A{900, 1800, 0.1f};
  // 1. first assessment: raw thresholds
  CHECK(putuskanTingkatKA(0, TINGKAT_BELUM, A) == T::Penuh);
  CHECK(putuskanTingkatKA(899, TINGKAT_BELUM, A) == T::Penuh);
  CHECK(putuskanTingkatKA(900, TINGKAT_BELUM, A) == T::Ringan);
  CHECK(putuskanTingkatKA(1799, TINGKAT_BELUM, A) == T::Ringan);
  CHECK(putuskanTingkatKA(1800, TINGKAT_BELUM, A) == T::Sembunyi);
  CHECK(putuskanTingkatKA(1e9f, TINGKAT_BELUM, A) == T::Sembunyi);
  // 2. hysteresis
  CHECK(putuskanTingkatKA(950, (int)T::Penuh, A) == T::Penuh);
  CHECK(putuskanTingkatKA(991, (int)T::Penuh, A) == T::Ringan);
  CHECK(putuskanTingkatKA(850, (int)T::Ringan, A) == T::Ringan);
  CHECK(putuskanTingkatKA(800, (int)T::Ringan, A) == T::Penuh);
  CHECK(putuskanTingkatKA(1850, (int)T::Ringan, A) == T::Ringan);
  CHECK(putuskanTingkatKA(1981, (int)T::Ringan, A) == T::Sembunyi);
  CHECK(putuskanTingkatKA(1700, (int)T::Sembunyi, A) == T::Sembunyi);
  CHECK(putuskanTingkatKA(1600, (int)T::Sembunyi, A) == T::Ringan);
  {   // camera swaying +-30 m right on the LOD line: the tier must not change
    T tk = putuskanTingkatKA(900, TINGKAT_BELUM, A); int ganti = 0;
    for (int i = 0; i < 200; ++i) { T b = putuskanTingkatKA(900 + 30 * std::sin(i * 0.7f), (int)tk, A); if (b != tk) ++ganti; tk = b; }
    CHECK_EQ(ganti, 0);
  }
  // 3. two tiers at once (camera teleport / subject change)
  CHECK(putuskanTingkatKA(50, (int)T::Sembunyi, A) == T::Penuh);
  CHECK(putuskanTingkatKA(5000, (int)T::Penuh, A) == T::Sembunyi);
  // 4. the subject is never culled
  CHECK(putuskanTingkatKA(1e6f, (int)T::Sembunyi, A, true) == T::Penuh);
  CHECK(putuskanTingkatKA(1e6f, TINGKAT_BELUM, A, true) == T::Penuh);
  // 5. hide threshold follows the fog; no fog = fallback; never below the LOD line
  CHECK_NEAR(batasSembunyiKA(1400), 1400 * KA_SEMBUNYI_KABUT, 1e-3);
  CHECK_NEAR(batasSembunyiKA(0), JARAK_KA_SEMBUNYI, 1e-3);
  CHECK_NEAR(batasSembunyiKA(300), AMBANG_LOD, 1e-3);
  CHECK_NEAR(batasSembunyiKA(INFINITY), JARAK_KA_SEMBUNYI, 1e-3);
  CHECK_NEAR(batasSembunyiKA(NAN), JARAK_KA_SEMBUNYI, 1e-3);
})

// Per-train culling tiers (the old src/tiga/cullKA.ts): a PURE decision from the distance head-of-train ->
// camera and last frame's tier, so it can be tested headless (tests/test_train_cull.cpp). Used by
// TrainVisuals::update.
//
//   Penuh    - the full path: sway, track noise (lift + lateral), rail height at both couplers.
//   Ringan   - still drawn, but without sway / noise; one rail-height sample per car (pitch 0): from that
//              far a 3 cm sway and the pitch between the couplers do not reach a pixel.
//   Sembunyi - behind the fog: no vehicle instance at all (no lights, no S21), the per-car work skipped.
//
// HYSTERESIS: moving to a FARTHER tier needs distance > threshold * (1 + h), to a NEARER tier distance <
// threshold * (1 - h). Without it a train standing right on a threshold flickers every frame (orbit sway).
// The camera's subject train (kabin / samping / atas / ekor) is never culled.
#pragma once
#include <algorithm>
#include <cmath>

namespace eng {

enum class TingkatKA : int { Penuh = 0, Ringan = 1, Sembunyi = 2 };
constexpr int TINGKAT_BELUM = -1;   // "no previous tier" (first assessment: no hysteresis)

// dunia3dKonst.ts
constexpr float AMBANG_LOD = 900;          // penuh -> ringan (m); also refDistance >= this = box LOD instead of GLB
constexpr float KA_SEMBUNYI_KABUT = 1.2f;  // hide threshold = fog far x this (a fading train must be white before it vanishes)
constexpr float JARAK_KA_SEMBUNYI = 6000;  // hide threshold without fog (m)
constexpr float KA_HISTERESIS = 0.08f;     // +-8 % band at every threshold

struct AmbangCullKA { float lod = AMBANG_LOD; float sembunyi = JARAK_KA_SEMBUNYI; float histeresis = KA_HISTERESIS; };

// Hide threshold from the active fog: fogFar <= 0 / non-finite (no fog) -> the fixed fallback. Never below `lod`
// (fog nearer than the LOD line leaves the Ringan tier empty - the train does not vanish before it is a box).
inline float batasSembunyiKA(float fogFar, float faktor = KA_SEMBUNYI_KABUT, float cadangan = JARAK_KA_SEMBUNYI, float lod = AMBANG_LOD) {
  float b = fogFar > 0 && std::isfinite(fogFar) ? fogFar * faktor : cadangan;
  return std::max(b, lod);
}

// The tier for this frame. `lalu` = last frame's tier as int (TINGKAT_BELUM = never assessed -> raw thresholds).
// `fokus` = the camera's subject -> always Penuh.
inline TingkatKA putuskanTingkatKA(float jarak, int lalu, const AmbangCullKA& a, bool fokus = false) {
  if (fokus) return TingkatKA::Penuh;
  const float thr[2] = {a.lod, a.sembunyi};   // thr[i] = boundary between tier i and i + 1
  const float h = std::max(0.f, a.histeresis);
  int i = lalu;
  if (i < 0 || i > 2) {
    i = 0;
    while (i < 2 && jarak >= thr[i]) ++i;
    return (TingkatKA)i;
  }
  while (i < 2 && jarak > thr[i] * (1 + h)) ++i;        // moving away: one tier per step, each past its band
  while (i > 0 && jarak < thr[i - 1] * (1 - h)) --i;    // approaching: each below its band
  return (TingkatKA)i;
}

} // namespace eng

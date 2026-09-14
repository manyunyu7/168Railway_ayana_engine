// Body sway (docs/world-spec.md §7.5, uji3dGoyang.ts) — purely cosmetic. Noise is a function of
// POSITION (the track owns its bumps), so a standing train is still and a bump travels down the
// consist. Bodies use only `roll` and the `medan` field (lift / lateral shift at the two coupler
// points); pitch and yaw noise are for the cab camera only. Sign convention (scene space: model
// forward +X, right-of-travel +Z): roll > 0 = leaning right, geser > 0 = shifted right, naik > 0 = up.
#pragma once
#include <algorithm>
#include <cmath>

namespace eng::sway {

struct Octave { float k, ax, az, phase, weight; };
constexpr float TWO_PI = 6.283185307179586f;
inline Octave oct(float lambda, float angle, float phase, float weight) {
  return {TWO_PI / lambda, std::cos(angle), std::sin(angle), phase, weight};
}
// uji3dGoyang.ts:61-65 — wavelengths 20–114 m so the felt frequency at 25 m/s is 0.2–1.2 Hz
inline const Octave D_ROLL[3] = {oct(53.0f, 0.31f, 0.00f, 0.46f), oct(97.3f, 1.42f, 2.11f, 0.32f), oct(29.7f, 2.57f, 4.03f, 0.22f)};
inline const Octave D_NAIK[3] = {oct(41.9f, 0.94f, 1.27f, 0.44f), oct(79.1f, 2.05f, 3.44f, 0.32f), oct(23.3f, 3.09f, 5.51f, 0.24f)};
inline const Octave D_GESER[3] = {oct(61.3f, 0.62f, 0.83f, 0.46f), oct(113.7f, 1.77f, 2.96f, 0.32f), oct(31.1f, 2.88f, 4.62f, 0.22f)};
inline const Octave D_YAW[3] = {oct(37.7f, 1.13f, 3.71f, 0.45f), oct(71.3f, 2.31f, 0.44f, 0.30f), oct(19.9f, 0.15f, 5.09f, 0.25f)};
inline const Octave D_ANGGUK[3] = {oct(47.3f, 0.48f, 2.65f, 0.45f), oct(89.9f, 1.66f, 4.88f, 0.32f), oct(26.9f, 2.79f, 1.02f, 0.23f)};

inline float derau(float x, float z, const Octave (&ok)[3]) {
  float s = 0;
  for (const Octave& o : ok) s += o.weight * std::sin(o.k * (x * o.ax + z * o.az) + o.phase);
  return s;
}

constexpr float DEG = 3.14159265358979f / 180;
constexpr float AMP_ROLL = 0.40f * DEG, AMP_ANGGUK = 0.12f * DEG, AMP_YAW = 0.15f * DEG;
constexpr float AMP_NAIK = 0.020f, AMP_GESER = 0.015f;
constexpr float V_PENUH = 22;           // m/s at which the noise response is full (~80 km/h)
constexpr float G = 9.81f;
constexpr float PORSI_CANT = 0.5f, MAKS_CANT = 3 * DEG, MAKS_CANT_KERAS = 7 * DEG;
constexpr float ANGGUK_PER_A = 0.35f * DEG, MAKS_ANGGUK_A = 1.2f * DEG;

inline float clampf(float v, float lim) { return std::max(-lim, std::min(lim, v)); }
// respons(v) = r²(3 − 2r), r = min(1, |v| / 22): shunting at 5 km/h barely moves
inline float respons(float v) { float r = std::min(1.f, std::fabs(v) / V_PENUH); return r * r * (3 - 2 * r); }

struct Medan { float naik = 0, geser = 0; };
inline Medan medan(float x, float z, float v, float skala) {
  float r = respons(v) * std::max(0.f, skala);
  if (r <= 0) return {};
  return {AMP_NAIK * r * derau(x, z, D_NAIK), AMP_GESER * r * derau(x, z, D_GESER)};
}

// Scene yaw of a world tangent (world y is flipped into scene z): atan2(−ty, tx).
inline float yawTiga(float tx, float ty) { return std::atan2(-ty, tx); }
// Running curvature from the yaw at two points `jarak` metres apart, front minus rear (rad/m; + = left).
inline float kurvaRel(float yawDepan, float yawBelakang, float jarak) {
  if (!(jarak > 1e-6f)) return 0;
  float d = yawDepan - yawBelakang;
  while (d > 3.14159265f) d -= TWO_PI;
  while (d < -3.14159265f) d += TWO_PI;
  return d / jarak;
}
// Amplitude damping for fast clocks: above 2× the felt frequency would just be vibration.
inline float skalaLaju(double timeScale) { double ts = std::max(1.0, timeScale); return ts <= 2 ? 1.f : (float)(2 / ts); }

struct Masukan { float x, z, v, kurva, aksel, skala; };
struct Goyang { float roll = 0, angguk = 0, yaw = 0, naik = 0, geser = 0; };

inline Goyang hitungGoyang(const Masukan& m) {
  float skala = std::max(0.f, m.skala);
  if (skala <= 0) return {};
  float seimbang = -std::atan((m.v * m.v * m.kurva) / G) * PORSI_CANT;
  float cant = clampf(clampf(seimbang, MAKS_CANT) * skala, MAKS_CANT_KERAS);
  float r = respons(m.v) * skala;
  float anggukRem = clampf(ANGGUK_PER_A * m.aksel, MAKS_ANGGUK_A) * skala;
  Medan md = medan(m.x, m.z, m.v, skala);
  return {cant + AMP_ROLL * r * derau(m.x, m.z, D_ROLL),
          anggukRem + AMP_ANGGUK * r * derau(m.x, m.z, D_ANGGUK),
          AMP_YAW * r * derau(m.x, m.z, D_YAW), md.naik, md.geser};
}

// The share that reaches the DRIVER'S EYE (uji3dGoyang.ts goyangKabin): the neck damps, vertical bounce
// most of all (motion sickness), roll least (the tilted horizon in a curve is the "riding" feel).
inline Goyang goyangKabin(const Goyang& g) { return {g.roll * 0.75f, g.angguk * 0.60f, g.yaw * 0.50f, g.naik * 0.45f, g.geser * 0.60f}; }
constexpr float BASIS_KURVA = 25;   // m between the two yaw samples of the running curvature (dunia3dKonst.ts)

} // namespace eng::sway

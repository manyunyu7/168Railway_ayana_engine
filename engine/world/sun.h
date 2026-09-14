// Sun direction and light ladder from the sim clock (spec §9.2).
#pragma once
#include "engine/math/math.h"
#include "engine/render/model_renderer.h"
#include "engine/render/sky.h"

namespace eng {

// lon/lat in degrees; clock in seconds since 00:00 (local, UTC+7 reference meridian 105°E).
inline vec3 sunDirection(double clockSec, double lonDeg, double latDeg, float* elevationOut = nullptr) {
  double h = std::fmod(clockSec / 3600.0, 24.0);
  double solar = h + (lonDeg - 105.0) / 15.0;
  double H = radians((float)((solar - 12.0) * 15.0));
  double phi = radians((float)latDeg), delta = 0;
  double el = std::asin(std::sin(phi) * std::sin(delta) + std::cos(phi) * std::cos(delta) * std::cos(H));
  double az = std::atan2(-std::cos(delta) * std::sin(H), std::sin(delta) * std::cos(phi) - std::cos(delta) * std::sin(phi) * std::cos(H));
  if (elevationOut) *elevationOut = (float)degrees((float)el);
  return {(float)(std::cos(el) * std::sin(az)), (float)std::sin(el), (float)(-std::cos(el) * std::cos(az))};
}

inline vec3 rgb(unsigned hex) { return {((hex >> 16) & 255) / 255.f, ((hex >> 8) & 255) / 255.f, (hex & 255) / 255.f}; }

// Interpolate the light ladder (fog, hemi sky, hemi ground, sun colour, sun intensity, ambient, zenith, horizon).
// `cloud` = sprite tint column `awan` (uji3dLangit.ts:113-118).
struct LightRung { float el; unsigned fog, hemiSky, hemiGround, sunColor; float sun, ambient; unsigned zenith, horizon, cloud; };
inline const LightRung LADDER[] = {
  {-18, 0x121a2b, 0x44577a, 0x161c26, 0x8ea2cc, 0.26f, 0.72f, 0x0a1020, 0x18223a, 0x27303f},
  {-4,  0x2f3a52, 0x4a5f86, 0x1a2230, 0xff8f63, 0.55f, 0.84f, 0x152340, 0x3a3a52, 0x6a6178},
  {2,   0xc98a5e, 0x7f9bc4, 0x3a3630, 0xffb070, 1.70f, 0.95f, 0x2f5f9c, 0xd39a76, 0xffc79a},
  {12,  0xd6c1a6, 0xa8c4e6, 0x4d4a40, 0xffd9a8, 2.45f, 1.20f, 0x3f7dbf, 0xd8cbb8, 0xffe8d2},
  {32,  0xbdd0e4, 0xbcd6f0, 0x52524a, 0xfff2dc, 3.00f, 1.50f, 0x4a8bc9, 0xc4d8ec, 0xfdfdff},
  {75,  0xc8dcef, 0xcadff5, 0x53534a, 0xfffaf0, 3.20f, 1.60f, 0x4f92cf, 0xc7dcef, 0xffffff},
};

inline void applySun(double clockSec, double lonDeg, double latDeg, Lighting& light, Sky& sky) {
  float el; vec3 dir = sunDirection(clockSec, lonDeg, latDeg, &el);
  const int n = (int)(sizeof LADDER / sizeof LADDER[0]);
  int i = 0; while (i < n - 2 && el > LADDER[i + 1].el) ++i;
  const LightRung &a = LADDER[i], &b = LADDER[i + 1];
  float t = (el - a.el) / (b.el - a.el); t = t < 0 ? 0 : t > 1 ? 1 : t;
  auto mixc = [&](unsigned x, unsigned y) { return lerp(rgb(x), rgb(y), t); };
  float sunI = a.sun + (b.sun - a.sun) * t, amb = a.ambient + (b.ambient - a.ambient) * t;
  // theme "terang": sun 1.8·(I/3.2), ambient 2.0·(A/1.6); our hemisphere term is unlit-scaled so fold ambient in
  light.sunDir = el > -6 ? normalize(dir) : normalize(vec3{dir.x, 0.05f, dir.z});
  light.sunColor = mixc(a.sunColor, b.sunColor) * (1.8f * sunI / 3.2f) * 1.6f;
  light.skyColor = mixc(a.hemiSky, b.hemiSky) * (2.0f * amb / 1.6f) * 0.55f;
  light.groundColor = mixc(a.hemiGround, b.hemiGround) * (2.0f * amb / 1.6f) * 0.55f;
  // theme tint (TEMA.kabut): a fifth of the way towards the theme's fog colour — the clock stays in charge
  vec3 fog = lerp(mixc(a.fog, b.fog), rgb(sky.darkTheme ? 0x121821u : 0xcadcedu), 0.2f);
  light.fogColor = fog;
  sky.zenith = mixc(a.zenith, b.zenith); sky.horizon = mixc(a.horizon, b.horizon);
  sky.ground = fog * 0.8f; sky.sunDir = dir; sky.cloudTint = mixc(a.cloud, b.cloud);
}

// Inverse Mercator (spec §2.1), world y is south-positive.
inline void worldToLonLat(double wx, double wy, double& lon, double& lat) {
  const double R = 6378137.0;
  lon = wx / R * 180.0 / PI;
  lat = (2.0 * std::atan(std::exp(-wy / R)) - PI / 2) * 180.0 / PI;
}

} // namespace eng

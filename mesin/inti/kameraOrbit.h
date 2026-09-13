// Kamera mengorbit satu titik sasaran. Y ke atas.
#pragma once
#include "mesin/matek/matek.h"

namespace mesin {

struct KameraOrbit {
  vec3 sasaran{0, 0, 0};
  float jarak = 5, yaw = rad(35), pitch = rad(25);
  float fovY = rad(50), dekat = 0.05f, jauh = 2000;

  vec3 posisi() const {
    return sasaran + vec3{std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw)} * jarak;
  }
  mat4 pandang() const { return mat4::lihat(posisi(), sasaran, {0, 1, 0}); }
  mat4 proyeksi(float aspek) const { return mat4::perspektif(fovY, aspek, dekat, jauh); }

  void putar(float dx, float dy) {
    yaw -= dx * 0.005f; pitch += dy * 0.005f;
    const float batas = rad(89);
    if (pitch > batas) pitch = batas; if (pitch < -batas) pitch = -batas;
  }
  void zoom(float langkah) { jarak *= std::pow(0.9f, langkah); if (jarak < dekat * 4) jarak = dekat * 4; }
};

} // namespace mesin

// Core engine math: vec2/vec3/vec4, mat4 (column-major like OpenGL), quat.
// Conventions: meters, Y up, right-handed (camera looks down -Z).
#pragma once
#include <cmath>
#include <cstdint>

namespace eng {

constexpr float PI = 3.14159265358979323846f;
constexpr float radians(float deg) { return deg * (PI / 180.f); }
constexpr float degrees(float rad) { return rad * (180.f / PI); }

struct vec2 { float x = 0, y = 0; };

struct vec3 {
  float x = 0, y = 0, z = 0;
  constexpr vec3() = default;
  constexpr vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
  constexpr vec3 operator+(vec3 b) const { return {x + b.x, y + b.y, z + b.z}; }
  constexpr vec3 operator-(vec3 b) const { return {x - b.x, y - b.y, z - b.z}; }
  constexpr vec3 operator-() const { return {-x, -y, -z}; }
  constexpr vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
  constexpr vec3 operator*(vec3 b) const { return {x * b.x, y * b.y, z * b.z}; }
  constexpr vec3 operator/(float s) const { return {x / s, y / s, z / s}; }
  vec3& operator+=(vec3 b) { x += b.x; y += b.y; z += b.z; return *this; }
  vec3& operator-=(vec3 b) { x -= b.x; y -= b.y; z -= b.z; return *this; }
  vec3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
};
constexpr vec3 operator*(float s, vec3 v) { return v * s; }
constexpr float dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
constexpr vec3 cross(vec3 a, vec3 b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(vec3 v) { return std::sqrt(dot(v, v)); }
inline vec3 normalize(vec3 v) { float l = length(v); return l > 0 ? v / l : v; }
constexpr vec3 lerp(vec3 a, vec3 b, float t) { return a + (b - a) * t; }
constexpr vec3 vmin(vec3 a, vec3 b) { return {a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z}; }
constexpr vec3 vmax(vec3 a, vec3 b) { return {a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y, a.z > b.z ? a.z : b.z}; }

struct vec4 {
  float x = 0, y = 0, z = 0, w = 0;
  constexpr vec4() = default;
  constexpr vec4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
  constexpr vec4(vec3 v, float w_) : x(v.x), y(v.y), z(v.z), w(w_) {}
  constexpr vec3 xyz() const { return {x, y, z}; }
};

// 4x4 column-major matrix: m[col][row]; m[3] holds translation.
struct mat4 {
  float m[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};

  static constexpr mat4 identity() { return {}; }
  const float* data() const { return &m[0][0]; }

  mat4 operator*(const mat4& b) const {
    mat4 r;
    for (int k = 0; k < 4; ++k)
      for (int j = 0; j < 4; ++j) {
        float s = 0;
        for (int i = 0; i < 4; ++i) s += m[i][j] * b.m[k][i];
        r.m[k][j] = s;
      }
    return r;
  }
  vec4 operator*(vec4 v) const {
    return {m[0][0] * v.x + m[1][0] * v.y + m[2][0] * v.z + m[3][0] * v.w,
            m[0][1] * v.x + m[1][1] * v.y + m[2][1] * v.z + m[3][1] * v.w,
            m[0][2] * v.x + m[1][2] * v.y + m[2][2] * v.z + m[3][2] * v.w,
            m[0][3] * v.x + m[1][3] * v.y + m[2][3] * v.z + m[3][3] * v.w};
  }
  vec3 transformPoint(vec3 p) const { vec4 r = *this * vec4(p, 1); return r.xyz() / r.w; }
  vec3 transformDir(vec3 d) const { return (*this * vec4(d, 0)).xyz(); }

  static mat4 translation(vec3 t) { mat4 r; r.m[3][0] = t.x; r.m[3][1] = t.y; r.m[3][2] = t.z; return r; }
  static mat4 scale(vec3 s) { mat4 r; r.m[0][0] = s.x; r.m[1][1] = s.y; r.m[2][2] = s.z; return r; }
  static mat4 rotationY(float a) {
    mat4 r; float c = std::cos(a), s = std::sin(a);
    r.m[0][0] = c; r.m[0][2] = -s; r.m[2][0] = s; r.m[2][2] = c; return r;
  }
  static mat4 rotationX(float a) {
    mat4 r; float c = std::cos(a), s = std::sin(a);
    r.m[1][1] = c; r.m[1][2] = s; r.m[2][1] = -s; r.m[2][2] = c; return r;
  }
  // Perspective projection, NDC z in [-1,1] (OpenGL). fovY in radians.
  static mat4 perspective(float fovY, float aspect, float near, float far) {
    mat4 r{}; float f = 1.f / std::tan(fovY / 2);
    r.m[0][0] = f / aspect; r.m[1][1] = f;
    r.m[2][2] = (far + near) / (near - far); r.m[2][3] = -1;
    r.m[3][2] = (2 * far * near) / (near - far); r.m[3][3] = 0;
    return r;
  }
  static mat4 lookAt(vec3 eye, vec3 target, vec3 up) {
    vec3 f = normalize(target - eye), s = normalize(cross(f, up)), u = cross(s, f);
    mat4 r;
    r.m[0][0] = s.x; r.m[1][0] = s.y; r.m[2][0] = s.z;
    r.m[0][1] = u.x; r.m[1][1] = u.y; r.m[2][1] = u.z;
    r.m[0][2] = -f.x; r.m[1][2] = -f.y; r.m[2][2] = -f.z;
    r.m[3][0] = -dot(s, eye); r.m[3][1] = -dot(u, eye); r.m[3][2] = dot(f, eye);
    return r;
  }
  mat4 inverse() const;  // general cofactor inverse; fine for cameras/transforms
  mat4 transpose() const {
    mat4 r; for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) r.m[i][j] = m[j][i]; return r;
  }
};

struct quat {
  float x = 0, y = 0, z = 0, w = 1;
  static quat axisAngle(vec3 axis, float a) {
    vec3 n = normalize(axis); float s = std::sin(a / 2);
    return {n.x * s, n.y * s, n.z * s, std::cos(a / 2)};
  }
  quat operator*(quat b) const {
    return {w * b.x + x * b.w + y * b.z - z * b.y,
            w * b.y - x * b.z + y * b.w + z * b.x,
            w * b.z + x * b.y - y * b.x + z * b.w,
            w * b.w - x * b.x - y * b.y - z * b.z};
  }
  mat4 toMat4() const {
    mat4 r;
    float xx = x * x, yy = y * y, zz = z * z, xy = x * y, xz = x * z, yz = y * z, wx = w * x, wy = w * y, wz = w * z;
    r.m[0][0] = 1 - 2 * (yy + zz); r.m[0][1] = 2 * (xy + wz);     r.m[0][2] = 2 * (xz - wy);
    r.m[1][0] = 2 * (xy - wz);     r.m[1][1] = 1 - 2 * (xx + zz); r.m[1][2] = 2 * (yz + wx);
    r.m[2][0] = 2 * (xz + wy);     r.m[2][1] = 2 * (yz - wx);     r.m[2][2] = 1 - 2 * (xx + yy);
    return r;
  }
};

// TRS -> mat4 (scale, then rotate, then translate) — same as glTF.
inline mat4 trs(vec3 t, quat r, vec3 s) {
  return mat4::translation(t) * r.toMat4() * mat4::scale(s);
}

inline mat4 mat4::inverse() const {
  const float* a = data(); float inv[16];
  inv[0] = a[5]*a[10]*a[15] - a[5]*a[11]*a[14] - a[9]*a[6]*a[15] + a[9]*a[7]*a[14] + a[13]*a[6]*a[11] - a[13]*a[7]*a[10];
  inv[4] = -a[4]*a[10]*a[15] + a[4]*a[11]*a[14] + a[8]*a[6]*a[15] - a[8]*a[7]*a[14] - a[12]*a[6]*a[11] + a[12]*a[7]*a[10];
  inv[8] = a[4]*a[9]*a[15] - a[4]*a[11]*a[13] - a[8]*a[5]*a[15] + a[8]*a[7]*a[13] + a[12]*a[5]*a[11] - a[12]*a[7]*a[9];
  inv[12] = -a[4]*a[9]*a[14] + a[4]*a[10]*a[13] + a[8]*a[5]*a[14] - a[8]*a[6]*a[13] - a[12]*a[5]*a[10] + a[12]*a[6]*a[9];
  inv[1] = -a[1]*a[10]*a[15] + a[1]*a[11]*a[14] + a[9]*a[2]*a[15] - a[9]*a[3]*a[14] - a[13]*a[2]*a[11] + a[13]*a[3]*a[10];
  inv[5] = a[0]*a[10]*a[15] - a[0]*a[11]*a[14] - a[8]*a[2]*a[15] + a[8]*a[3]*a[14] + a[12]*a[2]*a[11] - a[12]*a[3]*a[10];
  inv[9] = -a[0]*a[9]*a[15] + a[0]*a[11]*a[13] + a[8]*a[1]*a[15] - a[8]*a[3]*a[13] - a[12]*a[1]*a[11] + a[12]*a[3]*a[9];
  inv[13] = a[0]*a[9]*a[14] - a[0]*a[10]*a[13] - a[8]*a[1]*a[14] + a[8]*a[2]*a[13] + a[12]*a[1]*a[10] - a[12]*a[2]*a[9];
  inv[2] = a[1]*a[6]*a[15] - a[1]*a[7]*a[14] - a[5]*a[2]*a[15] + a[5]*a[3]*a[14] + a[13]*a[2]*a[7] - a[13]*a[3]*a[6];
  inv[6] = -a[0]*a[6]*a[15] + a[0]*a[7]*a[14] + a[4]*a[2]*a[15] - a[4]*a[3]*a[14] - a[12]*a[2]*a[7] + a[12]*a[3]*a[6];
  inv[10] = a[0]*a[5]*a[15] - a[0]*a[7]*a[13] - a[4]*a[1]*a[15] + a[4]*a[3]*a[13] + a[12]*a[1]*a[7] - a[12]*a[3]*a[5];
  inv[14] = -a[0]*a[5]*a[14] + a[0]*a[6]*a[13] + a[4]*a[1]*a[14] - a[4]*a[2]*a[13] - a[12]*a[1]*a[6] + a[12]*a[2]*a[5];
  inv[3] = -a[1]*a[6]*a[11] + a[1]*a[7]*a[10] + a[5]*a[2]*a[11] - a[5]*a[3]*a[10] - a[9]*a[2]*a[7] + a[9]*a[3]*a[6];
  inv[7] = a[0]*a[6]*a[11] - a[0]*a[7]*a[10] - a[4]*a[2]*a[11] + a[4]*a[3]*a[10] + a[8]*a[2]*a[7] - a[8]*a[3]*a[6];
  inv[11] = -a[0]*a[5]*a[11] + a[0]*a[7]*a[9] + a[4]*a[1]*a[11] - a[4]*a[3]*a[9] - a[8]*a[1]*a[7] + a[8]*a[3]*a[5];
  inv[15] = a[0]*a[5]*a[10] - a[0]*a[6]*a[9] - a[4]*a[1]*a[10] + a[4]*a[2]*a[9] + a[8]*a[1]*a[6] - a[8]*a[2]*a[5];
  float det = a[0]*inv[0] + a[1]*inv[4] + a[2]*inv[8] + a[3]*inv[12];
  mat4 r; if (det == 0) return r;
  float id = 1.f / det; float* o = &r.m[0][0];
  for (int i = 0; i < 16; ++i) o[i] = inv[i] * id;
  return r;
}

} // namespace eng

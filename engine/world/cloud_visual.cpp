#include "engine/world/cloud_visual.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace eng {

namespace {
constexpr int TEX = 128;

// Vertex: centre xyz, billboard offset xy (m), uv, opacity — 8 floats.
const char* VS = R"(
layout(location=0) in vec3 aCenter; layout(location=1) in vec2 aOffset; layout(location=2) in vec2 aUv; layout(location=3) in float aOpacity;
uniform mat4 uVP; uniform vec3 uRight, uUp, uEye; uniform vec2 uDrift; uniform vec4 uBox;
out vec2 vUv; out float vOpacity; out float vDist;
void main() {
  vec3 c = aCenter;
  c.x = uBox.x + mod(c.x - uBox.x + uDrift.x, uBox.z);   // wrap the drift inside the scatter box
  c.z = uBox.y + mod(c.z - uBox.y + uDrift.y, uBox.w);
  vec3 p = c + uRight * aOffset.x + uUp * aOffset.y;
  vUv = aUv; vOpacity = aOpacity; vDist = length(p - uEye);
  gl_Position = uVP * vec4(p, 1.0);
})";
const char* FS = R"(
in vec2 vUv; in float vOpacity; in float vDist;
uniform sampler2D uTex; uniform vec3 uTint, uFog; uniform float uFogD;
out vec4 oColor;
void main() {
  float a = texture(uTex, vUv).a * vOpacity;
  float f = 1.0 - exp(-vDist * uFogD);
  vec3 c = mix(uTint, uFog, f);
  oColor = vec4(pow(c, vec3(1.0/2.2)), a);
})";

struct Rng { uint32_t s; float next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return (float)(s & 0xffffff) / 16777216.f; } };
} // namespace

void CloudVisual::build(float x0, float z0, float x1, float z1, uint32_t seed) {
  destroy();
  Rng rng{seed * 2654435761u + 12345u};
  // texture atlas: VARIANTS blobs side by side, each 9 radial gradients composited source-over (texturAwan)
  std::vector<uint8_t> px((size_t)TEX * VARIANTS * TEX * 4, 255);
  std::vector<float> alpha((size_t)TEX * TEX);
  for (int v = 0; v < VARIANTS; ++v) {
    std::fill(alpha.begin(), alpha.end(), 0.f);
    for (int i = 0; i < 9; ++i) {
      float r = TEX * (0.16f + rng.next() * 0.2f), cx = TEX * (0.25f + rng.next() * 0.5f), cy = TEX * (0.35f + rng.next() * 0.32f);
      for (int y = 0; y < TEX; ++y)
        for (int x = 0; x < TEX; ++x) {
          float d = std::hypot((float)x + 0.5f - cx, (float)y + 0.5f - cy) / r;
          if (d >= 1) continue;
          float g = 0.85f * (1 - d);
          float& a = alpha[(size_t)y * TEX + (size_t)x];
          a = a + g * (1 - a);
        }
    }
    for (int y = 0; y < TEX; ++y)
      for (int x = 0; x < TEX; ++x) px[(((size_t)y * TEX * VARIANTS) + (size_t)(v * TEX + x)) * 4 + 3] = (uint8_t)std::lround(std::clamp(alpha[(size_t)y * TEX + (size_t)x], 0.f, 1.f) * 255);
  }
  tex_ = rhi::createTexture(TEX * VARIANTS, TEX, rhi::Format::RGBA8, std::as_bytes(std::span(px)), true, false, rhi::Wrap::Clamp, rhi::Wrap::Clamp);

  x0_ = x0 - MARGIN; z0_ = z0 - MARGIN; lx_ = (x1 - x0) + 2 * MARGIN; lz_ = (z1 - z0) + 2 * MARGIN;
  int n = std::clamp((int)std::lround(lx_ / 1000 * lz_ / 1000 * DENSITY), MIN_N, MAX_N);
  struct Layer { int n; float y0, y1, s0, s1; };
  int nLow = (int)std::lround(n * 0.75);
  const Layer layers[2] = {{nLow, Y_LOW, 1100, 420, 1250}, {n - nLow, 1250, 1750, 900, 2200}};
  const float ops[3] = {0.34f, 0.5f, 0.68f};
  std::vector<float> vb; std::vector<uint32_t> ib;
  for (const Layer& L : layers)
    for (int i = 0; i < L.n; ++i) {
      int variant = (int)(rng.next() * VARIANTS) % VARIANTS; float op = ops[(int)(rng.next() * 3) % 3];
      float sk = L.s0 + rng.next() * (L.s1 - L.s0), w = sk, h = sk * 0.42f;
      vec3 c{x0_ + rng.next() * lx_, L.y0 + rng.next() * (L.y1 - L.y0), z0_ + rng.next() * lz_};
      uint32_t base = (uint32_t)(vb.size() / 8);
      const float cx[4] = {-0.5f, 0.5f, 0.5f, -0.5f}, cy[4] = {-0.5f, -0.5f, 0.5f, 0.5f};
      for (int k = 0; k < 4; ++k) {
        float u = ((float)variant + cx[k] + 0.5f) / VARIANTS, vv = 0.5f - cy[k];
        float data[8] = {c.x, c.y, c.z, cx[k] * w, cy[k] * h, u, vv, op};
        vb.insert(vb.end(), data, data + 8);
      }
      ib.insert(ib.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
  const rhi::Attribute layout[] = {{0, 3, 32, 0}, {1, 2, 32, 12}, {2, 2, 32, 20}, {3, 1, 32, 28}};
  mesh_ = rhi::createMesh(std::as_bytes(std::span(vb)), layout, ib);
  count_ = n;
  prog_ = rhi::createProgram(VS, FS);
  uVP_ = rhi::uniformLocation(prog_, "uVP"); uRight_ = rhi::uniformLocation(prog_, "uRight"); uUp_ = rhi::uniformLocation(prog_, "uUp");
  uEye_ = rhi::uniformLocation(prog_, "uEye"); uTint_ = rhi::uniformLocation(prog_, "uTint"); uFog_ = rhi::uniformLocation(prog_, "uFog");
  uFogD_ = rhi::uniformLocation(prog_, "uFogD"); uDrift_ = rhi::uniformLocation(prog_, "uDrift"); uBox_ = rhi::uniformLocation(prog_, "uBox");
  rhi::useProgram(prog_); rhi::setUniform(rhi::uniformLocation(prog_, "uTex"), 0);
}

void CloudVisual::draw(const mat4& viewProj, const mat4& view, vec3 eye, const Sky& sky, const Lighting& light, float dt) {
  if (!mesh_.indexCount) return;
  drift_.x += wind.x * dt; drift_.y += wind.y * dt;
  rhi::useProgram(prog_);
  rhi::setUniform(uVP_, viewProj.data());
  rhi::setUniform(uRight_, view.m[0][0], view.m[1][0], view.m[2][0]);   // camera axes = rows of the view matrix
  rhi::setUniform(uUp_, view.m[0][1], view.m[1][1], view.m[2][1]);
  rhi::setUniform(uEye_, eye.x, eye.y, eye.z);
  rhi::setUniform(uTint_, sky.cloudTint.x, sky.cloudTint.y, sky.cloudTint.z);
  rhi::setUniform(uFog_, light.fogColor.x, light.fogColor.y, light.fogColor.z);
  rhi::setUniform(uFogD_, light.fogDensity);
  rhi::setUniform(uDrift_, drift_.x, drift_.y);
  rhi::setUniform(uBox_, x0_, z0_, lx_, lz_);
  rhi::bindTexture(0, tex_);
  rhi::setBlend(true); rhi::setBlendAdditive(false); rhi::setDepthWrite(false); rhi::setCullFace(false);
  rhi::drawMesh(mesh_);
  rhi::setBlend(false); rhi::setDepthWrite(true); rhi::setCullFace(true);
}

void CloudVisual::destroy() {
  if (mesh_.vao) rhi::destroyMesh(mesh_);
  if (tex_.id) { rhi::destroyTexture(tex_); tex_ = {}; }
  if (prog_.id) { rhi::destroyProgram(prog_); prog_ = {}; }
  count_ = 0;
}

} // namespace eng

#include "engine/render/sky.h"

namespace eng {

static const char* VS = R"(
layout(location=0) in vec2 aPos;
out vec2 vNdc;
void main() { vNdc = aPos; gl_Position = vec4(aPos, 0.999999, 1.0); })";

static const char* FS = R"(
in vec2 vNdc;
uniform mat4 uInvVP; uniform vec3 uEye, uZenith, uHorizon, uGround, uSun;
out vec4 oColor;
void main() {
  vec4 p = uInvVP * vec4(vNdc, 1.0, 1.0);
  vec3 dir = normalize(p.xyz / p.w - uEye);
  float t = dir.y;
  vec3 c = t >= 0.0 ? mix(uHorizon, uZenith, pow(t, 0.6)) : mix(uHorizon, uGround, clamp(-t * 4.0, 0.0, 1.0));
  float s = max(dot(dir, normalize(uSun)), 0.0);
  c += vec3(1.0, 0.95, 0.8) * (pow(s, 512.0) * 3.0 + pow(s, 8.0) * 0.12);
  oColor = vec4(pow(c, vec3(1.0/2.2)), 1.0);
})";

void Sky::init() {
  prog_ = rhi::createProgram(VS, FS);
  uInvVP_ = rhi::uniformLocation(prog_, "uInvVP"); uEye_ = rhi::uniformLocation(prog_, "uEye");
  uZenith_ = rhi::uniformLocation(prog_, "uZenith"); uHorizon_ = rhi::uniformLocation(prog_, "uHorizon");
  uGround_ = rhi::uniformLocation(prog_, "uGround"); uSun_ = rhi::uniformLocation(prog_, "uSun");
  const float v[6] = {-1, -1, 3, -1, -1, 3};
  const uint32_t i[3] = {0, 1, 2};
  const rhi::Attribute layout[] = {{0, 2, 8, 0}};
  tri_ = rhi::createMesh(std::as_bytes(std::span(v)), layout, i);
}
void Sky::shutdown() { rhi::destroyProgram(prog_); rhi::destroyMesh(tri_); }

void Sky::draw(const mat4& invViewProj, vec3 eye) {
  rhi::useProgram(prog_);
  rhi::setUniform(uInvVP_, invViewProj.data());
  rhi::setUniform(uEye_, eye.x, eye.y, eye.z);
  rhi::setUniform(uZenith_, zenith.x, zenith.y, zenith.z);
  rhi::setUniform(uHorizon_, horizon.x, horizon.y, horizon.z);
  rhi::setUniform(uGround_, ground.x, ground.y, ground.z);
  rhi::setUniform(uSun_, sunDir.x, sunDir.y, sunDir.z);
  rhi::setDepthWrite(false); rhi::setCullFace(false);
  rhi::drawMesh(tri_);
  rhi::setDepthWrite(true); rhi::setCullFace(true);
}

} // namespace eng

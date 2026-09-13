// Metallic-roughness PBR shader source (GLSL, version header added by RHI).
#pragma once

namespace eng::shaders {

inline const char* PBR_VS = R"(
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
layout(location=2) in vec2 aUV;
uniform mat4 uViewProj, uModel;
out vec3 vNormal, vWorldPos;
out vec2 vUV;
void main() {
  vec4 wp = uModel * vec4(aPos, 1.0);
  vWorldPos = wp.xyz;
  vNormal = mat3(uModel) * aNormal;   // fine for uniform scale; use inverse-transpose later
  vUV = aUV;
  gl_Position = uViewProj * wp;
})";

inline const char* PBR_FS = R"(
in vec3 vNormal, vWorldPos;
in vec2 vUV;
uniform vec3 uEye, uSunDir, uSunColor, uSkyColor, uGroundColor;
uniform vec4 uBaseColor;
uniform vec3 uEmissive;
uniform float uMetallic, uRoughness, uAlphaCutoff;
uniform int uHasBaseTex, uHasMRTex, uHasEmissiveTex, uAlphaMode; // 0 opaque 1 mask 2 blend
uniform sampler2D uBaseTex, uMRTex, uEmissiveTex;
out vec4 oColor;

const float PI = 3.14159265;

float D_GGX(float NoH, float a) { float a2 = a*a; float d = NoH*NoH*(a2-1.0)+1.0; return a2/(PI*d*d); }
float G_Smith(float NoV, float NoL, float a) {
  float k = (a+1.0)*(a+1.0)/8.0;
  return (NoV/(NoV*(1.0-k)+k)) * (NoL/(NoL*(1.0-k)+k));
}
vec3 F_Schlick(float VoH, vec3 f0) { return f0 + (1.0-f0)*pow(1.0-VoH, 5.0); }

void main() {
  vec4 base = uBaseColor;
  if (uHasBaseTex == 1) base *= texture(uBaseTex, vUV);   // sRGB texture → linear by GPU
  if (uAlphaMode == 1 && base.a < uAlphaCutoff) discard;
  float metallic = uMetallic, roughness = uRoughness;
  if (uHasMRTex == 1) { vec3 mr = texture(uMRTex, vUV).rgb; roughness *= mr.g; metallic *= mr.b; }
  roughness = clamp(roughness, 0.04, 1.0);

  vec3 n = normalize(vNormal);
  if (!gl_FrontFacing) n = -n;
  vec3 v = normalize(uEye - vWorldPos);
  vec3 l = normalize(uSunDir);
  vec3 h = normalize(l + v);
  float NoL = max(dot(n,l), 0.0), NoV = max(dot(n,v), 1e-4), NoH = max(dot(n,h), 0.0), VoH = max(dot(v,h), 0.0);

  vec3 f0 = mix(vec3(0.04), base.rgb, metallic);
  vec3 diffuseColor = base.rgb * (1.0 - metallic);
  float a = roughness * roughness;
  vec3 F = F_Schlick(VoH, f0);
  vec3 spec = D_GGX(NoH, a) * G_Smith(NoV, NoL, a) * F / max(4.0*NoV*NoL, 1e-4);
  vec3 direct = (diffuseColor/PI * (1.0-F) + spec) * uSunColor * NoL;

  // hemisphere ambient (sky above, ground below) — placeholder until IBL
  float up = n.y * 0.5 + 0.5;
  vec3 ambient = mix(uGroundColor, uSkyColor, up) * (diffuseColor + f0 * 0.3);

  vec3 emissive = uEmissive;
  if (uHasEmissiveTex == 1) emissive *= texture(uEmissiveTex, vUV).rgb;

  vec3 color = direct + ambient + emissive;
  color = color / (color + vec3(1.0));          // Reinhard tonemap
  oColor = vec4(pow(color, vec3(1.0/2.2)), uAlphaMode == 2 ? base.a : 1.0);
})";

} // namespace eng::shaders

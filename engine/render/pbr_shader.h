// Metallic-roughness PBR shader source (GLSL, version header added by RHI).
#pragma once

namespace eng::shaders {

// Skinned variant of PBR_VS: prepend this to the source and the shader reads the joint/weight attributes
// and the uJoints[] palette (ModelRenderer keeps a second program compiled with it). The palette is a plain
// uniform array: 64 mat4 = 4 KB, within the GL 4.1 / GLES 3.0 minimum for the vertex stage.
inline const char* SKINNING_DEFINE = "#define SKINNED 1\n#define MAX_JOINTS 64\n";

inline const char* PBR_VS = R"(
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
layout(location=2) in vec2 aUV;
layout(location=3) in vec4 aInst0;   // per-instance model matrix columns (rhi::attachInstances)
layout(location=4) in vec4 aInst1;
layout(location=5) in vec4 aInst2;
layout(location=6) in vec4 aInst3;
layout(location=7) in float aShade;  // optional per-vertex brightness (rhi::ATTR_SHADE), 1.0 when absent
#ifdef SKINNED
layout(location=8) in vec4 aJoints;  // joint indices, u8x4 unnormalized (rhi::ATTR_JOINTS)
layout(location=9) in vec4 aWeights; // influences, sum 1 (rhi::ATTR_WEIGHTS)
uniform mat4 uJoints[MAX_JOINTS];
#endif
uniform mat4 uViewProj, uModel;
uniform int uInstanced;              // 1: model = uModel * instance matrix
out vec3 vNormal, vWorldPos;
out vec2 vUV;
out float vShade;
void main() {
  vShade = aShade;
  mat4 model = uInstanced == 1 ? uModel * mat4(aInst0, aInst1, aInst2, aInst3) : uModel;
  vec3 pos = aPos, nrm = aNormal;
#ifdef SKINNED
  mat4 skin = aWeights.x * uJoints[int(aJoints.x)] + aWeights.y * uJoints[int(aJoints.y)]
            + aWeights.z * uJoints[int(aJoints.z)] + aWeights.w * uJoints[int(aJoints.w)];
  pos = (skin * vec4(aPos, 1.0)).xyz;
  nrm = mat3(skin) * aNormal;        // uniform joint scale assumed (rigs never squash bones)
#endif
  vec4 wp = model * vec4(pos, 1.0);
  vWorldPos = wp.xyz;
  vNormal = mat3(model) * nrm;       // fine for uniform scale; use inverse-transpose later
  vUV = aUV;
  gl_Position = uViewProj * wp;
})";

inline const char* PBR_FS = R"(
in vec3 vNormal, vWorldPos;
in vec2 vUV;
in float vShade;
uniform vec3 uEye, uSunDir, uSunColor, uSkyColor, uGroundColor;
uniform vec4 uBaseColor;
uniform vec3 uEmissive;
uniform float uMetallic, uRoughness, uAlphaCutoff;
uniform int uHasBaseTex, uHasMRTex, uHasEmissiveTex, uHasNormalTex, uHasOcclusionTex, uAlphaMode, uUnlit; // 0 opaque 1 mask 2 blend
uniform vec3 uFogColor;
uniform float uFogDensity;   // 0 = off
uniform sampler2D uBaseTex, uMRTex, uEmissiveTex, uNormalTex, uOcclusionTex;
out vec4 oColor;

const float PI = 3.14159265;

float D_GGX(float NoH, float a) { float a2 = a*a; float d = NoH*NoH*(a2-1.0)+1.0; return a2/(PI*d*d); }
float G_Smith(float NoV, float NoL, float a) {
  float k = (a+1.0)*(a+1.0)/8.0;
  return (NoV/(NoV*(1.0-k)+k)) * (NoL/(NoL*(1.0-k)+k));
}
vec3 F_Schlick(float VoH, vec3 f0) { return f0 + (1.0-f0)*pow(1.0-VoH, 5.0); }

// Tangent-space normal map without vertex tangents: the tangent (direction of increasing u) comes from the
// screen-space derivatives of position and UV, the bitangent from cross(n, t) as the Khronos sample viewer
// does — that matches the glTF convention (+X right, +Y up in the map) for non-mirrored UVs.
vec3 perturbNormal(vec3 n, vec3 p, vec2 uv) {
  vec3 dpx = dFdx(p), dpy = dFdy(p);
  vec2 duvx = dFdx(uv), duvy = dFdy(uv);
  float det = duvx.x * duvy.y - duvy.x * duvx.y;
  if (abs(det) < 1e-12) return n;                      // degenerate UVs (or a flat-coloured quad)
  vec3 t = (duvy.y * dpx - duvx.y * dpy) / det;
  t = normalize(t - n * dot(n, t));
  vec3 b = cross(n, t);
  vec3 tn = texture(uNormalTex, uv).xyz * 2.0 - 1.0;
  return normalize(mat3(t, b, n) * tn);
}

void main() {
  vec4 base = uBaseColor;
  if (uHasBaseTex == 1) base *= texture(uBaseTex, vUV);   // sRGB texture → linear by GPU
  if (uAlphaMode == 1 && base.a < uAlphaCutoff) discard;
  if (uAlphaMode == 2 && base.a < 0.02) discard;   // blend writes depth: keep cut-out holes open
  if (uUnlit == 1) { oColor = vec4(pow(base.rgb, vec3(1.0/2.2)), uAlphaMode == 2 ? base.a : 1.0); return; }
  float metallic = uMetallic, roughness = uRoughness;
  if (uHasMRTex == 1) { vec3 mr = texture(uMRTex, vUV).rgb; roughness *= mr.g; metallic *= mr.b; }
  roughness = clamp(roughness, 0.04, 1.0);

  vec3 n = normalize(vNormal);
  if (!gl_FrontFacing) n = -n;
  if (uHasNormalTex == 1) n = perturbNormal(n, vWorldPos, vUV);
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
  if (uHasOcclusionTex == 1) ambient *= texture(uOcclusionTex, vUV).r;   // baked AO darkens indirect light only (glTF)

  vec3 emissive = uEmissive;
  if (uHasEmissiveTex == 1) emissive *= texture(uEmissiveTex, vUV).rgb;

  vec3 color = (direct + ambient) * vShade + emissive;
  color = color / (color + vec3(1.0));          // Reinhard tonemap
  if (uFogDensity > 0.0) {
    float dist = length(uEye - vWorldPos);
    float f = 1.0 - exp(-dist * dist * uFogDensity * uFogDensity);
    color = mix(color, uFogColor, clamp(f, 0.0, 1.0));
  }
  oColor = vec4(pow(color, vec3(1.0/2.2)), uAlphaMode == 2 ? base.a : 1.0);
})";

} // namespace eng::shaders

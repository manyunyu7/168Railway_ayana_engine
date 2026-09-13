// In-memory model: what both the GLB parser and the engine's own binary format produce.
#pragma once
#include "engine/math/math.h"
#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace eng {

struct Vertex { vec3 pos; vec3 normal; vec2 uv; };   // 32 bytes, interleaved

// GPU texture formats an image may be stored in (EMOD v5). Compressed data is uploaded as-is with the
// full mip chain; the runtime never encodes or decodes block formats.
enum class TexFormat : uint8_t { RGBA8, ETC2_RGB, ETC2_RGBA, BC1, BC3, BC7 };
constexpr bool isCompressed(TexFormat f) { return f != TexFormat::RGBA8; }
constexpr size_t texLevelBytes(TexFormat f, int w, int h) {
  size_t blocks = (size_t)((w + 3) / 4) * (size_t)((h + 3) / 4);
  return f == TexFormat::RGBA8 ? (size_t)w * h * 4 : blocks * (f == TexFormat::ETC2_RGB || f == TexFormat::BC1 ? 8 : 16);
}
struct MipLevel { int width = 0, height = 0; std::vector<uint8_t> data; };
struct ImageVariant { TexFormat format = TexFormat::RGBA8; std::vector<MipLevel> mips; };   // mips[0] = full size

struct Image {
  std::string mime;                 // "image/png" etc. when still encoded
  std::vector<uint8_t> encoded;     // raw file bytes (GLB) — empty once decoded
  int width = 0, height = 0, channels = 0;
  std::vector<uint8_t> pixels;      // decoded RGBA8 (engine format; EMOD ≤ v4, or converter output before encoding)
  std::vector<ImageVariant> variants;   // v5: encoded forms in preference order (e.g. ETC2 chain, then a small RGBA8
                                    // fallback); the loader uploads the first one the GPU supports. Empty = use pixels.
  uint8_t wrapS = 0, wrapT = 0;     // glTF sampler wrap of the textures using this image: 0 repeat, 1 clamp, 2 mirror
  bool linear = false;              // sampled as data (metal/rough, normal, occlusion): upload without sRGB decode
  int source = -1;                  // v6: index of this image in the external texture file (KTX2 GLB twin) when the
                                    // EMOD carries only a placeholder (no variants, no pixels); the host streams the
                                    // texture in later. -1 = the image is stored in the file.
  bool placeholder() const { return variants.empty() && pixels.empty() && encoded.empty(); }
};

enum class AlphaMode : uint8_t { Opaque, Mask, Blend };

struct Material {
  std::string name;
  vec4 baseColor{1, 1, 1, 1};
  float metallic = 1, roughness = 1;
  vec3 emissive{0, 0, 0};
  int baseColorTex = -1, metalRoughTex = -1, normalTex = -1, emissiveTex = -1, occlusionTex = -1;
  int baseColorUv = 0;              // which TEXCOORD_n the base colour texture samples (glTF texCoord)
  vec2 uvOffset{0, 0}, uvScale{1, 1}; float uvRotation = 0;   // KHR_texture_transform of the base colour texture; the
                                    // GLB parser bakes it into the vertex UVs, so this is informational only (not serialised)
  AlphaMode alphaMode = AlphaMode::Opaque;
  float alphaCutoff = 0.5f;
  bool doubleSided = false;
  bool unlit = false;               // KHR_materials_unlit: base colour shown as-is
  bool depthTest = true;            // false = overlay drawn on top (reticles, gizmos); not serialised
  bool additive = false;            // Blend only: additive blending (light coronas, glows); not serialised
};

struct Primitive {
  int material = -1;
  std::vector<Vertex> vertices;
  std::vector<uint32_t> indices;
  vec3 boundsMin, boundsMax;
};

struct Mesh { std::string name; std::vector<Primitive> primitives; };

struct Node {
  std::string name;
  int mesh = -1, parent = -1;
  std::vector<int> children;
  mat4 local;                       // TRS baked into a matrix
  vec3 translation{0, 0, 0}; quat rotation; vec3 scale{1, 1, 1};   // the same TRS split (animation targets replace
                                    // one component and rebuild `local`); a matrix-form node is decomposed
  std::map<std::string, std::string> extras;   // scalar glTF node extras (numbers/bools/strings as text)
  float extraNumber(const std::string& key, float d) const {
    auto it = extras.find(key); if (it == extras.end()) return d;
    char* e = nullptr; float v = std::strtof(it->second.c_str(), &e); return e && *e == 0 ? v : d;
  }
};

// Animation clips (glTF animations[]): keyframed node translation / rotation / scale. Sampler
// keyframe times are seconds; values are comps floats per key (3, 4 for rotation quaternions, 3).
// CUBICSPLINE samplers are stored with only their keyframe values (tangents dropped) as LINEAR.
struct AnimSampler {
  std::vector<float> times, values;
  uint8_t comps = 3;
  bool step = false;                // STEP interpolation (else LINEAR)
};
enum class AnimPath : uint8_t { Translation, Rotation, Scale };
struct AnimChannel { int sampler = -1, node = -1; AnimPath path = AnimPath::Translation; };
struct Animation {
  std::string name;
  float duration = 0;               // last keyframe time over all samplers
  std::vector<AnimSampler> samplers;
  std::vector<AnimChannel> channels;
};

struct Model {
  std::vector<Image> images;        // texture index == image index (samplers ignored for now)
  std::vector<Material> materials;
  std::vector<Mesh> meshes;
  std::vector<Node> nodes;
  std::vector<int> roots;
  std::vector<Animation> animations;
  vec3 boundsMin, boundsMax;        // in model space, all nodes applied

  void computeBounds();
};

// Node-tree evaluation shared by the CPU model and GpuModel (both keep `nodes`/`roots`).
// `local` starts as every node's rest `local`; scrubbing a clip at `t` seconds (clamped to the
// clip) rebuilds the matrices of the nodes it targets; `computeWorld` chains parents.
void restPose(const std::vector<Node>& nodes, std::vector<mat4>& local);
void scrubAnimation(const std::vector<Node>& nodes, const Animation& a, float t, std::vector<mat4>& local);
void computeWorld(const std::vector<Node>& nodes, const std::vector<int>& roots, const std::vector<mat4>& local, std::vector<mat4>& world);

} // namespace eng

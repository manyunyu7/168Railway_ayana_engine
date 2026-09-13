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

struct Image {
  std::string mime;                 // "image/png" etc. when still encoded
  std::vector<uint8_t> encoded;     // raw file bytes (GLB) — empty once decoded
  int width = 0, height = 0, channels = 0;
  std::vector<uint8_t> pixels;      // decoded RGBA8 (engine format)
  uint8_t wrapS = 0, wrapT = 0;     // glTF sampler wrap of the textures using this image: 0 repeat, 1 clamp, 2 mirror
  bool linear = false;              // sampled as data (metal/rough, normal, occlusion): upload without sRGB decode
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
  std::map<std::string, std::string> extras;   // scalar glTF node extras (numbers/bools/strings as text)
  float extraNumber(const std::string& key, float d) const {
    auto it = extras.find(key); if (it == extras.end()) return d;
    char* e = nullptr; float v = std::strtof(it->second.c_str(), &e); return e && *e == 0 ? v : d;
  }
};

struct Model {
  std::vector<Image> images;        // texture index == image index (samplers ignored for now)
  std::vector<Material> materials;
  std::vector<Mesh> meshes;
  std::vector<Node> nodes;
  std::vector<int> roots;
  vec3 boundsMin, boundsMax;        // in model space, all nodes applied

  void computeBounds();
};

} // namespace eng

// Uploads a Model to the GPU and draws it with the PBR shader.
#pragma once
#include "engine/asset/model.h"
#include "engine/rhi/rhi.h"
#include <vector>

namespace eng {

struct Lighting {
  vec3 sunDir = normalize(vec3{0.4f, 0.8f, 0.5f});
  vec3 sunColor{3.0f, 2.9f, 2.7f};
  vec3 skyColor{0.35f, 0.45f, 0.65f};
  vec3 groundColor{0.18f, 0.16f, 0.13f};
};

struct GpuPrimitive { rhi::Mesh mesh; int material; };
struct GpuMesh { std::vector<GpuPrimitive> primitives; };

struct GpuModel {
  std::vector<rhi::Texture> textures;
  std::vector<GpuMesh> meshes;
  std::vector<Material> materials;
  std::vector<Node> nodes;
  std::vector<int> roots;
  std::vector<mat4> world;          // per node, computed at upload (static models)
  vec3 boundsMin, boundsMax;

  void upload(const Model& m);      // CPU model may be discarded afterwards
  void destroy();
};

class ModelRenderer {
public:
  void init();
  void shutdown();
  void beginFrame(const mat4& viewProj, vec3 eye, const Lighting& light);
  void draw(const GpuModel& model, const mat4& transform = mat4::identity());
private:
  struct DrawItem { const GpuModel* model; const GpuPrimitive* prim; mat4 world; float depth; };
  void drawItem(const DrawItem& d);
  rhi::Program prog_;
  rhi::Texture white_;   // 1x1 fallback so every sampler unit has a texture
  struct { int viewProj, model, eye, sunDir, sunColor, skyColor, groundColor, baseColor, emissive, metallic, roughness,
           alphaCutoff, hasBase, hasMR, hasEmissive, alphaMode; } u_{};
  std::vector<DrawItem> transparent_;
  vec3 eye_;
};

} // namespace eng

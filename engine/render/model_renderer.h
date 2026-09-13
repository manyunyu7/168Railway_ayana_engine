// Uploads a Model to the GPU and draws it with the PBR shader.
#pragma once
#include "engine/asset/model.h"
#include "engine/math/geometry.h"
#include "engine/rhi/rhi.h"
#include <vector>

namespace eng {

struct Lighting {
  vec3 sunDir = normalize(vec3{0.4f, 0.8f, 0.5f});
  vec3 sunColor{3.0f, 2.9f, 2.7f};
  vec3 skyColor{0.35f, 0.45f, 0.65f};
  vec3 groundColor{0.18f, 0.16f, 0.13f};
  vec3 fogColor{0.55f, 0.65f, 0.8f};   // should match the sky/clear color
  float fogDensity = 0;               // ~1/visibility_m
};

struct GpuPrimitive { rhi::Mesh mesh; int material; AABB bounds; };
struct GpuMesh { std::vector<GpuPrimitive> primitives; };

struct GpuModel {
  std::vector<rhi::Texture> textures;
  std::vector<GpuMesh> meshes;
  std::vector<Material> materials;
  std::vector<Node> nodes;
  std::vector<int> roots;
  std::vector<mat4> world;          // per node, computed at upload (static models)
  AABB bounds;

  void upload(const Model& m);      // CPU model may be discarded afterwards
  void destroy();
};

class ModelRenderer {
public:
  void init();
  void shutdown();
  void beginFrame(const mat4& viewProj, vec3 eye, const Lighting& light);
  // frustum: optional per-primitive culling (world-space bounds computed on the fly)
  void draw(const GpuModel& model, const mat4& transform = mat4::identity(), const Frustum* frustum = nullptr);
  // Procedural geometry: one mesh, one material, optional single base-color texture.
  void drawMesh(const rhi::Mesh& mesh, const Material& mat, rhi::Texture baseTex, const mat4& transform);
  void flushTransparent();   // call after all draws of the frame
  unsigned drawCalls = 0, culled = 0;   // per-frame stats (reset in beginFrame)
private:
  struct DrawItem { const rhi::Mesh* mesh; const Material* material; const std::vector<rhi::Texture>* textures; rhi::Texture baseTex; mat4 world; float depth; };
  void drawItem(const DrawItem& d);
  void submit(DrawItem d);
  rhi::Program prog_;
  rhi::Texture white_;   // 1x1 fallback so every sampler unit has a texture
  struct { int viewProj, model, eye, sunDir, sunColor, skyColor, groundColor, baseColor, emissive, metallic, roughness,
           alphaCutoff, hasBase, hasMR, hasEmissive, alphaMode, fogColor, fogDensity; } u_{};
  std::vector<DrawItem> transparent_;
  vec3 eye_;
};

} // namespace eng

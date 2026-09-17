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

// Uploads an Image: the first variant the GPU supports (compressed chains as-is, RGBA8 with generated mips),
// or `pixels` when the image has no variants; a placeholder (v6, textures streamed separately) becomes a 1x1 white.
// Returns id 0 when nothing is usable (logged).
rhi::Texture uploadImage(const Image& im);

// Vertex layout of a skinned primitive (52 B): the plain Vertex plus the glTF influences. Built at upload
// from Primitive::vertices + Primitive::skin; the CPU model keeps the two arrays apart.
struct SkinnedVertex { vec3 pos; vec3 normal; vec2 uv; uint8_t joints[4]; float weights[4]; };

// collisionPos/Idx: a CPU copy of the geometry (positions only) kept when uploaded with keepGeometry — the walk
// collider (engine/world/walk_collision.h) reads scenery models from it; vehicles do not pay for it.
struct GpuPrimitive { rhi::Mesh mesh; int material; AABB bounds; std::vector<vec3> collisionPos; std::vector<uint32_t> collisionIdx; bool skinned = false; };
struct GpuMesh { std::vector<GpuPrimitive> primitives; };

struct GpuModel {
  std::vector<rhi::Texture> textures;   // acquired through the texture cache (engine/render/texture_cache.h): byte-identical
                                        // atlases in different models are one GPU texture; replace entries via releaseTexture
  std::vector<GpuMesh> meshes;
  std::vector<Material> materials;
  std::vector<Node> nodes;
  std::vector<int> roots;
  std::vector<Animation> animations;   // clips kept for node-pose players (see scrubAnimation, engine/render/animator.h)
  std::vector<Skin> skins;             // joint lists + inverse bind matrices (jointPalette -> drawSkinned)
  std::vector<mat4> world;          // per node, computed at upload (static models)
  AABB bounds;
  // Streamed textures (v6 placeholders, web): per image 1 while the host still owes the texture; the model is
  // "textured-complete" once every placeholder answered (eng_texture_end) or the host declared that no KTX2 twin
  // exists (eng_model_textures_unavailable: placeholders become neutral grey). Consumers draw a fallback / skip
  // the model until textured() — a half-white vehicle or station is never shown.
  std::vector<uint8_t> texturePending;
  int texturesPending = 0;
  bool texturesUnavailable = false;
  bool textured() const { return texturesPending == 0; }

  void upload(const Model& m, bool keepGeometry = false);   // CPU model may be discarded afterwards
  void destroy();
};

class ModelRenderer {
public:
  void init();
  void shutdown();
  void beginFrame(const mat4& viewProj, vec3 eye, const Lighting& light);
  // frustum: optional per-primitive culling (world-space bounds computed on the fly).
  // worldOverride: per-node matrices replacing model.world (animated poses from computeWorld); same size.
  // materialOverride: one material for every primitive, textures ignored (editor ghosts).
  void draw(const GpuModel& model, const mat4& transform = mat4::identity(), const Frustum* frustum = nullptr,
            const std::vector<mat4>* worldOverride = nullptr, const Material* materialOverride = nullptr);
  // Procedural geometry: one mesh, one material, optional single base-color texture.
  void drawMesh(const rhi::Mesh& mesh, const Material& mat, rhi::Texture baseTex, const mat4& transform);
  // Skinned draw: every skinned primitive of the model, deformed on the GPU by `palette`
  // (engine/render/animator.h: jointPalette / AnimationPlayer::palette), at most MAX_JOINTS = 64 entries.
  // Non-skinned primitives of the same model are drawn as usual (a rig may carry static props).
  // Frustum culling uses the rest-pose bounds grown by `boundsPad` metres — a posed skeleton leaves them.
  void drawSkinned(const GpuModel& model, const mat4& transform, const std::vector<mat4>& palette,
                   const Frustum* frustum = nullptr, float boundsPad = 1.0f);
  // Instanced: draws `count` copies, world = transform * node * instance, the instance matrices read from
  // `instances` (mat4 per copy, rhi::createDynamicBuffer). The buffer is attached to every primitive at draw
  // time, so several users may instance the same model with their own buffers (trees, garis, hiasan).
  // `sortPoint`: where blended primitives sort against the other transparent draws (a representative instance).
  void drawInstanced(const GpuModel& model, const mat4& transform, uint32_t count, rhi::Buffer instances, vec3 sortPoint = {});
  // Satu node pustaka; matriks instansi sudah memuat pose dunia × pose node.
  void drawNodeInstanced(const GpuModel& model, int node, uint32_t count, rhi::Buffer instances);
  void flushTransparent();   // call after all draws of the frame
  unsigned drawCalls = 0, culled = 0;   // per-frame stats (reset in beginFrame)
  vec3 eye() const { return eye_; }     // camera position given to beginFrame
private:
  struct DrawItem { const rhi::Mesh* mesh; const Material* material; const std::vector<rhi::Texture>* textures; rhi::Texture baseTex; mat4 world; float depth; uint32_t instances = 0; const std::vector<mat4>* palette = nullptr; rhi::Buffer instanceBuf{}; vec3 sortPoint{}; };
  void drawItem(const DrawItem& d);
  void submit(DrawItem d);
  struct Uniforms { int viewProj, model, eye, sunDir, sunColor, skyColor, groundColor, baseColor, emissive, metallic, roughness,
                    alphaCutoff, hasBase, hasMR, hasEmissive, hasNormal, hasOcclusion, alphaMode, fogColor, fogDensity, unlit, instanced, joints; };
  void initProgram(rhi::Program& prog, Uniforms& u, bool skinned);
  void setFrameUniforms(rhi::Program prog, const Uniforms& u, const mat4& viewProj, vec3 eye, const Lighting& l);
  rhi::Program prog_, progSkin_;   // the same PBR shader, with and without SKINNING_DEFINE
  rhi::Texture white_;   // 1x1 fallback so every sampler unit has a texture
  Uniforms u_{}, uSkin_{};
  std::vector<DrawItem> transparent_;
  vec3 eye_;
};

} // namespace eng

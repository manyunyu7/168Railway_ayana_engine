#include "engine/render/model_renderer.h"
#include "engine/render/animator.h"
#include "engine/render/pbr_shader.h"
#include "engine/render/texture_cache.h"
#include <algorithm>
#include <string>
#include <cstdio>

namespace eng {

rhi::Texture uploadImage(const Image& im) {
  // colour images are sRGB; metal/rough, normal and occlusion maps are data
  auto wrapS = (rhi::Wrap)im.wrapS, wrapT = (rhi::Wrap)im.wrapT;
  if (im.placeholder()) {   // texture streamed later by the host: 1x1 white until it arrives
    const uint8_t px[4] = {255, 255, 255, 255};
    return rhi::createTexture(1, 1, rhi::Format::RGBA8, std::as_bytes(std::span(px)), false, false, wrapS, wrapT);
  }
  if (im.variants.empty())
    return rhi::createTexture(im.width, im.height, rhi::Format::RGBA8, std::as_bytes(std::span(im.pixels)), true, !im.linear, wrapS, wrapT);
  static const rhi::Format map[] = {rhi::Format::RGBA8, rhi::Format::ETC2_RGB, rhi::Format::ETC2_RGBA, rhi::Format::BC1, rhi::Format::BC3, rhi::Format::BC7};
  for (const ImageVariant& v : im.variants) {
    rhi::Format f = map[(int)v.format];
    if (v.mips.empty() || !rhi::supports(f)) continue;
    if (v.format == TexFormat::RGBA8) {
      const MipLevel& l = v.mips[0];
      return rhi::createTexture(l.width, l.height, rhi::Format::RGBA8, std::as_bytes(std::span(l.data)), true, !im.linear, wrapS, wrapT);
    }
    std::vector<rhi::MipData> mips;
    for (const MipLevel& l : v.mips) mips.push_back({l.width, l.height, std::as_bytes(std::span(l.data))});
    return rhi::createTextureCompressed(f, mips, !im.linear, wrapS, wrapT);
  }
  std::fprintf(stderr, "[model] no usable texture variant for a %dx%d image (formats: ", im.width, im.height);
  for (const ImageVariant& v : im.variants) std::fprintf(stderr, "%d ", (int)v.format);
  std::fprintf(stderr, ")\n");
  return {};
}

void GpuModel::upload(const Model& m, bool keepGeometry) {
  for (const Image& im : m.images) {
    textures.push_back(acquireTexture(im));
    texturePending.push_back(im.placeholder() ? 1 : 0);
    if (im.placeholder()) ++texturesPending;
  }
  const rhi::Attribute layout[] = {{0, 3, sizeof(Vertex), 0}, {1, 3, sizeof(Vertex), 12}, {2, 2, sizeof(Vertex), 24}};
  const rhi::Attribute skinLayout[] = {{0, 3, sizeof(SkinnedVertex), 0}, {1, 3, sizeof(SkinnedVertex), 12}, {2, 2, sizeof(SkinnedVertex), 24},
                                       {rhi::ATTR_JOINTS, 4, sizeof(SkinnedVertex), 32, false, rhi::AttrType::U8},
                                       {rhi::ATTR_WEIGHTS, 4, sizeof(SkinnedVertex), 36}};
  std::vector<SkinnedVertex> sv;
  for (const Mesh& me : m.meshes) {
    GpuMesh gm;
    for (const Primitive& p : me.primitives) {
      bool skinned = p.skin.size() == p.vertices.size() && !p.skin.empty();
      rhi::Mesh mesh;
      if (skinned) {
        sv.resize(p.vertices.size());
        for (size_t i = 0; i < p.vertices.size(); ++i) {
          sv[i] = {p.vertices[i].pos, p.vertices[i].normal, p.vertices[i].uv, {}, {}};
          for (int c = 0; c < 4; ++c) { sv[i].joints[c] = p.skin[i].joints[c]; sv[i].weights[c] = p.skin[i].weights[c]; }
        }
        mesh = rhi::createMesh(std::as_bytes(std::span(sv)), skinLayout, p.indices);
      } else mesh = rhi::createMesh(std::as_bytes(std::span(p.vertices)), layout, p.indices);
      GpuPrimitive gp{mesh, p.material, AABB{p.boundsMin, p.boundsMax}, {}, {}, skinned};
      if (keepGeometry) {
        gp.collisionPos.reserve(p.vertices.size());
        for (const Vertex& v : p.vertices) gp.collisionPos.push_back(v.pos);
        gp.collisionIdx = p.indices;
      }
      gm.primitives.push_back(std::move(gp));
    }
    meshes.push_back(std::move(gm));
  }
  materials = m.materials; nodes = m.nodes; roots = m.roots; animations = m.animations; skins = m.skins;
  bounds = {m.boundsMin, m.boundsMax};
  std::vector<mat4> local; restPose(nodes, local); computeWorld(nodes, roots, local, world);
}

void GpuModel::destroy() {
  for (auto& t : textures) releaseTexture(t);
  for (auto& me : meshes) for (auto& p : me.primitives) rhi::destroyMesh(p.mesh);
  *this = {};
}

void ModelRenderer::initProgram(rhi::Program& prog, Uniforms& u, bool skinned) {
  std::string vs = skinned ? std::string(shaders::SKINNING_DEFINE) + shaders::PBR_VS : shaders::PBR_VS;
  prog = rhi::createProgram(vs, shaders::PBR_FS);
  auto L = [&](const char* n) { return rhi::uniformLocation(prog, n); };
  u = {L("uViewProj"), L("uModel"), L("uEye"), L("uSunDir"), L("uSunColor"), L("uSkyColor"), L("uGroundColor"),
       L("uBaseColor"), L("uEmissive"), L("uMetallic"), L("uRoughness"), L("uAlphaCutoff"),
       L("uHasBaseTex"), L("uHasMRTex"), L("uHasEmissiveTex"), L("uHasNormalTex"), L("uHasOcclusionTex"), L("uAlphaMode"), L("uFogColor"), L("uFogDensity"), L("uUnlit"), L("uInstanced"),
       skinned ? L("uJoints") : -1};
  rhi::useProgram(prog);
  rhi::setUniform(L("uBaseTex"), 0); rhi::setUniform(L("uMRTex"), 1); rhi::setUniform(L("uEmissiveTex"), 2);
  rhi::setUniform(L("uNormalTex"), 3); rhi::setUniform(L("uOcclusionTex"), 4);
}

void ModelRenderer::init() {
  initProgram(prog_, u_, false);
  initProgram(progSkin_, uSkin_, true);
  rhi::useProgram(prog_);
  const uint8_t px[4] = {255, 255, 255, 255};
  white_ = rhi::createTexture(1, 1, rhi::Format::RGBA8, std::as_bytes(std::span(px)), false);
}
void ModelRenderer::shutdown() { rhi::destroyProgram(prog_); rhi::destroyProgram(progSkin_); rhi::destroyTexture(white_); }

void ModelRenderer::flushTransparent() {
  if (transparent_.empty()) return;
  std::sort(transparent_.begin(), transparent_.end(), [](const DrawItem& a, const DrawItem& b) { return a.depth > b.depth; });
  // Blend items keep writing depth (as three.js does): most BLEND materials in our assets are cut-out
  // textures (bogie spokes, decals) that would otherwise show everything behind them through their
  // opaque parts; fully transparent texels are discarded by the shader so holes stay holes. Additive
  // glows never write depth.
  rhi::setBlend(true);
  bool additive = false;
  for (const DrawItem& d : transparent_) {
    if (d.material->additive != additive) { additive = d.material->additive; rhi::setBlendAdditive(additive); rhi::setDepthWrite(!additive); }
    drawItem(d);
  }
  if (additive) rhi::setBlendAdditive(false);
  rhi::setBlend(false); rhi::setDepthWrite(true);
  transparent_.clear();
}

void ModelRenderer::setFrameUniforms(rhi::Program prog, const Uniforms& u, const mat4& viewProj, vec3 eye, const Lighting& l) {
  rhi::useProgram(prog);
  rhi::setUniform(u.viewProj, viewProj.data());
  rhi::setUniform(u.eye, eye.x, eye.y, eye.z);
  rhi::setUniform(u.sunDir, l.sunDir.x, l.sunDir.y, l.sunDir.z);
  rhi::setUniform(u.sunColor, l.sunColor.x, l.sunColor.y, l.sunColor.z);
  rhi::setUniform(u.skyColor, l.skyColor.x, l.skyColor.y, l.skyColor.z);
  rhi::setUniform(u.groundColor, l.groundColor.x, l.groundColor.y, l.groundColor.z);
  rhi::setUniform(u.fogColor, l.fogColor.x, l.fogColor.y, l.fogColor.z);
  rhi::setUniform(u.fogDensity, l.fogDensity);
}

void ModelRenderer::beginFrame(const mat4& viewProj, vec3 eye, const Lighting& l) {
  transparent_.clear(); drawCalls = 0; culled = 0;
  eye_ = eye;
  setFrameUniforms(progSkin_, uSkin_, viewProj, eye, l);
  setFrameUniforms(prog_, u_, viewProj, eye, l);
}

void ModelRenderer::drawItem(const DrawItem& d) {
  const bool skinned = d.palette != nullptr;
  const Uniforms& u = skinned ? uSkin_ : u_;
  rhi::useProgram(skinned ? progSkin_ : prog_);
  if (skinned) {
    int n = (int)std::min(d.palette->size(), (size_t)MAX_JOINTS);
    if (n > 0) rhi::setUniformMat4Array(u.joints, d.palette->front().data(), n);
  }
  const Material& mt = *d.material;
  rhi::setUniform(u.model, d.world.data());
  rhi::setUniform(u.baseColor, mt.baseColor.x, mt.baseColor.y, mt.baseColor.z, mt.baseColor.w);
  rhi::setUniform(u.emissive, mt.emissive.x, mt.emissive.y, mt.emissive.z);
  rhi::setUniform(u.metallic, mt.metallic);
  rhi::setUniform(u.roughness, mt.roughness);
  rhi::setUniform(u.alphaCutoff, mt.alphaCutoff);
  rhi::setUniform(u.alphaMode, (int)mt.alphaMode);
  rhi::setUniform(u.unlit, mt.unlit ? 1 : 0);
  auto bind = [&](int slot, int tex, int flagLoc) {
    bool has = d.textures && tex >= 0 && tex < (int)d.textures->size();
    rhi::setUniform(flagLoc, has ? 1 : 0);
    rhi::bindTexture(slot, has ? (*d.textures)[(size_t)tex] : white_);
  };
  if (d.textures) {
    bind(0, mt.baseColorTex, u.hasBase); bind(1, mt.metalRoughTex, u.hasMR); bind(2, mt.emissiveTex, u.hasEmissive);
    bind(3, mt.normalTex, u.hasNormal); bind(4, mt.occlusionTex, u.hasOcclusion);
  } else {
    bool has = d.baseTex.id != 0;
    rhi::setUniform(u.hasBase, has ? 1 : 0); rhi::bindTexture(0, has ? d.baseTex : white_);
    rhi::setUniform(u.hasMR, 0); rhi::bindTexture(1, white_);
    rhi::setUniform(u.hasEmissive, 0); rhi::bindTexture(2, white_);
    rhi::setUniform(u.hasNormal, 0); rhi::bindTexture(3, white_);
    rhi::setUniform(u.hasOcclusion, 0); rhi::bindTexture(4, white_);
  }
  rhi::setCullFace(!mt.doubleSided);
  // mirrored transforms (negative determinant) reverse the triangle winding
  const auto& w = d.world.m;
  float det = w[0][0] * (w[1][1] * w[2][2] - w[1][2] * w[2][1]) - w[1][0] * (w[0][1] * w[2][2] - w[0][2] * w[2][1]) + w[2][0] * (w[0][1] * w[1][2] - w[0][2] * w[1][1]);
  rhi::setFrontFaceCCW(det >= 0);
  rhi::setUniform(u.instanced, d.instances ? 1 : 0);
  if (d.instances) { rhi::attachInstances(*d.mesh, d.instanceBuf); rhi::drawMeshInstanced(*d.mesh, d.instances); }
  else rhi::drawMesh(*d.mesh);
  ++drawCalls;
}

void ModelRenderer::submit(DrawItem d) {
  if (d.material->alphaMode == AlphaMode::Blend) {
    d.depth = length((d.instances ? d.sortPoint : d.world.transformPoint({})) - eye_);
    transparent_.push_back(d);
  } else drawItem(d);
}

void ModelRenderer::draw(const GpuModel& model, const mat4& transform, const Frustum* frustum, const std::vector<mat4>* worldOverride, const Material* materialOverride) {
  static const Material DEFAULT;
  const std::vector<mat4>& world = worldOverride && worldOverride->size() == model.world.size() ? *worldOverride : model.world;
  for (size_t n = 0; n < model.nodes.size(); ++n) {
    int mi = model.nodes[n].mesh; if (mi < 0) continue;
    mat4 w = transform * world[n];
    for (const GpuPrimitive& p : model.meshes[(size_t)mi].primitives) {
      if (frustum && !frustum->contains(p.bounds.transformed(w))) { ++culled; continue; }
      const Material& mt = materialOverride ? *materialOverride : p.material >= 0 ? model.materials[(size_t)p.material] : DEFAULT;
      submit({&p.mesh, &mt, materialOverride ? nullptr : &model.textures, {}, w, 0});
    }
  }
}

void ModelRenderer::drawInstanced(const GpuModel& model, const mat4& transform, uint32_t count, rhi::Buffer instances, vec3 sortPoint) {
  static const Material DEFAULT;
  if (!count || !instances.id) return;
  for (size_t n = 0; n < model.nodes.size(); ++n) {
    int mi = model.nodes[n].mesh; if (mi < 0) continue;
    mat4 w = transform * model.world[n];
    for (const GpuPrimitive& p : model.meshes[(size_t)mi].primitives) {
      const Material& mt = p.material >= 0 ? model.materials[(size_t)p.material] : DEFAULT;
      submit({&p.mesh, &mt, &model.textures, {}, w, 0, count, nullptr, instances, sortPoint});
    }
  }
}

void ModelRenderer::drawMesh(const rhi::Mesh& mesh, const Material& mat, rhi::Texture baseTex, const mat4& transform) {
  submit({&mesh, &mat, nullptr, baseTex, transform, 0});
}

void ModelRenderer::drawSkinned(const GpuModel& model, const mat4& transform, const std::vector<mat4>& palette,
                                const Frustum* frustum, float boundsPad) {
  static const Material DEFAULT;
  if (palette.size() > (size_t)MAX_JOINTS)
    std::fprintf(stderr, "[model] skin with %zu joints, only %d are sent to the GPU\n", palette.size(), MAX_JOINTS);
  for (size_t n = 0; n < model.nodes.size(); ++n) {
    int mi = model.nodes[n].mesh; if (mi < 0) continue;
    // a skinned mesh node's own transform is ignored (the palette already places the vertices)
    bool nodeSkinned = model.nodes[n].skin >= 0;
    mat4 w = nodeSkinned ? transform : transform * model.world[n];
    for (const GpuPrimitive& p : model.meshes[(size_t)mi].primitives) {
      AABB b = p.bounds.transformed(w);
      if (p.skinned) { vec3 pad{boundsPad, boundsPad, boundsPad}; b = {b.min - pad, b.max + pad}; }
      if (frustum && !frustum->contains(b)) { ++culled; continue; }
      const Material& mt = p.material >= 0 ? model.materials[(size_t)p.material] : DEFAULT;
      submit({&p.mesh, &mt, &model.textures, {}, w, 0, 0, p.skinned ? &palette : nullptr});
    }
  }
}

} // namespace eng

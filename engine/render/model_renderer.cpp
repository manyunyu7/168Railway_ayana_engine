#include "engine/render/model_renderer.h"
#include "engine/render/pbr_shader.h"
#include <algorithm>

namespace eng {

void GpuModel::upload(const Model& m) {
  for (const Image& im : m.images)
    textures.push_back(rhi::createTexture(im.width, im.height, rhi::Format::RGBA8,
                                          std::as_bytes(std::span(im.pixels)), true, true));
  const rhi::Attribute layout[] = {{0, 3, sizeof(Vertex), 0}, {1, 3, sizeof(Vertex), 12}, {2, 2, sizeof(Vertex), 24}};
  for (const Mesh& me : m.meshes) {
    GpuMesh gm;
    for (const Primitive& p : me.primitives)
      gm.primitives.push_back({rhi::createMesh(std::as_bytes(std::span(p.vertices)), layout, p.indices), p.material, AABB{p.boundsMin, p.boundsMax}});
    meshes.push_back(std::move(gm));
  }
  materials = m.materials; nodes = m.nodes; roots = m.roots;
  bounds = {m.boundsMin, m.boundsMax};
  world.assign(nodes.size(), mat4::identity());
  auto visit = [&](auto&& self, int n, const mat4& parent) -> void {
    world[(size_t)n] = parent * nodes[(size_t)n].local;
    for (int c : nodes[(size_t)n].children) self(self, c, world[(size_t)n]);
  };
  for (int r : roots) visit(visit, r, mat4::identity());
}

void GpuModel::destroy() {
  for (auto& t : textures) rhi::destroyTexture(t);
  for (auto& me : meshes) for (auto& p : me.primitives) rhi::destroyMesh(p.mesh);
  *this = {};
}

void ModelRenderer::init() {
  prog_ = rhi::createProgram(shaders::PBR_VS, shaders::PBR_FS);
  auto L = [&](const char* n) { return rhi::uniformLocation(prog_, n); };
  u_ = {L("uViewProj"), L("uModel"), L("uEye"), L("uSunDir"), L("uSunColor"), L("uSkyColor"), L("uGroundColor"),
        L("uBaseColor"), L("uEmissive"), L("uMetallic"), L("uRoughness"), L("uAlphaCutoff"),
        L("uHasBaseTex"), L("uHasMRTex"), L("uHasEmissiveTex"), L("uAlphaMode"), L("uFogColor"), L("uFogDensity"), L("uUnlit"), L("uInstanced")};
  rhi::useProgram(prog_);
  rhi::setUniform(L("uBaseTex"), 0); rhi::setUniform(L("uMRTex"), 1); rhi::setUniform(L("uEmissiveTex"), 2);
  const uint8_t px[4] = {255, 255, 255, 255};
  white_ = rhi::createTexture(1, 1, rhi::Format::RGBA8, std::as_bytes(std::span(px)), false);
}
void ModelRenderer::shutdown() { rhi::destroyProgram(prog_); rhi::destroyTexture(white_); }

void ModelRenderer::flushTransparent() {
  if (transparent_.empty()) return;
  std::sort(transparent_.begin(), transparent_.end(), [](const DrawItem& a, const DrawItem& b) { return a.depth > b.depth; });
  rhi::setBlend(true); rhi::setDepthWrite(false);
  for (const DrawItem& d : transparent_) drawItem(d);
  rhi::setBlend(false); rhi::setDepthWrite(true);
  transparent_.clear();
}

void ModelRenderer::beginFrame(const mat4& viewProj, vec3 eye, const Lighting& l) {
  transparent_.clear(); drawCalls = 0; culled = 0;
  eye_ = eye;
  rhi::useProgram(prog_);
  rhi::setUniform(u_.viewProj, viewProj.data());
  rhi::setUniform(u_.eye, eye.x, eye.y, eye.z);
  rhi::setUniform(u_.sunDir, l.sunDir.x, l.sunDir.y, l.sunDir.z);
  rhi::setUniform(u_.sunColor, l.sunColor.x, l.sunColor.y, l.sunColor.z);
  rhi::setUniform(u_.skyColor, l.skyColor.x, l.skyColor.y, l.skyColor.z);
  rhi::setUniform(u_.groundColor, l.groundColor.x, l.groundColor.y, l.groundColor.z);
  rhi::setUniform(u_.fogColor, l.fogColor.x, l.fogColor.y, l.fogColor.z);
  rhi::setUniform(u_.fogDensity, l.fogDensity);
}

void ModelRenderer::drawItem(const DrawItem& d) {
  const Material& mt = *d.material;
  rhi::setUniform(u_.model, d.world.data());
  rhi::setUniform(u_.baseColor, mt.baseColor.x, mt.baseColor.y, mt.baseColor.z, mt.baseColor.w);
  rhi::setUniform(u_.emissive, mt.emissive.x, mt.emissive.y, mt.emissive.z);
  rhi::setUniform(u_.metallic, mt.metallic);
  rhi::setUniform(u_.roughness, mt.roughness);
  rhi::setUniform(u_.alphaCutoff, mt.alphaCutoff);
  rhi::setUniform(u_.alphaMode, (int)mt.alphaMode);
  rhi::setUniform(u_.unlit, mt.unlit ? 1 : 0);
  auto bind = [&](int slot, int tex, int flagLoc) {
    bool has = d.textures && tex >= 0 && tex < (int)d.textures->size();
    rhi::setUniform(flagLoc, has ? 1 : 0);
    rhi::bindTexture(slot, has ? (*d.textures)[(size_t)tex] : white_);
  };
  if (d.textures) {
    bind(0, mt.baseColorTex, u_.hasBase); bind(1, mt.metalRoughTex, u_.hasMR); bind(2, mt.emissiveTex, u_.hasEmissive);
  } else {
    bool has = d.baseTex.id != 0;
    rhi::setUniform(u_.hasBase, has ? 1 : 0); rhi::bindTexture(0, has ? d.baseTex : white_);
    rhi::setUniform(u_.hasMR, 0); rhi::bindTexture(1, white_);
    rhi::setUniform(u_.hasEmissive, 0); rhi::bindTexture(2, white_);
  }
  rhi::setCullFace(!mt.doubleSided);
  rhi::setUniform(u_.instanced, d.instances ? 1 : 0);
  if (d.instances) rhi::drawMeshInstanced(*d.mesh, d.instances); else rhi::drawMesh(*d.mesh);
  ++drawCalls;
}

void ModelRenderer::submit(DrawItem d) {
  if (d.material->alphaMode == AlphaMode::Blend) {
    d.depth = length(d.world.transformPoint({}) - eye_);
    transparent_.push_back(d);
  } else drawItem(d);
}

void ModelRenderer::draw(const GpuModel& model, const mat4& transform, const Frustum* frustum) {
  static const Material DEFAULT;
  for (size_t n = 0; n < model.nodes.size(); ++n) {
    int mi = model.nodes[n].mesh; if (mi < 0) continue;
    mat4 w = transform * model.world[n];
    for (const GpuPrimitive& p : model.meshes[(size_t)mi].primitives) {
      if (frustum && !frustum->contains(p.bounds.transformed(w))) { ++culled; continue; }
      const Material& mt = p.material >= 0 ? model.materials[(size_t)p.material] : DEFAULT;
      submit({&p.mesh, &mt, &model.textures, {}, w, 0});
    }
  }
}

void ModelRenderer::drawInstanced(const GpuModel& model, const mat4& transform, uint32_t count) {
  static const Material DEFAULT;
  if (!count) return;
  for (size_t n = 0; n < model.nodes.size(); ++n) {
    int mi = model.nodes[n].mesh; if (mi < 0) continue;
    mat4 w = transform * model.world[n];
    for (const GpuPrimitive& p : model.meshes[(size_t)mi].primitives) {
      const Material& mt = p.material >= 0 ? model.materials[(size_t)p.material] : DEFAULT;
      submit({&p.mesh, &mt, &model.textures, {}, w, 0, count});
    }
  }
}

void ModelRenderer::drawMesh(const rhi::Mesh& mesh, const Material& mat, rhi::Texture baseTex, const mat4& transform) {
  submit({&mesh, &mat, nullptr, baseTex, transform, 0});
}

} // namespace eng

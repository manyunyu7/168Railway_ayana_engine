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
      gm.primitives.push_back({rhi::createMesh(std::as_bytes(std::span(p.vertices)), layout, p.indices), p.material});
    meshes.push_back(std::move(gm));
  }
  materials = m.materials; nodes = m.nodes; roots = m.roots;
  boundsMin = m.boundsMin; boundsMax = m.boundsMax;
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
        L("uHasBaseTex"), L("uHasMRTex"), L("uHasEmissiveTex"), L("uAlphaMode")};
  rhi::useProgram(prog_);
  rhi::setUniform(L("uBaseTex"), 0); rhi::setUniform(L("uMRTex"), 1); rhi::setUniform(L("uEmissiveTex"), 2);
  const uint8_t px[4] = {255, 255, 255, 255};
  white_ = rhi::createTexture(1, 1, rhi::Format::RGBA8, std::as_bytes(std::span(px)), false);
}
void ModelRenderer::shutdown() { rhi::destroyProgram(prog_); rhi::destroyTexture(white_); }

void ModelRenderer::beginFrame(const mat4& viewProj, vec3 eye, const Lighting& l) {
  // flush transparent items from the previous frame first (drawn back-to-front)
  if (!transparent_.empty()) {
    std::sort(transparent_.begin(), transparent_.end(), [](const DrawItem& a, const DrawItem& b) { return a.depth > b.depth; });
    rhi::setBlend(true); rhi::setDepthWrite(false);
    for (const DrawItem& d : transparent_) drawItem(d);
    rhi::setBlend(false); rhi::setDepthWrite(true);
    transparent_.clear();
  }
  eye_ = eye;
  rhi::useProgram(prog_);
  rhi::setUniform(u_.viewProj, viewProj.data());
  rhi::setUniform(u_.eye, eye.x, eye.y, eye.z);
  rhi::setUniform(u_.sunDir, l.sunDir.x, l.sunDir.y, l.sunDir.z);
  rhi::setUniform(u_.sunColor, l.sunColor.x, l.sunColor.y, l.sunColor.z);
  rhi::setUniform(u_.skyColor, l.skyColor.x, l.skyColor.y, l.skyColor.z);
  rhi::setUniform(u_.groundColor, l.groundColor.x, l.groundColor.y, l.groundColor.z);
}

void ModelRenderer::drawItem(const DrawItem& d) {
  static const Material DEFAULT;
  const Material& mt = d.prim->material >= 0 ? d.model->materials[(size_t)d.prim->material] : DEFAULT;
  rhi::setUniform(u_.model, d.world.data());
  rhi::setUniform(u_.baseColor, mt.baseColor.x, mt.baseColor.y, mt.baseColor.z, mt.baseColor.w);
  rhi::setUniform(u_.emissive, mt.emissive.x, mt.emissive.y, mt.emissive.z);
  rhi::setUniform(u_.metallic, mt.metallic);
  rhi::setUniform(u_.roughness, mt.roughness);
  rhi::setUniform(u_.alphaCutoff, mt.alphaCutoff);
  rhi::setUniform(u_.alphaMode, (int)mt.alphaMode);
  auto bind = [&](int slot, int tex, int flagLoc) {
    bool has = tex >= 0 && tex < (int)d.model->textures.size();
    rhi::setUniform(flagLoc, has ? 1 : 0);
    rhi::bindTexture(slot, has ? d.model->textures[(size_t)tex] : white_);
  };
  bind(0, mt.baseColorTex, u_.hasBase); bind(1, mt.metalRoughTex, u_.hasMR); bind(2, mt.emissiveTex, u_.hasEmissive);
  rhi::setCullFace(!mt.doubleSided);
  rhi::drawMesh(d.prim->mesh);
}

void ModelRenderer::draw(const GpuModel& model, const mat4& transform) {
  for (size_t n = 0; n < model.nodes.size(); ++n) {
    int mi = model.nodes[n].mesh; if (mi < 0) continue;
    mat4 w = transform * model.world[n];
    for (const GpuPrimitive& p : model.meshes[(size_t)mi].primitives) {
      bool blend = p.material >= 0 && model.materials[(size_t)p.material].alphaMode == AlphaMode::Blend;
      DrawItem d{&model, &p, w, 0};
      if (blend) { vec3 c = w.transformPoint({}); d.depth = length(c - eye_); transparent_.push_back(d); }
      else drawItem(d);
    }
  }
}

} // namespace eng

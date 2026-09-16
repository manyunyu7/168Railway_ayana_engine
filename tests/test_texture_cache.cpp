// Texture cache (engine/render/texture_cache.h): byte-identical images acquired twice are one GPU texture,
// different bytes / flags are not, references count down to a single destroy, and GpuModel upload/destroy
// goes through it. Needs a GL context: a hidden GLFW window (SKIP without a display); label `gpu`.
#include "engine/render/texture_cache.h"
#include "engine/render/model_renderer.h"
#include "engine/core/window.h"
#include "tests/check.h"
#include <GLFW/glfw3.h>
#include <cstdio>

using namespace eng;

namespace {
Image solid(int size, uint8_t v, bool linear = false) {
  Image im; im.width = im.height = size; im.channels = 4; im.linear = linear;
  im.pixels.assign((size_t)size * size * 4, v);
  return im;
}
Model cubeWith(const Image& im) {
  Model m; m.images.push_back(im);
  Material mt; mt.baseColorTex = 0; m.materials.push_back(mt);
  Primitive p; p.material = 0;
  p.vertices = {{{0, 0, 0}, {0, 0, 1}, {0, 0}}, {{1, 0, 0}, {0, 0, 1}, {1, 0}}, {{0, 1, 0}, {0, 0, 1}, {0, 1}}};
  p.indices = {0, 1, 2}; p.boundsMin = {0, 0, 0}; p.boundsMax = {1, 1, 0};
  m.meshes.push_back({"tri", {p}});
  Node n; n.mesh = 0; m.nodes.push_back(n); m.roots.push_back(0);
  m.computeBounds();
  return m;
}
}

int main() {
  if (!glfwInit()) { std::printf("SKIP: glfwInit failed (no display)\n"); return test::SKIP; }
  glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
  Window win;
  if (!win.open(64, 64, "test_texture_cache")) { std::printf("SKIP: no GL window\n"); return test::SKIP; }

  TextureCacheStats s0 = textureCacheStats();
  CHECK(s0.entries == 0 && s0.references == 0);

  // same bytes -> same texture; a differing byte, or the same bytes as data instead of colour -> new textures
  Image a = solid(16, 200), b = solid(16, 200), c = solid(16, 201), d = solid(16, 200, true);
  rhi::Texture ta = acquireTexture(a), tb = acquireTexture(b), tc = acquireTexture(c), td = acquireTexture(d);
  CHECK(ta.id != 0 && ta.id == tb.id);
  CHECK(tc.id != 0 && tc.id != ta.id);
  CHECK(td.id != 0 && td.id != ta.id && td.id != tc.id);
  TextureCacheStats s1 = textureCacheStats();
  CHECK(s1.entries == 3 && s1.references == 4);
  CHECK(s1.bytesShared == 16 * 16 * 4 * 4 / 3);

  // the shared texture survives its first release and dies with the second
  releaseTexture(ta);
  CHECK(textureCacheStats().entries == 3 && textureCacheStats().references == 3);
  releaseTexture(tb);
  CHECK(textureCacheStats().entries == 2 && textureCacheStats().references == 2);
  releaseTexture(tc); releaseTexture(td);
  CHECK(textureCacheStats().entries == 0 && textureCacheStats().references == 0);

  // a placeholder is never cached, and releasing a texture the cache never saw just destroys it
  Image ph; ph.width = ph.height = 8;
  rhi::Texture tp = acquireTexture(ph);
  CHECK(tp.id != 0 && textureCacheStats().entries == 0);
  releaseTexture(tp);
  const uint8_t px[4] = {1, 2, 3, 4};
  releaseTexture(rhi::createTexture(1, 1, rhi::Format::RGBA8, std::as_bytes(std::span(px)), false));

  // two GpuModels carrying the same atlas share it; destroying one keeps the other's texture alive
  Model m1 = cubeWith(solid(32, 90)), m2 = cubeWith(solid(32, 90));
  GpuModel g1, g2; g1.upload(m1); g2.upload(m2);
  CHECK(g1.textures.size() == 1 && g2.textures.size() == 1 && g1.textures[0].id == g2.textures[0].id);
  CHECK(textureCacheStats().entries == 1 && textureCacheStats().references == 2);
  g1.destroy();
  CHECK(textureCacheStats().entries == 1 && textureCacheStats().references == 1);
  g2.destroy();
  CHECK(textureCacheStats().entries == 0 && textureCacheStats().references == 0);
  rhi::checkErrors("texture cache");

  std::printf("%s: %d checks, %d failures\n", __FILE__, test::checks, test::failures);
  return test::failures ? 1 : 0;
}

// Instanced drawing equals repeated single draws: the same model placed three times through
// ModelRenderer::draw and once through drawInstanced (instance buffer attached at draw time) must produce
// the same pixels. Also checks that two users instancing one model with their own buffers do not disturb
// each other. Hidden GLFW window, label `gpu`.
#include "engine/render/model_renderer.h"
#include "engine/core/window.h"
#include "tests/check.h"
#include "tests/synth_model.h"
#include <GLFW/glfw3.h>
#include <cstdio>
#include <vector>

using namespace eng;

namespace {
int W = 320, H = 200;   // framebuffer size (the window is opened at 320x200 css px; retina doubles it)

struct Scene {
  ModelRenderer r; GpuModel model; mat4 viewProj; vec3 eye; Lighting light;
  void begin() { rhi::setViewport(W, H); rhi::clear(0.2f, 0.25f, 0.3f, 1); r.beginFrame(viewProj, eye, light); }
  void end() { r.flushTransparent(); }
};
std::vector<uint8_t> grab() { std::vector<uint8_t> px((size_t)W * H * 4); rhi::readPixels(0, 0, W, H, px.data()); return px; }
size_t differing(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
  size_t n = 0;
  for (size_t i = 0; i < a.size(); i += 4)
    for (int c = 0; c < 3; ++c) if (std::abs((int)a[i + c] - (int)b[i + c]) > 2) { ++n; break; }
  return n;
}
size_t nonBackground(const std::vector<uint8_t>& a) {   // pixels that differ from the clear colour (top-left corner)
  size_t n = 0;
  for (size_t i = 0; i < a.size(); i += 4)
    if (std::abs((int)a[i] - (int)a[0]) > 4 || std::abs((int)a[i + 1] - (int)a[1]) > 4 || std::abs((int)a[i + 2] - (int)a[2]) > 4) ++n;
  return n;
}
}

int main() {
  if (!glfwInit()) { std::printf("SKIP: glfwInit failed (no display)\n"); return test::SKIP; }
  glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
  Window win;
  if (!win.open(W, H, "test_instancing")) { std::printf("SKIP: no GL window\n"); return test::SKIP; }
  win.framebufferSize(W, H);

  Scene s; s.r.init(); s.model.upload(synth::bumpyCube());
  s.eye = {6, 5, 9};
  s.viewProj = mat4::perspective(radians(45), (float)W / H, 0.1f, 100) * mat4::lookAt(s.eye, {0, 0, 0}, {0, 1, 0});
  const mat4 places[3] = {mat4::translation({-3, 0, 0}), mat4::translation({0, 0.5f, -2}) * mat4::rotationY(0.7f),
                          mat4::translation({3, -0.5f, 1}) * mat4::scale({0.6f, 0.6f, 0.6f})};

  // reference: three plain draws
  s.begin(); for (const mat4& p : places) s.r.draw(s.model, p); s.end();
  std::vector<uint8_t> ref = grab();
  CHECK(nonBackground(ref) > 2000);   // the cubes are actually on screen
  unsigned singleDraws = s.r.drawCalls;

  // one instanced draw with the same matrices
  std::vector<mat4> mats(places, places + 3);
  rhi::Buffer buf = rhi::createDynamicBuffer(mats.size() * sizeof(mat4));
  rhi::updateBuffer(buf, std::as_bytes(std::span(mats)));
  s.begin(); s.r.drawInstanced(s.model, mat4::identity(), 3, buf); s.end();
  std::vector<uint8_t> inst = grab();
  CHECK(s.r.drawCalls == 1 && singleDraws == 3);
  size_t diff = differing(ref, inst);
  std::printf("instanced vs single: %zu of %d pixels differ\n", diff, W * H);
  CHECK(diff < (size_t)(W * H) / 500);

  // a second buffer on the same model: each draw reads its own instances (attach happens per draw)
  std::vector<mat4> other = {mat4::translation({0, 3, 0}) * mat4::scale({0.5f, 0.5f, 0.5f})};
  rhi::Buffer buf2 = rhi::createDynamicBuffer(other.size() * sizeof(mat4));
  rhi::updateBuffer(buf2, std::as_bytes(std::span(other)));
  s.begin(); s.r.draw(s.model, other[0]); for (const mat4& p : places) s.r.draw(s.model, p); s.end();
  std::vector<uint8_t> ref2 = grab();
  s.begin(); s.r.drawInstanced(s.model, mat4::identity(), 1, buf2); s.r.drawInstanced(s.model, mat4::identity(), 3, buf); s.end();
  std::vector<uint8_t> inst2 = grab();
  std::printf("cube pixels: %zu, with the extra cube %zu\n", nonBackground(ref), nonBackground(ref2));
  CHECK(nonBackground(ref2) > nonBackground(ref));   // the extra cube shows
  diff = differing(ref2, inst2);
  std::printf("two buffers vs singles: %zu pixels differ\n", diff);
  CHECK(diff < (size_t)(W * H) / 500);
  // after the instanced draws, a plain draw of the same mesh is unaffected by the attached instance attributes
  s.begin(); for (const mat4& p : places) s.r.draw(s.model, p); s.end();
  CHECK(differing(ref, grab()) < (size_t)(W * H) / 500);

  rhi::destroyBuffer(buf); rhi::destroyBuffer(buf2);
  s.model.destroy(); s.r.shutdown();
  rhi::checkErrors("instancing");
  std::printf("%s: %d checks, %d failures\n", __FILE__, test::checks, test::failures);
  return test::failures ? 1 : 0;
}

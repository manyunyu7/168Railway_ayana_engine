// Golden image for the normal map + ambient occlusion path of the PBR shader: a grey cube whose material
// carries a synthetic tangent-space normal map (a 4x4 grid of hemispherical bumps) and an occlusion map
// (dark grid lines with a vignette). The model is built here, saved as EMOD next to the build and rendered
// by the viewer with ENG_CAPTURE; compare with tests/golden/normalmap.ppm (mechanics in tests/golden_util.h).
// Without vertex tangents the shader derives the frame from screen-space derivatives, so every face of the
// cube must show the bumps lit from the sun side (top-left of each bump bright on the +Z face).
//   test_normalmap --update   regenerates the golden from the current build.
// Needs a window/GPU: labelled `gpu` in ctest.
#include "tests/golden_util.h"
#include "engine/asset/emod.h"
#include <cmath>
#include <cstring>

using namespace eng;

namespace {
Image makeNormalMap(int size, int cells) {
  Image im; im.width = im.height = size; im.channels = 4; im.linear = true; im.pixels.resize((size_t)size * size * 4);
  const float cell = (float)size / cells, radius = cell * 0.4f;
  for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x) {
    float cx = (std::floor(x / cell) + 0.5f) * cell, cy = (std::floor(y / cell) + 0.5f) * cell;
    float dx = (x + 0.5f - cx) / radius, dy = (cy - y - 0.5f) / radius;   // +Y up in the map = towards smaller v
    float r2 = dx * dx + dy * dy;
    vec3 n = r2 < 1 ? vec3{dx, dy, std::sqrt(1 - r2)} : vec3{0, 0, 1};
    uint8_t* p = &im.pixels[((size_t)y * size + x) * 4];
    p[0] = (uint8_t)std::lround((n.x * 0.5f + 0.5f) * 255); p[1] = (uint8_t)std::lround((n.y * 0.5f + 0.5f) * 255);
    p[2] = (uint8_t)std::lround((n.z * 0.5f + 0.5f) * 255); p[3] = 255;
  }
  return im;
}
Image makeOcclusion(int size, int cells) {
  Image im; im.width = im.height = size; im.channels = 4; im.linear = true; im.pixels.resize((size_t)size * size * 4);
  const float cell = (float)size / cells;
  for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x) {
    float fx = std::fmod((float)x, cell) / cell, fy = std::fmod((float)y, cell) / cell;
    float line = std::min({fx, 1 - fx, fy, 1 - fy}) < 0.06f ? 0.25f : 1.0f;   // dark seams between cells
    float vx = (x + 0.5f) / size - 0.5f, vy = (y + 0.5f) / size - 0.5f;
    float ao = line * (1 - 1.2f * (vx * vx + vy * vy));                        // vignette towards the edges
    uint8_t v = (uint8_t)std::lround(std::fmax(0.f, std::fmin(1.f, ao)) * 255);
    uint8_t* p = &im.pixels[((size_t)y * size + x) * 4]; p[0] = p[1] = p[2] = v; p[3] = 255;
  }
  return im;
}
// Unit cube (±1) with per-face UVs 0..1 and outward normals.
Primitive makeCube() {
  Primitive p; p.material = 0;
  const vec3 axes[6][3] = {{{0, 0, 1}, {1, 0, 0}, {0, 1, 0}}, {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}}, {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}},
                           {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}}, {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}}, {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}}};
  for (const auto& a : axes) {
    vec3 n = a[0], u = a[1], v = a[2];
    uint32_t base = (uint32_t)p.vertices.size();
    for (int i = 0; i < 4; ++i) {
      float su = i == 1 || i == 2 ? 1.f : -1.f, sv = i >= 2 ? 1.f : -1.f;
      p.vertices.push_back({n + u * su + v * sv, n, {su * 0.5f + 0.5f, 0.5f - sv * 0.5f}});   // v grows downwards (glTF)
    }
    for (uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) p.indices.push_back(base + i);
  }
  p.boundsMin = {-1, -1, -1}; p.boundsMax = {1, 1, 1};
  return p;
}
}

int main(int argc, char** argv) {
  bool update = argc > 1 && std::strcmp(argv[1], "--update") == 0;
  const std::string src = ENG_SOURCE_DIR, bin = ENG_BINARY_DIR;

  Model m;
  m.images.push_back(makeNormalMap(256, 4));
  m.images.push_back(makeOcclusion(256, 4));
  Material mt; mt.name = "bumpy"; mt.baseColor = {0.8f, 0.8f, 0.8f, 1}; mt.metallic = 0; mt.roughness = 0.6f;
  mt.normalTex = 0; mt.occlusionTex = 1;
  m.materials.push_back(mt);
  m.meshes.push_back({"cube", {makeCube()}});
  Node n; n.name = "cube"; n.mesh = 0; m.nodes.push_back(n); m.roots.push_back(0);
  m.computeBounds();
  const std::string emod = bin + "/normalmap.emod";
  std::string err; CHECK_MSG(saveEmod(m, emod, err), err);

  // the material texture slots survive the round trip and stay linear
  Model back; std::ifstream f(emod, std::ios::binary); std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
  CHECK_MSG(loadEmod(bytes, back, err), err);
  CHECK(back.materials.size() == 1 && back.materials[0].normalTex == 0 && back.materials[0].occlusionTex == 1);
  CHECK(back.images.size() == 2 && back.images[0].linear && back.images[1].linear);

  golden::Img got;
  if (int rc = golden::capture(src, bin, emod, got)) return rc;
  int rc = golden::compare(got, src + "/tests/golden/normalmap.ppm", bin, "test_normalmap", update);
  std::printf("%s: %d checks, %d failures\n", __FILE__, test::checks, test::failures);
  return rc;
}

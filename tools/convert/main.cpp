// Offline converter: GLB -> .emod. The only place third-party image decoding lives.
//   convert in.glb out.emod [--max-texture 1024]
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_NO_PNM
#include "tools/third_party/stb_image.h"
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "tools/third_party/stb_image_resize2.h"

#include "engine/asset/emod.h"
#include "engine/asset/gltf.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace eng;

int main(int argc, char** argv) {
  if (argc < 3) { std::fprintf(stderr, "usage: convert in.glb out.emod [--max-texture N]\n"); return 2; }
  int maxTex = 2048;
  for (int i = 3; i + 1 < argc; ++i) if (!std::strcmp(argv[i], "--max-texture")) maxTex = std::atoi(argv[++i]);

  auto t0 = std::chrono::steady_clock::now();
  Model model; std::string err;
  if (!loadGlbFile(argv[1], model, err)) { std::fprintf(stderr, "GLB: %s\n", err.c_str()); return 1; }

  size_t tris = 0; for (auto& me : model.meshes) for (auto& p : me.primitives) tris += p.indices.size() / 3;
  std::printf("%s: %zu meshes, %zu tris, %zu materials, %zu images, %zu nodes\n", argv[1],
              model.meshes.size(), tris, model.materials.size(), model.images.size(), model.nodes.size());

  for (size_t i = 0; i < model.images.size(); ++i) {
    Image& im = model.images[i];
    int w, h, c;
    unsigned char* px = stbi_load_from_memory(im.encoded.data(), (int)im.encoded.size(), &w, &h, &c, 4);
    if (!px) { std::fprintf(stderr, "image %zu (%s): decode failed: %s\n", i, im.mime.c_str(), stbi_failure_reason()); return 1; }
    int ow = w, oh = h;
    // budget: no side above 2*maxTex and at most (2*maxTex)^2 pixels — with 512: a 4096x2048 livery atlas
    // keeps 1024x512, a 4096x4096 loco atlas 1024x1024 (at 512x512 the numbers were unreadable)
    while ((size_t)w * h > (size_t)4 * maxTex * maxTex || w > 2 * maxTex || h > 2 * maxTex) { w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1; }
    if (w != ow || h != oh) {
      unsigned char* rs = (unsigned char*)std::malloc((size_t)w * h * 4);
      if (im.linear) stbir_resize_uint8_linear(px, ow, oh, 0, rs, w, h, 0, STBIR_RGBA);   // data maps: no sRGB curve
      else stbir_resize_uint8_srgb(px, ow, oh, 0, rs, w, h, 0, STBIR_RGBA);
      stbi_image_free(px); px = rs;
    }
    im.width = w; im.height = h; im.channels = 4;
    im.pixels.assign(px, px + (size_t)w * h * 4);
    (w != ow) ? std::free(px) : stbi_image_free(px);
    im.encoded.clear(); im.encoded.shrink_to_fit();
    std::printf("  image %zu: %dx%d -> %dx%d%s%s\n", i, ow, oh, w, h, im.linear ? " linear" : "", im.wrapS == 1 || im.wrapT == 1 ? " clamp" : "");
  }

  if (!saveEmod(model, argv[2], err)) { std::fprintf(stderr, "EMOD: %s\n", err.c_str()); return 1; }
  double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  std::printf("-> %s (%.0f ms), bounds [%.2f %.2f %.2f]..[%.2f %.2f %.2f]\n", argv[2], ms,
              model.boundsMin.x, model.boundsMin.y, model.boundsMin.z, model.boundsMax.x, model.boundsMax.y, model.boundsMax.z);
  return 0;
}

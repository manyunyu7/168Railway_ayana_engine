// Offline image converter: PNG/JPEG -> standalone ".eimg" (EIMG v2, one RGBA8 image record; engine/asset/emod.h).
//   imgconv in.jpg out.eimg [--max N] [--wrap-s repeat|clamp|mirror] [--wrap-t ...] [--flip] [--linear]
// `--max N` halves the picture until no side exceeds N (sRGB-correct resampling). `--flip` stores the rows
// bottom-up, which is what a three.js TextureLoader texture (flipY = true) samples: v = 0 is the bottom row.
// Used for the rail corridor atlas (catalog `tekstur.rel1067`, pilot asset — converted into the cache, not committed).
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "tools/third_party/stb_image.h"
#include "tools/third_party/stb_image_resize2.h"
#include "engine/asset/emod.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace eng;

static uint8_t parseWrap(const char* s) { return !std::strcmp(s, "clamp") ? 1 : !std::strcmp(s, "mirror") ? 2 : 0; }

int main(int argc, char** argv) {
  if (argc < 3) { std::fprintf(stderr, "usage: imgconv in.(png|jpg) out.eimg [--max N] [--wrap-s W] [--wrap-t W] [--flip] [--linear]\n"); return 2; }
  int maxSide = 0; bool flip = false; Image im;
  for (int i = 3; i < argc; ++i) {
    auto val = [&](const char* f) { if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", f); std::exit(2); } return argv[++i]; };
    if (!std::strcmp(argv[i], "--max")) maxSide = std::atoi(val("--max"));
    else if (!std::strcmp(argv[i], "--wrap-s")) im.wrapS = parseWrap(val("--wrap-s"));
    else if (!std::strcmp(argv[i], "--wrap-t")) im.wrapT = parseWrap(val("--wrap-t"));
    else if (!std::strcmp(argv[i], "--flip")) flip = true;
    else if (!std::strcmp(argv[i], "--linear")) im.linear = true;
    else { std::fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
  }
  int w, h, c;
  unsigned char* px = stbi_load(argv[1], &w, &h, &c, 4);
  if (!px) { std::fprintf(stderr, "%s: %s\n", argv[1], stbi_failure_reason()); return 1; }
  int ow = w, oh = h;
  while (maxSide > 0 && (w > maxSide || h > maxSide)) { w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1; }
  if (w != ow || h != oh) {
    unsigned char* rs = (unsigned char*)std::malloc((size_t)w * h * 4);
    if (im.linear) stbir_resize_uint8_linear(px, ow, oh, 0, rs, w, h, 0, STBIR_RGBA);
    else stbir_resize_uint8_srgb(px, ow, oh, 0, rs, w, h, 0, STBIR_RGBA);
    stbi_image_free(px); px = rs;
  }
  im.width = w; im.height = h; im.channels = 4;
  im.pixels.resize((size_t)w * h * 4);
  for (int y = 0; y < h; ++y) std::memcpy(&im.pixels[(size_t)(flip ? h - 1 - y : y) * w * 4], px + (size_t)y * w * 4, (size_t)w * 4);
  (w != ow || h != oh) ? std::free(px) : stbi_image_free(px);
  std::string err;
  if (!saveImageFile(im, argv[2], err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
  std::printf("%s: %dx%d -> %dx%d%s -> %s\n", argv[1], ow, oh, w, h, flip ? " flipped" : "", argv[2]);
  return 0;
}

// Offline bitmap-font generator: TTF -> .efnt (raw 8-bit alpha atlas + glyph metrics).
//   fontgen font.ttf out.efnt [pixel_height=32]
// Format: "EFNT" u32 version(1) u16 atlasW u16 atlasH f32 pixelHeight f32 ascent f32 descent f32 lineGap
//         u32 nGlyphs { u32 codepoint, u16 x0,y0,x1,y1 (atlas px), f32 xoff,yoff,xadvance } then atlas bytes
#define STB_TRUETYPE_IMPLEMENTATION
#include "tools/third_party/stb_truetype.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>

int main(int argc, char** argv) {
  if (argc < 3) { std::fprintf(stderr, "usage: fontgen font.ttf out.efnt [px]\n"); return 2; }
  float px = argc > 3 ? (float)std::atof(argv[3]) : 32.f;
  std::ifstream f(argv[1], std::ios::binary);
  if (!f) { std::fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
  std::vector<unsigned char> ttf((std::istreambuf_iterator<char>(f)), {});

  const int W = 512, H = 512;
  std::vector<unsigned char> atlas((size_t)W * H);
  stbtt_pack_context pc;
  if (!stbtt_PackBegin(&pc, atlas.data(), W, H, 0, 1, nullptr)) { std::fprintf(stderr, "pack begin failed\n"); return 1; }
  stbtt_PackSetOversampling(&pc, 1, 1);
  const int FIRST = 32, COUNT = 96;                  // printable ASCII
  std::vector<stbtt_packedchar> chars(COUNT);
  if (!stbtt_PackFontRange(&pc, ttf.data(), 0, px, FIRST, COUNT, chars.data())) { std::fprintf(stderr, "pack failed (atlas too small?)\n"); return 1; }
  stbtt_PackEnd(&pc);

  stbtt_fontinfo info; stbtt_InitFont(&info, ttf.data(), 0);
  int asc, desc, gap; stbtt_GetFontVMetrics(&info, &asc, &desc, &gap);
  float scale = stbtt_ScaleForPixelHeight(&info, px);

  std::ofstream o(argv[2], std::ios::binary);
  auto put = [&](const auto& v) { o.write((const char*)&v, sizeof v); };
  o.write("EFNT", 4); put((uint32_t)1); put((uint16_t)W); put((uint16_t)H);
  put(px); put(asc * scale); put(desc * scale); put(gap * scale);
  put((uint32_t)COUNT);
  for (int i = 0; i < COUNT; ++i) {
    const stbtt_packedchar& c = chars[(size_t)i];
    put((uint32_t)(FIRST + i)); put(c.x0); put(c.y0); put(c.x1); put(c.y1); put(c.xoff); put(c.yoff); put(c.xadvance);
  }
  o.write((const char*)atlas.data(), (std::streamsize)atlas.size());
  std::printf("-> %s (%dx%d atlas, %d glyphs, %.0f px)\n", argv[2], W, H, COUNT, px);
  return 0;
}

// Screen-space text from an .efnt bitmap font (see tools/fontgen). Pixel coordinates, origin top-left.
#pragma once
#include "engine/math/math.h"
#include "engine/rhi/rhi.h"
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace eng {

class TextRenderer {
public:
  bool load(const std::string& efntPath, std::string& error);
  void shutdown();
  // Queue text; `scale` 1 = the font's native pixel height.
  void draw(std::string_view text, float x, float y, vec4 color = {1, 1, 1, 1}, float scale = 1);
  float measure(std::string_view text, float scale = 1) const;
  float lineHeight(float scale = 1) const { return (ascent_ - descent_ + lineGap_) * scale; }
  // Filled rectangle (for HUD panels); drawn in submission order together with text.
  void rect(float x, float y, float w, float h, vec4 color);
  // Line segment of pixel `width` and filled disc, same batch as rects/text (HUD schematics).
  void line(float x0, float y0, float x1, float y1, float width, vec4 color);
  void circle(float cx, float cy, float r, vec4 color, int segments = 16);
  void ring(float cx, float cy, float r, float width, vec4 color, int segments = 20);
  void flush(int screenW, int screenH);   // render everything queued this frame
private:
  struct Glyph { float x0, y0, x1, y1, xoff, yoff, xadvance; };
  struct V { float x, y, u, v; float r, g, b, a; };
  std::unordered_map<uint32_t, Glyph> glyphs_;
  rhi::Texture atlas_; int atlasW_ = 0, atlasH_ = 0;
  float pixelHeight_ = 0, ascent_ = 0, descent_ = 0, lineGap_ = 0;
  rhi::Program prog_; int uScreen_ = -1;
  std::vector<V> verts_; std::vector<uint32_t> idx_;
};

} // namespace eng

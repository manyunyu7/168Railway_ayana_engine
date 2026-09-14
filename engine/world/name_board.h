// Station name boards on hiasan objects (port of ppka-wannabe-2 src/tiga/uji3dPapanNama.ts): a catalog model
// may carry a blank `papan-nama` quad (mesh / node or material name; 8:1, UV 0..1, e.g. kanopi-pwk-papan).
// The save's hiasan entry `teks` / `ketinggian` fields are painted at runtime into one 1024x128 RGBA texture
// per distinct text (cached, shared by same-named stations) and the quad is drawn again unlit over the
// model's own, 3 mm toward the eye (same trick as MejaBoard: the offset beats the depth tie). Text is
// normalised like the reference ("kebumen" -> "STASIUN KEBUMEN", "21" -> "+ 21 M"); an entry without text
// keeps the plain dark board. Unlit is deliberate: the reference makes the letters emissive because the
// board sits under a canopy without direct sun.
#pragma once
#include "engine/math/geometry.h"
#include "engine/render/model_renderer.h"
#include "engine/world/pixel_canvas.h"
#include <map>
#include <string>
#include <vector>

namespace eng {

class NameBoards {
public:
  static constexpr int TEX_W = 1024, TEX_H = 128;
  static constexpr const char* NAME = "papan-nama";
  struct Placed { const GpuModel* model; mat4 xf; int objIndex; std::string teks, ketinggian; };

  // Reference normalisation (bakuNama / bakuKetinggian).
  static std::string normName(const std::string& s);
  static std::string normHeight(const std::string& s);
  static bool hasBoard(const GpuModel& m);   // punyaPapan: does the model carry a `papan-nama` quad?

  void setFont(const std::string& efntPath) { font_.load(efntPath); }
  // Re-scan the placed models (after buildDecor / a hiasan edit): every `papan-nama` quad of the models whose
  // entry has text becomes a board; textures are looked up in the cache by the normalised text.
  void scan(const std::vector<Placed>& placed);
  void draw(ModelRenderer& r, vec3 eye, const Frustum* frustum) const;
  size_t count() const { return boards_.size(); }
  void destroy();   // boards + the texture cache

private:
  struct Board { const rhi::Mesh* mesh; mat4 xf; vec3 centre, normal; AABB bounds; rhi::Texture tex; };
  std::vector<Board> boards_;
  std::map<std::string, rhi::Texture> cache_;   // "<name>|<height>" -> texture
  BitmapFont font_; Material mat_; PixelCanvas canvas_;
  rhi::Texture texture(const std::string& name, const std::string& height, bool mirrored);
};

} // namespace eng

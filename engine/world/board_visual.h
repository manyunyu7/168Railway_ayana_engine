// Trackside boards (docs/world-spec.md §5.6, port of ppka-wannabe-2 src/tiga/uji3dPapan.ts):
// S35 whistle boards, taspat speed boards, 10G stop marks, buffer stops and km posts, all from the
// save (`world.trackside` / `world.scenery`). Boards stand 3 m from the track axis on `sisi`
// (default right of travel) on a 2.2 m pole and face the approaching train; the text is painted into
// one atlas texture (digits rasterised from assets/font.efnt). Everything is batched per material:
// face (atlas), dark metal, white, red — four draw calls for the whole corridor. Trackmarks are
// deliberately not drawn.
#pragma once
#include "engine/asset/model.h"
#include "engine/core/json.h"
#include "engine/render/model_renderer.h"
#include "engine/world/coords.h"
#include "engine/world/height_source.h"
#include "engine/world/track_graph.h"
#include <functional>
#include <string>
#include <vector>

namespace eng {

namespace boards {
constexpr float SIDE_OFFSET = 3;            // OFFSET_SISI_M
constexpr float POLE = 2.2f, POLE_SIDE = 0.07f, PLATE_THICK = 0.03f;
constexpr float S35_W = 0.6f, S35_H = 0.5f, TASPAT_W = 0.55f, TASPAT_H = 0.45f, KM_W = 0.42f, KM_H = 0.24f;
constexpr float KM_POST_H = 0.9f, KM_POST_SIDE = 0.12f, KM_PLATE_Y = 0.68f;
constexpr float MARK_SPAN = 2.8f, MARK_W = 0.28f, MARK_T = 0.06f;                       // 10G
constexpr float BUF_SPAN = 1.9f, BUF_BEAM_Y = 0.75f, BUF_BEAM = 0.3f, BUF_LEG = 0.14f, BUF_BACK = 0.9f;
constexpr int ATLAS_W = 2048, CELL_W = 256, CELL_H = 64, COLS = 8;
} // namespace boards

struct BoardSpec {
  enum Kind : uint8_t { S35, Taspat, KmPost } kind;
  std::string text;
  vec3 foot;                 // pole foot (scene)
  float hx, hz;              // unit facing direction in xz (toward the approaching train)
};

class TracksideBoards {
public:
  struct Stats { size_t boards = 0, marks = 0, buffers = 0; unsigned tris = 0; int drawCalls = 0; };
  using GroundFn = std::function<float(double wx, double wy)>;   // carved ground, scene y (km posts)

  // world = the save's `world` object; fontPath = assets/font.efnt (text left blank when unreadable).
  void build(const TrackGraph& g, const RailProfile& profile, const Json& world, const WorldOrigin& origin,
             const GroundFn& ground, const std::string& fontPath);
  void draw(ModelRenderer& r, const Frustum* frustum = nullptr) const;
  void destroy();
  const std::vector<BoardSpec>& boards() const { return boards_; }
  Stats stats;

private:
  struct Batch { rhi::Mesh mesh; AABB bounds; bool used = false; };
  Batch face_, dark_, white_, red_, grey_;   // grey = scenery `platform` boxes
  rhi::Texture atlas_{};
  Material faceMat_, darkMat_, whiteMat_, redMat_, greyMat_;
  std::vector<BoardSpec> boards_;
};

} // namespace eng

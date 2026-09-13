// Procedural Jabodetabek KRL station (spec §5.5, port of ppka-wannabe-2 src/tiga/uji3dStasiunKRL.ts).
// Catalog entries `stasiun-krl`, `stasiun-krl-<n>j`, `stasiun-krl-aula` (model.json, `prosedural`) are
// placed like any hiasan object; here one object also grows the island platforms, canopies and the
// LAA gantries/wires that the reference tiles along `garis` splines, so the station stands alone.
//
// Local frame: +X along the rails, +Z across them, y = 0 at the rail head, emplacement centred on
// z = 0, plaza and receiving hall at -Z. Anchors: platform floor 1.00 m, LAA contact wire 5.0 m,
// JPO/concourse deck 6.6 m, roof lip 12.1 m, roof ridge 14.3 m.
#pragma once
#include "engine/asset/model.h"
#include "engine/math/geometry.h"
#include "engine/render/mesh_builder.h"
#include "engine/render/model_renderer.h"
#include "engine/rhi/rhi.h"
#include <string>
#include <vector>

namespace eng {

namespace krl {
constexpr float PLATFORM_FLOOR = 1.0f, LAA_WIRE = 5.0f, DECK = 6.6f, ROOM_H = 5.5f, ROOF_LIP = 12.1f, ROOF_RIDGE = 14.3f;
constexpr float BOX_LENGTH = 26, BOX_DEPTH = 12, ROOF_THICK = 0.55f, ROOF_OVERHANG = 0.9f, COLUMN_D = 0.8f, COLUMN_GAP = 6;
constexpr float RISER = 0.175f, TREAD = 0.28f, STAIR_W_OUT = 2.4f, STAIR_W_PLAT = 2.0f, RAIL_H = 1.10f, LANDING = 1.4f;
constexpr float PLATFORM_W = 8.0f, CANOPY_H = 5.0f, CANOPY_OVERHANG = 0.6f, CANOPY_CURVE = 0.9f, POST_D = 0.32f;
constexpr float POST_BASE = 0.70f, POST_BASE_H = 0.55f, POST_GAP = 6.0f, FURNITURE_GAP = 18.0f;
constexpr float TACTILE_W = 0.60f, TACTILE_FROM_EDGE = 0.85f, PLATFORM_SKIRT = 1.8f;
constexpr float TRACK_FROM_PLATFORM = 1.7f, TRACK_GAP = 4.5f;   // SEPUR_DARI_PERON / SEPUR_ANTAR
}

struct KrlLayout {
  int tracks = 4;
  struct Platform { float z; std::vector<int> trackNumbers; };
  std::vector<Platform> platforms;                 // island platform axes
  std::vector<std::pair<float, int>> trackZ;       // (z, number from 1)
  float halfWidth = 0;                             // centre to the outermost track axis
};
// tataLetakKRL: tracks paired around island platforms, centred on z = 0.
KrlLayout krlLayout(int tracks, float platformWidth = krl::PLATFORM_W);

struct KrlOptions {
  std::string name = "BEKASI TIMUR";
  int tracks = 4;                 // 2 -> 1 platform, 4 -> 2, 6 -> 3
  float boxLength = krl::BOX_LENGTH, boxDepth = krl::BOX_DEPTH;
  bool concourse = true;          // waiting hall spanning the yard (false = `stasiun-krl-aula`)
  int stairs = 1;                 // outer plaza stairs: -1 left, 1 right, 0 none
  bool frontCanopy = true, platformStairs = true;
  float platformLength = 160;     // island platforms (0 = building only, as the reference object)
  float platformWidth = krl::PLATFORM_W;
  bool laa = true;                // contact/messenger wires + gantries over the platform length
};

class KrlStation {
public:
  void build(const KrlOptions& opt = {});
  void draw(ModelRenderer& r, const mat4& transform, const Frustum* frustum = nullptr) const;
  void destroy();

  const KrlLayout& layout() const { return layout_; }
  struct Opening { float z, x0, x1; };              // canopy gaps where the concourse stairs land
  const std::vector<Opening>& openings() const { return openings_; }
  float hallZ() const { return hallZ_; }             // axis of the receiving hall (plaza side)
  AABB bounds;                                       // local
  struct Stats { int parts = 0; uint32_t tris = 0; double buildMs = 0; };
  Stats stats;

private:
  struct Part { Material mat; rhi::Mesh mesh; AABB bounds; };
  std::vector<Part> parts_;
  KrlLayout layout_;
  std::vector<Opening> openings_;
  float hallZ_ = 0;
};

} // namespace eng

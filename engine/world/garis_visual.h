// hiasan.garis — spline objects (docs/world-spec.md §3.5, port of uji3dSpline.ts): fences, walls, LAA
// poles and platforms saved as `{kelas, naik, titik:[{x,y}]}` and tiled along a centripetal Catmull-Rom
// through the points. GLB classes (`model.json.garis[]` with `berkas`) are drawn instanced, one buffer
// per prototype; procedural classes (`prosedural`: peron, peron-kanopi, peron-tiang, tembok-beton)
// are baked into one static mesh per colour.
#pragma once
#include "engine/core/json.h"
#include "engine/math/geometry.h"
#include "engine/render/mesh_builder.h"
#include "engine/render/model_renderer.h"
#include "engine/world/asset_catalog.h"
#include "engine/world/coords.h"
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace eng {

namespace garis {
// Platform constants (uji3dSpline.ts badanPeron)
constexpr float PERON_TINGGI = 1.0f;         // platform floor above rail head (KAI high platform)
constexpr float PERON_LEBAR = 6.0f;          // island platform width
constexpr float PERON_ROK = 1.8f;            // skirt: body extends this far below the floor
constexpr float GARIS_KUNING_LEBAR = 0.3f, GARIS_KUNING_DARI_TEPI = 0.8f;
constexpr float KANOPI_TINGGI = 3.4f, KANOPI_LEBAR = 5.6f;
constexpr float PERON_UBIN = 2;              // tile length (short so curved canopies do not split)
constexpr float PERON_JARAK_TIANG = 6;
constexpr unsigned WARNA_LANTAI = 0xc9c5bd, WARNA_DINDING = 0xa8a49c, WARNA_KUNING = 0xe0ac10;
}

class GarisVisuals {
public:
  struct Stats { size_t lines = 0, tiles = 0, glbSlots = 0, procMeshes = 0, missing = 0; };
  using GroundFn = std::function<float(double, double)>;   // world (wx, wy) -> carved scene y

  // hiasan = world["hiasan"] (uses ["garis"]). Heights from `ground` unless the class is `datar`
  // (then one straight grade between the end heights).
  void build(const Json& hiasan, AssetCatalog& catalog, const WorldOrigin& origin, const GroundFn& ground);
  void draw(ModelRenderer& r, const Frustum* frustum = nullptr);
  void destroy();
  Stats stats;

private:
  struct ProcPart { vec3 centre, size; unsigned color; float rough; float rotX; };   // a box of the procedural prototype (tile-local, long axis +X)
  struct ModelSlot { GpuModel* model = nullptr; mat4 norm; rhi::Buffer instances{}; std::vector<mat4> mats; AABB bounds; };
  struct ProcMesh { rhi::Mesh mesh{}; Material mat; AABB bounds; };
  static bool procParts(const std::string& name, std::vector<ProcPart>& out);
  void placeGlb(const std::string& catalogId, AssetCatalog& catalog, const std::vector<mat4>& mats);
  void placeProc(const std::string& name, const std::vector<mat4>& mats);
  std::map<std::string, ModelSlot> slots_;                       // by catalog id
  std::map<unsigned, MeshBuilder> procBuild_;                     // by packed colour (rough in the high byte)
  std::vector<ProcMesh> proc_;
};

} // namespace eng

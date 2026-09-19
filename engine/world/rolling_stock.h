// Rolling-stock prototypes: a catalog body model normalised per docs/world-spec.md §7.3 / §10.3
// (forward = +X, centred on X/Z, rail head at y = 0, coupler-to-coupler `panjang`) with bogie and
// coupling models attached as rigid children at the body's `a.bog<n>` / `a.kopling<n>` nodes.
#pragma once
#include "engine/math/geometry.h"
#include "engine/world/asset_catalog.h"
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace eng {

struct Attachment { GpuModel* model; mat4 local; };   // local = normalised-body space

// Cargo point (`a.muatan<n>`, model.json `muatan`, the old src/tiga/muatan.ts): every listed variant, in catalog
// order, so the pick (pilihMuatan) is stable; a variant not (yet) resident is nullptr and simply not drawn. The
// point sits on the deck surface and the cargo models have their origin at bottom-centre: no offset, no flip.
struct CargoSlot { mat4 local; std::vector<GpuModel*> varian; };

// Light attach points (§7.3, dunia3dKonst.ts peranLampu / lampuKarangan): `sorot` = headlight,
// `muka` = ditch light, `akhiran` = tail light. `end` = which end of the body (+1 = +X, the front).
enum class LightRole : uint8_t { Sorot, Muka, Akhiran };
struct LightPoint { LightRole role; int end; vec3 pos; };   // pos in normalised body space (unscaled)

struct VehicleProto {
  GpuModel* body = nullptr;
  float panjang = 1;            // coupler-to-coupler length of the model (m)
  std::vector<Attachment> parts;
  std::vector<CargoSlot> muatan;   // empty for every passenger car and for a flat wagon whose catalog says nothing
  mat4 normalize;               // raw model space -> normalised body space
  AABB bounds;                  // in normalised body space, body + parts
  vec3 size;                    // body bbox size after normalisation
  // Animation rigs. `doorClips` = `pintu-kiri` / `pintu-kanan` clips (scrubbed by the dwell timer);
  // `restLocal`/`restWorld` = per-node matrices with every `panto-*` clip frozen at its last frame
  // (pantograph raised), empty when the body has no clips at all (draw with the static pose).
  std::vector<int> doorClips;
  float doorDuration = 0;
  std::vector<mat4> restLocal, restWorld;
  std::vector<LightPoint> lights;   // from the model's light nodes, else the synthetic fallback points
  bool lightsFromModel = false;
  // INTERIOR meshes (node or mesh name containing `interior`) are only drawn near the camera (INTERIOR_JARAK) or
  // from inside: keretaVisual3d.ts sakelarInterior — the CC 206 interior is a shell 5 mm inside the cab skin
  // (428 coplanar triangle pairs, both double-sided, same atlas); past ~300 m the depth buffer no longer tells
  // them apart and the two walls fight for pixels. `worldFar` = the rest pose with those nodes collapsed to a
  // point (zero scale: nothing rasterises, no renderer change); empty when the body has no interior node.
  std::vector<int> interiorNodes;
  std::vector<mat4> worldFar;
  // Cab view (dunia3dKonst.ts KACA_KABIN / KULIT_DUA_SISI, keretaVisual3d.ts sisiTunggal): the body's materials
  // as seen from INSIDE — single-sided so the skin does not hide the world (unless the slot is in
  // KULIT_DUA_SISI: a one-layer skin with outward normals would be "holed" from inside), and the glass materials
  // that ship opaque (regex per slot) blended at KACA_OPASITAS. Same order as body->materials.
  std::vector<Material> cabMaterials;
  // Every streamed texture of the body and the attachments arrived (GpuModel::textured); box fallback until then.
  bool textured() const {
    if (body && !body->textured()) return false;
    for (const Attachment& a : parts) if (a.model && !a.model->textured()) return false;
    return true;
  }
};

class RollingStock {
public:
  void init(AssetCatalog& catalog) { catalog_ = &catalog; }
  // Prototype for a catalog slot id; nullptr when the body model is unavailable. Cached.
  const VehicleProto* proto(const std::string& slotId);
  void forget() { protos_.clear(); }   // streamed models: rebuild the prototypes after a model arrived
  // Normalisation of a raw model (also usable for scenery: steps 1-2 of §10.3).
  static mat4 normalizeTransform(const GpuModel& m, bool dropToGround, vec3* sizeOut = nullptr);
  // FNV-1a of "<kunci>#<i>" (muatan.ts pilihMuatan): the same train always shows the same containers, two trains
  // differ, and every machine agrees. cargoVariant() folds it onto a point's variant count (n <= 1 -> 0).
  static uint32_t cargoHash(const std::string& kunci, size_t i);
  static size_t cargoVariant(uint32_t hash, size_t n) { return n <= 1 ? 0 : hash % n; }
  static constexpr float INTERIOR_JARAK = 80;   // m (dunia3dKonst.ts)
  static constexpr float KACA_OPASITAS = 0.12f;
  // Collapse the interior nodes of `pose` (a per-node world matrix set of the body) — used for a scrubbed door pose
  static void sembunyikanInterior(const VehicleProto& p, std::vector<mat4>& pose);
private:
  AssetCatalog* catalog_ = nullptr;
  std::map<std::string, VehicleProto> protos_;
};

// Node-name flattening used by the reference implementation: lowercase, non-alphanumerics dropped.
std::string flatNodeName(const std::string& s);
// Light role of a node name (`light\d`, `ditch_white`, `ditch_red`, `s21[-_]`, `red\d`, `coronacenter`,
// with an optional `a`/`a.`/`a_` attach prefix); false when the node is not a light.
bool lightRole(const std::string& nodeName, LightRole& role);
// Synthetic light points for a body of size `sz` (lampuKarangan): two headlights, one tail light.
std::vector<LightPoint> fallbackLights(vec3 sz);
// Interior node (sakelarInterior): the name contains `interior`, any case.
bool isInteriorName(const std::string& nodeName);
// The body's materials as seen from the cab (VehicleProto::cabMaterials) for a catalog slot (a sarana alias such as
// `krl-kuha` resolves to its slot first): single-sided unless the slot is in KULIT_DUA_SISI, opaque glass
// (KACA_KABIN regex on the material name) blended at KACA_OPASITAS.
std::vector<Material> cabMaterialsFor(const std::string& slotId, const std::vector<Material>& asli);

} // namespace eng

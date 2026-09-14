// Rolling-stock prototypes: a catalog body model normalised per docs/world-spec.md §7.3 / §10.3
// (forward = +X, centred on X/Z, rail head at y = 0, coupler-to-coupler `panjang`) with bogie and
// coupling models attached as rigid children at the body's `a.bog<n>` / `a.kopling<n>` nodes.
#pragma once
#include "engine/math/geometry.h"
#include "engine/world/asset_catalog.h"
#include <map>
#include <string>
#include <vector>

namespace eng {

struct Attachment { GpuModel* model; mat4 local; };   // local = normalised-body space

// Light attach points (§7.3, dunia3dKonst.ts peranLampu / lampuKarangan): `sorot` = headlight,
// `muka` = ditch light, `akhiran` = tail light. `end` = which end of the body (+1 = +X, the front).
enum class LightRole : uint8_t { Sorot, Muka, Akhiran };
struct LightPoint { LightRole role; int end; vec3 pos; };   // pos in normalised body space (unscaled)

struct VehicleProto {
  GpuModel* body = nullptr;
  float panjang = 1;            // coupler-to-coupler length of the model (m)
  std::vector<Attachment> parts;
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

} // namespace eng

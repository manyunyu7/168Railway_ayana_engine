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

struct VehicleProto {
  GpuModel* body = nullptr;
  float panjang = 1;            // coupler-to-coupler length of the model (m)
  std::vector<Attachment> parts;
  mat4 normalize;               // raw model space -> normalised body space
  AABB bounds;                  // in normalised body space, body + parts
  vec3 size;                    // body bbox size after normalisation
};

class RollingStock {
public:
  void init(AssetCatalog& catalog) { catalog_ = &catalog; }
  // Prototype for a catalog slot id; nullptr when the body model is unavailable. Cached.
  const VehicleProto* proto(const std::string& slotId);
  // Normalisation of a raw model (also usable for scenery: steps 1-2 of §10.3).
  static mat4 normalizeTransform(const GpuModel& m, bool dropToGround, vec3* sizeOut = nullptr);
private:
  AssetCatalog* catalog_ = nullptr;
  std::map<std::string, VehicleProto> protos_;
};

// Node-name flattening used by the reference implementation: lowercase, non-alphanumerics dropped.
std::string flatNodeName(const std::string& s);

} // namespace eng

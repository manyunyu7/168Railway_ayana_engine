// Track graph: nodes, segments, points (junctions) and per-segment cubic Bézier geometry with an
// arc-length LUT — a faithful port of ppka-wannabe-2 src/engine/track.ts (docs/world-spec.md §3.2).
// All world coordinates are double (Web Mercator metres, y south-positive).
#pragma once
#include "engine/core/json.h"
#include "engine/world/coords.h"
#include <string>
#include <unordered_map>
#include <vector>

namespace eng {

enum class RailKind : uint8_t { Ground, Bridge, Tunnel };
enum class BridgeShape : uint8_t { Auto, Deck, Truss, Viaduct };
constexpr double VIADUCT_CLEARANCE_DEFAULT = 12;   // m, rail head over the ground under a "layang" segment

struct TrackNode {
  std::string id;
  double wx = 0, wy = 0;
  bool hasHeight = false;   // hand-written `tinggi` (raw DEM metres)
  double height = 0;
  std::vector<int> segs;    // segment indices, in file order
  // Junction (points / wesel): valid when segs.size() == 3.
  int facingSeg = -1;
  int legs[2] = {-1, -1};
  int setting = 0;          // indexes legs
  int spring = -1;          // -1 = none
  bool isPoint() const { return facingSeg >= 0; }
};

struct TrackSegment {
  std::string id;
  int a = -1, b = -1;
  bool straight = false;
  RailKind kind = RailKind::Ground;
  BridgeShape bridge = BridgeShape::Auto;
  // Viaduct ("layang", save field `layang`: metres): a bridge whose rail FOLLOWS the ground at this clearance
  // instead of chording between its abutments — a high-speed line over a plain, an elevated urban line. 0 = a plain
  // bridge. The profile adds it to the DEM samples (they are not blind), the builder defaults the shape to Viaduct.
  float clearance = 0;
  double cp1x = 0, cp1y = 0, cp2x = 0, cp2y = 0;
  double length = 0;
  struct Lut { double px, py, tx, ty, s; };
  std::vector<Lut> lut;     // uniform-t samples, tx/ty normalised
  double bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;   // bbox of the LUT
};

struct TrackSample { double wx = 0, wy = 0, tx = 1, ty = 0; };

class TrackGraph {
public:
  static constexpr double CP_K = 0.35;

  // Accepts the raw save `world` object ({graph:{nodes,segments},...}), a whole save ({world:{...}})
  // or a bare graph ({nodes,segments}). Recomputes junctions like TrackGraph.fromJSON.
  bool fromJson(const Json& world, std::string* error = nullptr);

  std::vector<TrackNode> nodes;
  std::vector<TrackSegment> segments;
  int nodeIndex(const std::string& id) const { auto it = nodeIdx_.find(id); return it == nodeIdx_.end() ? -1 : it->second; }
  int segIndex(const std::string& id) const { auto it = segIdx_.find(id); return it == segIdx_.end() ? -1 : it->second; }

  double length(int seg) const { return segments[(size_t)seg].length; }
  TrackSample sampleAt(int seg, double s) const;                 // clamps s to [0, length]
  TrackSample sampleAt(const std::string& segId, double s) const { int i = segIndex(segId); return i < 0 ? TrackSample{} : sampleAt(i, s); }
  int otherNode(int seg, int node) const { const TrackSegment& s = segments[(size_t)seg]; return s.a == node ? s.b : s.a; }

  int pointCount() const;
  double totalLength() const;
  const WorldOrigin& origin() const { return origin_; }   // bbox centre of the track nodes

private:
  void refreshJunction(int node);
  void nodeTangentAxis(int node, double& tx, double& ty) const;
  void buildGeometry(int seg);
  std::unordered_map<std::string, int> nodeIdx_, segIdx_;
  WorldOrigin origin_;
};

} // namespace eng

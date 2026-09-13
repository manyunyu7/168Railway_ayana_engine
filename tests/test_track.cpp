// TrackGraph vs the TypeScript reference (tests/ref/mojokerto_track.json, produced by tests/ref/track_ref.ts).
// The arc-length LUT must be bit-for-bit the same algorithm, so agreement is demanded to 1e-6 m.
#include "engine/core/json.h"
#include "engine/world/track_graph.h"
#include "tests/check.h"
#include <cmath>
#include <fstream>
#include <sstream>

using namespace eng;

static Json readJson(const std::string& path) {
  std::ifstream f(path); std::stringstream ss; ss << f.rdbuf();
  std::string err; Json j = Json::parse(ss.str(), &err);
  CHECK_MSG(f.good() || !ss.str().empty(), "cannot read " + path); CHECK_MSG(err.empty(), err);
  return j;
}

TEST_MAIN({
  const std::string root = ENG_SOURCE_DIR;
  Json save = readJson(root + "/../ppka-wannabe-2/src/data/mojokerto.json");
  Json ref = readJson(root + "/tests/ref/mojokerto_track.json");
  CHECK(ref["segments"].size() > 0);

  TrackGraph g; std::string err;
  CHECK_MSG(g.fromJson(save["world"], &err), err);
  // the loader accepts all three shapes
  { TrackGraph g2, g3; CHECK(g2.fromJson(save, &err)); CHECK(g3.fromJson(save["world"]["graph"], &err)); CHECK(g2.segments.size() == g.segments.size() && g3.nodes.size() == g.nodes.size()); }
  { TrackGraph bad; CHECK(!bad.fromJson(save["session"], &err)); CHECK(!err.empty()); }

  CHECK(g.nodes.size() == (size_t)ref["nodes"].intOr(-1));
  CHECK(g.segments.size() == ref["segments"].size());
  CHECK(g.pointCount() == 16);
  CHECK(ref["points"].size() == 16);
  CHECK_NEAR(g.totalLength() / 1000, 34.703, 0.0005);
  CHECK_NEAR(g.totalLength(), ref["totalLength"].numberOr(0), 1e-6);

  // segment lengths and samples
  double worstPos = 0, worstTan = 0, worstLen = 0;
  for (const Json& rs : ref["segments"].arr) {
    std::string id = rs["id"].stringOr("");
    int si = g.segIndex(id);
    CHECK_MSG(si >= 0, "segment " + id + " missing");
    if (si < 0) continue;
    const TrackSegment& s = g.segments[(size_t)si];
    CHECK(g.nodes[(size_t)s.a].id == rs["a"].stringOr("")); CHECK(g.nodes[(size_t)s.b].id == rs["b"].stringOr(""));
    double dl = std::fabs(s.length - rs["length"].numberOr(0)); worstLen = std::max(worstLen, dl);
    CHECK_MSG(dl <= 1e-6, id + " length " + std::to_string(s.length) + " vs " + std::to_string(rs["length"].numberOr(0)));
    for (const Json& smp : rs["samples"].arr) {
      TrackSample t = g.sampleAt(si, smp["s"].numberOr(0));
      double dp = std::hypot(t.wx - smp["x"].numberOr(0), t.wy - smp["y"].numberOr(0));
      double dt = std::hypot(t.tx - smp["tx"].numberOr(0), t.ty - smp["ty"].numberOr(0));
      worstPos = std::max(worstPos, dp); worstTan = std::max(worstTan, dt);
      CHECK_MSG(dp <= 1e-6, id + " s=" + std::to_string(smp["s"].numberOr(0)) + " position off by " + std::to_string(dp));
      CHECK_MSG(dt <= 1e-9, id + " s=" + std::to_string(smp["s"].numberOr(0)) + " tangent off by " + std::to_string(dt));
    }
    // clamping and LUT shape
    TrackSample before = g.sampleAt(si, -5), at0 = g.sampleAt(si, 0), after = g.sampleAt(si, s.length + 5), atL = g.sampleAt(si, s.length);
    CHECK(before.wx == at0.wx && before.wy == at0.wy && after.wx == atL.wx && after.wy == atL.wy);
    CHECK(s.lut.size() >= 11 && s.lut.size() <= 241);
    CHECK(s.lut.front().s == 0 && s.lut.back().s == s.length);
    CHECK(s.bx0 <= s.bx1 && s.by0 <= s.by1);
    CHECK(g.sampleAt(id, 1).wx == g.sampleAt(si, 1).wx);   // by-id overload
  }
  std::printf("worst deviation from the TS reference: length %.3g m, position %.3g m, tangent %.3g\n", worstLen, worstPos, worstTan);
  CHECK(g.sampleAt("no-such-segment", 1).wx == 0);

  // junctions: facingSeg / legs / setting / spring exactly as refreshJunction in track.ts
  for (const Json& rp : ref["points"].arr) {
    std::string id = rp["id"].stringOr("");
    int ni = g.nodeIndex(id);
    CHECK_MSG(ni >= 0, "point node " + id + " missing");
    if (ni < 0) continue;
    const TrackNode& n = g.nodes[(size_t)ni];
    CHECK_MSG(n.isPoint(), id + " is not a point in C++");
    if (!n.isPoint()) continue;
    CHECK_MSG(g.segments[(size_t)n.facingSeg].id == rp["facingSeg"].stringOr(""), id + " facingSeg");
    CHECK_MSG(g.segments[(size_t)n.legs[0]].id == rp["legs"][0].stringOr(""), id + " legs[0]");
    CHECK_MSG(g.segments[(size_t)n.legs[1]].id == rp["legs"][1].stringOr(""), id + " legs[1]");
    CHECK_MSG(n.setting == rp["setting"].intOr(-1), id + " setting");
    CHECK_MSG(n.spring == rp["spring"].intOr(-1), id + " spring");
    CHECK(n.legs[0] != n.facingSeg && n.legs[1] != n.facingSeg && n.legs[0] != n.legs[1]);
  }
  for (const TrackNode& n : g.nodes) CHECK(n.isPoint() == (n.segs.size() == 3));

  // origin = bbox centre of the nodes
  double x0 = 1e30, x1 = -1e30, y0 = 1e30, y1 = -1e30;
  for (const TrackNode& n : g.nodes) { x0 = std::min(x0, n.wx); x1 = std::max(x1, n.wx); y0 = std::min(y0, n.wy); y1 = std::max(y1, n.wy); }
  CHECK_NEAR(g.origin().ox, (x0 + x1) / 2, 1e-9); CHECK_NEAR(g.origin().oz, (y0 + y1) / 2, 1e-9);
  vec3 sc = g.origin().toScene(g.origin().ox + 10, g.origin().oz - 5, 2); CHECK(sc.x == 10 && sc.y == 2 && sc.z == -5);
  double wx, wy; g.origin().toWorld(sc, wx, wy); CHECK_NEAR(wx, g.origin().ox + 10, 1e-6); CHECK_NEAR(wy, g.origin().oz - 5, 1e-6);
  // yaw convention: tangent +x (east) -> yaw 0; tangent +y (south in flipped Mercator) -> local +X maps to +z, yaw = -90°
  CHECK_NEAR(WorldOrigin::yawFromTangent(1, 0), 0, 1e-7);
  CHECK_NEAR(WorldOrigin::yawFromTangent(0, 1), -PI / 2, 1e-6);
  vec3 f = WorldOrigin::yawMatrix(WorldOrigin::yawFromTangent(0, 1)).transformDir({1, 0, 0});
  CHECK_NEAR(f.x, 0, 1e-6); CHECK_NEAR(f.z, 1, 1e-6);
})

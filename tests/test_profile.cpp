// VerticalProfile invariants on mojokerto with a synthetic DEM (rolling hills + a ridge).
#include "engine/core/json.h"
#include "engine/world/rail_profile.h"
#include "tests/check.h"
#include <cmath>
#include <fstream>
#include <set>
#include <sstream>

using namespace eng;

struct HillHeight : HeightSource {
  float rawHeight(double wx, double wy) const override {
    double x = wx - 12513000, y = wy - 835500;
    return 40 + 25 * (float)std::sin(x / 900.0) * (float)std::cos(y / 1300.0) + 12 * (float)std::sin((x + y) / 350.0)
         + 30 * (float)std::exp(-((x - 2000) * (x - 2000)) / (2 * 400.0 * 400.0));   // a steep ridge to exercise the clamp
  }
};

TEST_MAIN({
  const std::string root = ENG_SOURCE_DIR;
  std::ifstream f(root + "/../ppka-wannabe-2/src/data/mojokerto.json"); std::stringstream ss; ss << f.rdbuf();
  std::string err; Json save = Json::parse(ss.str(), &err); CHECK_MSG(err.empty(), err);
  TrackGraph g; CHECK_MSG(g.fromJson(save["world"], &err), err);

  std::vector<StationZone> stations;
  for (const Json& sc : save["world"]["scenery"].arr)
    if (sc["kind"].stringOr("") == "station") stations.push_back({sc["pos"]["x"].numberOr(0), sc["pos"]["y"].numberOr(0), 160});
  CHECK(stations.size() == 1);

  HillHeight dem;
  VerticalProfile prof;
  CHECK(!prof.built());
  CHECK(prof.railHeight("any", 0) == 0);   // unbuilt -> 0
  float base = dem.rawHeight(g.origin().ox, g.origin().oz);
  prof.build(g, dem, stations, base);
  CHECK(prof.built()); CHECK(prof.demBase() == base);
  CHECK(prof.chainCount() > 0); CHECK(prof.sampleCount() > 1000);
  CHECK(prof.gmax() >= 0.005 - 1e-12 && prof.gmax() <= 0.05 + 1e-12);
  std::printf("profile: %d chains, gmax %.2f permille, %zu samples\n", prof.chainCount(), prof.gmax() * 1000, prof.sampleCount());

  // 1. every segment meeting at a node reports the same height there (within 0.05 m)
  double worst = 0; std::string worstNode;
  for (size_t ni = 0; ni < g.nodes.size(); ++ni) {
    const TrackNode& n = g.nodes[ni];
    double lo = 1e30, hi = -1e30;
    for (int si : n.segs) {
      const TrackSegment& s = g.segments[(size_t)si];
      double h = prof.railHeight(s.id.c_str(), s.a == (int)ni ? 0.0 : s.length);
      lo = std::min(lo, h); hi = std::max(hi, h);
    }
    if (n.segs.size() > 1 && hi - lo > worst) { worst = hi - lo; worstNode = n.id; }
    CHECK_MSG(n.segs.size() < 2 || hi - lo <= 0.05, "node " + n.id + " height spread " + std::to_string(hi - lo));
  }
  std::printf("worst node agreement: %.4f m at %s\n", worst, worstNode.c_str());

  // 2. railHeight is consistent between the id and index overloads, and rawHeight - demBase
  for (const TrackSegment& s : g.segments) {
    CHECK(prof.railHeight(s.id.c_str(), 7.5) == prof.railHeight(g.segIndex(s.id), 7.5));
    CHECK_NEAR(prof.railHeight(g.segIndex(s.id), 7.5), prof.rawHeight(g.segIndex(s.id), 7.5) - base, 1e-4);
    CHECK(prof.railHeight(g.segIndex(s.id), -10) == prof.railHeight(g.segIndex(s.id), 0));           // clamped
    CHECK(prof.railHeight(g.segIndex(s.id), s.length + 10) == prof.railHeight(g.segIndex(s.id), s.length));
  }
  CHECK(prof.rawHeight(-1, 0) == base); CHECK(prof.rawHeight(9999, 0) == base);

  // 3. gradients: ≤ gmax on every 20 m window except across short crossover chains (segments between two points,
  //    whose ends are pinned to datums agreed with the neighbouring long chains). Those are listed explicitly.
  std::set<std::string> allowed;   // crossovers: both end nodes are points (or an end) and the segment is < 200 m
  for (const TrackSegment& s : g.segments) {
    bool aP = g.nodes[(size_t)s.a].isPoint() || g.nodes[(size_t)s.a].segs.size() == 1;
    bool bP = g.nodes[(size_t)s.b].isPoint() || g.nodes[(size_t)s.b].segs.size() == 1;
    if (aP && bP && s.length < 200) allowed.insert(s.id);
  }
  double gworst = 0; std::string gseg; int over = 0, overAllowed = 0;
  const double tol = 1e-3;
  for (const TrackSegment& s : g.segments) {
    for (double a = 0; a + 20 <= s.length; a += 20) {
      double gr = std::fabs(prof.railHeight(s.id.c_str(), a + 20) - prof.railHeight(s.id.c_str(), a)) / 20;
      if (gr > prof.gmax() + tol) { if (allowed.count(s.id)) ++overAllowed; else { ++over; CHECK_MSG(false, "gradient " + std::to_string(gr * 1000) + " permille on " + s.id); } }
      if (gr > gworst) { gworst = gr; gseg = s.id; }
      CHECK_MSG(gr <= 0.05 + tol, "gradient above the 50 permille ceiling on " + s.id);   // hard ceiling, even for crossovers
    }
  }
  std::printf("max gradient over 20 m: %.2f permille at %s (gmax %.2f); %d windows over gmax (%d in %zu allowed crossovers)\n",
              gworst * 1000, gseg.c_str(), prof.gmax() * 1000, over + overAllowed, overAllowed, allowed.size());

  // 4. station zones are flat: every rail sample within r of a station is within 0.25 m of the zone datum
  for (const StationZone& z : stations) {
    std::vector<double> hs;
    for (const TrackSegment& s : g.segments)
      for (double a = 0; a <= s.length; a += 10) {
        TrackSample p = g.sampleAt(s.id, a);
        if (std::hypot(p.wx - z.wx, p.wy - z.wy) <= z.r) hs.push_back(prof.railHeight(s.id.c_str(), a));
      }
    CHECK(hs.size() > 20);
    double lo = 1e30, hi = -1e30; for (double h : hs) { lo = std::min(lo, h); hi = std::max(hi, h); }
    std::printf("station zone: %zu samples, height spread %.3f m\n", hs.size(), hi - lo);
    CHECK_MSG(hi - lo <= 0.25, "station zone not flat: spread " + std::to_string(hi - lo));
  }

  // 5. the profile follows the terrain: mean |rail - DEM| over ground segments stays small (no runaway smoothing)
  double sum = 0; int cnt = 0;
  for (const TrackSegment& s : g.segments) {
    if (s.kind != RailKind::Ground) continue;
    for (double a = 0; a <= s.length; a += 50) { TrackSample p = g.sampleAt(s.id, a); sum += std::fabs(prof.rawHeight(g.segIndex(s.id), a) - dem.rawHeight(p.wx, p.wy)); ++cnt; }
  }
  std::printf("mean |rail - DEM| = %.2f m\n", sum / cnt);
  CHECK(sum / cnt < 8);

  // 6. flat DEM -> flat profile (exact), custom options
  FlatHeight flat(123);
  VerticalProfile pf; VerticalProfile::Options o; o.step = 40; pf.build(g, flat, stations, 123, o);
  for (const TrackSegment& s : g.segments) { CHECK_NEAR(pf.railHeight(s.id.c_str(), s.length / 2), 0, 1e-6); }
  CHECK(pf.sampleCount() < prof.sampleCount());
  CHECK_NEAR(pf.gmax(), 0.005, 1e-12);   // floor
})

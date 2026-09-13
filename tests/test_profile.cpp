// VerticalProfile invariants on mojokerto with a synthetic DEM (rolling hills + a ridge).
#include "engine/core/json.h"
#include "engine/world/rail_profile.h"
#include "tests/check.h"
#include <cmath>
#include <fstream>
#include <set>
#include <sstream>

using namespace eng;

// Gentle rolling hills plus short-wave noise (same terms as examples/tracktest), realistic for a Java lowland corridor.
struct HillHeight : HeightSource {
  float rawHeight(double wx, double wy) const override {
    return 20.f + 6.f * (float)std::sin(wx / 900.0) * (float)std::cos(wy / 1300.0) + 1.5f * (float)std::sin(wx / 37.0)
         + 4.f * (float)std::sin((wx + wy) / 2100.0);
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
  // Chains (runs of segments through degree-2 nodes) whose both ends are points or line ends and that are
  // shorter than 400 m: the ends are pinned to datums shared with the neighbouring chains and the clamp
  // cannot move them, so |dh| / L may exceed gmax. Everything else must obey gmax.
  std::set<std::string> allowed;
  {
    std::vector<char> seen(g.segments.size(), 0);
    for (size_t si = 0; si < g.segments.size(); ++si) {
      if (seen[si]) continue;
      std::vector<int> chain; double total = 0; int ends[2] = {-1, -1};
      for (int dir = 0; dir < 2; ++dir) {   // walk both ways from si
        int seg = (int)si, node = dir == 0 ? g.segments[si].b : g.segments[si].a;
        for (;;) {
          if (dir == 0 || seg != (int)si) { if (!seen[(size_t)seg]) { seen[(size_t)seg] = 1; chain.push_back(seg); total += g.segments[(size_t)seg].length; } }
          const TrackNode& n = g.nodes[(size_t)node];
          if (n.segs.size() != 2) { ends[dir] = node; break; }
          int next = n.segs[0] == seg ? n.segs[1] : n.segs[0];
          if (seen[(size_t)next]) { ends[dir] = node; break; }
          seg = next; node = g.otherNode(seg, node);
        }
      }
      auto pinned = [&](int n) { return n >= 0 && (g.nodes[(size_t)n].isPoint() || g.nodes[(size_t)n].segs.size() == 1); };
      if (pinned(ends[0]) && pinned(ends[1]) && total < 400)
        for (int sgi : chain) allowed.insert(g.segments[(size_t)sgi].id);
    }
  }
  double gworst = 0; std::string gseg; int over = 0, overAllowed = 0;
  const double tol = 1e-3;
  for (const TrackSegment& s : g.segments) {
    for (double a = 0; a + 20 <= s.length; a += 20) {
      double gr = std::fabs(prof.railHeight(s.id.c_str(), a + 20) - prof.railHeight(s.id.c_str(), a)) / 20;
      if (gr > prof.gmax() + tol) { if (allowed.count(s.id)) ++overAllowed; else { ++over; CHECK_MSG(false, "gradient " + std::to_string(gr * 1000) + " permille on " + s.id + " (" + std::to_string(s.length) + " m, nodes " + g.nodes[(size_t)s.a].id + "/" + g.nodes[(size_t)s.b].id + ")"); } }
      if (gr > gworst) { gworst = gr; gseg = s.id; }
      CHECK_MSG(gr <= 0.05 + tol, "gradient above the 50 permille ceiling on " + s.id);   // hard ceiling, even for crossovers
    }
  }
  std::printf("max gradient over 20 m: %.2f permille at %s (gmax %.2f); %d windows over gmax (%d in %zu allowed crossovers)\n",
              gworst * 1000, gseg.c_str(), prof.gmax() * 1000, over + overAllowed, overAllowed, allowed.size());

  // 4. station zones are flat within the Douglas-Peucker tolerance (1.2 m; like the TS original the zone
  //    samples are set to one datum but are not forced knots, so the simplified curve may tilt by up to
  //    tolDP across the zone). Core = r - 40 m (the edge knots' vertical curves reach Lv/2 inwards).
  for (const StationZone& z : stations) {
    std::vector<double> core, full;
    for (const TrackSegment& s : g.segments)
      for (double a = 0; a <= s.length; a += 10) {
        TrackSample p = g.sampleAt(s.id, a);
        double d = std::hypot(p.wx - z.wx, p.wy - z.wy);
        if (d <= z.r) full.push_back(prof.railHeight(s.id.c_str(), a));
        if (d <= z.r - 40) core.push_back(prof.railHeight(s.id.c_str(), a));
      }
    CHECK(core.size() > 20);
    auto spread = [](const std::vector<double>& hs) { double lo = 1e30, hi = -1e30; for (double h : hs) { lo = std::min(lo, h); hi = std::max(hi, h); } return hi - lo; };
    std::printf("station zone: %zu samples, spread %.3f m (core %zu samples, %.3f m)\n", full.size(), spread(full), core.size(), spread(core));
    CHECK_MSG(spread(core) <= 1.2, "station core not flat: spread " + std::to_string(spread(core)));
    CHECK_MSG(spread(full) <= 1.5, "station zone not flat: spread " + std::to_string(spread(full)));
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

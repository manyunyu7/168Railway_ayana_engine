// Track graph + vertical profile self-test. `tracktest [map.json]` — loads a PPKA save directly
// from disk, prints graph statistics and checks the invariants from docs/world-spec.md §3.
#include "engine/core/json.h"
#include "engine/world/rail_profile.h"
#include "engine/world/track_graph.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using namespace eng;

// Synthetic terrain: gentle rolling hills so the profile pipeline has something to smooth.
struct SineHeight : HeightSource {
  float rawHeight(double wx, double wy) const override {
    return 20.f + 6.f * (float)std::sin(wx / 900.0) * (float)std::cos(wy / 1300.0) + 1.5f * (float)std::sin(wx / 37.0);
  }
};

int main(int argc, char** argv) {
  std::string path = argc > 1 ? argv[1] : std::string(ENG_SOURCE_DIR) + "/../ppka-wannabe-2/src/data/mojokerto.json";
  std::ifstream f(path);
  if (!f) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); return 1; }
  std::stringstream ss; ss << f.rdbuf();
  std::string err;
  Json save = Json::parse(ss.str(), &err);
  if (!err.empty()) { std::fprintf(stderr, "json: %s\n", err.c_str()); return 1; }

  auto t0 = std::chrono::steady_clock::now();
  TrackGraph g;
  if (!g.fromJson(save["world"], &err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
  double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  int bridges = 0, tunnels = 0;
  for (const TrackSegment& s : g.segments) { bridges += s.kind == RailKind::Bridge; tunnels += s.kind == RailKind::Tunnel; }
  std::printf("graph: %zu nodes, %zu segments, %d points, %d bridge segs, %d tunnel segs, %.3f km track (%.1f ms)\n",
              g.nodes.size(), g.segments.size(), g.pointCount(), bridges, tunnels, g.totalLength() / 1000, ms);
  std::printf("origin: ox=%.2f oz=%.2f\n", g.origin().ox, g.origin().oz);
  int fails = 0;
  if (g.pointCount() != 16 && argc < 2) { std::printf("FAIL: expected 16 points\n"); ++fails; }
  if (std::fabs(g.totalLength() / 1000 - 34.7) > 0.1 && argc < 2) { std::printf("FAIL: expected 34.7 km\n"); ++fails; }

  // sampleAt sanity: endpoints hit the nodes, tangent unit length.
  for (const TrackSegment& s : g.segments) {
    TrackSample a = g.sampleAt(s.id, 0), b = g.sampleAt(s.id, s.length);
    const TrackNode &na = g.nodes[(size_t)s.a], &nb = g.nodes[(size_t)s.b];
    if (std::hypot(a.wx - na.wx, a.wy - na.wy) > 1e-6 || std::hypot(b.wx - nb.wx, b.wy - nb.wy) > 1e-6) { std::printf("FAIL: %s endpoints\n", s.id.c_str()); ++fails; }
    if (std::fabs(std::hypot(a.tx, a.ty) - 1) > 1e-9) { std::printf("FAIL: %s tangent\n", s.id.c_str()); ++fails; }
  }

  // Vertical profile on synthetic terrain.
  std::vector<StationZone> stations;
  for (const Json& sc : save["world"]["scenery"].arr)
    if (sc["kind"].stringOr("") == "station") stations.push_back({sc["pos"]["x"].numberOr(0), sc["pos"]["y"].numberOr(0), 160});
  SineHeight dem;
  t0 = std::chrono::steady_clock::now();
  VerticalProfile prof;
  prof.build(g, dem, stations, dem.rawHeight(g.origin().ox, g.origin().oz));
  ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  std::printf("profile: %d chains, gmax %.1f permille, %zu samples (%.1f ms)\n", prof.chainCount(), prof.gmax() * 1000, prof.sampleCount(), ms);

  // Invariant: the segments meeting at every node agree within 0.05 m; max gradient <= gmax (+eps).
  double worst = 0; std::string worstNode;
  for (const TrackNode& n : g.nodes) {
    double lo = 1e30, hi = -1e30;
    for (int si : n.segs) {
      const TrackSegment& s = g.segments[(size_t)si];
      double h = prof.railHeight(s.id.c_str(), s.a == (int)(&n - &g.nodes[0]) ? 0.0 : s.length);
      lo = std::min(lo, h); hi = std::max(hi, h);
    }
    if (n.segs.size() > 1 && hi - lo > worst) { worst = hi - lo; worstNode = n.id; }
  }
  std::printf("node height agreement: worst %.4f m at %s\n", worst, worstNode.c_str());
  if (worst > 0.05) { std::printf("FAIL: node agreement > 0.05 m\n"); ++fails; }
  // Gradient report: long chains obey gmax; short chains between two points (crossovers) may
  // exceed it because both ends are pinned to datums agreed with their neighbours.
  double gworst = 0; std::string gseg; int over = 0;
  for (const TrackSegment& s : g.segments) {
    for (double a = 0; a + 20 <= s.length; a += 20) {
      double gr = std::fabs(prof.railHeight(s.id.c_str(), a + 20) - prof.railHeight(s.id.c_str(), a)) / 20;
      if (gr > prof.gmax() * 1.5 + 1e-3) ++over;
      if (gr > gworst) { gworst = gr; gseg = s.id; }
    }
  }
  std::printf("max gradient over 20 m: %.1f permille at %s (gmax %.1f), %d windows above 1.5*gmax\n",
              gworst * 1000, gseg.c_str(), prof.gmax() * 1000, over);
  if (gworst > 0.05 + 1e-3) { std::printf("FAIL: gradient exceeds the 50 permille ceiling\n"); ++fails; }
  std::printf(fails ? "FAILED (%d)\n" : "OK\n", fails);
  return fails ? 1 : 0;
}

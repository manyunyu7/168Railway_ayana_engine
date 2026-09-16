// 1-D vertical rail profile (docs/world-spec.md §3.4, port of ppka-wannabe-2 src/tiga/uji3dProfil.ts).
// Chains of segments joined through degree-2 nodes are sampled every 20 m from the raw DEM,
// bridge/tunnel runs are straightened, the result is Gaussian-smoothed, flattened at stations,
// pinned at chain ends (so all segments at a point agree), simplified (Douglas–Peucker),
// gradient-clamped and rounded with parabolic vertical curves.
// Parallel chains within one roadbed width are paired (jodohkanBadan): followers copy the leader's height, blind
// runs are shared, and point nodes take their datum from the roadbed chains.
#pragma once
#include "engine/world/height_source.h"
#include "engine/world/track_graph.h"
#include <vector>

namespace eng {

struct StationZone { double wx, wy, r; };   // r = PERON_PANJANG * 0.8 = 160 m

class VerticalProfile : public RailProfile {
public:
  struct Options {
    double step = 20, sigma = 120, tolDP = 1.2, baseline = 1000, margin = 1.3;
    double gradFloor = 0.005, gradCeil = 0.05, radiusVertical = 4000, rampStation = 150;
  };

  // demBase: raw DEM at the origin; railHeight() returns (raw - demBase) so it is a scene y.
  void build(const TrackGraph& g, const HeightSource& dem, const std::vector<StationZone>& stations,
             float demBase, const Options& opt);
  void build(const TrackGraph& g, const HeightSource& dem, const std::vector<StationZone>& stations, float demBase);

  float railHeight(const char* segId, double s) const override;
  float railHeight(int seg, double s) const { return (float)(rawHeight(seg, s) - demBase_); }
  double rawHeight(int seg, double s) const;   // raw DEM metres
  bool built() const { return g_ != nullptr; }

  int chainCount() const { return chains_; }
  double gmax() const { return gmax_; }
  size_t sampleCount() const { return samples_; }
  float demBase() const { return demBase_; }

private:
  const TrackGraph* g_ = nullptr;
  std::vector<std::vector<double>> segH_;   // per segment, uniform samples from s = 0 to length
  float demBase_ = 0;
  int chains_ = 0; double gmax_ = 0; size_t samples_ = 0;
};

// Constant-height source for tests and until a DEM is available.
struct FlatHeight : HeightSource {
  float h;
  explicit FlatHeight(float height = 0) : h(height) {}
  float rawHeight(double, double) const override { return h; }
};

} // namespace eng

#include "engine/world/rail_profile.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <unordered_set>

namespace eng {

namespace {

struct Link { int seg; bool reversed; double L, s0; int enterNode; };
struct Chain { std::vector<Link> links; double total = 0; int nodeStart = -1, nodeEnd = -1; };

// Walk from every node of degree != 2 through degree-2 nodes; chains break at points and ends.
std::vector<Chain> buildChains(const TrackGraph& g) {
  std::vector<char> left((size_t)g.segments.size(), 1);
  std::vector<Chain> out;
  auto degree = [&](int n) { return (int)g.nodes[(size_t)n].segs.size(); };
  auto trace = [&](int segStart, int nodeStart) {
    Chain c; c.nodeStart = nodeStart;
    int seg = segStart, node = nodeStart; double s0 = 0; int end = nodeStart;
    for (;;) {
      if (!left[(size_t)seg]) break;
      left[(size_t)seg] = 0;
      const TrackSegment& s = g.segments[(size_t)seg];
      bool rev = s.b == node;
      double L = s.length;
      c.links.push_back({seg, rev, L, s0, node});
      s0 += L;
      int other = rev ? s.a : s.b;
      end = other;
      if (degree(other) != 2) break;
      int next = -1;
      for (int x : g.nodes[(size_t)other].segs) if (x != seg) { next = x; break; }
      if (next < 0 || !left[(size_t)next]) break;
      seg = next; node = other;
    }
    c.total = s0; c.nodeEnd = end;
    if (!c.links.empty()) out.push_back(std::move(c));
  };
  for (size_t ni = 0; ni < g.nodes.size(); ++ni) {
    const TrackNode& n = g.nodes[ni];
    if (n.segs.size() == 2) continue;
    std::vector<int> segs = n.segs;
    for (int sid : segs) if (left[(size_t)sid]) trace(sid, (int)ni);
  }
  for (size_t si = 0; si < g.segments.size(); ++si)
    if (left[si]) trace((int)si, g.segments[si].a);   // pure loops
  return out;
}

void localAt(const Chain& c, double s, int& seg, double& sLocal) {
  size_t lo = 0, hi = c.links.size() - 1;
  while (lo < hi) { size_t mid = (lo + hi + 1) >> 1; if (c.links[mid].s0 <= s) lo = mid; else hi = mid - 1; }
  const Link& m = c.links[lo];
  double d = std::max(0.0, std::min(m.L, s - m.s0));
  seg = m.seg; sLocal = m.reversed ? m.L - d : d;
}

std::vector<double> gauss(const std::vector<double>& h, double sigmaSamples) {
  if (sigmaSamples < 0.5 || h.size() < 3) return h;
  int r = std::max(1, (int)std::ceil(sigmaSamples * 3));
  std::vector<double> k((size_t)(2 * r + 1)); double sum = 0;
  for (int i = -r; i <= r; ++i) { double w = std::exp(-(double)(i * i) / (2 * sigmaSamples * sigmaSamples)); k[(size_t)(i + r)] = w; sum += w; }
  for (double& w : k) w /= sum;
  std::vector<double> out(h.size());
  int n = (int)h.size();
  for (int i = 0; i < n; ++i) {
    double s = 0;
    for (int j = -r; j <= r; ++j) { int idx = std::max(0, std::min(n - 1, i + j)); s += h[(size_t)idx] * k[(size_t)(j + r)]; }
    out[(size_t)i] = s;
  }
  return out;
}

std::vector<int> douglasPeucker(const std::vector<double>& s, const std::vector<double>& h, double tol, const std::vector<int>& forced) {
  int n = (int)s.size();
  std::vector<int> out;
  if (n < 3) { for (int i = 0; i < n; ++i) out.push_back(i); return out; }
  std::vector<char> keep((size_t)n, 0);
  keep[0] = keep[(size_t)n - 1] = 1;
  for (int i : forced) if (i > 0 && i < n - 1) keep[(size_t)i] = 1;
  std::vector<int> bounds;
  for (int i = 0; i < n; ++i) if (keep[(size_t)i]) bounds.push_back(i);
  std::vector<std::pair<int, int>> stack;
  for (size_t b = 0; b + 1 < bounds.size(); ++b) stack.push_back({bounds[b], bounds[b + 1]});
  while (!stack.empty()) {
    auto [a, z] = stack.back(); stack.pop_back();
    if (z - a < 2) continue;
    double ds = s[(size_t)z] - s[(size_t)a], dh = h[(size_t)z] - h[(size_t)a];
    double L = std::hypot(ds, dh); if (L == 0) L = 1;
    double far = 0; int idx = -1;
    for (int i = a + 1; i < z; ++i) {
      double d = std::fabs((h[(size_t)i] - h[(size_t)a]) * ds - (s[(size_t)i] - s[(size_t)a]) * dh) / L;
      if (d > far) { far = d; idx = i; }
    }
    if (far > tol && idx > 0) { keep[(size_t)idx] = 1; stack.push_back({a, idx}); stack.push_back({idx, z}); }
  }
  for (int i = 0; i < n; ++i) if (keep[(size_t)i]) out.push_back(i);
  return out;
}

void clampGradient(const std::vector<double>& s, std::vector<double>& h, const std::vector<char>& locked, double gmax) {
  int n = (int)s.size();
  for (int iter = 0; iter < 80; ++iter) {
    bool changed = false; bool fwd = iter % 2 == 0;
    for (int t = 0; t + 1 < n; ++t) {
      int i = fwd ? t : n - 2 - t;
      double ds = s[(size_t)i + 1] - s[(size_t)i]; if (ds <= 0) continue;
      double maxD = gmax * ds, d = h[(size_t)i + 1] - h[(size_t)i];
      double over = std::fabs(d) - maxD; if (over <= 1e-6) continue;
      changed = true;
      double dir = d > 0 ? 1 : -1;
      bool a = locked[(size_t)i], b = locked[(size_t)i + 1];
      if (a && b) continue;
      if (a) h[(size_t)i + 1] -= dir * over;
      else if (b) h[(size_t)i] += dir * over;
      else { h[(size_t)i + 1] -= dir * over / 2; h[(size_t)i] += dir * over / 2; }
    }
    if (!changed) break;
  }
}

double smoothstep01(double t) { t = std::max(0.0, std::min(1.0, t)); return t * t * (3 - 2 * t); }

double lerpSample(const std::vector<double>& s, const std::vector<double>& v, double x) {
  size_t n = s.size();
  if (n == 0) return 0;
  if (x <= s[0]) return v[0];
  if (x >= s[n - 1]) return v[n - 1];
  double step = s[n - 1] / (double)(n - 1);
  double f = x / step; size_t i = std::min(n - 2, (size_t)std::floor(f)); double w = f - (double)i;
  return v[i] * (1 - w) + v[i + 1] * w;
}

struct ChainData { Chain chain; std::vector<double> s, h; std::vector<char> blind; std::vector<double> px, py; };

// Per-chain knot curve (after DP, clamp, vertical curves).
struct Curve {
  std::vector<double> ks, kh, grad, Lv;
  double at(double s) const {
    size_t m = ks.size();
    if (m == 0) return 0;
    if (m == 1) return kh[0];
    size_t lo = 0, hi = m - 2;
    while (lo < hi) { size_t mid = (lo + hi + 1) >> 1; if (ks[mid] <= s) lo = mid; else hi = mid - 1; }
    size_t j = lo;
    if (j > 0 && Lv[j] > 0 && s < ks[j] + Lv[j] / 2) {
      double t0 = ks[j] - Lv[j] / 2, d = s - t0;
      return kh[j] + grad[j - 1] * (s - ks[j]) + ((grad[j] - grad[j - 1]) / (2 * Lv[j])) * d * d;
    }
    if (j + 1 < m - 1 && Lv[j + 1] > 0 && s > ks[j + 1] - Lv[j + 1] / 2) {
      double t0 = ks[j + 1] - Lv[j + 1] / 2, d = s - t0;
      return kh[j + 1] + grad[j] * (s - ks[j + 1]) + ((grad[j + 1] - grad[j]) / (2 * Lv[j + 1])) * d * d;
    }
    return kh[j] + grad[j] * (s - ks[j]);
  }
};

} // namespace

void VerticalProfile::build(const TrackGraph& g, const HeightSource& dem, const std::vector<StationZone>& stations,
                            float demBase, const Options& opt) {
  g_ = &g; demBase_ = demBase;
  segH_.assign(g.segments.size(), {});
  std::vector<Chain> chains = buildChains(g);
  chains_ = (int)chains.size();

  // 1. sample the DEM along each chain, mark blind (bridge/tunnel) samples
  std::vector<ChainData> data; data.reserve(chains.size());
  samples_ = 0;
  for (Chain& c : chains) {
    ChainData d; d.chain = std::move(c);
    int n = std::max(1, (int)std::lround(d.chain.total / opt.step));
    d.blind.assign((size_t)n + 1, 0);
    for (int i = 0; i <= n; ++i) {
      double sc = d.chain.total * i / n;
      int seg; double sl; localAt(d.chain, sc, seg, sl);
      TrackSample p = g.sampleAt(seg, sl);
      d.s.push_back(sc); d.px.push_back(p.wx); d.py.push_back(p.wy);
      d.h.push_back(dem.rawHeight(p.wx, p.wy));
      if (g.segments[(size_t)seg].kind != RailKind::Ground) d.blind[(size_t)i] = 1;
    }
    samples_ += d.s.size();
    data.push_back(std::move(d));
  }

  // 4. blind runs -> straight line between their edges
  for (ChainData& d : data) {
    int n = (int)d.h.size(), i = 0;
    while (i < n) {
      if (!d.blind[(size_t)i]) { ++i; continue; }
      int j = i; while (j < n && d.blind[(size_t)j]) ++j;
      int left = i - 1, right = j;
      double hL = left >= 0 ? d.h[(size_t)left] : (right < n ? d.h[(size_t)right] : d.h[(size_t)i]);
      double hR = right < n ? d.h[(size_t)right] : hL;
      double sL = left >= 0 ? d.s[(size_t)left] : d.s[(size_t)i];
      double sR = right < n ? d.s[(size_t)right] : d.s[(size_t)j - 1];
      double span = sR - sL;
      for (int k = i; k < j; ++k) d.h[(size_t)k] = span > 0 ? hL + (hR - hL) * ((d.s[(size_t)k] - sL) / span) : hL;
      i = j;
    }
  }

  // 5. Gaussian smoothing in the length domain
  std::vector<std::vector<double>> smooth; smooth.reserve(data.size());
  for (ChainData& d : data) smooth.push_back(gauss(d.h, opt.sigma / opt.step));

  // 6. calibrate gmax from the corridor itself (p98 of |dh/ds| over 1 km windows)
  std::vector<double> samples;
  int w = (int)std::lround(opt.baseline / opt.step);
  for (size_t di = 0; di < data.size(); ++di) {
    const ChainData& d = data[di]; const std::vector<double>& hh = smooth[di];
    if ((int)d.s.size() <= w) continue;
    for (size_t i = 0; i + (size_t)w < d.s.size(); ++i)
      samples.push_back(std::fabs(hh[i + (size_t)w] - hh[i]) / (d.s[i + (size_t)w] - d.s[i]));
  }
  std::sort(samples.begin(), samples.end());
  double p98 = samples.empty() ? 0 : samples[(size_t)std::floor((double)samples.size() * 0.98)];
  gmax_ = std::min(opt.gradCeil, std::max(opt.gradFloor, p98 * opt.margin));

  // 7. station flattening: datum = median of smoothed heights within r, over all chains
  std::vector<std::vector<double>> collect(stations.size());
  std::vector<std::vector<int>> zone(data.size());
  std::vector<std::vector<double>> weight(data.size());
  for (size_t di = 0; di < data.size(); ++di) {
    const ChainData& d = data[di];
    zone[di].assign(d.s.size(), -1); weight[di].assign(d.s.size(), 0);
    for (size_t i = 0; i < d.s.size(); ++i) {
      for (size_t k = 0; k < stations.size(); ++k) {
        double dist = std::hypot(d.px[i] - stations[k].wx, d.py[i] - stations[k].wy);
        if (dist > stations[k].r + opt.rampStation) continue;
        double t = dist <= stations[k].r ? 1 : 1 - (dist - stations[k].r) / opt.rampStation;
        double ww = t * t * (3 - 2 * t);
        if (ww <= weight[di][i]) continue;
        weight[di][i] = ww; zone[di][i] = (int)k;
        if (dist <= stations[k].r) collect[k].push_back(smooth[di][i]);
      }
    }
  }
  std::vector<double> datum(stations.size(), std::numeric_limits<double>::quiet_NaN());
  for (size_t k = 0; k < stations.size(); ++k) {
    if (collect[k].empty()) continue;
    std::sort(collect[k].begin(), collect[k].end());
    datum[k] = collect[k][collect[k].size() >> 1];
  }
  for (size_t di = 0; di < data.size(); ++di)
    for (size_t i = 0; i < smooth[di].size(); ++i) {
      int k = zone[di][i];
      if (k < 0 || std::isnan(datum[(size_t)k])) continue;
      double ww = weight[di][i];
      smooth[di][i] = smooth[di][i] * (1 - ww) + datum[(size_t)k] * ww;
    }

  // 8. pins: chain ends (shared node datum) and hand-written node heights; exact at the pin,
  //    smoothstep between pins, ramp to zero over RAMP_NODE samples outside the outermost pins.
  const int RAMP_NODE = std::max(2, (int)std::lround(120 / opt.step));
  std::vector<std::unordered_set<int>> lockedIdx(data.size());
  auto pinAll = [&](std::vector<std::vector<double>>& series, bool lock) {
    std::map<int, std::vector<double>> nodeVals;
    for (size_t di = 0; di < data.size(); ++di) {
      const std::vector<double>& hh = series[di];
      nodeVals[data[di].chain.nodeStart].push_back(hh.front());
      nodeVals[data[di].chain.nodeEnd].push_back(hh.back());
    }
    std::map<int, double> nodeDatum;
    for (auto& [id, a] : nodeVals) {
      const TrackNode& n = g.nodes[(size_t)id];
      if (n.hasHeight) { nodeDatum[id] = n.height; continue; }
      double sum = 0; for (double v : a) sum += v;
      nodeDatum[id] = sum / (double)a.size();
    }
    for (size_t di = 0; di < data.size(); ++di) {
      const ChainData& d = data[di]; std::vector<double>& hh = series[di];
      int n = (int)hh.size();
      struct Pin { int i; double target; };
      std::vector<Pin> pins;
      auto addPin = [&](double sChain, double target) {
        int i = std::max(0, std::min(n - 1, (int)std::lround((sChain / d.chain.total) * (n - 1))));
        pins.push_back({i, target});
      };
      addPin(0, nodeDatum[d.chain.nodeStart]);
      for (const Link& lk : d.chain.links) {
        if (lk.s0 <= 0) continue;
        const TrackNode& nn = g.nodes[(size_t)lk.enterNode];
        if (nn.hasHeight) addPin(lk.s0, nn.height);
      }
      addPin(d.chain.total, nodeDatum[d.chain.nodeEnd]);
      std::stable_sort(pins.begin(), pins.end(), [](const Pin& a, const Pin& b) { return a.i < b.i; });
      std::vector<Pin> tidy;
      for (const Pin& p : pins) { if (!tidy.empty() && tidy.back().i == p.i) tidy.back() = p; else tidy.push_back(p); }
      if (tidy.empty()) continue;
      std::vector<double> diff; for (const Pin& p : tidy) diff.push_back(p.target - hh[(size_t)p.i]);
      std::vector<double> corr((size_t)n, 0);
      size_t last = tidy.size() - 1;
      for (int i = 0; i < n; ++i) {
        if (i <= tidy[0].i) {
          int R = std::min(RAMP_NODE, tidy[0].i);
          corr[(size_t)i] = R <= 0 ? diff[0] : diff[0] * smoothstep01(std::max(0.0, (double)(i - (tidy[0].i - R)) / R));
        } else if (i >= tidy[last].i) {
          int R = std::min(RAMP_NODE, n - 1 - tidy[last].i);
          corr[(size_t)i] = R <= 0 ? diff[last] : diff[last] * smoothstep01(std::max(0.0, (double)((tidy[last].i + R) - i) / R));
        } else {
          size_t j = 0;
          while (j + 1 < tidy.size() && tidy[j + 1].i <= i) ++j;
          double t = (double)(i - tidy[j].i) / std::max(1, tidy[j + 1].i - tidy[j].i);
          corr[(size_t)i] = diff[j] + (diff[j + 1] - diff[j]) * smoothstep01(t);
        }
      }
      for (int i = 0; i < n; ++i) hh[(size_t)i] += corr[(size_t)i];
      if (lock) for (const Pin& p : tidy) lockedIdx[di].insert(p.i);
    }
  };
  pinAll(smooth, true);

  // 9-11. simplify -> clamp -> vertical curves, per chain
  std::vector<Curve> curves(data.size());
  for (size_t di = 0; di < data.size(); ++di) {
    const ChainData& d = data[di]; const std::vector<double>& hh = smooth[di];
    int n = (int)d.s.size();
    std::vector<int> forced;
    for (int i = 1; i < n; ++i)
      if (d.blind[(size_t)i] != d.blind[(size_t)i - 1] || zone[di][(size_t)i] != zone[di][(size_t)i - 1]) { forced.push_back(i - 1); forced.push_back(i); }
    for (int i : lockedIdx[di]) forced.push_back(i);
    std::vector<int> idx;
    if (n >= 3) idx = douglasPeucker(d.s, hh, opt.tolDP, forced); else for (int i = 0; i < n; ++i) idx.push_back(i);
    Curve& c = curves[di];
    std::vector<char> locked(idx.size(), 0);
    for (size_t i = 0; i < idx.size(); ++i) {
      c.ks.push_back(d.s[(size_t)idx[i]]); c.kh.push_back(hh[(size_t)idx[i]]);
      if (zone[di][(size_t)idx[i]] >= 0 && weight[di][(size_t)idx[i]] >= 0.999) locked[i] = 1;
      if (lockedIdx[di].count(idx[i])) locked[i] = 1;
    }
    clampGradient(c.ks, c.kh, locked, gmax_);
    size_t m = c.ks.size();
    c.grad.assign(m > 0 ? m - 1 : 0, 0);
    for (size_t i = 0; i + 1 < m; ++i) { double ds = c.ks[i + 1] - c.ks[i]; c.grad[i] = (c.kh[i + 1] - c.kh[i]) / (ds != 0 ? ds : 1); }
    c.Lv.assign(m, 0);
    for (size_t i = 1; i + 1 < m; ++i) {
      double dg = std::fabs(c.grad[i] - c.grad[i - 1]);
      c.Lv[i] = std::min({opt.radiusVertical * dg, 0.8 * (c.ks[i] - c.ks[i - 1]), 0.8 * (c.ks[i + 1] - c.ks[i])});
    }
  }

  // Re-pin on the final values (5c in the original), then pour back per segment.
  std::vector<std::vector<double>> final_(data.size());
  for (size_t di = 0; di < data.size(); ++di) {
    final_[di].resize(data[di].s.size());
    for (size_t i = 0; i < data[di].s.size(); ++i) final_[di][i] = curves[di].at(data[di].s[i]);
  }
  pinAll(final_, false);

  for (size_t di = 0; di < data.size(); ++di) {
    const ChainData& d = data[di];
    for (const Link& lk : d.chain.links) {
      int cnt = std::max(1, (int)std::lround(lk.L / opt.step));
      std::vector<double>& hl = segH_[(size_t)lk.seg];
      hl.clear(); hl.reserve((size_t)cnt + 1);
      for (int i = 0; i <= cnt; ++i) {
        double local = lk.L * i / cnt;
        double sc = lk.s0 + (lk.reversed ? lk.L - local : local);
        hl.push_back(lerpSample(d.s, final_[di], sc));
      }
    }
  }
}

void VerticalProfile::build(const TrackGraph& g, const HeightSource& dem, const std::vector<StationZone>& stations, float demBase) {
  build(g, dem, stations, demBase, Options());
}

double VerticalProfile::rawHeight(int seg, double s) const {
  if (!g_ || seg < 0 || (size_t)seg >= segH_.size()) return demBase_;
  const std::vector<double>& h = segH_[(size_t)seg];
  size_t n = h.size();
  if (n == 0) return demBase_;
  if (n == 1) return h[0];
  double L = g_->segments[(size_t)seg].length;
  if (s <= 0) return h[0];
  if (s >= L) return h[n - 1];
  double step = L / (double)(n - 1);
  double f = s / step; size_t i = std::min(n - 2, (size_t)std::floor(f)); double w = f - (double)i;
  return h[i] * (1 - w) + h[i + 1] * w;
}

float VerticalProfile::railHeight(const char* segId, double s) const {
  if (!g_) return 0;
  int i = g_->segIndex(segId);
  return i < 0 ? 0.f : railHeight(i, s);
}

} // namespace eng

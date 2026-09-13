#include "engine/world/track_graph.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace eng {

namespace {
void normalise(double& x, double& y) { double l = std::hypot(x, y); if (l > 0) { x /= l; y /= l; } }
}

bool TrackGraph::fromJson(const Json& in, std::string* error) {
  const Json* graph = &in;
  if (in.has("world")) graph = &in["world"];
  if (graph->has("graph")) graph = &(*graph)["graph"];
  if (!(*graph)["nodes"].isArray() || !(*graph)["segments"].isArray()) {
    if (error) *error = "track graph: missing nodes/segments";
    return false;
  }
  nodes.clear(); segments.clear(); nodeIdx_.clear(); segIdx_.clear();

  for (const Json& nd : (*graph)["nodes"].arr) {
    TrackNode n;
    n.id = nd["id"].stringOr("");
    n.wx = nd["x"].numberOr(0); n.wy = nd["y"].numberOr(0);
    if (nd["tinggi"].isNumber()) { n.hasHeight = true; n.height = nd["tinggi"].num; }
    nodeIdx_[n.id] = (int)nodes.size();
    nodes.push_back(std::move(n));
  }
  for (const Json& sd : (*graph)["segments"].arr) {
    TrackSegment s;
    s.id = sd["id"].stringOr("");
    s.a = nodeIndex(sd["a"].stringOr("")); s.b = nodeIndex(sd["b"].stringOr(""));
    if (s.a < 0 || s.b < 0) { if (error) *error = "track graph: segment " + s.id + " references a missing node"; return false; }
    s.straight = sd["straight"].boolOr(false);
    std::string jr = sd["jenisRel"].stringOr("");
    s.kind = jr == "jembatan" ? RailKind::Bridge : jr == "terowongan" ? RailKind::Tunnel : RailKind::Ground;
    std::string jj = sd["jenisJembatan"].stringOr("");
    s.bridge = jj == "dek" ? BridgeShape::Deck : jj == "rangka" ? BridgeShape::Truss : jj == "viaduk" ? BridgeShape::Viaduct : BridgeShape::Auto;
    int idx = (int)segments.size();
    segIdx_[s.id] = idx;
    nodes[(size_t)s.a].segs.push_back(idx);
    nodes[(size_t)s.b].segs.push_back(idx);
    segments.push_back(std::move(s));
  }
  // Junctions: recomputed from topology, then setting/spring restored from the file.
  size_t ni = 0;
  for (const Json& nd : (*graph)["nodes"].arr) {
    refreshJunction((int)ni);
    TrackNode& n = nodes[ni++];
    if (n.isPoint() && nd["junction"].isObject()) {
      int st = nd["junction"]["setting"].intOr(0);
      n.setting = st == 1 ? 1 : 0;
      if (nd["junction"]["spring"].isNumber()) n.spring = nd["junction"]["spring"].intOr(0) == 1 ? 1 : 0;
    }
  }
  for (size_t i = 0; i < segments.size(); ++i) buildGeometry((int)i);

  // Origin = centre of the bounding box of all track nodes (§2.2).
  if (nodes.empty()) { origin_ = {}; return true; }
  double x0 = std::numeric_limits<double>::infinity(), y0 = x0, x1 = -x0, y1 = -x0;
  for (const TrackNode& n : nodes) { x0 = std::min(x0, n.wx); x1 = std::max(x1, n.wx); y0 = std::min(y0, n.wy); y1 = std::max(y1, n.wy); }
  origin_.ox = (x0 + x1) / 2; origin_.oz = (y0 + y1) / 2;
  return true;
}

void TrackGraph::refreshJunction(int ni) {
  TrackNode& n = nodes[(size_t)ni];
  n.facingSeg = -1; n.legs[0] = n.legs[1] = -1; n.setting = 0; n.spring = -1;
  if (n.segs.size() != 3) return;
  double dx[3], dy[3];
  for (int i = 0; i < 3; ++i) {
    const TrackNode& on = nodes[(size_t)otherNode(n.segs[(size_t)i], ni)];
    dx[i] = on.wx - n.wx; dy[i] = on.wy - n.wy; normalise(dx[i], dy[i]);
  }
  int best = 0; double bestScore = std::numeric_limits<double>::infinity();
  for (int i = 0; i < 3; ++i) {
    double score = 0;
    for (int j = 0; j < 3; ++j) if (j != i) score += dx[i] * dx[j] + dy[i] * dy[j];
    if (score < bestScore) { bestScore = score; best = i; }
  }
  n.facingSeg = n.segs[(size_t)best];
  int k = 0;
  for (int i = 0; i < 3; ++i) if (i != best) n.legs[k++] = n.segs[(size_t)i];
}

void TrackGraph::nodeTangentAxis(int ni, double& tx, double& ty) const {
  const TrackNode& n = nodes[(size_t)ni];
  if (!n.isPoint()) {
    for (int si : n.segs) {
      const TrackSegment& s = segments[(size_t)si];
      if (!s.straight) continue;
      tx = nodes[(size_t)s.b].wx - nodes[(size_t)s.a].wx; ty = nodes[(size_t)s.b].wy - nodes[(size_t)s.a].wy;
      normalise(tx, ty); return;
    }
  }
  if (n.segs.size() == 1) {
    const TrackNode& on = nodes[(size_t)otherNode(n.segs[0], ni)];
    tx = on.wx - n.wx; ty = on.wy - n.wy; normalise(tx, ty); return;
  }
  if (n.isPoint()) {
    const TrackNode& fn = nodes[(size_t)otherNode(n.facingSeg, ni)];
    tx = n.wx - fn.wx; ty = n.wy - fn.wy; normalise(tx, ty); return;
  }
  if (n.segs.size() == 2) {
    const TrackNode& p1 = nodes[(size_t)otherNode(n.segs[0], ni)];
    const TrackNode& p2 = nodes[(size_t)otherNode(n.segs[1], ni)];
    tx = p2.wx - p1.wx; ty = p2.wy - p1.wy; normalise(tx, ty); return;
  }
  tx = 1; ty = 0;
}

void TrackGraph::buildGeometry(int si) {
  TrackSegment& seg = segments[(size_t)si];
  const double pax = nodes[(size_t)seg.a].wx, pay = nodes[(size_t)seg.a].wy;
  const double pbx = nodes[(size_t)seg.b].wx, pby = nodes[(size_t)seg.b].wy;
  const double d = std::hypot(pbx - pax, pby - pay);
  if (seg.straight) {
    seg.cp1x = pax + (pbx - pax) / 3; seg.cp1y = pay + (pby - pay) / 3;
    seg.cp2x = pax + (pbx - pax) * 2 / 3; seg.cp2y = pay + (pby - pay) * 2 / 3;
  } else {
    double tax, tay, tbx, tby;
    nodeTangentAxis(seg.a, tax, tay); nodeTangentAxis(seg.b, tbx, tby);
    if (tax * (pbx - pax) + tay * (pby - pay) < 0) { tax = -tax; tay = -tay; }
    if (tbx * (pax - pbx) + tby * (pay - pby) < 0) { tbx = -tbx; tby = -tby; }
    seg.cp1x = pax + tax * d * CP_K; seg.cp1y = pay + tay * d * CP_K;
    seg.cp2x = pbx + tbx * d * CP_K; seg.cp2y = pby + tby * d * CP_K;
  }
  const int count = std::min(240, std::max(10, (int)std::ceil(d / 4)));
  seg.lut.clear(); seg.lut.reserve((size_t)count + 1);
  double sAcc = 0, prevx = pax, prevy = pay;
  seg.bx0 = seg.by0 = std::numeric_limits<double>::infinity(); seg.bx1 = seg.by1 = -seg.bx0;
  for (int i = 0; i <= count; ++i) {
    const double t = (double)i / count, u = 1 - t;
    const double a = u * u * u, b = 3 * u * u * t, c = 3 * u * t * t, e = t * t * t;
    const double px = a * pax + b * seg.cp1x + c * seg.cp2x + e * pbx;
    const double py = a * pay + b * seg.cp1y + c * seg.cp2y + e * pby;
    double tx = 3 * u * u * (seg.cp1x - pax) + 6 * u * t * (seg.cp2x - seg.cp1x) + 3 * t * t * (pbx - seg.cp2x);
    double ty = 3 * u * u * (seg.cp1y - pay) + 6 * u * t * (seg.cp2y - seg.cp1y) + 3 * t * t * (pby - seg.cp2y);
    normalise(tx, ty);
    if (i > 0) sAcc += std::hypot(px - prevx, py - prevy);
    seg.lut.push_back({px, py, tx, ty, sAcc});
    prevx = px; prevy = py;
    seg.bx0 = std::min(seg.bx0, px); seg.bx1 = std::max(seg.bx1, px);
    seg.by0 = std::min(seg.by0, py); seg.by1 = std::max(seg.by1, py);
  }
  seg.length = sAcc;
}

TrackSample TrackGraph::sampleAt(int si, double sMeter) const {
  const TrackSegment& g = segments[(size_t)si];
  const double s = std::max(0.0, std::min(g.length, sMeter));
  size_t lo = 0, hi = g.lut.size() - 1;
  while (lo + 1 < hi) {
    size_t mid = (lo + hi) >> 1;
    if (g.lut[mid].s <= s) lo = mid; else hi = mid;
  }
  const TrackSegment::Lut &s0 = g.lut[lo], &s1 = g.lut[hi];
  const double span = s1.s - s0.s;
  const double f = span < 1e-9 ? 0 : (s - s0.s) / span;
  TrackSample r;
  r.wx = s0.px + (s1.px - s0.px) * f; r.wy = s0.py + (s1.py - s0.py) * f;
  r.tx = s0.tx + (s1.tx - s0.tx) * f; r.ty = s0.ty + (s1.ty - s0.ty) * f;
  normalise(r.tx, r.ty);
  return r;
}

int TrackGraph::pointCount() const {
  int n = 0; for (const TrackNode& nd : nodes) if (nd.isPoint()) ++n; return n;
}

double TrackGraph::totalLength() const {
  double t = 0; for (const TrackSegment& s : segments) t += s.length; return t;
}

} // namespace eng

#include "engine/sim/sim_state.h"

namespace eng {

SimState parseSimState(const Json& j) {
  SimState st;
  st.clock = j["clock"].numberOr(0);
  st.score = j["score"].intOr(0);
  st.violations = j["violations"].intOr(0);
  st.pending = j["pending"].intOr(0);
  for (const Json& t : j["trains"].arr) {
    SimTrain tr;
    tr.id = t["id"].stringOr(""); tr.no = t["no"].stringOr(""); tr.name = t["name"].stringOr("");
    tr.consist = t["consist"].stringOr(""); tr.state = t["state"].stringOr(""); tr.hold = t["hold"].stringOr("");
    tr.seg = t["seg"].stringOr(""); tr.kelas = t["kelas"].intOr(0);
    tr.speed = (float)t["speed"].numberOr(0); tr.s = (float)t["s"].numberOr(0);
    tr.x = t["x"].numberOr(0); tr.y = t["y"].numberOr(0);
    tr.heading = (float)t["heading"].numberOr(0); tr.length = (float)t["len"].numberOr(0);
    tr.dir = t["dir"].intOr(1);
    tr.tungguS40 = t["tungguS40"].boolOr(false); tr.s40Siap = t["s40Siap"].boolOr(false);
    tr.istirahat = t["istirahat"].boolOr(false);
    for (const Json& v : t["vehicles"].arr) {
      SimVehicle vh;
      vh.model = v["model"].stringOr(""); vh.kind = v["kind"].stringOr("");
      vh.x = v["x"].numberOr(0); vh.y = v["y"].numberOr(0);
      vh.heading = (float)v["heading"].numberOr(0); vh.length = (float)v["len"].numberOr(0);
      vh.sarana = v["sarana"].stringOr(""); vh.seg = v["seg"].stringOr(""); vh.seg2 = v["seg2"].stringOr(vh.seg);
      vh.s = (float)v["s"].numberOr(0); vh.s2 = (float)v["s2"].numberOr(vh.s);
      vh.x1 = v["x1"].numberOr(vh.x); vh.y1 = v["y1"].numberOr(vh.y);
      vh.x2 = v["x2"].numberOr(vh.x); vh.y2 = v["y2"].numberOr(vh.y);
      vh.t1 = (float)v["t1"].numberOr(vh.heading); vh.t2 = (float)v["t2"].numberOr(vh.heading);
      tr.vehicles.push_back(std::move(vh));
    }
    st.trains.push_back(std::move(tr));
  }
  for (const Json& p : j["points"].arr)
    st.points.push_back({p["id"].stringOr(""), p["locked"].stringOr(""), p["setting"].intOr(0)});
  for (const Json& s : j["signals"].arr)
    st.signals.push_back({s["id"].stringOr(""), s["aspect"].stringOr("")});
  for (const Json& r : j["routes"].arr) {
    SimRoute rt;
    rt.id = r["id"].stringOr(""); rt.entry = r["entry"].stringOr(""); rt.exit = r["exit"].stringOr(""); rt.exitLabel = r["exitLabel"].stringOr("");
    for (const Json& sg : r["segs"].arr) rt.segs.push_back(sg.stringOr(""));
    for (const Json& sg : r["released"].arr) rt.released.push_back(sg.stringOr(""));
    for (const Json& jn : r["junctions"].arr) if (jn.size() >= 2) rt.junctions.emplace_back(jn[0].stringOr(""), jn[1].stringOr(""));
    st.routes.push_back(std::move(rt));
  }
  for (const Json& o : j["occupancy"].arr) {
    SimOccupancy oc; oc.seg = o["seg"].stringOr("");
    for (const Json& iv : o["iv"].arr) oc.intervals.push_back({iv[0].stringOr(""), (float)iv[1].numberOr(0), (float)iv[2].numberOr(0)});
    st.occupancy.push_back(std::move(oc));
  }
  for (const Json& o : j["langsir"].arr) {
    SimLangsir lg; lg.lok = o["lok"].stringOr(""); lg.kendali = o["kendali"].stringOr("auto"); lg.legIdx = o["legIdx"].intOr(0);
    for (const Json& leg : o["legs"].arr) { std::vector<std::string> segs; for (const Json& sg : leg.arr) segs.push_back(sg.stringOr("")); lg.legs.push_back(std::move(segs)); }
    st.langsir.push_back(std::move(lg));
  }
  for (const Json& o : j["jpl"].arr) st.jpl.push_back({o["id"].stringOr(""), o["closed"].boolOr(false)});
  for (const Json& l : j["log"].arr)
    st.log.push_back({l["t"].numberOr(0), l["kind"].stringOr(""), l["text"].stringOr("")});
  return st;
}

// ---- panel layout (bridge `panel`) ----
static PanelObj parsePanelObj(const Json& o) {
  PanelObj p;
  p.id = o["id"].stringOr(""); p.kind = o["kind"].stringOr(""); p.name = o["name"].stringOr(""); p.seg = o["seg"].stringOr("");
  p.signalType = o["signalType"].stringOr(""); p.station = o["station"].stringOr("");
  p.s = (float)o["s"].numberOr(0); p.x = (float)o["x"].numberOr(0); p.y = (float)o["y"].numberOr(0);
  p.tx = (float)o["tx"].numberOr(1); p.ty = (float)o["ty"].numberOr(0);
  p.dir = o["dir"].intOr(1); p.lampu = o["lampu"].intOr(3); p.jalur = o["jalur"].intOr(0);
  return p;
}

PanelLayout parsePanelLayout(const Json& j) {
  PanelLayout L;
  if (!j["ok"].boolOr(false)) return L;
  L.yScale = (float)j["yScale"].numberOr(3);
  L.x0 = (float)j["bbox"]["x0"].numberOr(0); L.y0 = (float)j["bbox"]["y0"].numberOr(0);
  L.x1 = (float)j["bbox"]["x1"].numberOr(0); L.y1 = (float)j["bbox"]["y1"].numberOr(0);
  for (const Json& sg : j["segments"].arr) {
    PanelSeg ps; ps.id = sg["id"].stringOr(""); ps.a = sg["a"].stringOr(""); ps.b = sg["b"].stringOr("");
    ps.sepur = sg["sepur"].stringOr(""); ps.jalur = sg["jalur"].intOr(0); ps.len = (float)sg["len"].numberOr(0);
    for (const Json& v : sg["pts"].arr) ps.pts.push_back((float)v.numberOr(0));
    for (const Json& v : sg["cum"].arr) ps.cum.push_back((float)v.numberOr(0));
    L.segIndex[ps.id] = L.segments.size(); L.segments.push_back(std::move(ps));
  }
  for (const Json& p : j["points"].arr) {
    PanelPoint pp; pp.id = p["id"].stringOr(""); pp.facing = p["facing"].stringOr("");
    pp.legs[0] = p["legs"][0].stringOr(""); pp.legs[1] = p["legs"][1].stringOr("");
    pp.x = (float)p["x"].numberOr(0); pp.y = (float)p["y"].numberOr(0); L.points.push_back(pp);
  }
  for (const Json& o : j["signals"].arr) L.signals.push_back(parsePanelObj(o));
  for (const Json& o : j["berths"].arr) L.berths.push_back(parsePanelObj(o));
  for (const Json& o : j["portals"].arr) L.portals.push_back(parsePanelObj(o));
  for (const Json& s : j["stations"].arr)
    L.stations.push_back({s["code"].stringOr(""), s["label"].stringOr(""), (float)s["x0"].numberOr(0), (float)s["y0"].numberOr(0), (float)s["x1"].numberOr(0), (float)s["y1"].numberOr(0)});
  for (const Json& s : j["jalur"].arr) L.jalur.push_back({s["station"].stringOr(""), s["n"].intOr(0), (float)s["x"].numberOr(0), (float)s["y"].numberOr(0)});
  L.ok = true;
  return L;
}

bool PanelLayout::posOnSeg(const std::string& id, float s, float& x, float& y, float& tx, float& ty) const {
  const PanelSeg* sg = seg(id);
  if (!sg || sg->cum.size() < 2 || sg->pts.size() < 4) return false;
  size_t n = sg->cum.size();
  float sc = std::fmax(0.f, std::fmin(sg->cum[n - 1], s));
  size_t i = 1; while (i < n - 1 && sg->cum[i] < sc) ++i;
  float c0 = sg->cum[i - 1], c1 = sg->cum[i], f = c1 > c0 ? (sc - c0) / (c1 - c0) : 0;
  float ax = sg->pts[2 * (i - 1)], ay = sg->pts[2 * (i - 1) + 1], bx = sg->pts[2 * i], by = sg->pts[2 * i + 1];
  x = ax + (bx - ax) * f; y = ay + (by - ay) * f;
  float dx = bx - ax, dy = by - ay, l = std::sqrt(dx * dx + dy * dy);
  if (l < 1e-6f) { tx = 1; ty = 0; } else { tx = dx / l; ty = dy / l; }
  return true;
}

} // namespace eng

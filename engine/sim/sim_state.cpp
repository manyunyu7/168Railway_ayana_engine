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
    st.routes.push_back(std::move(rt));
  }
  for (const Json& o : j["occupancy"].arr) {
    SimOccupancy oc; oc.seg = o["seg"].stringOr("");
    for (const Json& iv : o["iv"].arr) oc.intervals.push_back({iv[0].stringOr(""), (float)iv[1].numberOr(0), (float)iv[2].numberOr(0)});
    st.occupancy.push_back(std::move(oc));
  }
  for (const Json& o : j["jpl"].arr) st.jpl.push_back({o["id"].stringOr(""), o["closed"].boolOr(false)});
  for (const Json& l : j["log"].arr)
    st.log.push_back({l["t"].numberOr(0), l["kind"].stringOr(""), l["text"].stringOr("")});
  return st;
}

} // namespace eng

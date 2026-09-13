// SimProcess round trip against the TypeScript simulator (needs Node + npx tsx; skips otherwise).
#include "engine/sim/sim_process.h"
#include "tests/check.h"
#include <cmath>
#include <cstdlib>
#include <cstdio>

using namespace eng;

int main() {
  if (std::system("command -v npx >/dev/null 2>&1") != 0) { std::printf("SKIP: npx not found\n"); return test::SKIP; }
  SimProcess sim;
  SimProcess::Options opt; opt.inheritStderr = false;
  if (!sim.start(opt)) { std::printf("SKIP: bridge failed to start: %s\n", sim.error().c_str()); return test::SKIP; }
  CHECK(sim.running()); CHECK(sim.startupMs() > 0);

  // load
  const Json& loaded = sim.load("mojokerto");
  CHECK_MSG(loaded["ok"].boolOr(false), loaded["error"].stringOr(sim.error()));
  CHECK(loaded["map"].stringOr("") == "mojokerto");
  const Json& sum = sim.summary();
  CHECK(sum["geo"].boolOr(false));
  CHECK(sum["nodes"].size() == 358); CHECK(sum["segments"].size() == 364); CHECK(sum["points"].size() == 16);
  CHECK(sum["stations"].size() == 1); CHECK(sum["stations"][0]["code"].stringOr("") == "MR");
  CHECK(sum["signals"].size() > 10);
  CHECK(sim.world()["graph"]["nodes"].size() == 358);   // raw save kept
  double segLen = 0; for (const Json& s : sum["segments"].arr) segLen += s["len"].numberOr(0);
  CHECK_NEAR(segLen / 1000, 34.703, 0.01);
  // summary points carry facing/legs consistent with the graph
  for (const Json& p : sum["points"].arr) { CHECK(p["legs"].size() == 2); CHECK(!p["facing"].stringOr("").empty()); }

  // start 07:05, AI off
  const Json& started = sim.startSession(false, "07:05");
  CHECK_MSG(started["ok"].boolOr(false), started["error"].stringOr(""));
  CHECK(!started["ai"].boolOr(true));
  CHECK_NEAR(started["clock"].numberOr(0), 7 * 3600 + 5 * 60, 0.5);
  int trainsAtStart = started["trains"].intOr(-1); CHECK(trainsAtStart >= 0);

  // step 60 s in 0.5 s steps
  for (int i = 0; i < 120; ++i) { const Json& r = sim.step(0.5); CHECK_MSG(r["ok"].boolOr(false), sim.error()); }
  const SimState& st = sim.state();
  CHECK_NEAR(st.clock, 7 * 3600 + 5 * 60 + 60, 0.6);
  CHECK(st.points.size() == 16);
  CHECK(st.signals.size() == sum["signals"].size());
  for (const SimSignal& s : st.signals) CHECK(s.aspect == "red" || s.aspect == "yellow" || s.aspect == "green");
  CHECK(!st.trains.empty());
  for (const SimTrain& t : st.trains) {
    CHECK(!t.id.empty()); CHECK(!t.seg.empty()); CHECK(t.dir == 1 || t.dir == -1);
    CHECK(t.x > 1e7 && t.x < 1.3e7);   // Web Mercator metres
    CHECK(t.length > 0); CHECK(!t.vehicles.empty());
    CHECK_NEAR(t.vehicles.front().x1, t.x, 0.5);   // front coupler of the first car = train front
    float sumLen = 0; for (const SimVehicle& v : t.vehicles) { sumLen += v.length; CHECK(std::hypot(v.x1 - v.x2, v.y1 - v.y2) <= v.length + 0.5); }
    CHECK_NEAR(sumLen, t.length, 1.0);
  }
  std::printf("07:06: %zu trains, %zu points, %zu signals, %zu routes\n", st.trains.size(), st.points.size(), st.signals.size(), st.routes.size());
  // AI off: nobody set a route
  CHECK(st.routes.empty());

  // click_signal: an interlocking signal whose path needs a point set the other way -> rejection with the point named
  std::string sigWesel, sigAny;
  for (const Json& s : sum["signals"].arr) if (s["signalType"].stringOr("") == "interlocking") { sigAny = s["id"].stringOr(""); break; }
  CHECK(!sigAny.empty());
  {
    const Json& c = sim.clickSignal("no-such-signal");
    CHECK(!c["ok"].boolOr(true)); CHECK(!c["reason"].stringOr("").empty() || !c["error"].stringOr("").empty());
  }
  // a non-interlocking signal (auto/muka) is refused with the exact game wording
  for (const Json& s : sum["signals"].arr) if (s["signalType"].stringOr("") != "interlocking" && s["signalType"].stringOr("") != "bersama") {
    const Json& c = sim.clickSignal(s["id"].stringOr(""));
    CHECK(!c["ok"].boolOr(true)); CHECK(c["reason"].stringOr("") == "bukan sinyal interlocking");
    break;
  }
  // every interlocking signal: the response is either a set route, a request, or a rejection with a reason
  int set = 0, rejected = 0;
  for (const Json& s : sum["signals"].arr) {
    if (s["signalType"].stringOr("") != "interlocking") continue;
    std::string id = s["id"].stringOr("");
    Json c = sim.clickSignal(id);
    if (c["ok"].boolOr(false)) { ++set; CHECK(c["status"].stringOr("") == "set" || c["status"].stringOr("") == "minta"); if (c["status"].stringOr("") == "set") { Json cc = sim.clickSignal(id); CHECK(cc["ok"].boolOr(false) && cc["action"].stringOr("") == "cancel"); } }
    else { ++rejected; CHECK_MSG(!c["reason"].stringOr("").empty(), id + " rejected without a reason"); if (c["reason"].stringOr("") == "wesel belum mengarah ke jalur ini" && sigWesel.empty()) sigWesel = id; }
  }
  std::printf("click_signal: %d set, %d rejected; wrong-way point example: %s\n", set, rejected, sigWesel.c_str());
  CHECK(set + rejected > 0);
  CHECK_MSG(!sigWesel.empty(), "expected at least one signal blocked by a point set the other way");
  // pinned: signal t766 at 07:06 with the save's point settings is blocked by a point set the other way
  { Json c = sim.clickSignal("t766");
    CHECK_MSG(c["reason"].stringOr("") == "wesel belum mengarah ke jalur ini", "t766: " + c["reason"].stringOr("(ok)"));
    std::printf("t766 -> reason \"%s\" wesel %s\n", c["reason"].stringOr("").c_str(), c["wesel"].stringOr("").c_str());
    CHECK(c["wesel"].stringOr("") == "n587"); }
  if (!sigWesel.empty()) {
    Json c = sim.clickSignal(sigWesel);
    CHECK(!c["ok"].boolOr(true)); CHECK(c["action"].stringOr("") == "set");
    std::string wesel = c["wesel"].stringOr(""); CHECK(!wesel.empty());
    CHECK(c["path"].isArray());
    // preview agrees
    Json pv = sim.preview(sigWesel);
    CHECK(pv["ok"].boolOr(false)); CHECK(pv["manual"].boolOr(false)); CHECK(pv["cand"].isNull()); CHECK(pv["wesel"].stringOr("") == wesel);
    // flip that point, then the click goes through (or is rejected for another reason, never the same point)
    Json fp = sim.flipPoint(wesel);
    CHECK_MSG(fp["ok"].boolOr(false), fp["reason"].stringOr("flip failed"));
    int setting = fp["setting"].intOr(-1); CHECK(setting == 0 || setting == 1);
    Json c2 = sim.clickSignal(sigWesel);
    CHECK(c2["ok"].boolOr(false) || c2["wesel"].stringOr("") != wesel);
    if (c2["ok"].boolOr(false) && c2["status"].stringOr("") == "set") {
      Json cr = sim.cancelRoute(sigWesel); CHECK(cr["ok"].boolOr(false));
      Json fp2 = sim.flipPoint(wesel); CHECK(fp2["ok"].boolOr(false)); CHECK(fp2["setting"].intOr(-1) == 1 - setting);
    }
  }
  // flip_point on a fresh point toggles and is reflected by step()
  {
    std::string pid = sum["points"][0]["id"].stringOr("");
    int before = -1; for (const SimPoint& p : sim.state().points) if (p.id == pid) before = p.setting;
    Json fp = sim.flipPoint(pid);
    if (fp["ok"].boolOr(false)) {
      CHECK(fp["setting"].intOr(-1) == 1 - before);
      sim.step(0.1);
      int after = -1; for (const SimPoint& p : sim.state().points) if (p.id == pid) after = p.setting;
      CHECK(after == 1 - before);
      CHECK(sim.flipPoint(pid)["ok"].boolOr(false));
    } else CHECK(!fp["reason"].stringOr("").empty());   // locked by a route set above
    CHECK(!sim.flipPoint("no-such-point")["ok"].boolOr(true));
  }

  // panel layout
  const PanelLayout& pl = sim.panel();
  CHECK(pl.ok); CHECK(!pl.segments.empty()); CHECK(pl.points.size() == 16); CHECK(!pl.signals.empty()); CHECK(pl.stations.size() == 1);
  CHECK(pl.x1 > pl.x0 && pl.y1 > pl.y0);
  CHECK(pl.seg(sum["segments"][0]["id"].stringOr("")) != nullptr); CHECK(pl.seg("nope") == nullptr);
  { const PanelSeg* ps = pl.seg(sum["segments"][0]["id"].stringOr("")); CHECK(ps && ps->pts.size() >= 4 && ps->cum.size() * 2 == ps->pts.size());
    float x, y, tx, ty; CHECK(pl.posOnSeg(ps->id, ps->len / 2, x, y, tx, ty)); CHECK_NEAR(std::hypot(tx, ty), 1, 1e-3); CHECK(!pl.posOnSeg("nope", 0, x, y, tx, ty)); }
  CHECK(&sim.panel() == &pl);   // cached

  // set_time_scale / set_clock round trip
  { Json r = sim.setTimeScale(4); CHECK_MSG(r["ok"].boolOr(false), r["error"].stringOr("")); 
    double c0 = sim.state().clock; sim.step(1.0); CHECK_NEAR(sim.state().clock - c0, 4, 0.6);   // bridge scales real dt
    r = sim.setTimeScale(1); CHECK(r["ok"].boolOr(false)); c0 = sim.state().clock; sim.step(1.0); CHECK_NEAR(sim.state().clock - c0, 1, 0.6); }
  { Json r = sim.setClock("09:30"); CHECK_MSG(r["ok"].boolOr(false), r["error"].stringOr(""));
    sim.step(0.5); CHECK_NEAR(sim.state().clock, 9 * 3600 + 30 * 60 + 0.5, 1.0);
    CHECK(sim.state().routes.empty()); }   // session restarted

  // raw command + seq echo, error on unknown command
  { Json r = sim.command("{\"cmd\":\"state\",\"seq\":42}"); CHECK(r["ok"].boolOr(false)); CHECK(r["seq"].intOr(0) == 42); CHECK(r["trains"].isArray()); }
  { Json r = sim.command("{\"cmd\":\"bogus\"}"); CHECK(!r["ok"].boolOr(true)); }
  CHECK(SimProcess::escape("a\"b\\c\n") == "a\\\"b\\\\c\\n");

  sim.stop(); CHECK(!sim.running());
  std::printf("%s: %d checks, %d failures\n", __FILE__, test::checks, test::failures);
  return test::failures ? 1 : 0;
}

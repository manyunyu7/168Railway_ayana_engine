// Example 3: sim bridge test. `simtest [map] [clock]` — loads a PPKA map through the
// TypeScript engine (child process), runs 10 simulated minutes with AI PPKA on,
// prints trains every simulated minute, then flips one point and clicks one signal.
#include "engine/sim/sim_process.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace eng;

static void printTrains(const SimState& st) {
  int h = (int)st.clock / 3600 % 24, m = (int)st.clock / 60 % 60, s = (int)st.clock % 60;
  std::printf("[%02d:%02d:%02d] %zu trains (pending %d, score %d)\n", h, m, s, st.trains.size(), st.pending, st.score);
  for (const SimTrain& t : st.trains) {
    std::printf("  KA %-6s %-22.22s %-5s %5.1f km/h  %s@%.1f  xy=(%.2f, %.2f) hdg=%.2f  %zu veh",
                t.no.c_str(), t.name.c_str(), t.state.c_str(), t.speed * 3.6, t.seg.c_str(), t.s, t.x, t.y, t.heading,
                t.vehicles.size());
    if (!t.vehicles.empty()) std::printf(" [%s ...]", t.vehicles[0].model.c_str());
    if (!t.hold.empty()) std::printf("  (%s)", t.hold.c_str());
    std::printf("\n");
  }
  for (const SimLogLine& l : st.log) std::printf("  log %8.1f %s\n", l.time, l.text.c_str());
}

int main(int argc, char** argv) {
  std::string map = argc > 1 ? argv[1] : "kroya";
  std::string clock = argc > 2 ? argv[2] : "";

  SimProcess sim;
  if (!sim.start()) { std::fprintf(stderr, "start failed: %s\n", sim.error().c_str()); return 1; }
  std::printf("bridge ready in %.0f ms\n", sim.startupMs());

  const Json& loaded = sim.load(map);
  if (!loaded["ok"].boolOr(false)) { std::fprintf(stderr, "load failed: %s\n", loaded["error"].stringOr(sim.error()).c_str()); return 1; }
  const Json& sum = sim.summary();   // kept by SimProcess; loaded[] is overwritten by the next command
  std::printf("loaded %s in %.0f ms (%zu bytes): %zu nodes, %zu segments, %zu points, %zu signals, %zu stations\n",
              map.c_str(), sim.lastRoundTripMs(), sim.lastResponseBytes(), sum["nodes"].size(), sum["segments"].size(),
              sum["points"].size(), sum["signals"].size(), sum["stations"].size());

  const Json& started = sim.startSession(true, clock);
  if (!started["ok"].boolOr(false)) { std::fprintf(stderr, "start failed: %s\n", started["error"].stringOr("").c_str()); return 1; }
  std::printf("session started: clock %.0f, %d trains on line, %d pending\n", started["clock"].numberOr(0),
              started["trains"].intOr(0), started["pending"].intOr(0));

  // 10 simulated minutes in 0.5 s steps; report every simulated minute.
  const double dt = 0.5; const int stepsPerMinute = (int)std::lround(60.0 / dt);
  std::vector<double> ms; std::vector<size_t> bytes;
  for (int minute = 0; minute < 10; ++minute) {
    for (int i = 0; i < stepsPerMinute; ++i) {
      const Json& r = sim.step(dt);
      if (!r["ok"].boolOr(false)) { std::fprintf(stderr, "step failed: %s\n", sim.error().c_str()); return 1; }
      ms.push_back(sim.lastRoundTripMs()); bytes.push_back(sim.lastResponseBytes());
    }
    printTrains(sim.state());
  }

  // Pick a point and a signal from the static summary and operate them.
  std::string pointId = sum["points"][0]["id"].stringOr("");
  std::string signalId;
  for (const Json& s : sum["signals"].arr)
    if (s["signalType"].stringOr("") == "interlocking") { signalId = s["id"].stringOr(""); break; }

  const Json& fp = sim.flipPoint(pointId);
  std::printf("flip_point %s -> ok=%d setting=%d reason=%s (%.2f ms)\n", pointId.c_str(), (int)fp["ok"].boolOr(false),
              fp["setting"].intOr(-1), fp["reason"].stringOr("-").c_str(), sim.lastRoundTripMs());
  const Json& cs = sim.clickSignal(signalId);
  std::printf("click_signal %s -> ok=%d action=%s status=%s reason=%s route=%s (%.2f ms)\n", signalId.c_str(),
              (int)cs["ok"].boolOr(false), cs["action"].stringOr("-").c_str(), cs["status"].stringOr("-").c_str(),
              cs["reason"].stringOr("-").c_str(), cs["route"].stringOr("-").c_str(), sim.lastRoundTripMs());

  // Latency / size statistics.
  std::sort(ms.begin(), ms.end()); std::sort(bytes.begin(), bytes.end());
  double sum_ = 0; for (double v : ms) sum_ += v;
  size_t n = ms.size();
  std::printf("\nstep(): n=%zu avg=%.2f ms p50=%.2f ms p95=%.2f ms max=%.2f ms | state JSON min=%zu p50=%zu max=%zu bytes\n",
              n, sum_ / (double)n, ms[n / 2], ms[n * 95 / 100], ms[n - 1], bytes[0], bytes[n / 2], bytes[n - 1]);
  sim.stop();
  return 0;
}

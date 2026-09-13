#include "engine/sim/sim_process.h"
#include <cerrno>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef ENG_SOURCE_DIR
#define ENG_SOURCE_DIR "."
#endif

namespace eng {

using Clock = std::chrono::steady_clock;
static double msSince(Clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

std::string SimProcess::escape(const std::string& s) {
  std::string o; o.reserve(s.size() + 2);
  for (char c : s) {
    switch (c) {
      case '"': o += "\\\""; break; case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break; case '\r': o += "\\r"; break; case '\t': o += "\\t"; break;
      default:
        if ((unsigned char)c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += c;
    }
  }
  return o;
}

SimProcess::~SimProcess() { stop(); }

bool SimProcess::start(const Options& opt) {
  stop();
  err_.clear();
  std::string ppka = opt.ppkaRoot;
  if (ppka.empty()) { if (const char* e = std::getenv("PPKA_ROOT")) ppka = e; }
  if (ppka.empty()) ppka = std::string(ENG_SOURCE_DIR) + "/../ppka-wannabe-2";
  std::string script = opt.bridgeScript.empty() ? std::string(ENG_SOURCE_DIR) + "/bridge/sim-bridge.ts" : opt.bridgeScript;
  if (access((ppka + "/src/engine/world.ts").c_str(), R_OK) != 0) { err_ = "PPKA repo not found at " + ppka; return false; }
  if (access(script.c_str(), R_OK) != 0) { err_ = "bridge script not found: " + script; return false; }

  int in[2], out[2]; // in: parent -> child stdin, out: child stdout -> parent
  if (pipe(in) != 0 || pipe(out) != 0) { err_ = std::string("pipe: ") + std::strerror(errno); return false; }

  auto t0 = Clock::now();
  pid_t pid = fork();
  if (pid < 0) { err_ = std::string("fork: ") + std::strerror(errno); return false; }
  if (pid == 0) {
    // child: wire pipes, chdir to the PPKA repo (so `npx tsx` and the engine's
    // node_modules resolve there), exec.
    dup2(in[0], STDIN_FILENO); dup2(out[1], STDOUT_FILENO);
    close(in[0]); close(in[1]); close(out[0]); close(out[1]);
    if (!opt.inheritStderr) { int nul = open("/dev/null", O_WRONLY); if (nul >= 0) { dup2(nul, STDERR_FILENO); close(nul); } }
    if (chdir(ppka.c_str()) != 0) _exit(126);
    setenv("PPKA_ROOT", ppka.c_str(), 1);
    execlp("npx", "npx", "tsx", script.c_str(), (char*)nullptr);
    _exit(127);
  }
  close(in[0]); close(out[1]);
  pid_ = pid; toChild_ = in[1]; fromChild_ = out[0];
  signal(SIGPIPE, SIG_IGN); // a dead child must not kill us on write()

  std::string line;
  if (!readLine(line)) { err_ = "bridge exited before ready: " + err_; stop(); return false; }
  std::string perr;
  Json ready = Json::parse(line, &perr);
  if (!ready["ready"].boolOr(false)) { err_ = "unexpected first line from bridge: " + line; stop(); return false; }
  startupMs_ = msSince(t0);
  return true;
}

void SimProcess::stop() {
  if (pid_ <= 0) return;
  if (toChild_ >= 0) { (void)writeLine("{\"cmd\":\"quit\"}"); close(toChild_); toChild_ = -1; }
  // Drain until EOF so the bridge's final "bye" line never hits a closed pipe (EPIPE).
  if (fromChild_ >= 0) { std::string line; while (readLine(line)) {} close(fromChild_); fromChild_ = -1; }
  int status = 0;
  // Give it a moment to exit on its own, then insist.
  for (int i = 0; i < 50; ++i) {
    if (waitpid(pid_, &status, WNOHANG) == pid_) { pid_ = -1; return; }
    usleep(20 * 1000);
  }
  kill(pid_, SIGTERM);
  waitpid(pid_, &status, 0);
  pid_ = -1;
}

bool SimProcess::writeLine(const std::string& line) {
  std::string s = line; s += '\n';
  size_t off = 0;
  while (off < s.size()) {
    ssize_t n = write(toChild_, s.data() + off, s.size() - off);
    if (n < 0) { if (errno == EINTR) continue; err_ = std::string("write: ") + std::strerror(errno); return false; }
    off += (size_t)n;
  }
  return true;
}

bool SimProcess::readLine(std::string& out) {
  for (;;) {
    size_t nl = buf_.find('\n');
    if (nl != std::string::npos) { out.assign(buf_, 0, nl); buf_.erase(0, nl + 1); return true; }
    char chunk[1 << 16];
    ssize_t n = read(fromChild_, chunk, sizeof chunk);
    if (n < 0) { if (errno == EINTR) continue; err_ = std::string("read: ") + std::strerror(errno); return false; }
    if (n == 0) { err_ = "bridge closed stdout"; return false; }
    buf_.append(chunk, (size_t)n);
  }
}

const Json& SimProcess::command(const std::string& jsonLine) {
  last_ = Json{};
  if (pid_ <= 0) { err_ = "bridge not running"; return last_; }
  auto t0 = Clock::now();
  std::string line;
  if (!writeLine(jsonLine) || !readLine(line)) { lastMs_ = msSince(t0); return last_; }
  lastMs_ = msSince(t0);
  lastBytes_ = line.size();
  std::string perr;
  last_ = Json::parse(line, &perr);
  if (!perr.empty()) { err_ = "bad JSON from bridge: " + perr; last_ = Json{}; }
  return last_;
}

const Json& SimProcess::load(const std::string& map) {
  const Json& r = command("{\"cmd\":\"load\",\"map\":\"" + escape(map) + "\"}");
  if (r["ok"].boolOr(false)) { world_ = r["world"]; summary_ = r["summary"]; panel_ = PanelLayout{}; }
  return r;
}

const Json& SimProcess::startSession(bool ai, const std::string& clock) {
  std::string c = std::string("{\"cmd\":\"start\",\"ai\":") + (ai ? "true" : "false");
  if (!clock.empty()) c += ",\"clock\":\"" + escape(clock) + "\"";
  return command(c + "}");
}

const Json& SimProcess::step(double dt) {
  char b[128]; std::snprintf(b, sizeof b, "{\"cmd\":\"step\",\"dt\":%.6g}", dt);
  const Json& r = command(b);
  if (r["ok"].boolOr(false)) parseState(r);
  return r;
}

const Json& SimProcess::clickSignal(const std::string& id) { return command("{\"cmd\":\"click_signal\",\"id\":\"" + escape(id) + "\"}"); }
const Json& SimProcess::flipPoint(const std::string& id) { return command("{\"cmd\":\"flip_point\",\"id\":\"" + escape(id) + "\"}"); }
const Json& SimProcess::setRoute(const std::string& from, const std::string& to) {
  return command("{\"cmd\":\"set_route\",\"from\":\"" + escape(from) + "\",\"to\":\"" + escape(to) + "\"}");
}
const Json& SimProcess::cancelRoute(const std::string& signalId) { return command("{\"cmd\":\"cancel_route\",\"signal\":\"" + escape(signalId) + "\"}"); }
const Json& SimProcess::routes(const std::string& signalId) { return command("{\"cmd\":\"routes\",\"id\":\"" + escape(signalId) + "\"}"); }
const Json& SimProcess::preview(const std::string& signalId) { return command("{\"cmd\":\"preview\",\"signal\":\"" + escape(signalId) + "\"}"); }

const Json& SimProcess::setTimeScale(double k) { char b[96]; std::snprintf(b, sizeof b, "{\"cmd\":\"set_time_scale\",\"k\":%.6g}", k); return command(b); }
const Json& SimProcess::setClock(const std::string& c) { return command("{\"cmd\":\"set_clock\",\"clock\":\"" + escape(c) + "\"}"); }
const Json& SimProcess::beriS40(const std::string& t) { return command("{\"cmd\":\"beri_s40\",\"train\":\"" + escape(t) + "\"}"); }
const Json& SimProcess::hapusKA(const std::string& t) { return command("{\"cmd\":\"hapus_ka\",\"train\":\"" + escape(t) + "\"}"); }
const Json& SimProcess::trainDetail(const std::string& t) { return command("{\"cmd\":\"train_detail\",\"train\":\"" + escape(t) + "\"}"); }
const Json& SimProcess::routeMenu(const std::string& s) { return command("{\"cmd\":\"route_menu\",\"signal\":\"" + escape(s) + "\"}"); }

static PanelObj parsePanelObj(const Json& o) {
  PanelObj p;
  p.id = o["id"].stringOr(""); p.kind = o["kind"].stringOr(""); p.name = o["name"].stringOr(""); p.seg = o["seg"].stringOr("");
  p.signalType = o["signalType"].stringOr(""); p.station = o["station"].stringOr("");
  p.s = (float)o["s"].numberOr(0); p.x = (float)o["x"].numberOr(0); p.y = (float)o["y"].numberOr(0);
  p.tx = (float)o["tx"].numberOr(1); p.ty = (float)o["ty"].numberOr(0);
  p.dir = o["dir"].intOr(1); p.lampu = o["lampu"].intOr(3); p.jalur = o["jalur"].intOr(0);
  return p;
}

const PanelLayout& SimProcess::panel() {
  if (panel_.ok) return panel_;
  const Json& j = command("{\"cmd\":\"panel\"}");
  PanelLayout L;
  if (!j["ok"].boolOr(false)) { panel_ = L; return panel_; }
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
  panel_ = std::move(L);
  return panel_;
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

void SimProcess::parseState(const Json& j) {
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
  state_ = std::move(st);
}

} // namespace eng

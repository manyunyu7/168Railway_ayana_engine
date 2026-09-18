// HUD projection ABI (see engine_api.h, "HUD projection" block): screen anchors for the DOM overlays the
// host draws over the canvas — signal name plates (hud3d.ts perbaruiNamaSinyal), station bubbles
// (perbaruiPapanTempat) and their tethers. Pure read-only queries on the last drawn frame; the state
// itself is reached through eng_hud_view() (engine_api_hud.h).
#include "engine/api/engine_api.h"
#include "engine/api/engine_api_hud.h"
#include "engine/app/world_scene.h"
#include <cmath>
#include <cstdio>
#include <string>
#include "engine/api/eng_export.h"
#define KEEP ENG_EXPORT

using namespace eng;

// Weak fallback: until engine_api.cpp defines the bridge every HUD call answers "nothing on screen".
__attribute__((weak)) bool eng_hud_view(EngHudView&) { return false; }

namespace {

constexpr float STATION_BUBBLE_HEIGHT = 55;   // TINGGI_PAPAN (dunia3dKonst.ts): bubble anchor above the carved ground

std::string g_hudBuf;
const char* hudRet(std::string s) { g_hudBuf = std::move(s); return g_hudBuf.c_str(); }

std::string jsonEscape(const std::string& s) {
  std::string o; o.reserve(s.size() + 2);
  for (char c : s) {
    switch (c) {
      case '"': o += "\\\""; break; case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break; case '\r': o += "\\r"; break; case '\t': o += "\\t"; break;
      default: if ((unsigned char)c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; } else o += c;
    }
  }
  return o;
}

struct Projected { float x = 0, y = 0; bool front = false, visible = false; };

Projected project(const EngHudView& v, vec3 p) {
  Projected r;
  vec4 c = v.viewProj * vec4(p, 1);
  if (c.w <= 0) return r;
  r.front = true;
  r.x = (c.x / c.w * 0.5f + 0.5f) * (float)v.w;
  r.y = (1 - (c.y / c.w * 0.5f + 0.5f)) * (float)v.h;
  r.visible = r.x >= 0 && r.x <= (float)v.w && r.y >= 0 && r.y <= (float)v.h && c.z / c.w <= 1;
  return r;
}

// Rail head height at the track point nearest to (wx, wy): every segment LUT is scanned (bbox first),
// then the vertical profile is sampled at that arc length. Falls back to the carved ground far from any track.
float railHeightNear(WorldScene& sc, double wx, double wy, float fallback) {
  const TrackGraph& g = sc.graph();
  if (!sc.profile().built()) return fallback;
  int bestSeg = -1; double bestS = 0, bd = 1e30;
  for (size_t i = 0; i < g.segments.size(); ++i) {
    const TrackSegment& sg = g.segments[i];
    double dx = std::fmax(0.0, std::fmax(sg.bx0 - wx, wx - sg.bx1)), dy = std::fmax(0.0, std::fmax(sg.by0 - wy, wy - sg.by1));
    if (dx * dx + dy * dy >= bd) continue;
    for (const TrackSegment::Lut& l : sg.lut) {
      double ex = l.px - wx, ey = l.py - wy, d = ex * ex + ey * ey;
      if (d < bd) { bd = d; bestSeg = (int)i; bestS = l.s; }
    }
  }
  if (bestSeg < 0 || bd > 250.0 * 250.0) return fallback;
  return sc.profile().railHeight(bestSeg, bestS);
}

} // namespace

extern "C" {

KEEP int eng_project(double wx, double wy, int heightMode, float* out) {
  if (!out) return 0;
  out[0] = out[1] = out[2] = out[3] = 0;
  EngHudView v; if (!eng_hud_view(v) || !v.scene) return 0;
  float ground = v.scene->groundHeight(wx, wy);
  float h = heightMode == 1 ? railHeightNear(*v.scene, wx, wy, ground) : ground;
  vec3 p = v.scene->origin().toScene(wx, wy, h);
  Projected r = project(v, p);
  out[0] = r.x; out[1] = r.y; out[2] = r.visible ? 1.f : 0.f; out[3] = length(p - v.eye);
  return r.front ? 1 : 0;
}

KEEP const char* eng_signal_screen(void) {
  EngHudView v; if (!eng_hud_view(v) || !v.scene) return "[]";
  const SignalVisuals& sv = v.scene->signals();
  std::vector<ScreenPoint> pts = sv.screenPositions(v.viewProj, v.w, v.h, v.eye);
  const std::vector<SignalInstance>& sigs = sv.signals();
  std::string out = "["; out.reserve(pts.size() * 120 + 2);
  for (size_t i = 0; i < pts.size() && i < sigs.size(); ++i) {
    const ScreenPoint& sp = pts[i]; const SignalInstance& s = sigs[i];
    const char* aspect = s.aspect == Aspect::Green ? "green" : s.aspect == Aspect::Yellow ? "yellow" : "red";
    char b[160];
    std::snprintf(b, sizeof b, "%s{\"id\":\"%s\",\"name\":\"%s\",\"type\":\"%s\",\"x\":%.1f,\"y\":%.1f,\"visible\":%s,\"aspect\":\"%s\",\"dist\":%.0f}",
                  out.size() > 1 ? "," : "", jsonEscape(s.id).c_str(), jsonEscape(s.name).c_str(), jsonEscape(s.signalType).c_str(),
                  sp.x, sp.y, sp.visible ? "true" : "false", aspect, length(s.pos - v.eye));
    out += b;
  }
  return hudRet(out + "]");
}

KEEP const char* eng_station_screen(void) {
  EngHudView v; if (!eng_hud_view(v) || !v.scene || !v.world) return "[]";
  std::string out = "[";
  for (const Json& sc : (*v.world)["scenery"].arr) {
    if (sc["kind"].stringOr("") != "station") continue;
    double wx = sc["pos"]["x"].numberOr(0), wy = sc["pos"]["y"].numberOr(0);
    vec3 p = v.scene->origin().toScene(wx, wy, v.scene->groundHeight(wx, wy) + STATION_BUBBLE_HEIGHT);
    Projected r = project(v, p);
    if (!r.front) continue;
    char b[200];
    std::snprintf(b, sizeof b, "%s{\"id\":\"%s\",\"code\":\"%s\",\"name\":\"%s\",\"x\":%.1f,\"y\":%.1f,\"visible\":%s,\"dist\":%.0f}",
                  out.size() > 1 ? "," : "", jsonEscape(sc["id"].stringOr("")).c_str(), jsonEscape(sc["code"].stringOr("")).c_str(),
                  jsonEscape(sc["label"].stringOr("")).c_str(), r.x, r.y, r.visible ? "true" : "false", length(p - v.eye));
    out += b;
  }
  return hudRet(out + "]");
}

} // extern "C"

// Markers ABI (engine_api.h "markers" block; docs/SURVEYOR.md): a batch of world-space editor markers set
// from JSON by the host, drawn each frame after the world overlays (engine_api.cpp calls eng_markers_draw),
// picked by pixel and projected for the host's DOM labels. State: one file-local eng::Markers, reached
// through eng_edit_ctx() for the scene / view like the editing calls.
#include "engine/api/engine_api.h"
#include "engine/api/engine_api_edit.h"
#include "engine/api/engine_api_markers.h"
#include "engine/app/markers.h"
#include "engine/app/world_scene.h"
#include <cmath>
#include <cstdio>
#include <string>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define KEEP EMSCRIPTEN_KEEPALIVE
#else
#define KEEP
#endif

using namespace eng;

namespace {

Markers g_markers;
std::string g_buf;
const char* ret(std::string s) { g_buf = std::move(s); return g_buf.c_str(); }

// Rail head height at the track point nearest to (wx, wy) within 250 m, else `fallback` (the same rule as
// eng_project mode 1 in engine_api_hud.cpp; WorldScene::railHeadNear will replace both once it lands).
float railHead(WorldScene& sc, double wx, double wy, float fallback) {
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

} // namespace

void eng_markers_draw(ModelRenderer& r, vec3 eye, float fovY, int viewportH) { g_markers.draw(r, eye, fovY, viewportH); }
void eng_markers_destroy(void) { g_markers.destroy(); }

extern "C" {

KEEP int eng_markers(const char* json) {
  EngEditCtx c; if (!eng_edit_ctx(c) || !c.ready || !c.scene) { g_markers.clear(); return 0; }
  std::string err; Json a = Json::parse(json && *json ? json : "[]", &err);
  if (!err.empty()) { std::fprintf(stderr, "[ayana] markers: %s\n", err.c_str()); g_markers.clear(); return 0; }
  WorldScene& sc = *c.scene;
  return g_markers.set(a, sc.origin(), [&sc](double wx, double wy, int hm) {
    float ground = sc.groundHeight(wx, wy);
    return hm == 1 ? railHead(sc, wx, wy, ground) : ground;
  }) ? 1 : 0;
}

KEEP int eng_markers_count(void) { return (int)g_markers.count(); }

KEEP const char* eng_markers_pick(float x, float y, float maxPx) {
  EngEditCtx c; if (!eng_edit_ctx(c) || !c.ready || !c.viewValid) return "";
  return ret(g_markers.pick(x * c.dpr, y * c.dpr, (int)(c.w * c.dpr), (int)(c.h * c.dpr), c.viewProj, c.eye, (maxPx > 0 ? maxPx : 16) * c.dpr));
}

KEEP const char* eng_markers_screen(void) {
  EngEditCtx c; if (!eng_edit_ctx(c) || !c.ready || !c.viewValid) return "[]";
  std::string out = "[";
  for (const MarkerScreen& s : g_markers.screen(c.viewProj, c.w, c.h, c.eye)) {
    char b[96]; std::snprintf(b, sizeof b, "%s{\"id\":\"", out.size() > 1 ? "," : "");
    out += b; out += jsonEscape(s.id);
    std::snprintf(b, sizeof b, "\",\"x\":%.1f,\"y\":%.1f,\"d\":%.1f,\"v\":%s}", s.x, s.y, s.dist, s.visible ? "true" : "false");
    out += b;
  }
  out += "]";
  return ret(out);
}

} // extern "C"

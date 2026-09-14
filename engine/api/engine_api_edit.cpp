// Editing ABI (see engine_api.h, "editing" block; docs/SURVEYOR.md): picking, live hiasan / garis / node-height /
// brush edits, the partial rebuilds behind them and the editor overlays. The state is reached through
// eng_edit_ctx() (engine_api_edit.h); the work is WorldScene's (engine/app/world_edit.cpp).
#include "engine/api/engine_api.h"
#include "engine/api/engine_api_edit.h"
#include "engine/app/world_scene.h"
#include <chrono>
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

// Weak fallback: until engine_api.cpp defines the bridge every editing call answers "nothing".
__attribute__((weak)) bool eng_edit_ctx(EngEditCtx&) { return false; }

namespace {

std::string g_editBuf;
const char* editRet(std::string s) { g_editBuf = std::move(s); return g_editBuf.c_str(); }

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

// Ready scene + a drawn frame (picking needs the view).
bool view(EngEditCtx& c) { return eng_edit_ctx(c) && c.ready && c.viewValid && c.scene; }
bool live(EngEditCtx& c) { return eng_edit_ctx(c) && c.ready && c.scene && c.world; }
Ray pixelRay(const EngEditCtx& c, float x, float y) { return screenRay(x / (float)c.w, y / (float)c.h, c.viewProj.inverse()); }
constexpr float NODE_PX = 14, SEG_PX = 12, GARIS_PX = 16;   // editorRel3d.ts nodeDiLayar / pilihRelDi / uji3dSpline garisDi

} // namespace

extern "C" {

// ---- picking ----
KEEP const char* eng_pick_ground(float x, float y) {
  EngEditCtx c; if (!view(c)) return "";
  vec3 hit; if (!c.scene->rayGround(pixelRay(c, x, y), hit)) return "";
  double wx, wy; c.scene->origin().toWorld(hit, wx, wy);
  char b[96]; std::snprintf(b, sizeof b, "%.3f,%.3f,%.3f", wx, wy, hit.y);
  return editRet(b);
}

KEEP float eng_ground(double wx, double wy) {
  EngEditCtx c; if (!eng_edit_ctx(c) || !c.ready) return 0;
  return c.scene->groundHeight(wx, wy);
}

KEEP const char* eng_pick_object(float x, float y) {
  EngEditCtx c; if (!view(c)) return "";
  const float px = x * c.dpr, py = y * c.dpr; const int fw = (int)(c.w * c.dpr), fh = (int)(c.h * c.dpr);
  Ray ray = pixelRay(c, x, y);
  int hi = c.scene->pickHiasan(ray, px, py, fw, fh, c.viewProj, c.eye);
  if (hi >= 0) return editRet("hiasan:" + std::to_string(hi));
  int gi = c.scene->pickGaris(*c.world, px, py, fw, fh, c.viewProj, GARIS_PX * c.dpr);
  if (gi >= 0) return editRet("garis:" + std::to_string(gi));
  std::string sig, pt;
  c.scene->pickAt(px, py, fw, fh, c.viewProj, c.eye, sig, pt);
  if (!sig.empty()) return editRet("signal:" + sig);
  if (!pt.empty()) return editRet("point:" + pt);
  {   // track node within NODE_PX (rail head + 0.6 like posNode3D)
    const TrackGraph& g = c.scene->graph(); float best = NODE_PX * c.dpr; std::string id;
    for (size_t ni = 0; ni < g.nodes.size(); ++ni) {
      const TrackNode& n = g.nodes[ni];
      float h = 0; if (!n.segs.empty()) { const TrackSegment& s = g.segments[(size_t)n.segs[0]]; h = c.scene->profile().railHeight(n.segs[0], s.a == (int)ni ? 0.0 : s.length); }
      vec4 cl = c.viewProj * vec4(c.scene->origin().toScene(n.wx, n.wy, h + 0.6f), 1);
      if (cl.w <= 0) continue;
      float sx = (cl.x / cl.w * 0.5f + 0.5f) * fw, sy = (1 - (cl.y / cl.w * 0.5f + 0.5f)) * fh;
      float d = std::hypot(sx - px, sy - py); if (d < best) { best = d; id = n.id; }
    }
    if (!id.empty()) return editRet("node:" + id);
  }
  int seg; double s; int side;
  if (c.scene->pickTrack(px, py, fw, fh, c.viewProj, SEG_PX * c.dpr, seg, s, side)) {
    char b[160]; std::snprintf(b, sizeof b, "segment:%s:%.2f", c.scene->graph().segments[(size_t)seg].id.c_str(), s);
    return editRet(b);
  }
  return "";
}

KEEP const char* eng_pick_track(float x, float y, float maxPx) {
  EngEditCtx c; if (!view(c)) return "";
  int seg; double s; int side;
  if (!c.scene->pickTrack(x * c.dpr, y * c.dpr, (int)(c.w * c.dpr), (int)(c.h * c.dpr), c.viewProj, (maxPx > 0 ? maxPx : 40) * c.dpr, seg, s, side)) return "";
  char b[160]; std::snprintf(b, sizeof b, "%s,%.2f,%d", c.scene->graph().segments[(size_t)seg].id.c_str(), s, side);
  return editRet(b);
}

// ---- live edits ----
KEEP int eng_hiasan_set(int index, double wx, double wy, float naik, float rotDeg, float skala) {
  EngEditCtx c; if (!live(c)) return 0;
  return c.scene->hiasanSet(*c.world, index, wx, wy, naik, rotDeg, skala);
}
KEEP int eng_hiasan_add(const char* json) {
  EngEditCtx c; if (!live(c) || !json) return -1;
  std::string err; Json o = Json::parse(json, &err);
  if (!err.empty() || !o.isObject()) { std::fprintf(stderr, "[ayana] hiasan_add: %s\n", err.c_str()); return -1; }
  return c.scene->hiasanAdd(*c.world, o);
}
KEEP int eng_hiasan_remove(int index) {
  EngEditCtx c; if (!live(c)) return 0;
  return c.scene->hiasanRemove(*c.world, index);
}
KEEP const char* eng_hiasan_info(int index) {
  EngEditCtx c; if (!live(c)) return "";
  const Json& objs = (*c.world)["hiasan"]["objek"];
  if (index < 0 || index >= (int)objs.size()) return "";
  const Json& o = objs[(size_t)index];
  std::string model = o["model"].stringOr("");
  GpuModel* m = c.scene->catalog().streamedModel(model);
  vec3 sz{0, 0, 0}; if (m) RollingStock::normalizeTransform(*m, true, &sz);
  char b[400];
  std::snprintf(b, sizeof b, "{\"model\":\"%s\",\"x\":%.3f,\"y\":%.3f,\"naik\":%.3f,\"rot\":%.2f,\"skala\":%.3f,\"resident\":%s,\"size\":[%.2f,%.2f,%.2f]}",
                jsonEscape(model).c_str(), o["x"].numberOr(0), o["y"].numberOr(0), o["naik"].numberOr(0), o["rot"].numberOr(0), o["skala"].numberOr(1),
                m ? "true" : "false", sz.x, sz.y, sz.z);
  return editRet(b);
}
KEEP const char* eng_model_size(const char* id) {
  EngEditCtx c; if (!eng_edit_ctx(c) || !c.scene || !id) return "";
  GpuModel* m = c.scene->catalog().streamedModel(id);   // resident only: a palette query must not fetch the whole catalog
  if (!m) return "";
  vec3 sz; RollingStock::normalizeTransform(*m, true, &sz);
  char b[96]; std::snprintf(b, sizeof b, "%.3f,%.3f,%.3f", sz.x, sz.y, sz.z);
  return editRet(b);
}
KEEP int eng_garis_set(const char* json) {
  EngEditCtx c; if (!live(c) || !json) return 0;
  std::string err; Json p = Json::parse(json, &err);
  if (!err.empty()) { std::fprintf(stderr, "[ayana] garis_set: %s\n", err.c_str()); return 0; }
  return c.scene->garisSet(*c.world, p);
}
KEEP int eng_node_height(const char* nodeId, double h, int hasHeight) {
  EngEditCtx c; if (!live(c) || !nodeId) return 0;
  return c.scene->nodeHeight(*c.world, nodeId, h, hasHeight != 0);
}
KEEP double eng_rails_rebuild(void) {
  EngEditCtx c; if (!live(c)) return 0;
  double ms = c.scene->railsRebuild(*c.world, "/assets/font.efnt");
  std::printf("[ayana] rails rebuilt in %.0f ms\n", ms);
  return ms;
}
KEEP int eng_terrain_delta(const char* json) {
  EngEditCtx c; if (!live(c)) return 0;
  std::string err; Json t = Json::parse(json && *json ? json : "null", &err);
  if (!err.empty()) { std::fprintf(stderr, "[ayana] terrain_delta: %s\n", err.c_str()); return 0; }
  return c.scene->terrainDelta(*c.world, t);
}
KEEP int eng_track_edit(const char* worldJson) {
  EngEditCtx c; if (!live(c) || !worldJson) return 0;
  std::string err; Json w = Json::parse(worldJson, &err);
  if (!err.empty() || !w["graph"]["nodes"].size()) { std::fprintf(stderr, "[ayana] track_edit: %s\n", err.empty() ? "no graph.nodes" : err.c_str()); return 0; }
  *c.world = std::move(w);
  auto t0 = std::chrono::steady_clock::now();
  bool ok = c.scene->trackEdit(*c.world, c.map, "/assets/font.efnt", c.summary);
  std::printf("[ayana] track edit: %s, %.0f ms\n", ok ? "rebuilt" : "FAILED", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
  return ok;
}

// ---- overlays ----
KEEP void eng_highlight(const char* id, int mode) {
  EngEditCtx c; if (!eng_edit_ctx(c) || !c.scene) return;
  std::string s = id ? id : ""; size_t k = s.find(':');
  if (mode <= 0 || k == std::string::npos) { c.scene->setHighlight("", -1, WorldScene::HighlightMode::Off); return; }
  c.scene->setHighlight(s.substr(0, k), std::atoi(s.c_str() + k + 1), mode == 2 ? WorldScene::HighlightMode::Locked : WorldScene::HighlightMode::Selected);
}
KEEP void eng_gizmo(const char* kind, double wx, double wy, float h, float yaw, float scale, int axisHover) {
  EngEditCtx c; if (!eng_edit_ctx(c) || !c.scene) return;
  c.scene->setGizmo(kind ? kind : "", c.scene->origin().toScene(wx, wy, h), yaw, scale, axisHover);
}
KEEP int eng_gizmo_hit(float x, float y) {
  EngEditCtx c; if (!view(c)) return 0;
  return c.scene->gizmoHit(pixelRay(c, x, y));
}
KEEP float eng_gizmo_angle(float x, float y) {
  EngEditCtx c; if (!view(c)) return 1e9f;
  float a; return c.scene->gizmoAngle(pixelRay(c, x, y), a) ? a : 1e9f;
}
KEEP int eng_ghost(const char* modelId, double wx, double wy, float rotDeg, float skala) {
  EngEditCtx c; if (!eng_edit_ctx(c) || !c.scene || !c.ready) return 0;
  std::string id = modelId ? modelId : "";
  c.scene->setGhost(id, wx, wy, rotDeg, skala);
  return id.empty() || c.scene->catalog().model(id) != nullptr;   // 0 = the model is still streaming (requested now)
}
KEEP int eng_ukur_line(const char* json) {
  EngEditCtx c; if (!eng_edit_ctx(c) || !c.scene || !c.ready) return 0;
  std::string err; Json a = Json::parse(json && *json ? json : "[]", &err);
  if (!err.empty()) return 0;
  std::vector<std::pair<double, double>> pts;
  for (const Json& p : a.arr) pts.emplace_back(p["x"].numberOr(0), p["y"].numberOr(0));
  c.scene->setUkur(pts);
  return 1;
}

} // extern "C"

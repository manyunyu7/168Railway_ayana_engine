#include "engine/world/meja_board.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>

namespace eng {

// VDU/CTC dark palette (src/render/tema.ts PANEL_GAYA.vdu.gelap)
static const vec4 C_RAIL{0.30f, 0.36f, 0.41f, 1}, C_LURUS{1.0f, 0.67f, 0.14f, 1};
static const vec4 C_ROUTE{0.125f, 0.94f, 0.44f, 1}, C_CAND{0.35f, 0.65f, 1.0f, 1}, C_OCC{0.95f, 0.2f, 0.2f, 1};
static const vec4 C_LABEL{0.37f, 0.44f, 0.51f, 1}, C_PILL{0.10f, 0.14f, 0.18f, 1}, C_PILLINK{0.56f, 0.64f, 0.71f, 1};
static const vec4 C_WHITE{1, 1, 1, 1}, C_TRAIN{0.85f, 0.9f, 1, 1}, C_SEL{0.3f, 0.9f, 1, 1}, C_LOCK{1, 0.6f, 0.2f, 1};

void panelToScreen(const PanelLayout& L, const PanelCamera& cam, const PanelFrame& f, float px, float py, float& sx, float& sy) {
  sx = f.x + f.w / 2 + (px - cam.x) * cam.zoom;
  sy = f.y + f.h / 2 + (py - cam.y) * cam.zoom * L.yScale;
}

void panelFitAll(const PanelLayout& L, const PanelFrame& f, PanelCamera& cam) {
  if (!L.ok) return;
  float bw = std::fmax(60.f, L.x1 - L.x0), bh = std::fmax(20.f, L.y1 - L.y0) * L.yScale;
  cam.zoom = std::fmax(0.02f, std::fmin(f.w * 0.94f / bw, f.h * 0.8f / bh));
  cam.x = (L.x0 + L.x1) / 2; cam.y = (L.y0 + L.y1) / 2; cam.fitted = true;
}

// mejaKanvas kameraMeja: the station box (entry signal to entry signal) fitted to the canvas
void panelFitStation(const PanelLayout& L, const std::string& code, const PanelFrame& f, PanelCamera& cam) {
  if (!L.ok) return;
  const PanelStation* st = nullptr;
  for (const PanelStation& s : L.stations) if (code.empty() || s.code == code) { st = &s; break; }
  if (!st) { panelFitAll(L, f, cam); return; }
  float bw = std::fmax(60.f, st->x1 - st->x0), bh = std::fmax(20.f, st->y1 - st->y0) * L.yScale;
  cam.zoom = std::fmax(0.05f, std::fmin(8.f, std::fmin(f.w * 0.94f / bw, f.h * 0.7f / bh)));
  cam.x = (st->x0 + st->x1) / 2; cam.y = (st->y0 + st->y1) / 2; cam.fitted = true;
}

// Liang–Barsky clip of a segment to a rect.
static bool clipLine(const PanelFrame& r, float& x0, float& y0, float& x1, float& y1) {
  float t0 = 0, t1 = 1, dx = x1 - x0, dy = y1 - y0;
  float p[4] = {-dx, dx, -dy, dy}, q[4] = {x0 - r.x, r.x + r.w - x0, y0 - r.y, r.y + r.h - y0};
  for (int i = 0; i < 4; ++i) {
    if (p[i] == 0) { if (q[i] < 0) return false; continue; }
    float t = q[i] / p[i];
    if (p[i] < 0) { if (t > t1) return false; if (t > t0) t0 = t; } else { if (t < t0) return false; if (t < t1) t1 = t; }
  }
  float nx0 = x0 + dx * t0, ny0 = y0 + dy * t0, nx1 = x0 + dx * t1, ny1 = y0 + dy * t1;
  x0 = nx0; y0 = ny0; x1 = nx1; y1 = ny1;
  return true;
}

PanelHit panelPick(const PanelLayout& L, const PanelCamera& cam, const PanelFrame& f, float px, float py) {
  PanelHit hit;
  float ds = 22, dn = 16;
  for (const PanelObj& o : L.signals) {
    float sx, sy; panelToScreen(L, cam, f, o.x, o.y, sx, sy);
    float d = std::hypot(sx - px, sy - py); if (d < ds) { ds = d; hit.signalId = o.id; hit.sx = sx; hit.sy = sy; }
  }
  if (!hit.signalId.empty()) return hit;
  for (const PanelPoint& p : L.points) {
    float sx, sy; panelToScreen(L, cam, f, p.x, p.y, sx, sy);
    float d = std::hypot(sx - px, sy - py); if (d < dn) { dn = d; hit.pointId = p.id; hit.sx = sx; hit.sy = sy; }
  }
  return hit;
}

void panelDraw(PanelCanvas& T, const PanelLayout& L, const SimState& st, const PanelCamera& cam, const PanelFrame& r,
               const std::string& hoverId, const std::vector<std::string>& lit, const std::string& selectedTrain, const char* hint) {
  if (!L.ok) { T.text("panel layout unavailable", r.x + 10, r.y + 10, C_LABEL, 0.8f); return; }
  const float ys = L.yScale, zoom = cam.zoom, railW = std::fmax(2.f, std::fmin(6.f, zoom * 3)), lh = T.lineHeight(0.6f);
  auto toScreen = [&](float px, float py, float& sx, float& sy) { panelToScreen(L, cam, r, px, py, sx, sy); };
  auto inside = [&](float sx, float sy, float m) { return sx >= r.x - m && sx <= r.x + r.w + m && sy >= r.y - m && sy <= r.y + r.h + m; };
  auto line = [&](float x0, float y0, float x1, float y1, float wd, vec4 c) { if (clipLine(r, x0, y0, x1, y1)) T.line(x0, y0, x1, y1, wd, c); };
  auto polyline = [&](const PanelSeg& sg, float wd, vec4 c, float a = 0, float b = 1e30f) {
    // portion [a,b] metres of the segment, following the schematic vertices
    if (sg.pts.size() < 4) return;
    float px, py, tx, ty; float qx, qy;
    if (!L.posOnSeg(sg.id, a, px, py, tx, ty)) return;
    toScreen(px, py, px, py);
    for (size_t i = 0; i < sg.cum.size(); ++i) {
      if (sg.cum[i] <= a || sg.cum[i] >= b) continue;
      toScreen(sg.pts[2 * i], sg.pts[2 * i + 1], qx, qy);
      line(px, py, qx, qy, wd, c); px = qx; py = qy;
    }
    float ex, ey; L.posOnSeg(sg.id, std::fmin(b, sg.len), ex, ey, tx, ty); toScreen(ex, ey, qx, qy);
    line(px, py, qx, qy, wd, c);
  };

  // station boxes + names (drawPanelAreaStasiun)
  for (const PanelStation& s : L.stations) {
    float x0, y0, x1, y1; toScreen(s.x0, s.y0, x0, y0); toScreen(s.x1, s.y1, x1, y1);
    if (x1 - x0 < 14) continue;
    float bx = x0 - 18, by = std::fmin(y0, y1) - 16, bw = x1 - x0 + 36, bh = std::fabs(y1 - y0) + 32;
    vec4 c{0.37f, 0.44f, 0.51f, 0.55f};
    line(bx, by, bx + bw, by, 1.2f, c); line(bx, by + bh, bx + bw, by + bh, 1.2f, c); line(bx, by, bx, by + bh, 1.2f, c); line(bx + bw, by, bx + bw, by + bh, 1.2f, c);
    if (inside(bx, by, 0)) T.text(s.label.empty() ? s.code : s.label + " (" + s.code + ")", bx + 6, by - lh - 2, C_LABEL, 0.6f);
  }

  // rails: locked routes lit, menu candidate blue, mainline accent
  std::unordered_map<std::string, int> segState;   // 1 = locked route, 2 = candidate
  for (const SimRoute& rt : st.routes) {
    for (const std::string& sg : rt.segs) {
      bool rel = false; for (const std::string& rs : rt.released) if (rs == sg) { rel = true; break; }
      if (!rel) segState[sg] = 1;
    }
  }
  for (const std::string& sg : lit) if (!segState.count(sg)) segState[sg] = 2;
  for (const PanelSeg& sg : L.segments) {
    auto it = segState.find(sg.id);
    vec4 c = it != segState.end() ? (it->second == 1 ? C_ROUTE : C_CAND) : sg.sepur == "lurus" ? C_LURUS : C_RAIL;
    polyline(sg, it != segState.end() ? railW + 1 : railW, c);
  }
  // track-circuit lamps: occupied intervals in red
  for (const SimOccupancy& oc : st.occupancy) {
    const PanelSeg* sg = L.seg(oc.seg); if (!sg) continue;
    for (const SimOccupancy::Interval& iv : oc.intervals) polyline(*sg, railW + 2, C_OCC, iv.a, iv.b);
  }
  // shunting plans: yellow dashed (renderer.ts overlay putar-lok), the active leg bright
  for (const SimLangsir& lg : st.langsir)
    for (size_t li = 0; li < lg.legs.size(); ++li) {
      bool active = lg.kendali == "auto" && (int)li == lg.legIdx;
      vec4 c{1, 0.78f, 0, active ? 0.95f : 0.45f};
      for (const std::string& sid : lg.legs[li]) {
        const PanelSeg* sg = L.seg(sid); if (!sg) continue;
        for (float a = 0; a < sg->len; a += 16) polyline(*sg, railW + (active ? 1 : 0), c, a, std::fmin(sg->len, a + 8));
      }
    }

  // points: disc + a short bar along the selected leg (drawJunction, simplified)
  std::unordered_map<std::string, const SimPoint*> ptState;
  for (const SimPoint& p : st.points) ptState[p.id] = &p;
  for (const PanelPoint& p : L.points) {
    float sx, sy; toScreen(p.x, p.y, sx, sy);
    if (!inside(sx, sy, 20)) continue;
    const SimPoint* ps = ptState.count(p.id) ? ptState[p.id] : nullptr;
    int setting = ps ? ps->setting : 0; bool locked = ps && !ps->lockedBy.empty();
    const PanelSeg* leg = L.seg(p.legs[setting == 1 ? 1 : 0]);
    if (leg && leg->pts.size() >= 4) {
      bool atA = leg->a == p.id; size_t n = leg->pts.size() / 2;
      float ex = atA ? leg->pts[2] : leg->pts[2 * (n - 2)], ey = atA ? leg->pts[3] : leg->pts[2 * (n - 2) + 1];
      float qx, qy; toScreen(ex, ey, qx, qy);
      float dx = qx - sx, dy = qy - sy, l = std::hypot(dx, dy); if (l > 1) { dx /= l; dy /= l; line(sx, sy, sx + dx * 14, sy + dy * 14, railW + 2, locked ? C_LOCK : C_WHITE); }
    }
    T.circle(sx, sy, 4.5f, locked ? C_LOCK : vec4{0.75f, 0.8f, 0.85f, 1});
    if (hoverId == p.id) T.ring(sx, sy, 10, 2, C_SEL);
    if (zoom > 0.5f) T.text(p.id, sx + 6, sy + 4, C_LABEL, 0.5f);
  }

  // signals: disc with the aspect colour on the right-hand side of travel, name label
  std::unordered_map<std::string, const SimSignal*> sigState;
  for (const SimSignal& s : st.signals) sigState[s.id] = &s;
  for (const PanelObj& o : L.signals) {
    float sx, sy; toScreen(o.x, o.y, sx, sy);
    if (!inside(sx, sy, 30)) continue;
    float tx = o.tx, ty = o.ty * ys, l = std::hypot(tx, ty); if (l > 0) { tx /= l; ty /= l; }
    float ox = ty * 9, oy = -tx * 9;   // perpendicular offset (above for eastbound, below for westbound)
    const SimSignal* ss = sigState.count(o.id) ? sigState[o.id] : nullptr;
    vec4 c = !ss || ss->aspect == "red" ? vec4{0.95f, 0.2f, 0.2f, 1} : ss->aspect == "yellow" ? vec4{1, 0.85f, 0.2f, 1} : vec4{0.2f, 0.95f, 0.35f, 1};
    bool manual = o.signalType == "interlocking" || o.signalType == "bersama";
    line(sx, sy, sx + ox, sy + oy, 2, vec4{0.6f, 0.65f, 0.7f, 1});
    T.circle(sx + ox, sy + oy, manual ? 5.f : 3.5f, c);
    // direction tick: a small bar pointing along travel
    line(sx + ox, sy + oy, sx + ox + tx * 9, sy + oy + ty * 9, 2, c);
    if (hoverId == o.id) T.ring(sx + ox, sy + oy, 11, 2, C_SEL);
    if (zoom > 0.25f) { float tw = T.measure(o.name, 0.55f); T.text(o.name, sx + ox - tw / 2, oy < 0 ? sy + oy - 8 - lh : sy + oy + 8, manual ? C_PILLINK : C_LABEL, 0.55f); }
  }

  // "JALUR n" pills and portals
  if (zoom > 0.12f) {
    for (const PanelJalur& j : L.jalur) {
      float sx, sy; toScreen(j.x, j.y, sx, sy); if (!inside(sx, sy, 0)) continue;
      char b[24]; std::snprintf(b, sizeof b, "JALUR %d", j.n); float tw = T.measure(b, 0.5f);
      T.rect(sx - tw / 2 - 4, sy - lh * 0.45f - 1, tw + 8, lh * 0.9f, C_PILL); T.text(b, sx - tw / 2, sy - lh * 0.45f, C_PILLINK, 0.5f);
    }
    for (const PanelObj& o : L.portals) {
      float sx, sy; toScreen(o.x, o.y, sx, sy); if (!inside(sx, sy, 0)) continue;
      T.circle(sx, sy, 4, C_LABEL); T.text(o.name, sx + 6, sy - lh / 2, C_LABEL, 0.55f);
    }
  }

  // trains: each vehicle as a bar between its couplers, label at the front
  for (const SimTrain& t : st.trains) {
    bool sel = t.id == selectedTrain;
    vec4 c = sel ? C_SEL : t.tungguS40 ? vec4{1, 0.8f, 0.3f, 1} : C_TRAIN;
    float fx = 0, fy = 0; bool have = false;
    for (const SimVehicle& v : t.vehicles) {
      float ax, ay, bx, by, tx, ty;
      if (!L.posOnSeg(v.seg, v.s, ax, ay, tx, ty) || !L.posOnSeg(v.seg2, v.s2, bx, by, tx, ty)) continue;
      toScreen(ax, ay, ax, ay); toScreen(bx, by, bx, by);
      if (!have) { fx = ax; fy = ay; have = true; }
      line(ax, ay, bx, by, railW + 4, c);
      if (v.kind == "loco") T.circle(ax, ay, railW / 2 + 2.5f, {0.1f, 0.1f, 0.12f, 1});
    }
    if (!have) { float tx, ty; if (!L.posOnSeg(t.seg, t.s, fx, fy, tx, ty)) continue; toScreen(fx, fy, fx, fy); }
    if (!inside(fx, fy, 40)) continue;
    std::string lab = "KA " + t.no + (t.tungguS40 ? "  S40" : "");
    float tw = T.measure(lab, 0.6f);
    T.rect(fx - tw / 2 - 4, fy - lh - 10, tw + 8, lh, sel ? vec4{0.05f, 0.4f, 0.6f, 0.9f} : t.hold.empty() ? vec4{0.05f, 0.2f, 0.4f, 0.85f} : vec4{0.45f, 0.3f, 0.05f, 0.85f});
    T.text(lab, fx - tw / 2, fy - lh - 9, C_WHITE, 0.6f);
  }

  if (hint) T.text(hint, r.x + 8, r.y + r.h - lh - 4, C_LABEL, 0.55f);
}

// ---- in-world board ----

namespace {
// PanelCanvas over the CPU canvas. `scale` 1 = the font's native height; the canvas is drawn like a screen.
struct CanvasTarget : PanelCanvas {
  PixelCanvas& c; const BitmapFont& f;
  CanvasTarget(PixelCanvas& canvas, const BitmapFont& font) : c(canvas), f(font) {}
  static uint32_t hex(vec4 v) { return ((uint32_t)std::lround(v.x * 255) << 16) | ((uint32_t)std::lround(v.y * 255) << 8) | (uint32_t)std::lround(v.z * 255); }
  void line(float x0, float y0, float x1, float y1, float w, vec4 col) override { c.line(x0, y0, x1, y1, w, hex(col), col.w); }
  void circle(float cx, float cy, float r, vec4 col) override { c.circle(cx, cy, r, hex(col), col.w); }
  void ring(float cx, float cy, float r, float w, vec4 col) override { c.ring(cx, cy, r, w, hex(col), col.w); }
  void rect(float x, float y, float w, float h, vec4 col) override { c.rect(x, y, w, h, hex(col), col.w); }
  void text(const std::string& s, float x, float yTop, vec4 col, float scale) override {
    float size = f.pixelHeight * scale;
    c.textLeft(f, s, x, yTop + lineHeight(scale) / 2, size, hex(col), false, col.w);
  }
  float measure(const std::string& s, float scale) const override { return f.measure(s, f.pixelHeight * scale); }
  float lineHeight(float scale) const override { return (f.ascent - f.descent) * scale; }
};
} // namespace

void MejaBoard::scan(const std::vector<Placed>& placed, const std::vector<Station>& stations, const WorldOrigin& origin) {
  destroy();
  for (const Placed& p : placed) {
    const GpuModel& m = *p.model;
    for (size_t ni = 0; ni < m.nodes.size(); ++ni) {
      const Node& n = m.nodes[ni];
      if (n.mesh < 0 || n.mesh >= (int)m.meshes.size()) continue;
      const GpuMesh& gm = m.meshes[(size_t)n.mesh];
      for (const GpuPrimitive& prim : gm.primitives) {
        bool named = n.name == "mejalayan" || n.name.rfind("mejalayan", 0) == 0;
        bool byMat = prim.material >= 0 && prim.material < (int)m.materials.size() && m.materials[(size_t)prim.material].name == "mejalayan";
        if (!named && !byMat) continue;
        Board b; b.mesh = &prim.mesh; b.xf = p.xf * m.world[ni];
        b.centre = b.xf.transformPoint((prim.bounds.min + prim.bounds.max) * 0.5f);
        { vec3 e = prim.bounds.max - prim.bounds.min; vec3 axis = e.x <= e.y && e.x <= e.z ? vec3{1, 0, 0} : e.y <= e.z ? vec3{0, 1, 0} : vec3{0, 0, 1};
          vec3 nw = b.xf.transformPoint(axis) - b.xf.transformPoint({0, 0, 0}); float l = length(nw); b.normal = l > 1e-6f ? nw / l : vec3{0, 1, 0}; }
        { const auto& w = b.xf.m; b.mirrored = w[0][0] * (w[1][1] * w[2][2] - w[1][2] * w[2][1]) - w[1][0] * (w[0][1] * w[2][2] - w[0][2] * w[2][1]) + w[2][0] * (w[0][1] * w[1][2] - w[0][2] * w[1][1]) < 0; }
        double wx, wy; origin.toWorld(b.centre, wx, wy);
        double best = 250 * 250;
        for (const Station& s : stations) { double d = (s.wx - wx) * (s.wx - wx) + (s.wy - wy) * (s.wy - wy); if (d < best) { best = d; b.code = s.code; } }
        if (std::getenv("ENG_MEJA_DEBUG")) std::fprintf(stderr, "meja board '%s' at world (%.1f, %.1f) scene y %.2f, station %s\n", n.name.c_str(), wx, wy, b.centre.y, b.code.c_str());
        std::vector<uint8_t> blank((size_t)TEX_W * TEX_H * 4, 0);
        b.tex = rhi::createTexture(TEX_W, TEX_H, rhi::Format::RGBA8, std::as_bytes(std::span(blank)), true, true, rhi::Wrap::Clamp, rhi::Wrap::Clamp);
        boards_.push_back(std::move(b));
      }
    }
  }
  mat_ = {}; mat_.name = "mejalayan"; mat_.baseColor = {1, 1, 1, 1}; mat_.unlit = true; mat_.doubleSided = true;
}

void MejaBoard::update(const SimState& st, vec3 eye, double nowMs) {
  if (!layout_ || boards_.empty()) return;
  Board* near = nullptr; float bd = JANGKAU_GAMBAR;
  for (Board& b : boards_) { float d = length(b.centre - eye); if (d < bd) { bd = d; near = &b; } }
  if (!near) return;
  Board& b = *near;
  if (bd > JARAK_LAJU_PENUH && nowMs - b.drawnMs < JEDA_GAMBAR_MS) return;
  b.drawnMs = nowMs;
  const PanelFrame frame{0, 0, (float)TEX_W, (float)TEX_H};
  if (!b.cam.fitted) panelFitStation(*layout_, b.code, frame, b.cam);
  canvas_.reset(TEX_W, TEX_H);
  canvas_.fill(0x0a0e13);
  CanvasTarget target(canvas_, font_);
  panelDraw(target, *layout_, st, b.cam, frame, "", {}, "", nullptr);
  if (b.mirrored) for (int y = 0; y < TEX_H; ++y) for (int x = 0; x < TEX_W / 2; ++x) {   // the mesh is mirrored: flip the picture back
    uint8_t* a = &canvas_.px[((size_t)y * TEX_W + (size_t)x) * 4]; uint8_t* c = &canvas_.px[((size_t)y * TEX_W + (size_t)(TEX_W - 1 - x)) * 4];
    for (int k = 0; k < 4; ++k) std::swap(a[k], c[k]);
  }
  rhi::updateTextureRGBA(b.tex, TEX_W, TEX_H, std::as_bytes(std::span(canvas_.px)));
}

void MejaBoard::draw(ModelRenderer& r, vec3 eye) const {
  for (const Board& b : boards_) {
    if (!b.mesh || !b.tex.id) continue;
    float side = dot(b.normal, eye - b.centre) >= 0 ? 1.f : -1.f;
    r.drawMesh(*b.mesh, mat_, b.tex, mat4::translation(b.normal * (0.003f * side)) * b.xf);
  }
}

void MejaBoard::destroy() {
  for (Board& b : boards_) if (b.tex.id) rhi::destroyTexture(b.tex);
  boards_.clear();
}

} // namespace eng

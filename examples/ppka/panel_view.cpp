#include "examples/ppka/panel_view.h"
#include <cmath>
#include <cstdio>

namespace eng {

// VDU/CTC dark palette (src/render/tema.ts PANEL_GAYA.vdu.gelap)
static const vec4 C_BG{0.039f, 0.055f, 0.075f, 0.93f}, C_RAIL{0.30f, 0.36f, 0.41f, 1}, C_LURUS{1.0f, 0.67f, 0.14f, 1};
static const vec4 C_ROUTE{0.125f, 0.94f, 0.44f, 1}, C_CAND{0.35f, 0.65f, 1.0f, 1}, C_OCC{0.95f, 0.2f, 0.2f, 1};
static const vec4 C_LABEL{0.37f, 0.44f, 0.51f, 1}, C_PILL{0.10f, 0.14f, 0.18f, 1}, C_PILLINK{0.56f, 0.64f, 0.71f, 1};
static const vec4 C_WHITE{1, 1, 1, 1}, C_TRAIN{0.85f, 0.9f, 1, 1}, C_SEL{0.3f, 0.9f, 1, 1}, C_LOCK{1, 0.6f, 0.2f, 1};

void PanelView::init(const PanelLayout* layout) { lay_ = layout; fitted_ = false; }

void PanelView::toScreen(float px, float py, int w, int h, float& sx, float& sy) const {
  UiRect r = rect(w, h);
  sx = (float)w / 2 + (px - camX_) * zoom_;
  sy = r.y + r.h / 2 + (py - camY_) * zoom_ * (lay_ ? lay_->yScale : 3);
}

void PanelView::fitAll(int w, int) {
  if (!lay_ || !lay_->ok) return;
  float bw = std::fmax(60.f, lay_->x1 - lay_->x0), bh = std::fmax(20.f, lay_->y1 - lay_->y0) * lay_->yScale;
  zoom_ = std::fmax(0.02f, std::fmin((float)w * 0.94f / bw, height * 0.8f / bh));
  camX_ = (lay_->x0 + lay_->x1) / 2; camY_ = (lay_->y0 + lay_->y1) / 2; fitted_ = true;
}

// mejaKanvas kameraMeja: the station box (entry signal to entry signal) fitted to the canvas
void PanelView::fitStation(const std::string& code, int w, int) {
  if (!lay_ || !lay_->ok) return;
  const PanelStation* st = nullptr;
  for (const PanelStation& s : lay_->stations) if (code.empty() || s.code == code) { st = &s; break; }
  if (!st) { fitAll(w, 0); return; }
  float bw = std::fmax(60.f, st->x1 - st->x0), bh = std::fmax(20.f, st->y1 - st->y0) * lay_->yScale;
  zoom_ = std::fmax(0.05f, std::fmin(8.f, std::fmin((float)w * 0.94f / bw, height * 0.7f / bh)));
  camX_ = (st->x0 + st->x1) / 2; camY_ = (st->y0 + st->y1) / 2; fitted_ = true;
}

void PanelView::zoomAt(float wheel, float px, float py, int w, int h) {
  if (!lay_) return;
  UiRect r = rect(w, h);
  float ux = camX_ + (px - (float)w / 2) / zoom_, uy = camY_ + (py - r.y - r.h / 2) / (zoom_ * lay_->yScale);
  zoom_ = std::fmax(0.02f, std::fmin(12.f, zoom_ * std::pow(1.15f, wheel)));
  camX_ = ux - (px - (float)w / 2) / zoom_; camY_ = uy - (py - r.y - r.h / 2) / (zoom_ * lay_->yScale);
}

void PanelView::pan(float dx, float dy) { if (lay_) { camX_ -= dx / zoom_; camY_ -= dy / (zoom_ * lay_->yScale); } }

// Liang–Barsky clip of a segment to a rect.
static bool clipLine(const UiRect& r, float& x0, float& y0, float& x1, float& y1) {
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

bool PanelView::screenPos(const std::string& id, bool isSignal, int w, int h, float& sx, float& sy) const {
  if (!lay_) return false;
  if (isSignal) { for (const PanelObj& o : lay_->signals) if (o.id == id) { toScreen(o.x, o.y, w, h, sx, sy); return true; } }
  else { for (const PanelPoint& p : lay_->points) if (p.id == id) { toScreen(p.x, p.y, w, h, sx, sy); return true; } }
  return false;
}

PanelHit PanelView::pick(float px, float py, int w, int h) const {
  PanelHit hit;
  if (!lay_ || !visible || !rect(w, h).contains(px, py)) return hit;
  float ds = 22, dn = 16;
  for (const PanelObj& o : lay_->signals) {
    float sx, sy; toScreen(o.x, o.y, w, h, sx, sy);
    float d = std::hypot(sx - px, sy - py); if (d < ds) { ds = d; hit.signalId = o.id; hit.sx = sx; hit.sy = sy; }
  }
  if (!hit.signalId.empty()) return hit;
  for (const PanelPoint& p : lay_->points) {
    float sx, sy; toScreen(p.x, p.y, w, h, sx, sy);
    float d = std::hypot(sx - px, sy - py); if (d < dn) { dn = d; hit.pointId = p.id; hit.sx = sx; hit.sy = sy; }
  }
  return hit;
}

void PanelView::draw(Ui& ui, const SimState& st, int w, int h, const std::string& hoverId, const std::vector<std::string>& lit,
                     const std::string& selectedTrain) {
  if (!visible || !lay_) return;
  TextRenderer& T = *ui.text;
  UiRect r = rect(w, h);
  ui.panel(r, C_BG);
  T.rect(r.x, r.y, r.w, 2, {0.3f, 0.36f, 0.42f, 1});   // top edge (drag handle)
  if (!lay_->ok) { T.draw("panel layout unavailable", r.x + 10, r.y + 10, C_LABEL, 0.8f); return; }
  if (!fitted_) fitStation("", w, h);
  const float ys = lay_->yScale, railW = std::fmax(2.f, std::fmin(6.f, zoom_ * 3)), lh = T.lineHeight(0.6f);
  auto inside = [&](float sx, float sy, float m) { return sx >= r.x - m && sx <= r.x + r.w + m && sy >= r.y - m && sy <= r.y + r.h + m; };
  auto line = [&](float x0, float y0, float x1, float y1, float wd, vec4 c) { if (clipLine(r, x0, y0, x1, y1)) T.line(x0, y0, x1, y1, wd, c); };
  auto polyline = [&](const PanelSeg& sg, float wd, vec4 c, float a = 0, float b = 1e30f) {
    // portion [a,b] metres of the segment, following the schematic vertices
    if (sg.pts.size() < 4) return;
    float px, py, tx, ty; float qx, qy;
    if (!lay_->posOnSeg(sg.id, a, px, py, tx, ty)) return;
    toScreen(px, py, w, h, px, py);
    for (size_t i = 0; i < sg.cum.size(); ++i) {
      if (sg.cum[i] <= a || sg.cum[i] >= b) continue;
      toScreen(sg.pts[2 * i], sg.pts[2 * i + 1], w, h, qx, qy);
      line(px, py, qx, qy, wd, c); px = qx; py = qy;
    }
    float ex, ey; lay_->posOnSeg(sg.id, std::fmin(b, sg.len), ex, ey, tx, ty); toScreen(ex, ey, w, h, qx, qy);
    line(px, py, qx, qy, wd, c);
  };

  // station boxes + names (drawPanelAreaStasiun)
  for (const PanelStation& s : lay_->stations) {
    float x0, y0, x1, y1; toScreen(s.x0, s.y0, w, h, x0, y0); toScreen(s.x1, s.y1, w, h, x1, y1);
    if (x1 - x0 < 14) continue;
    float bx = x0 - 18, by = std::fmin(y0, y1) - 16, bw = x1 - x0 + 36, bh = std::fabs(y1 - y0) + 32;
    vec4 c{0.37f, 0.44f, 0.51f, 0.55f};
    line(bx, by, bx + bw, by, 1.2f, c); line(bx, by + bh, bx + bw, by + bh, 1.2f, c); line(bx, by, bx, by + bh, 1.2f, c); line(bx + bw, by, bx + bw, by + bh, 1.2f, c);
    if (inside(bx, by, 0)) T.draw(s.label.empty() ? s.code : s.label + " (" + s.code + ")", bx + 6, by - lh - 2, C_LABEL, 0.6f);
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
  for (const PanelSeg& sg : lay_->segments) {
    auto it = segState.find(sg.id);
    vec4 c = it != segState.end() ? (it->second == 1 ? C_ROUTE : C_CAND) : sg.sepur == "lurus" ? C_LURUS : C_RAIL;
    polyline(sg, it != segState.end() ? railW + 1 : railW, c);
  }
  // track-circuit lamps: occupied intervals in red
  for (const SimOccupancy& oc : st.occupancy) {
    const PanelSeg* sg = lay_->seg(oc.seg); if (!sg) continue;
    for (const SimOccupancy::Interval& iv : oc.intervals) polyline(*sg, railW + 2, C_OCC, iv.a, iv.b);
  }

  // points: disc + a short bar along the selected leg (drawJunction, simplified)
  std::unordered_map<std::string, const SimPoint*> ptState;
  for (const SimPoint& p : st.points) ptState[p.id] = &p;
  for (const PanelPoint& p : lay_->points) {
    float sx, sy; toScreen(p.x, p.y, w, h, sx, sy);
    if (!inside(sx, sy, 20)) continue;
    const SimPoint* ps = ptState.count(p.id) ? ptState[p.id] : nullptr;
    int setting = ps ? ps->setting : 0; bool locked = ps && !ps->lockedBy.empty();
    const PanelSeg* leg = lay_->seg(p.legs[setting == 1 ? 1 : 0]);
    if (leg && leg->pts.size() >= 4) {
      bool atA = leg->a == p.id; size_t n = leg->pts.size() / 2;
      float ex = atA ? leg->pts[2] : leg->pts[2 * (n - 2)], ey = atA ? leg->pts[3] : leg->pts[2 * (n - 2) + 1];
      float qx, qy; toScreen(ex, ey, w, h, qx, qy);
      float dx = qx - sx, dy = qy - sy, l = std::hypot(dx, dy); if (l > 1) { dx /= l; dy /= l; line(sx, sy, sx + dx * 14, sy + dy * 14, railW + 2, locked ? C_LOCK : C_WHITE); }
    }
    T.circle(sx, sy, 4.5f, locked ? C_LOCK : vec4{0.75f, 0.8f, 0.85f, 1}, 12);
    if (hoverId == p.id) T.ring(sx, sy, 10, 2, C_SEL);
    if (zoom_ > 0.5f) T.draw(p.id, sx + 6, sy + 4, C_LABEL, 0.5f);
  }

  // signals: disc with the aspect colour on the right-hand side of travel, name label
  std::unordered_map<std::string, const SimSignal*> sigState;
  for (const SimSignal& s : st.signals) sigState[s.id] = &s;
  for (const PanelObj& o : lay_->signals) {
    float sx, sy; toScreen(o.x, o.y, w, h, sx, sy);
    if (!inside(sx, sy, 30)) continue;
    float tx = o.tx, ty = o.ty * ys, l = std::hypot(tx, ty); if (l > 0) { tx /= l; ty /= l; }
    float ox = ty * 9, oy = -tx * 9;   // perpendicular offset (above for eastbound, below for westbound)
    const SimSignal* ss = sigState.count(o.id) ? sigState[o.id] : nullptr;
    vec4 c = !ss || ss->aspect == "red" ? vec4{0.95f, 0.2f, 0.2f, 1} : ss->aspect == "yellow" ? vec4{1, 0.85f, 0.2f, 1} : vec4{0.2f, 0.95f, 0.35f, 1};
    bool manual = o.signalType == "interlocking" || o.signalType == "bersama";
    line(sx, sy, sx + ox, sy + oy, 2, vec4{0.6f, 0.65f, 0.7f, 1});
    T.circle(sx + ox, sy + oy, manual ? 5.f : 3.5f, c, 14);
    // direction tick: a small triangle-ish bar pointing along travel
    line(sx + ox, sy + oy, sx + ox + tx * 9, sy + oy + ty * 9, 2, c);
    if (hoverId == o.id) T.ring(sx + ox, sy + oy, 11, 2, C_SEL);
    if (zoom_ > 0.25f) { float tw = T.measure(o.name, 0.55f); T.draw(o.name, sx + ox - tw / 2, oy < 0 ? sy + oy - 8 - lh : sy + oy + 8, manual ? C_PILLINK : C_LABEL, 0.55f); }
  }

  // "JALUR n" pills and portals
  if (zoom_ > 0.12f) {
    for (const PanelJalur& j : lay_->jalur) {
      float sx, sy; toScreen(j.x, j.y, w, h, sx, sy); if (!inside(sx, sy, 0)) continue;
      char b[24]; std::snprintf(b, sizeof b, "JALUR %d", j.n); float tw = T.measure(b, 0.5f);
      T.rect(sx - tw / 2 - 4, sy - lh * 0.45f - 1, tw + 8, lh * 0.9f, C_PILL); T.draw(b, sx - tw / 2, sy - lh * 0.45f, C_PILLINK, 0.5f);
    }
    for (const PanelObj& o : lay_->portals) {
      float sx, sy; toScreen(o.x, o.y, w, h, sx, sy); if (!inside(sx, sy, 0)) continue;
      T.circle(sx, sy, 4, C_LABEL, 8); T.draw(o.name, sx + 6, sy - lh / 2, C_LABEL, 0.55f);
    }
  }

  // trains: each vehicle as a bar between its couplers, label at the front
  for (const SimTrain& t : st.trains) {
    bool sel = t.id == selectedTrain;
    vec4 c = sel ? C_SEL : t.tungguS40 ? vec4{1, 0.8f, 0.3f, 1} : C_TRAIN;
    float fx = 0, fy = 0; bool have = false;
    for (const SimVehicle& v : t.vehicles) {
      float ax, ay, bx, by, tx, ty;
      if (!lay_->posOnSeg(v.seg, v.s, ax, ay, tx, ty) || !lay_->posOnSeg(v.seg2, v.s2, bx, by, tx, ty)) continue;
      toScreen(ax, ay, w, h, ax, ay); toScreen(bx, by, w, h, bx, by);
      if (!have) { fx = ax; fy = ay; have = true; }
      line(ax, ay, bx, by, railW + 4, c);
      if (v.kind == "loco") T.circle(ax, ay, railW / 2 + 2.5f, {0.1f, 0.1f, 0.12f, 1}, 8);
    }
    if (!have) { float tx, ty; if (!lay_->posOnSeg(t.seg, t.s, fx, fy, tx, ty)) continue; toScreen(fx, fy, w, h, fx, fy); }
    if (!inside(fx, fy, 40)) continue;
    std::string lab = "KA " + t.no + (t.tungguS40 ? "  S40" : "");
    float tw = T.measure(lab, 0.6f);
    T.rect(fx - tw / 2 - 4, fy - lh - 10, tw + 8, lh, sel ? vec4{0.05f, 0.4f, 0.6f, 0.9f} : t.hold.empty() ? vec4{0.05f, 0.2f, 0.4f, 0.85f} : vec4{0.45f, 0.3f, 0.05f, 0.85f});
    T.draw(lab, fx - tw / 2, fy - lh - 9, C_WHITE, 0.6f);
  }

  // corner hint
  char z[96]; std::snprintf(z, sizeof z, "meja layan  zoom %.2f  |  wheel zoom, drag pan, drag top edge = resize, M hide", zoom_);
  T.draw(z, r.x + 8, r.y + r.h - lh - 4, C_LABEL, 0.55f);
}

} // namespace eng

#include "examples/ppka/panel_view.h"
#include <cmath>
#include <cstdio>

namespace eng {

void PanelView::init(const PanelLayout* layout) { lay_ = layout; cam_ = {}; }

void PanelView::fitAll(int w, int h) { if (lay_) panelFitAll(*lay_, frame(w, h), cam_); }
void PanelView::fitStation(const std::string& code, int w, int h) { if (lay_) panelFitStation(*lay_, code, frame(w, h), cam_); }

void PanelView::zoomAt(float wheel, float px, float py, int w, int h) {
  if (!lay_) return;
  UiRect r = rect(w, h);
  float ux = cam_.x + (px - (float)w / 2) / cam_.zoom, uy = cam_.y + (py - r.y - r.h / 2) / (cam_.zoom * lay_->yScale);
  cam_.zoom = std::fmax(0.02f, std::fmin(12.f, cam_.zoom * std::pow(1.15f, wheel)));
  cam_.x = ux - (px - (float)w / 2) / cam_.zoom; cam_.y = uy - (py - r.y - r.h / 2) / (cam_.zoom * lay_->yScale);
}

void PanelView::pan(float dx, float dy) { if (lay_) { cam_.x -= dx / cam_.zoom; cam_.y -= dy / (cam_.zoom * lay_->yScale); } }

bool PanelView::screenPos(const std::string& id, bool isSignal, int w, int h, float& sx, float& sy) const {
  if (!lay_) return false;
  if (isSignal) { for (const PanelObj& o : lay_->signals) if (o.id == id) { toScreen(o.x, o.y, w, h, sx, sy); return true; } }
  else { for (const PanelPoint& p : lay_->points) if (p.id == id) { toScreen(p.x, p.y, w, h, sx, sy); return true; } }
  return false;
}

PanelHit PanelView::pick(float px, float py, int w, int h) const {
  if (!lay_ || !visible || !rect(w, h).contains(px, py)) return {};
  return panelPick(*lay_, cam_, frame(w, h), px, py);
}

namespace {
// PanelCanvas over the HUD text batch.
struct TextTarget : PanelCanvas {
  TextRenderer& T;
  explicit TextTarget(TextRenderer& t) : T(t) {}
  void line(float x0, float y0, float x1, float y1, float w, vec4 c) override { T.line(x0, y0, x1, y1, w, c); }
  void circle(float cx, float cy, float r, vec4 c) override { T.circle(cx, cy, r, c, r > 4 ? 14 : 8); }
  void ring(float cx, float cy, float r, float w, vec4 c) override { T.ring(cx, cy, r, w, c); }
  void rect(float x, float y, float w, float h, vec4 c) override { T.rect(x, y, w, h, c); }
  void text(const std::string& s, float x, float y, vec4 c, float scale) override { T.draw(s, x, y, c, scale); }
  float measure(const std::string& s, float scale) const override { return T.measure(s, scale); }
  float lineHeight(float scale) const override { return T.lineHeight(scale); }
};
} // namespace

void PanelView::draw(Ui& ui, const SimState& st, int w, int h, const std::string& hoverId, const std::vector<std::string>& lit,
                     const std::string& selectedTrain) {
  if (!visible || !lay_) return;
  UiRect r = rect(w, h);
  ui.panel(r, {0.039f, 0.055f, 0.075f, 0.93f});
  ui.text->rect(r.x, r.y, r.w, 2, {0.3f, 0.36f, 0.42f, 1});   // top edge (drag handle)
  if (!cam_.fitted) fitStation("", w, h);
  char z[96]; std::snprintf(z, sizeof z, "meja layan  zoom %.2f  |  wheel zoom, drag pan, drag top edge = resize, M hide", cam_.zoom);
  TextTarget target(*ui.text);
  panelDraw(target, *lay_, st, cam_, frame(w, h), hoverId, lit, selectedTrain, z);
}

} // namespace eng

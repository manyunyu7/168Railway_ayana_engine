// Meja layan — the schematic control table drawn as a 2D overlay at the bottom of the screen
// (port of the web panel view: src/render/renderer.ts panelMode + src/tiga/mejaKanvas.ts).
// Layout comes from the bridge `panel` command (engine/panel.ts PanelLayout); dynamic state
// (aspects, point settings, locked routes, occupancy, trains) from the per-step SimState.
// Pure drawing + hit-testing: clicks only NAME a signal/point id, Game acts on them. The schematic itself is
// engine/world/meja_board.h panelDraw (shared with the in-world board); this class keeps the screen rectangle,
// the camera and the input.
#pragma once
#include "engine/render/text.h"
#include "engine/sim/sim_process.h"
#include "engine/world/meja_board.h"
#include "examples/ppka/ui.h"
#include <string>
#include <vector>

namespace eng {

class PanelView {
public:
  void init(const PanelLayout* layout);
  bool visible = false;
  float height = 280;                      // px, dragged at the top edge

  UiRect rect(int w, int h) const { return {0, (float)h - height, (float)w, height}; }
  bool contains(float px, float py, int w, int h) const { return visible && rect(w, h).contains(px, py); }
  bool onTopEdge(float px, float py, int w, int h) const { UiRect r = rect(w, h); return visible && px >= 0 && px < r.w && std::fabs(py - r.y) <= 6; }

  void fitStation(const std::string& code, int w, int h);   // camera on one station box (mejaKanvas kameraMeja)
  void fitAll(int w, int h);
  void zoomAt(float wheel, float px, float py, int w, int h);
  void pan(float dxPx, float dyPx);
  void resizeTo(float newHeight, int h) { height = std::fmax(120.f, std::fmin((float)h - 140, newHeight)); }

  // Draw into the text batch. `hoverId` = the currently hovered signal/point (ring), `lit` = extra
  // segments to light (beginner-mode menu candidate), `selectedTrain` = highlighted train id.
  void draw(Ui& ui, const SimState& st, int w, int h, const std::string& hoverId, const std::vector<std::string>& lit,
            const std::string& selectedTrain);
  // Hit-test (mejaKanvas klikMejaKanvas): signal within 22 px wins, then a point within 16 px.
  PanelHit pick(float px, float py, int w, int h) const;
  // Screen position of a panel object (for the tooltip). false when unknown.
  bool screenPos(const std::string& id, bool isSignal, int w, int h, float& sx, float& sy) const;

private:
  PanelFrame frame(int w, int h) const { UiRect r = rect(w, h); return {r.x, r.y, r.w, r.h}; }
  void toScreen(float px, float py, int w, int h, float& sx, float& sy) const { if (lay_) panelToScreen(*lay_, cam_, frame(w, h), px, py, sx, sy); }
  const PanelLayout* lay_ = nullptr;
  PanelCamera cam_;
};

} // namespace eng

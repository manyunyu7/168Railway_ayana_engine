// Meja layan (schematic control table) drawing shared by the screen overlay (examples/ppka/panel_view) and
// the in-world board: a station GLB may carry a `mejalayan` quad (4:1, UV 0..1; nry-stasiun-kroya-v2,
// pemalang) which mejaLayan3d.ts turns into a live texture of the panel fitted to the owning station's
// box. The schematic is drawn through PanelCanvas (screen: TextRenderer; board: engine/world/pixel_canvas.h),
// so both read the same PanelLayout (bridge `panel`) + SimState.
//
// MejaBoard: finds `mejalayan` meshes (node or material name) in the placed hiasan models, owns one RGBA
// texture per board (1024x256), redraws only the nearest board within JANGKAU_GAMBAR 45 m of the eye, at
// most every JEDA_GAMBAR_MS 120 ms (every frame under JARAK_LAJU_PENUH 8 m), and draws the quad unlit over
// the model's own (a self-lit screen: readable at night). No click mapping yet.
#pragma once
#include "engine/render/model_renderer.h"
#include "engine/sim/sim_process.h"
#include "engine/world/coords.h"
#include "engine/world/pixel_canvas.h"
#include <string>
#include <vector>

namespace eng {

// Draw target for the schematic (pixel coordinates, origin top-left, text `scale` 1 = the font's native height).
struct PanelCanvas {
  virtual ~PanelCanvas() = default;
  virtual void line(float x0, float y0, float x1, float y1, float width, vec4 c) = 0;
  virtual void circle(float cx, float cy, float r, vec4 c) = 0;
  virtual void ring(float cx, float cy, float r, float width, vec4 c) = 0;
  virtual void rect(float x, float y, float w, float h, vec4 c) = 0;
  virtual void text(const std::string& s, float x, float yTop, vec4 c, float scale) = 0;
  virtual float measure(const std::string& s, float scale) const = 0;
  virtual float lineHeight(float scale) const = 0;
};

struct PanelCamera { float x = 0, y = 0, zoom = 1; bool fitted = false; };   // panel units at the frame centre; px per unit (x)
struct PanelFrame { float x = 0, y = 0, w = 0, h = 0; };                    // the schematic's rectangle on the canvas
struct PanelHit { std::string signalId, pointId; float sx = 0, sy = 0; };

// Camera fits (mejaKanvas kameraMeja): one station's box (code "" or unknown = whole layout).
void panelFitAll(const PanelLayout& L, const PanelFrame& f, PanelCamera& cam);
void panelFitStation(const PanelLayout& L, const std::string& code, const PanelFrame& f, PanelCamera& cam);
void panelToScreen(const PanelLayout& L, const PanelCamera& cam, const PanelFrame& f, float px, float py, float& sx, float& sy);
// Hit-test (mejaKanvas klikMejaKanvas): signal within 22 px wins, then a point within 16 px.
PanelHit panelPick(const PanelLayout& L, const PanelCamera& cam, const PanelFrame& f, float px, float py);
// The schematic: station boxes, rails (locked routes lit, `lit` candidates blue), occupancy, points, signals,
// JALUR pills, portals, trains. `hint` = the corner help line (screen overlay only).
void panelDraw(PanelCanvas& c, const PanelLayout& L, const SimState& st, const PanelCamera& cam, const PanelFrame& f,
               const std::string& hoverId, const std::vector<std::string>& lit, const std::string& selectedTrain, const char* hint);

class MejaBoard {
public:
  static constexpr int TEX_W = 1024, TEX_H = 256;
  static constexpr float JANGKAU_GAMBAR = 45, JARAK_LAJU_PENUH = 8; static constexpr double JEDA_GAMBAR_MS = 120;
  struct Placed { const GpuModel* model; mat4 xf; };
  struct Station { std::string code; double wx, wy; };
  void setLayout(const PanelLayout* L) { layout_ = L; for (Board& b : boards_) { b.cam = {}; b.fingerprint.clear(); } }
  void setFont(const std::string& efntPath) { font_.load(efntPath); }
  // Re-scan the placed models (after buildDecor). `stations` = scenery stations (world XY) for the owner code
  // (nearest within 250 m; none = whole panel). toWorld: scene (x,z) -> world XY.
  void scan(const std::vector<Placed>& placed, const std::vector<Station>& stations, const WorldOrigin& origin);
  // Redraws the nearest board (see the header) from `st`; nowMs = a monotonic clock in ms.
  void update(const SimState& st, vec3 eye, double nowMs);
  // Draws the quads 3 mm toward `eye` over the model's own (same geometry: the offset beats the depth tie).
  void draw(ModelRenderer& r, vec3 eye) const;
  size_t count() const { return boards_.size(); }
  void destroy();

private:
  struct Board {
    const rhi::Mesh* mesh = nullptr; mat4 xf; vec3 centre, normal; bool mirrored = false;   // normal = the quad's thin axis (scene)
    std::string code; PanelCamera cam; rhi::Texture tex{}; double drawnMs = -1e9, liveUntil = 0; std::string fingerprint;
  };
  std::vector<Board> boards_;
  const PanelLayout* layout_ = nullptr;
  BitmapFont font_;
  Material mat_;
  PixelCanvas canvas_;
};

} // namespace eng

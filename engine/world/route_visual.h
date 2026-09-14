// Route ribbons on the track (docs/world-spec.md §8.4, port of hud3d.ts perbaruiPitaOperasi /
// setHoverSinyal): translucent strips 2.4 m wide floating 0.15 m above the rail head.
//  - set routes (segs minus released) in blue 0x1cb0f6,
//  - occupied intervals (world.occupancy) in red-orange, drawn above the route ribbon,
//  - a hover preview (the path a click would lock) in light blue, or red when it dead-ends,
//  - shunting plans (hud3d.ts perbaruiPitaLangsir): yellow 0xffc800 dashed ribbons (8 m on / 8 m off) per leg,
//    the active leg of an automatic plan wide (3.4 m) and bright (0.9), the others 2.4 m at 0.5 (0.35 manual).
//    They ignore the `pita` layer (the plan is the only hint where the loco goes) but hide in the cab.
// Meshes are rebuilt only when the route/occupancy/plan set changes (fingerprint compare).
#pragma once
#include "engine/asset/model.h"
#include "engine/render/model_renderer.h"
#include "engine/sim/sim_process.h"
#include "engine/world/height_source.h"
#include "engine/world/track_graph.h"
#include <string>
#include <vector>

namespace eng {

class RouteVisuals {
public:
  static constexpr float WIDTH = 2.4f, LIFT = 0.15f, SAMPLE_STEP = 4.0f;

  void init(const TrackGraph* g, const RailProfile* profile) { graph_ = g; profile_ = profile; }
  // Rebuilds the route/occupancy ribbons when the set changed since the last call.
  void update(const SimState& st);
  // Hover preview along `segs`; `deadEnd` = the trace stops at a wrongly set point (red).
  void setPreview(const std::vector<std::string>& segs, bool deadEnd);
  void clearPreview();
  bool hasPreview() const { return preview_.indexCount > 0; }
  // Draws blended, in the order route -> occupied -> langsir -> preview (each flushes the renderer's transparent
  // queue). `ribbons` = false skips the route/occupancy ribbons (layer "pita" off); `cab` = true hides those and
  // the shunting ribbons too (kabin mode); the hover preview is always drawn.
  void draw(ModelRenderer& r, bool ribbons = true, bool cab = false) const;
  size_t langsirCount() const { return langsir_.size(); }
  // Hover highlight: white ring (radius 0.86..1.0 m) lying flat at `base`, scaled by `scale`.
  void drawHoverRing(ModelRenderer& r, vec3 base, float scale);
  void destroy();

private:
  struct Span { std::string seg; double a = 0, b = -1; };   // b < 0 = whole segment
  // `dash` > 0: only every other `dash` metres of each span gets geometry (measured from the span start).
  rhi::Mesh buildRibbon(const std::vector<Span>& spans, float lift, float width = WIDTH, float dash = 0) const;
  struct Langsir { rhi::Mesh mesh; Material mat; };
  std::vector<Langsir> langsir_;
  std::string langsirFp_;
  const TrackGraph* graph_ = nullptr;
  const RailProfile* profile_ = nullptr;
  rhi::Mesh route_, occupied_, preview_, ring_;
  Material ringMat_;
  Material routeMat_, occupiedMat_, previewMat_, deadEndMat_;
  bool previewDead_ = false;
  std::string fingerprint_;
};

} // namespace eng

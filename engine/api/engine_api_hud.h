// Internal bridge between engine_api.cpp (owner of the file-local `Api` state) and engine_api_hud.cpp
// (screen projection for the host's DOM overlays: signal name plates, station bubbles, tethers).
// engine_api.cpp defines eng_hud_view(); engine_api_hud.cpp carries a weak fallback that reports
// "no view" so the HUD calls answer empty until the definition is pasted in.
#pragma once
#include "engine/core/json.h"
#include "engine/math/math.h"

namespace eng {
class WorldScene;

struct EngHudView {
  WorldScene* scene = nullptr;   // non-const: WorldScene::graph() has no const overload (read-only use here)
  const Json* world = nullptr;   // the save's "world" object (scenery stations)
  mat4 viewProj;                 // the matrix the last frame was drawn with
  vec3 eye;                      // camera position (scene)
  int w = 1, h = 1;              // css px
};

} // namespace eng

// Fills `v` from the current Api state; false before the static world is built / before the first frame.
// Strong definition in engine_api.cpp:
//   bool eng_hud_view(eng::EngHudView& v) {
//     if (!g || !g->ready || !g->viewValid) return false;
//     v.scene = &g->scene; v.world = &g->world; v.viewProj = g->viewProj; v.eye = camEye(); v.w = g->w; v.h = g->h;
//     return true;
//   }
bool eng_hud_view(eng::EngHudView& v);

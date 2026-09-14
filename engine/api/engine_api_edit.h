// Internal bridge between engine_api.cpp (owner of the file-local `Api` state) and engine_api_edit.cpp (the
// editing ABI: picking, live hiasan / garis / node-height / brush edits, gizmo and ukur overlays). Same
// pattern as engine_api_hud.h: engine_api.cpp defines eng_edit_ctx(), engine_api_edit.cpp carries a weak
// fallback so the editing calls answer "nothing" until the definition is linked in.
#pragma once
#include "engine/core/json.h"
#include "engine/math/math.h"
#include <string>

namespace eng {
class WorldScene;

struct EngEditCtx {
  WorldScene* scene = nullptr;
  Json* world = nullptr;         // the save's "world" object, edited in place
  const Json* summary = nullptr; // bridge summary (decor rebuild after a track edit)
  std::string map;               // map slug (city bake / terrain files)
  mat4 viewProj; bool viewValid = false;
  vec3 eye;
  int w = 1, h = 1; float dpr = 1;   // css px + device pixel ratio (framebuffer = css * dpr)
  bool ready = false;
};

} // namespace eng

// Fills `c` from the current Api state; false before eng_init. Strong definition in engine_api.cpp.
bool eng_edit_ctx(eng::EngEditCtx& c);
// The bitmap font (boards' text): the Emscripten preload path in the Wasm build, the checkout's copy natively (tests).
#ifdef __EMSCRIPTEN__
#define ENG_API_FONT "/assets/font.efnt"
#else
#define ENG_API_FONT ENG_SOURCE_DIR "/assets/font.efnt"
#endif

// Frame hooks of the marker overlay (engine_api_markers.cpp): engine_api.cpp draws the batch after the world
// overlays and destroys it at shutdown. Weak no-op fallbacks live in engine_api.cpp so the core still links
// without the markers source (the same pattern as eng_edit_ctx).
#pragma once
#include "engine/math/math.h"

namespace eng { class ModelRenderer; }

void eng_markers_draw(eng::ModelRenderer& r, eng::vec3 eye, float fovY, int viewportH);
void eng_markers_destroy(void);

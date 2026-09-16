// Frame hooks of the avatar layer (engine_api_avatar.cpp): engine_api.cpp drives the local avatar from the
// camera rig, draws every figure with the world and destroys the meshes at shutdown. Weak no-op fallbacks
// live in engine_api.cpp so the core still links without this source (the same pattern as eng_markers_*).
#pragma once
#include "engine/math/math.h"

namespace eng { class ModelRenderer; class CameraRig; struct WorldOrigin; }

// Local avatar <- the rig's walker (only while the camera is in CamMode::Orang), gesture clocks, remote smoothing.
void eng_avatar_update(eng::CameraRig& rig, const eng::WorldOrigin& origin, float dt, int drivenByRig);
void eng_avatar_draw(eng::ModelRenderer& r);
void eng_avatar_destroy(void);

// Avatar ABI (engine_api.h "avatar" block; ppka-wannabe-2/docs/multiplayer.md §4/§5): the local player figure
// driven by the engine's walker plus the remote figures the host relays at ~15 Hz. State: one file-local
// eng::AvatarSet reached through eng_edit_ctx() for the scene / view, exactly like the marker overlay.
//
// The renderer is a PLACEHOLDER (coloured boxes, engine/world/avatar_visual.h) until the skinned models of §6
// exist; swapping it changes nothing here — the ABI never mentions how a body is drawn.
#include "engine/api/engine_api.h"
#include "engine/api/engine_api_avatar.h"
#include "engine/api/engine_api_edit.h"
#include "engine/app/camera_rig.h"
#include "engine/app/world_scene.h"
#include "engine/world/avatar.h"
#include "engine/world/avatar_visual.h"
#include <cmath>
#include <string>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define KEEP EMSCRIPTEN_KEEPALIVE
#else
#define KEEP
#endif

using namespace eng;

namespace {
AvatarSet g_set;
AvatarVisuals g_vis;
double g_clock = 0;      // seconds since the first frame (sample timestamps)
bool g_localAda = false; // the rig has driven the local avatar at least once (else it is not drawn)
float g_anim = 0;        // limb-swing phase
std::string g_buf;
const char* ret(std::string s) { g_buf = std::move(s); return g_buf.c_str(); }
} // namespace

void eng_avatar_update(CameraRig& rig, const WorldOrigin& origin, float dt, int drivenByRig) {
  if (dt < 0 || dt > 1) dt = 0;
  g_clock += dt; g_anim += dt;
  if (drivenByRig) g_localAda = true;
  if (drivenByRig) g_set.driveLocal(rig.walkerPos(), rig.walkerBodyYaw(), rig.walkerSpeed(), rig.walkerOnGround(), origin);
  g_set.tick(dt, g_clock);
}

void eng_avatar_draw(ModelRenderer& r) {
  if (!g_localAda && g_set.remote.empty()) return;
  if (!g_vis.built()) g_vis.build();
  if (g_localAda) g_vis.drawAvatar(r, g_set.local, g_anim);
  for (const auto& [id, a] : g_set.remote) { (void)id; g_vis.drawAvatar(r, a, g_anim); }
}

void eng_avatar_destroy(void) { g_vis.destroy(); g_set.clear(); g_localAda = false; }

extern "C" {

KEEP void eng_avatar_set_local(const char* json) {
  EngEditCtx c; if (!eng_edit_ctx(c)) return;
  bool ok = false;
  AvatarState s = AvatarState::parse(json ? json : "", &ok);
  if (!ok) return;
  g_set.setLocal(s, c.scene ? c.scene->origin() : WorldOrigin{});
}

KEEP const char* eng_avatar_local_json(void) { return ret(g_set.local.state.toJson()); }

KEEP void eng_avatar_upsert(int id, const char* json) {
  EngEditCtx c; if (!eng_edit_ctx(c) || !c.scene) return;
  bool ok = false;
  AvatarState s = AvatarState::parse(json ? json : "", &ok);
  if (!ok) return;
  g_set.upsert(id, s, c.scene->origin(), g_clock);
}

KEEP void eng_avatar_remove(int id) { g_set.remove(id); }

KEEP void eng_avatar_gesture(const char* nama) {
  AvatarGesture g = AvatarGesture::None;
  if (nama && *nama && !parseAvatarGesture(nama, g)) return;
  g_set.startGesture(g);
}

KEEP void eng_avatar_outfit(int id, const char* pakaianJson) {
  Avatar* a = g_set.find(id); if (!a || !pakaianJson) return;
  std::string err; Json j = Json::parse(*pakaianJson ? pakaianJson : "{}", &err);
  if (!err.empty()) return;
  a->state.pakaian.fromJson(j);
}

// Screen anchor of one avatar (css px, y down): the top of its head, for the host's DOM name plate.
// out[0] = x, out[1] = y; returns 1 when the point is in front of the camera.
KEEP int eng_avatar_screen(int id, float* out) {
  if (!out) return 0;
  out[0] = out[1] = 0;
  const Avatar* a = g_set.find(id);
  EngEditCtx c;
  if (!a || !eng_edit_ctx(c) || !c.ready || !c.viewValid) return 0;
  vec4 clip = c.viewProj * vec4{a->pos.x, a->pos.y + 1.78f, a->pos.z, 1};
  if (clip.w <= 0.0001f) return 0;
  out[0] = (clip.x / clip.w * 0.5f + 0.5f) * (float)c.w;
  out[1] = (1 - (clip.y / clip.w * 0.5f + 0.5f)) * (float)c.h;
  return 1;
}

} // extern "C"

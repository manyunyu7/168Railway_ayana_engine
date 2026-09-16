// Avatar — the player figure of the multiplayer sessions (ppka-wannabe-2/docs/multiplayer.md §4/§5).
// One local avatar (id 0, driven by the engine's walker: CameraRig CamMode::Orang) plus the remote ones the
// host relays at ~15 Hz. Nothing here renders: AvatarVisuals (engine/world/avatar_visual.h) draws the
// placeholder body until the skinned models land, and the ABI lives in engine/api/engine_api_avatar.cpp.
//
// Coordinates. AvatarState is in SIM WORLD space like every other host coordinate: x / y = Mercator metres
// with y south-positive, z = height in metres, yaw in radians with 0 = +x and positive turning towards +y.
// Scene space is float and y-up (engine/world/coords.h), so:
//     scene = origin.toScene(state.x, state.y, state.z)     (world y -> scene z, world z -> scene y)
//     sceneYaw = -state.yaw                                  (mat4::rotationY maps +X to (cos, -sin) in (x, z))
// Avatar::place() / Avatar::store() are the only places that know this.
//
// Remote smoothing. Every upsert pushes a timestamped sample; the drawn pose is what the avatar was
// LAG (100 ms) ago, interpolated between the last two samples. When the newest sample is older than that
// the motion is extrapolated for at most EKSTRA (200 ms) and then frozen. Yaw takes the short way round.
#pragma once
#include "engine/core/json.h"
#include "engine/math/math.h"
#include "engine/world/coords.h"
#include <map>
#include <string>
#include <vector>

namespace eng {

enum class AvatarAnim { Diam, Jalan, Lari, Lompat, Duduk };
const char* avatarAnimName(AvatarAnim a);
bool parseAvatarAnim(const std::string& s, AvatarAnim& out);

enum class AvatarGesture { None, S40, S3, S1, Hormat, Lambai, Tunjuk };
const char* avatarGestureName(AvatarGesture g);            // "" for None
bool parseAvatarGesture(const std::string& s, AvatarGesture& out);
float avatarGestureDuration(AvatarGesture g);              // clip length in seconds (§6 table); 0 for None

// Catalog ids of the outfit pieces (model.json `avatar[]`); `kulit` indexes the six skin tones,
// `warnaBaju` is an RGB24 tint used by the tintable shirts (< 0 = the piece's own colour).
struct Pakaian {
  std::string tubuh = "tubuh-baku", baju = "baju-ppka-putih", celana = "celana-hitam", sepatu = "sepatu-hitam", topi = "topi-ppka-merah";
  std::vector<std::string> atribut{"atribut-tongkat-s40"};
  int kulit = 2, warnaBaju = -1;
  void fromJson(const Json& j);
  std::string toJson() const;
};

struct AvatarState {
  double x = 0, y = 0, z = 0;          // world metres (see the header note)
  float yaw = 0;                       // radians, 0 = +x
  AvatarAnim anim = AvatarAnim::Diam;
  AvatarGesture gesture = AvatarGesture::None;
  float gestureT = 0;                  // seconds since the gesture started
  Pakaian pakaian;
  void fromJson(const Json& j);        // missing fields keep their current value
  std::string toJson() const;
  static AvatarState parse(const std::string& json, bool* ok = nullptr);
};

// One figure in the scene. `pos` / `bodyYaw` are what the renderer uses (scene space) and are written by
// place() (local: straight from the walker) or sample() (remote: the smoothed pose).
struct Avatar {
  int id = 0;
  AvatarState state;
  vec3 pos;                 // scene space, at the FEET
  float bodyYaw = 0;        // scene yaw (rotationY convention)
  AvatarAnim anim = AvatarAnim::Diam;

  static constexpr double LAG = 0.10, EKSTRA = 0.20;
  struct Sample { double t = 0; vec3 pos; float yaw = 0; AvatarAnim anim = AvatarAnim::Diam; };

  void place(const WorldOrigin& o);                        // state -> pos / bodyYaw (no smoothing: local avatar)
  void store(const WorldOrigin& o);                        // pos / bodyYaw -> state (local avatar, for localJson)
  void push(const AvatarState& s, const WorldOrigin& o, double now);   // remote sample
  void sample(double now);                                 // pose LAG behind `now` into pos / bodyYaw / anim
  void tickGesture(float dt);                              // advance gestureT, drop the gesture past its clip

private:
  Sample s0_, s1_; int n_ = 0;
};

// Local avatar (id 0) + the remote ones by user id.
class AvatarSet {
public:
  Avatar local;
  std::map<int, Avatar> remote;

  void setLocal(const AvatarState& s, const WorldOrigin& o);   // outfit + anim from the host; position ignored
  void upsert(int id, const AvatarState& s, const WorldOrigin& o, double now);
  void remove(int id) { remote.erase(id); }
  void clear() { remote.clear(); local = Avatar{}; }
  Avatar* find(int id) { return id == 0 ? &local : (remote.count(id) ? &remote[id] : nullptr); }

  // The engine drives the local avatar: feet position and body yaw in scene space, speed (m/s) and whether
  // the feet are on the ground pick the animation.
  void driveLocal(vec3 feet, float bodyYaw, float speed, bool onGround, const WorldOrigin& o);
  void startGesture(AvatarGesture g) { local.state.gesture = g; local.state.gestureT = 0; }
  void tick(float dt, double now);   // gesture clocks + remote smoothing
  double clock() const { return clock_; }

private:
  double clock_ = 0;
};

} // namespace eng

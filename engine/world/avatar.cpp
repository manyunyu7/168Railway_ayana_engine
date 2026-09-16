#include "engine/world/avatar.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace eng {

namespace {
const char* ANIM[] = {"diam", "jalan", "lari", "lompat", "duduk"};
struct GestureDef { const char* nama; float durasi; };
// §6 clip lengths; index = AvatarGesture.
const GestureDef GESTURE[] = {{"", 0}, {"s40", 2.0f}, {"s3", 1.5f}, {"s1", 1.5f}, {"hormat", 1.5f}, {"lambai", 3.2f}, {"tunjuk", 1.2f}};
constexpr int GESTURE_N = (int)(sizeof(GESTURE) / sizeof(GESTURE[0]));

std::string esc(const std::string& s) {
  std::string o; o.reserve(s.size() + 2);
  for (char c : s) {
    switch (c) {
      case '"': o += "\\\""; break; case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break; case '\r': o += "\\r"; break; case '\t': o += "\\t"; break;
      default: if ((unsigned char)c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; } else o += c;
    }
  }
  return o;
}

float shortAngle(float a) { while (a > PI) a -= 2 * PI; while (a < -PI) a += 2 * PI; return a; }
float lerpAngle(float a, float b, float t) { return a + shortAngle(b - a) * t; }
} // namespace

const char* avatarAnimName(AvatarAnim a) { return ANIM[(int)a]; }
bool parseAvatarAnim(const std::string& s, AvatarAnim& out) {
  for (int i = 0; i < 5; ++i) if (s == ANIM[i]) { out = (AvatarAnim)i; return true; }
  return false;
}
const char* avatarGestureName(AvatarGesture g) { return GESTURE[(int)g].nama; }
bool parseAvatarGesture(const std::string& s, AvatarGesture& out) {
  for (int i = 1; i < GESTURE_N; ++i) if (s == GESTURE[i].nama) { out = (AvatarGesture)i; return true; }
  return false;
}
float avatarGestureDuration(AvatarGesture g) { return GESTURE[(int)g].durasi; }

// ---- Pakaian ----
void Pakaian::fromJson(const Json& j) {
  if (!j.isObject()) return;
  tubuh = j["tubuh"].stringOr(tubuh); baju = j["baju"].stringOr(baju); celana = j["celana"].stringOr(celana);
  sepatu = j["sepatu"].stringOr(sepatu);
  if (j.has("topi")) topi = j["topi"].stringOr("");
  if (j.has("atribut")) { atribut.clear(); for (const Json& a : j["atribut"].arr) if (a.isString()) atribut.push_back(a.str); }
  kulit = std::clamp(j["kulit"].intOr(kulit), 0, 5);
  warnaBaju = j.has("warnaBaju") ? (j["warnaBaju"].isNumber() ? j["warnaBaju"].intOr(-1) : -1) : warnaBaju;
}

std::string Pakaian::toJson() const {
  std::string o = "{\"tubuh\":\"" + esc(tubuh) + "\",\"baju\":\"" + esc(baju) + "\",\"celana\":\"" + esc(celana) +
                  "\",\"sepatu\":\"" + esc(sepatu) + "\"";
  if (!topi.empty()) o += ",\"topi\":\"" + esc(topi) + "\"";
  if (!atribut.empty()) {
    o += ",\"atribut\":[";
    for (size_t i = 0; i < atribut.size(); ++i) { if (i) o += ","; o += "\"" + esc(atribut[i]) + "\""; }
    o += "]";
  }
  char b[64]; std::snprintf(b, sizeof b, ",\"kulit\":%d", kulit); o += b;
  if (warnaBaju >= 0) { std::snprintf(b, sizeof b, ",\"warnaBaju\":%d", warnaBaju); o += b; }
  return o + "}";
}

// ---- AvatarState ----
void AvatarState::fromJson(const Json& j) {
  if (!j.isObject()) return;
  x = j["x"].numberOr(x); y = j["y"].numberOr(y); z = j["z"].numberOr(z);
  yaw = (float)j["yaw"].numberOr(yaw);
  if (j.has("anim")) parseAvatarAnim(j["anim"].stringOr(""), anim);
  if (j.has("gesture")) {
    AvatarGesture g = AvatarGesture::None;
    gesture = parseAvatarGesture(j["gesture"].stringOr(""), g) ? g : AvatarGesture::None;
    gestureT = (float)j["gestureT"].numberOr(0);
  }
  pakaian.fromJson(j["pakaian"]);
}

std::string AvatarState::toJson() const {
  char b[256];
  std::snprintf(b, sizeof b, "{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f,\"yaw\":%.4f,\"anim\":\"%s\"", x, y, z, yaw, avatarAnimName(anim));
  std::string o = b;
  if (gesture != AvatarGesture::None) {
    std::snprintf(b, sizeof b, ",\"gesture\":\"%s\",\"gestureT\":%.3f", avatarGestureName(gesture), gestureT);
    o += b;
  }
  return o + ",\"pakaian\":" + pakaian.toJson() + "}";
}

AvatarState AvatarState::parse(const std::string& json, bool* ok) {
  AvatarState s; std::string err;
  Json j = Json::parse(json.empty() ? "{}" : json, &err);
  bool good = err.empty() && j.isObject();
  if (good) s.fromJson(j);
  if (ok) *ok = good;
  return s;
}

// ---- Avatar ----
void Avatar::place(const WorldOrigin& o) {
  pos = o.toScene(state.x, state.y, (float)state.z);
  bodyYaw = -state.yaw;
  anim = state.anim;
}

void Avatar::store(const WorldOrigin& o) {
  double wx, wy; o.toWorld(pos, wx, wy);
  state.x = wx; state.y = wy; state.z = pos.y;
  state.yaw = -bodyYaw;
  state.anim = anim;
}

void Avatar::push(const AvatarState& s, const WorldOrigin& o, double now) {
  AvatarState keep = state;
  state = s;
  if (s.pakaian.tubuh.empty()) state.pakaian = keep.pakaian;   // outfit-less updates keep the known outfit
  Sample sm;
  sm.t = now;
  sm.pos = o.toScene(s.x, s.y, (float)s.z);
  sm.yaw = -s.yaw;
  sm.anim = s.anim;
  if (n_ == 0) { s0_ = s1_ = sm; n_ = 1; pos = sm.pos; bodyYaw = sm.yaw; anim = sm.anim; return; }
  if (sm.t <= s1_.t) { s1_ = sm; return; }   // out-of-order / same instant: replace the newest
  s0_ = s1_; s1_ = sm; n_ = 2;
}

void Avatar::sample(double now) {
  if (n_ == 0) return;
  const double t = now - LAG;
  if (n_ == 1 || s1_.t <= s0_.t) { pos = s1_.pos; bodyYaw = s1_.yaw; anim = s1_.anim; return; }
  double f = (t - s0_.t) / (s1_.t - s0_.t);
  const double fMax = 1 + EKSTRA / (s1_.t - s0_.t);   // extrapolate at most EKSTRA seconds past the newest sample
  f = std::clamp(f, 0.0, fMax);
  pos = s0_.pos + (s1_.pos - s0_.pos) * (float)f;
  bodyYaw = lerpAngle(s0_.yaw, s1_.yaw, (float)std::clamp(f, 0.0, 1.0));
  anim = f >= 0.5 ? s1_.anim : s0_.anim;
}

void Avatar::tickGesture(float dt) {
  if (state.gesture == AvatarGesture::None) return;
  state.gestureT += std::max(0.f, dt);
  if (state.gestureT >= avatarGestureDuration(state.gesture)) { state.gesture = AvatarGesture::None; state.gestureT = 0; }
}

// ---- AvatarSet ----
void AvatarSet::setLocal(const AvatarState& s, const WorldOrigin&) {
  // The host owns the outfit and may pre-set an animation / gesture; the POSITION is the engine's (§5).
  local.state.pakaian = s.pakaian;
  local.state.anim = s.anim;
  local.state.gesture = s.gesture;
  local.state.gestureT = s.gestureT;
}

void AvatarSet::upsert(int id, const AvatarState& s, const WorldOrigin& o, double now) {
  if (id == 0) { setLocal(s, o); return; }
  Avatar& a = remote[id];
  a.id = id;
  a.push(s, o, now);
}

void AvatarSet::driveLocal(vec3 feet, float bodyYaw, float speed, bool onGround, const WorldOrigin& o) {
  local.pos = feet;
  local.bodyYaw = bodyYaw;
  local.anim = !onGround ? AvatarAnim::Lompat : speed > 2.2f ? AvatarAnim::Lari : speed > 0.15f ? AvatarAnim::Jalan : AvatarAnim::Diam;
  local.store(o);
}

void AvatarSet::tick(float dt, double now) {
  clock_ = now;
  local.tickGesture(dt);
  for (auto& [id, a] : remote) { a.tickGesture(dt); a.sample(now); }
}

} // namespace eng

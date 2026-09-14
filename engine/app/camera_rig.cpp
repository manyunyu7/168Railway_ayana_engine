#include "engine/app/camera_rig.h"
#include "engine/world/sway.h"
#include <algorithm>
#include <cmath>

namespace eng {

namespace {
constexpr float ZOOM_LIPAT = 4, FOV_MIN = 8, ZOOM_TAU = 0.09f;     // uji3dZoom.ts
constexpr float EL_MIN = -8 * PI / 180, EL_MAKS = PI / 2;            // uji3dOrbit.ts elevation clamp
constexpr float MATA_JALAN = 1.62f, LAJU_JALAN = 4.5f, LAJU_LARI = 12; // walker eye / speeds
constexpr float DRAG_KABIN = 0.004f, DRAG_ORBIT = 0.005f, DRAG_JALAN = 0.005f;
constexpr float KABIN_SISI_MAKS = 1.2f, KABIN_TINGGI_MIN = 0.5f, KABIN_TINGGI_MAKS = 3.5f;   // cab eye limits
constexpr float LAJU_KABIN = 1.5f, LAJU_KABIN_CEPAT = 4, KABIN_SCROLL = 0.5f;               // m/s, m per scroll step
constexpr float AKSEL_TAU = 0.35f;

float fovTeropong(float baku) { return std::max(FOV_MIN, baku / ZOOM_LIPAT); }
float fovDariCampur(float baku, float c) { c = std::clamp(c, 0.f, 1.f); return baku + (fovTeropong(baku) - baku) * c; }
float skalaSeret(float kini, float baku) { return std::tan(kini * PI / 360) / std::tan(baku * PI / 360); }
vec3 horizontal(vec3 v) { v.y = 0; return v; }
} // namespace

bool parseCamMode(const std::string& s, CamMode& out) {
  for (int i = 0; i < 6; ++i) if (s == camModeName((CamMode)i)) { out = (CamMode)i; return true; }
  return false;
}

CamProfile camProfile(CamMode m) {
  switch (m) {
    case CamMode::Jalan:   return {"Jalan-jalan", 0.1f, 12000, 70, false, 400, 5000, 999};
    case CamMode::Kabin:   return {"Kabin", 0.15f, 12000, 62, false, 500, 6000, 26};
    case CamMode::Samping: return {"Samping", 0.5f, 20000, 52, false, 700, 9000, 7};
    case CamMode::Atas:    return {"Atas", 1, 40000, 52, true, 0, 0, 5};
    case CamMode::Ekor:    return {"Ekor", 0.5f, 20000, 52, false, 700, 9000, 8};
    default:               return {"Bebas", 1, 40000, 52, true, 0, 0, 999};
  }
}

MataKabin mataKabin(const std::string& k) {
  if (k == "cc201" || k == "cc203") return {2.4f, 3.05f, 0.6f};
  if (k == "cc206") return {0, 3.05f, 0.6f};
  if (k == "krl-kuha") return {0.5f, 2.75f, -0.55f};
  return {2.2f, 3.2f, 0};   // MATA_BAWAAN
}

vec3 TrainPath::pointBehind(float d) const {
  if (pts.empty()) return {};
  if (pts.size() == 1) return pts[0];
  d = std::max(0.f, d);
  if (d >= cum.back()) { vec3 dir = normalize(pts.back() - pts[pts.size() - 2]); return pts.back() + dir * (d - cum.back()); }
  size_t i = 1; while (i < cum.size() && cum[i] < d) ++i;
  float seg = cum[i] - cum[i - 1], t = seg > 0 ? (d - cum[i - 1]) / seg : 0;
  return lerp(pts[i - 1], pts[i], t);
}

float CameraRig::fovBaku() const {
  return mode == CamMode::Kabin ? fovKabin : mode == CamMode::Jalan ? fovJalan : camProfile(mode).fov;
}

void CameraRig::setMode(CamMode m, vec3 eye, vec3 look) {
  mode = m;
  campurKam_ = 0;
  lihatYaw_ = lihatPitch_ = orbitAz_ = orbitEl_ = 0; toleh_ = 0;
  upOrbit_ = upKam_ = {0, 1, 0};
  camPos_ = eye; camLook_ = look; camUp_ = {0, 1, 0};
  fovKini_ = fovDariCampur(fovBaku(), zoomCampur_);   // keep a running telescope blend (no jump while Z is held)
}

void CameraRig::enterWalk(vec3 eye, vec3 look, const GroundFn& ground) {
  vec3 arah = horizontal(look - eye);
  jalanYaw_ = dot(arah, arah) > 1e-6f ? std::atan2(-arah.x, -arah.z) : 0;
  jalanPitch_ = 0; tempuh_ = 0;
  pejalan_ = {look.x, ground(look.x, look.z), look.z};
}

// dunia3d.ts rig(): position + look point for the train modes
bool CameraRig::rig(const TrainPath& t, const GroundFn& ground) {
  auto arahKA = [&](float dari) {
    vec3 a = t.pointBehind(dari), b = t.pointBehind(dari + 12);
    vec3 f = horizontal(a - b); if (dot(f, f) < 1e-6f) return false;
    vFwd_ = normalize(f); vKanan_ = {-vFwd_.z, 0, vFwd_.x}; return true;
  };
  auto jagaTanah = [&](vec3& p, float minH) { p.y = std::max(p.y, ground(p.x, p.z) + minH); };
  float panjang = t.length;
  switch (mode) {
    case CamMode::Kabin: {
      MataKabin mk = mataKabin(t.sarana);
      float lim = std::max(0.f, panjang * 0.5f);
      kabinMaju = std::clamp(kabinMaju, -lim, lim);
      kabinSisi = std::clamp(kabinSisi, -KABIN_SISI_MAKS - mk.sisi, KABIN_SISI_MAKS - mk.sisi);
      kabinNaik = std::clamp(kabinNaik, KABIN_TINGGI_MIN - mk.tinggi, KABIN_TINGGI_MAKS - mk.tinggi);
      float mundur = mk.mundur - kabinMaju;   // metres behind the nose (negative = ahead of it)
      vec3 mata = t.pointBehind(std::max(0.f, mundur));
      if (!arahKA(std::max(0.f, mundur))) return false;
      posKam_ = mata + vFwd_ * std::max(0.f, -mundur) + vKanan_ * (mk.sisi + kabinSisi); posKam_.y = mata.y + mk.tinggi + kabinNaik;
      // cab sway (dunia3d.ts rig 'kabin'): shifting terms damped while zoomed (redamGoyang), roll not
      sway::Goyang g;
      float skala = sway::skalaLaju(t.timeScale) * std::max(0.f, goyangSkala);
      if (skala > 0) {
        auto yawAt = [&](float d) { vec3 f = horizontal(t.pointBehind(d) - t.pointBehind(d + 12)); return std::atan2(-f.z, f.x); };
        float kurva = sway::kurvaRel(yawAt(0), yawAt(sway::BASIS_KURVA), sway::BASIS_KURVA);
        g = sway::goyangKabin(sway::hitungGoyang({posKam_.x, posKam_.z, t.speed, kurva, aksel_, skala}));
      }
      float rg = std::sqrt(skalaSeret(fovKini_, fovBaku()));
      posKam_ += vKanan_ * (g.geser * rg); posKam_.y += g.naik * rg;
      lihatKam_ = posKam_ + vFwd_ * 100 + vKanan_ * (-100 * g.yaw * rg); lihatKam_.y += 100 * g.angguk * rg;
      upKam_ = vec3{0, 1, 0} * std::cos(g.roll) + vKanan_ * std::sin(g.roll);
      return true;
    }
    case CamMode::Samping: {
      vec3 hidung = t.pointBehind(0), sasaran = t.pointBehind(std::min(panjang * 0.28f, 55.f));
      if (!arahKA(0)) return false;
      posKam_ = hidung + vKanan_ * (jarakSamping * sisiSamping) + vFwd_ * (jarakSamping * 0.45f); posKam_.y = hidung.y + tinggiSamping;
      jagaTanah(posKam_, 2);
      lihatKam_ = sasaran; lihatKam_.y = sasaran.y + 2;
      return true;
    }
    case CamMode::Atas: {
      vec3 jangkar = t.pointBehind(panjang * 0.5f);
      if (!arahKA(0)) return false;
      posKam_ = jangkar; posKam_.y = jangkar.y + tinggiAtas;
      lihatKam_ = jangkar;
      return true;
    }
    case CamMode::Ekor: {
      vec3 buntut = t.pointBehind(panjang);
      if (!arahKA(std::max(0.f, panjang - 12))) return false;
      posKam_ = buntut - vFwd_ * jarakEkor; posKam_.y = buntut.y + jarakEkor * 0.35f + 3;
      jagaTanah(posKam_, 3);
      lihatKam_ = buntut; lihatKam_.y = buntut.y + 2.5f;
      return true;
    }
    default: return false;
  }
}

// KABIN: drag turns the LOOK DIRECTION about the eye (the neck), the world shifts.
void CameraRig::tolehKepala() {
  if (lihatYaw_ == 0 && lihatPitch_ == 0) return;
  vec3 arah = lihatKam_ - posKam_; float jauh = length(arah); if (jauh < 1e-4f) return;
  arah = arah / jauh;
  arah = quat::axisAngle({0, 1, 0}, lihatYaw_).toMat4().transformDir(arah);
  vec3 kanan = cross(arah, {0, 1, 0});
  if (dot(kanan, kanan) > 1e-8f) arah = quat::axisAngle(normalize(kanan), lihatPitch_).toMat4().transformDir(arah);
  lihatKam_ = posKam_ + arah * jauh;
}

// SAMPING/ATAS/EKOR: drag orbits AROUND the subject (uji3dOrbit.ts orbitSubjek) — azimuth relative to the
// train's heading so the camera does not swing on every curve; offset length preserved.
void CameraRig::orbitKe(const GroundFn& ground) {
  vec3 d = posKam_ - lihatKam_; float jauh = length(d); if (!(jauh > 1e-4f)) return;
  float f = dot(d, vFwd_), k = dot(d, vKanan_);
  float az0 = f * f + k * k < 1e-8f * jauh * jauh ? PI : std::atan2(k, f);   // vertical offset (ATAS): behind the train = heading-up
  float el0 = std::asin(std::clamp(d.y / jauh, -1.f, 1.f));
  float az = az0 + orbitAz_, elMau = el0 + orbitEl_, el = std::clamp(elMau, EL_MIN, EL_MAKS);
  float ce = std::cos(el), se = std::sin(el), ca = std::cos(az), sa = std::sin(az);
  vec3 h = vFwd_ * ca + vKanan_ * sa;
  posKam_ = lihatKam_ + h * (jauh * ce) + vec3{0, jauh * se, 0};
  upOrbit_ = {-h.x * se, ce - h.y * se, -h.z * se};
  orbitEl_ += el - elMau;   // clamped part written back so the drag does not accumulate invisible angle
  float minY = ground(posKam_.x, posKam_.z) + 2; if (posKam_.y < minY) posKam_.y = minY;
}

void CameraRig::langkahJalan(float dt, const GroundFn& ground, const WalkInput& in) {
  vec3 fwd{-std::sin(jalanYaw_), 0, -std::cos(jalanYaw_)}, right{std::cos(jalanYaw_), 0, -std::sin(jalanYaw_)};
  float v = in.run ? LAJU_LARI : LAJU_JALAN;
  vec3 move = fwd * in.forward + right * in.side;
  if (dot(move, move) > 1e-6f) { move = normalize(move) * (v * dt); pejalan_ += move; tempuh_ += length(move); }
  pejalan_.y = ground(pejalan_.x, pejalan_.z);
  float ayun = std::sin(tempuh_ * 2.1f) * 0.035f;   // step bob from distance walked, not time
  posKam_ = {pejalan_.x, pejalan_.y + MATA_JALAN + ayun, pejalan_.z};
  vec3 arah{-std::sin(jalanYaw_) * std::cos(jalanPitch_), std::sin(jalanPitch_), -std::cos(jalanYaw_) * std::cos(jalanPitch_)};
  lihatKam_ = posKam_ + arah * 60;
  camPos_ = posKam_; camLook_ = lihatKam_; camUp_ = {0, 1, 0};
}

// KABIN: move the eye with the keys (clamped in rig()) and filter the subject's acceleration for the brake pitch.
void CameraRig::langkahKabin(float dt, const WalkInput& in, const TrainPath& t) {
  if (in.reset) resetKabin();
  float v = (in.run ? LAJU_KABIN_CEPAT : LAJU_KABIN) * std::max(0.f, dt);
  kabinMaju += in.forward * v; kabinSisi += in.side * v; kabinNaik += in.up * v;
  if (t.id != subjekId_) { subjekId_ = t.id; vSebelum_ = t.speed; aksel_ = 0; return; }
  float dtSim = std::max(0.f, dt) * (float)std::max(0.0, t.timeScale);
  if (dtSim <= 0) return;
  aksel_ += ((t.speed - vSebelum_) / dtSim - aksel_) * std::min(1.f, dtSim / AKSEL_TAU);
  vSebelum_ = t.speed;
}

void CameraRig::langkahZoom(float dt) {
  float mau = bolehTeropong() && (teropongTahan || teropongKunci) ? 1.f : 0.f;
  if (zoomCampur_ != mau) {
    float n = zoomCampur_ + (mau - zoomCampur_) * (1 - std::exp(-std::max(0.f, dt) / ZOOM_TAU));
    zoomCampur_ = std::fabs(n - mau) < 1e-3f ? mau : n;
  }
  fovKini_ = fovDariCampur(fovBaku(), zoomCampur_);
}

bool CameraRig::step(float dt, const TrainPath* subject, const GroundFn& ground, const WalkInput& walk) {
  langkahZoom(dt);
  if (mode == CamMode::Bebas) return true;
  if (mode == CamMode::Jalan) { langkahJalan(dt, ground, walk); return true; }
  if (!subject || !subject->valid()) return false;
  if (mode == CamMode::Kabin) langkahKabin(dt, walk, *subject);
  if (!rig(*subject, ground)) return false;
  toleh_ += dt;
  if (mode == CamMode::Kabin) {
    if (toleh_ > 1.2f) { float kk = 1 - std::exp(-3 * dt); lihatYaw_ -= lihatYaw_ * kk; lihatPitch_ -= lihatPitch_ * kk; }   // the neck returns by itself
    tolehKepala();
  } else orbitKe(ground);
  campurKam_ = std::min(1.f, campurKam_ + dt / 0.4f);
  float redam = 4 + (camProfile(mode).redam - 4) * campurKam_;
  float k = 1 - std::exp(-redam * dt);
  if (mode == CamMode::Kabin && campurKam_ >= 1) camPos_ = posKam_;   // position sticks absolutely (at 16-32x a lerp lags into the loco body); the look point keeps the "neck"
  else camPos_ = lerp(camPos_, posKam_, k);
  camLook_ = lerp(camLook_, lihatKam_, k);
  if (mode == CamMode::Kabin) camUp_ = normalize(lerp(camUp_, upKam_, k)); else camUp_ = upOrbit_;   // heading-up ATAS is born here
  return true;
}

void CameraRig::drag(float dx, float dy) {
  float sk = skalaSeret(fovKini_, fovBaku());   // slower while zoomed: screen pixels per drag pixel stay constant
  if (mode == CamMode::Kabin) { lihatYaw_ -= dx * DRAG_KABIN * sk; lihatPitch_ = std::clamp(lihatPitch_ - dy * DRAG_KABIN * sk, -1.2f, 1.2f); toleh_ = 0; }
  else if (mode == CamMode::Jalan) { jalanYaw_ -= dx * DRAG_JALAN * sk; jalanPitch_ = std::clamp(jalanPitch_ - dy * DRAG_JALAN * sk, -1.4f, 1.4f); }
  else if (mode != CamMode::Bebas) { orbitAz_ += dx * DRAG_ORBIT * sk; orbitEl_ += dy * DRAG_ORBIT * sk; }
}

void CameraRig::scroll(float steps) {
  float f = std::pow(0.9f, steps);
  switch (mode) {
    case CamMode::Kabin: kabinMaju += steps * KABIN_SCROLL; break;   // forward/back along the vehicle (clamped in rig)
    case CamMode::Jalan: fovJalan = std::clamp(fovJalan * f, 45.f, 95.f); break;
    case CamMode::Samping: jarakSamping = std::clamp(jarakSamping * f, 8.f, 160.f); break;
    case CamMode::Atas: tinggiAtas = std::clamp(tinggiAtas * f, 40.f, 2000.f); break;
    case CamMode::Ekor: jarakEkor = std::clamp(jarakEkor * f, 10.f, 200.f); break;
    default: break;
  }
}

float CameraRig::fogDensity(float worldW) const {
  CamProfile p = camProfile(mode);
  float dekat = std::min(p.corridorFog ? worldW * 0.4f : p.fogNear, p.far * 0.6f);
  float jauh = std::min(p.corridorFog ? worldW * 2.2f : p.fogFar, p.far * 0.98f);
  float mid = (dekat + jauh) * 0.5f;
  return mid > 0 ? std::sqrt(std::log(2.f)) / mid : 0;   // 1 - exp(-(d·D)²) = 0.5 at the midpoint of the linear range
}

} // namespace eng

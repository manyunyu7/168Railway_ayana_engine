// Camera modes of the 3D client (docs/world-spec.md §9.1, port of dunia3d.ts:1546-2050 rig/langkahKamera,
// uji3dOrbit.ts orbitSubjek, uji3dZoom.ts telescope). `bebas` (orbit) stays in Game; this class owns the
// train-following rigs (kabin/samping/atas/ekor), the walk mode (jalan) and the damping/telescope maths.
// KABIN: the eye starts at MATA_KABIN and can be moved (engine addition): W/S or up/down = along the vehicle
// axis (−L/2..+L/2 from the default), A/D = sideways (±1.2 m from the centreline), Q/E = height (0.5..3.5 m
// over the rail head), scroll = along too, R = back to MATA_KABIN; nothing is persisted. The cab rocks with
// speed / curvature / braking (uji3dGoyang.ts goyangKabin, engine/world/sway.h), damped while zoomed.
#pragma once
#include "engine/math/geometry.h"
#include "engine/math/math.h"
#include <functional>
#include <string>
#include <vector>

namespace eng {

enum class CamMode { Bebas, Jalan, Kabin, Samping, Atas, Ekor };
inline const char* camModeName(CamMode m) { static const char* N[] = {"bebas", "jalan", "kabin", "samping", "atas", "ekor"}; return N[(int)m]; }
bool parseCamMode(const std::string& s, CamMode& out);

// Per-mode profile (profilKam). fog = linear [near, far] of the reference; corridor = the bbox-width formula.
struct CamProfile { const char* nama; float near, far, fov; bool corridorFog; float fogNear, fogFar; float redam; };
CamProfile camProfile(CamMode m);

// Driver's eye per loco type (dunia3dKonst.ts MATA_KABIN / MATA_BAWAAN): metres back from the nose, up, to the right.
struct MataKabin { float mundur, tinggi, sisi; };
MataKabin mataKabin(const std::string& sarana);

// The subject train as a polyline from the nose backwards (scene space, rail-head heights) so the rigs can
// ask for "the point d metres behind the nose" (Train.pointBehind).
struct TrainPath {
  std::vector<vec3> pts;      // nose first
  std::vector<float> cum;     // metres from the nose at each point
  float length = 0;           // consist length (cars + gaps)
  std::string sarana;         // first car's catalog id (cab eye lookup)
  std::string id;             // train id (the cab's acceleration filter restarts when the subject changes)
  float speed = 0;            // m/s (sim speed, not scaled by the clock)
  double timeScale = 1;       // session clock scale (sway amplitude damping, skalaLaju)
  bool valid() const { return pts.size() >= 2; }
  vec3 pointBehind(float d) const;   // clamped to [0, cum.back()], extrapolated past the tail
};

// A box the walker collides with (uji3dJalanKaki.ts DuniaJalan, with AABBs instead of mesh raycasts): `wall`
// boxes stop the body (slid along, RADIUS 0.38 m, tested at knee/chest/head) and every box is a floor when its
// top is within NAIK_MAKS 0.45 m of the feet (platform slabs are reached by jumping: LAJU_LOMPAT 4.6 m/s, double jump).
struct WalkBox { AABB box; bool wall = true; };
// Movement keys: jalan = walking (jump = Space, edge-triggered); kabin = moving the eye (forward/side/up, run = faster, reset = R).
struct WalkInput { float forward = 0, side = 0, up = 0; bool run = false, reset = false, jump = false; const std::vector<WalkBox>* boxes = nullptr; };

class CameraRig {
public:
  using GroundFn = std::function<float(float, float)>;   // scene (x, z) -> carved ground y

  CamMode mode = CamMode::Bebas;
  // adjustable framing (scroll in each mode)
  float fovKabin = 62, fovJalan = 70, jarakSamping = 28, tinggiSamping = 7, sisiSamping = 1, tinggiAtas = 120, jarakEkor = 34;
  // telescope (Z held / locked): fov/4, floor 8°, blend tau 0.09 s
  bool teropongTahan = false, teropongKunci = false;
  // cab eye offsets from MATA_KABIN (m): along the vehicle axis (+ forward), to the right, up; clamped in rig()
  float kabinMaju = 0, kabinSisi = 0, kabinNaik = 0;
  float goyangSkala = 1;      // sway slider (0 = still cab)
  void resetKabin() { kabinMaju = kabinSisi = kabinNaik = 0; }

  // Enter a mode. `eye/look` = the current camera so the damping starts from where the view is.
  void setMode(CamMode m, vec3 eye, vec3 look);
  // One frame: rig from the subject (may be null for jalan/bebas), then damping. Returns false when
  // the mode needs a train and none is available (caller falls back to orbit).
  bool step(float dt, const TrainPath* subject, const GroundFn& ground, const WalkInput& walk);
  void drag(float dx, float dy);     // pixels: kabin = turn the head, others = orbit the subject, jalan = look
  void scroll(float steps);          // adjusts the mode's parameter (profile "atur"); kabin = eye forward/back
  void enterWalk(vec3 eye, vec3 look, const GroundFn& ground);   // turunJalan: stand where the orbit target was

  bool walkerOnGround() const { return diTanah_; }
  vec3 eye() const { return camPos_; }
  vec3 look() const { return camLook_; }
  vec3 up() const { return camUp_; }
  float fovDeg() const { return fovKini_; }
  float fovBaku() const;
  // Reference distance of the mode (profilKam `acuan`, the LOD yardstick): bebas = the orbit distance given,
  // jalan 90, kabin 60, samping/ekor = their following distance, atas = its height.
  float acuan(float orbitDistance) const {
    switch (mode) { case CamMode::Jalan: return 90; case CamMode::Kabin: return 60; case CamMode::Samping: return jarakSamping;
                    case CamMode::Atas: return tinggiAtas; case CamMode::Ekor: return jarakEkor; default: return orbitDistance; }
  }
  bool bolehTeropong() const { return mode == CamMode::Kabin || mode == CamMode::Samping || mode == CamMode::Ekor || mode == CamMode::Jalan; }
  mat4 view() const { return mat4::lookAt(camPos_, camLook_, camUp_); }
  mat4 projection(float aspect) const { CamProfile p = camProfile(mode); return mat4::perspective(radians(fovKini_), aspect, p.near, p.far); }
  // Exponential-squared fog density equivalent to the linear range: matched at the midpoint (50 % fog).
  // worldW = bbox width for the corridor modes.
  float fogDensity(float worldW) const;

private:
  bool rig(const TrainPath& t, const GroundFn& ground);
  void langkahKabin(float dt, const WalkInput& in, const TrainPath& t);   // eye offsets + acceleration filter
  void tolehKepala();
  void orbitKe(const GroundFn& ground);
  void langkahJalan(float dt, const GroundFn& ground, const WalkInput& in);
  void langkahZoom(float dt);
  vec3 posKam_, lihatKam_, upKam_{0, 1, 0}, upOrbit_{0, 1, 0};   // rig output this frame
  vec3 camPos_, camLook_, camUp_{0, 1, 0};                        // damped camera
  vec3 vFwd_{1, 0, 0}, vKanan_{0, 0, 1};
  float campurKam_ = 1, zoomCampur_ = 0, fovKini_ = 52;
  float lihatYaw_ = 0, lihatPitch_ = 0, orbitAz_ = 0, orbitEl_ = 0, toleh_ = 0;
  // walker (uji3dJalanKaki): body position on the ground, heading, look pitch, distance walked (step bob)
  vec3 pejalan_; float jalanYaw_ = 0, jalanPitch_ = 0, tempuh_ = 0;
  float vyJalan_ = 0; bool diTanah_ = true, lompatSebelum_ = false; int lompatSisa_ = 2;
  // cab sway inputs: filtered longitudinal acceleration of the subject (keretaVisual3d.ts perbaruiAksel, τ 0.35 s)
  std::string subjekId_; float vSebelum_ = 0, aksel_ = 0;
};

} // namespace eng

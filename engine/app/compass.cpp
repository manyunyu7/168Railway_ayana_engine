#include "engine/app/compass.h"
#include "engine/render/mesh_builder.h"
#include <cmath>

namespace eng {

float glideSpeed(float r, float distance, float k) {
  if (!(r > COMPASS_DEAD_ZONE)) return 0;
  float u = std::fmin(1.f, (r - COMPASS_DEAD_ZONE) / (1 - COMPASS_DEAD_ZONE));
  return std::pow(u, 1.6f) * distance * 1.6f * k;
}

vec2 glideDirection(float nx, float ny, float ox, float oz) {
  float l = std::hypot(ox, oz); if (l == 0) l = 1;
  float sx = ox / l, sz = oz / l;
  float fwdX = -sx, fwdZ = -sz;              // camera → focus = forward
  float rightX = -fwdZ, rightZ = fwdX;
  return {fwdX * -ny + rightX * nx, fwdZ * -ny + rightZ * nx};
}

float rotationRate(float nx, float r) {
  if (!(r > COMPASS_DEAD_ZONE)) return 0;
  float u = std::fmin(1.f, (r - COMPASS_DEAD_ZONE) / (1 - COMPASS_DEAD_ZONE));
  return -nx * 1.1f * std::fmin(1.f, std::pow(u, 1.6f) * 2);
}

vec2 clampJump(vec2 d, float distance) {
  float l = std::hypot(d.x, d.y), mx = COMPASS_JUMP_LIMIT * distance;
  if (l <= mx || l == 0) return d;
  return {d.x / l * mx, d.y / l * mx};
}

int azimuthDegrees(float lx, float lz) {
  int a = (int)std::lround(std::atan2(lx, -lz) * 180 / PI);
  return ((a % 360) + 360) % 360;
}

// ---- reticle geometry (amber ring, cross ticks, north cone, view arrow) ----

static rhi::Mesh flatRing(float r0, float r1, int n) {
  MeshBuilder b;
  for (int i = 0; i < n; ++i) {
    float a0 = 2 * PI * i / n, a1 = 2 * PI * (i + 1) / n;
    vec3 p0{r0 * std::cos(a0), 0, r0 * std::sin(a0)}, p1{r1 * std::cos(a0), 0, r1 * std::sin(a0)};
    vec3 p2{r1 * std::cos(a1), 0, r1 * std::sin(a1)}, p3{r0 * std::cos(a1), 0, r0 * std::sin(a1)};
    b.quad(p0, p3, p2, p1);
  }
  return b.upload();
}
static void bar(MeshBuilder& b, vec3 a, vec3 c, float wdt) {   // flat bar from a to c
  vec3 d = normalize(c - a), n{-d.z, 0, d.x}; n = n * (wdt / 2);
  b.quad(a - n, c - n, c + n, a + n);
}
static rhi::Mesh cone(float radius, float height, int n, float yawOffset) {   // tip toward +Z... built along -Z (north)
  MeshBuilder b; vec3 tip{0, 0, -height / 2}, base{0, 0, height / 2};
  for (int i = 0; i < n; ++i) {
    float a0 = 2 * PI * i / n + yawOffset, a1 = 2 * PI * (i + 1) / n + yawOffset;
    vec3 p0 = base + vec3{radius * std::cos(a0), radius * std::sin(a0), 0}, p1 = base + vec3{radius * std::cos(a1), radius * std::sin(a1), 0};
    uint32_t i0 = b.vertex(tip, {0, 1, 0}, {}), i1 = b.vertex(p0, {0, 1, 0}, {}), i2 = b.vertex(p1, {0, 1, 0}, {});
    b.triangle(i0, i1, i2); b.triangle(i0, i2, i1);
  }
  return b.upload();
}

void Compass::init(std::function<float(float, float)> groundHeight) {
  ground_ = std::move(groundHeight);
  ring_ = flatRing(5.2f, 6.2f, 48);
  MeshBuilder cb;
  bar(cb, {-9, 0, 0}, {-6.6f, 0, 0}, 0.5f); bar(cb, {6.6f, 0, 0}, {9, 0, 0}, 0.5f);
  bar(cb, {0, 0, -9}, {0, 0, -6.6f}, 0.5f); bar(cb, {0, 0, 6.6f}, {0, 0, 9}, 0.5f);
  { MeshBuilder dot; dot.quad({-1, 0, -1}, {-1, 0, 1}, {1, 0, 1}, {1, 0, -1}); cb.append(dot, mat4::identity()); }
  cross_ = cb.upload();
  north_ = cone(1.1f, 3, 3, 0);
  viewArrow_ = cone(1.6f, 4, 3, 0);
  mat_.baseColor = {0.91f, 0.65f, 0.23f, 0}; mat_.metallic = 0; mat_.roughness = 1;
  mat_.emissive = {0.91f, 0.65f, 0.23f}; mat_.alphaMode = AlphaMode::Blend; mat_.doubleSided = true; mat_.depthTest = false;
}

void Compass::shutdown() { rhi::destroyMesh(ring_); rhi::destroyMesh(cross_); rhi::destroyMesh(north_); rhi::destroyMesh(viewArrow_); }

void Compass::wake() { opacity_ = 1; fadeAt_ = time_ + COMPASS_FADE_SECONDS; }

bool Compass::groundHit(const Ray& ray, vec3& out) const {
  // march along the ray until it dips under the heightfield, then bisect
  float step = 4, t = 0; vec3 prev = ray.origin;
  for (int i = 0; i < 4000; ++i) {
    t += step; vec3 p = ray.at(t);
    if (p.y <= ground_(p.x, p.z)) {
      float a = t - step, b = t;
      for (int k = 0; k < 12; ++k) { float m = (a + b) / 2; vec3 q = ray.at(m); (q.y <= ground_(q.x, q.z) ? b : a) = m; }
      out = ray.at((a + b) / 2); return true;
    }
    prev = p; step = std::fmin(step * 1.05f, 60);
    if (t > 60000) break;
  }
  return false;
}

bool Compass::update(OrbitCamera& cam, double mx, double my, int w, int h, bool rightDown, bool ctrl,
                     bool left, bool right, bool up, bool down, float dt, const mat4& invViewProj) {
  time_ += dt;
  float nx = (float)(mx / w) * 2 - 1, ny = (float)(my / h) * 2 - 1;
  float r = std::hypot(nx, ny);
  vec3 offset = cam.position() - cam.target;
  float dist = std::hypot(offset.x, offset.z);

  // short click vs hold
  if (rightDown && !pressed_) { pressed_ = true; pressX_ = mx; pressY_ = my; pressT_ = time_; }
  if (!rightDown && pressed_) {
    pressed_ = false;
    bool shortClick = time_ - pressT_ < 0.25f && std::fabs(mx - pressX_) + std::fabs(my - pressY_) < 6;
    gliding_ = false;
    if (shortClick) {
      vec3 hit; Ray ray = screenRay((float)(pressX_ / w), (float)(pressY_ / h), invViewProj);
      if (groundHit(ray, hit)) {
        vec2 d = clampJump({hit.x - cam.target.x, hit.z - cam.target.z}, cam.distance);
        jump_ = {cam.target, {cam.target.x + d.x, 0, cam.target.z + d.y}, 0, true};
        jump_.to.y = ground_(jump_.to.x, jump_.to.z);
        wake();
      }
    }
  }
  if (pressed_ && time_ - pressT_ >= 0.25f) gliding_ = true;

  // glide: velocity from cursor offset to the screen centre
  if (gliding_) {
    jump_.active = false;
    float v = glideSpeed(r, cam.distance, settings.speed);
    vec2 dir = glideDirection(nx, ny, offset.x, offset.z);
    float l = std::hypot(dir.x, dir.y); if (l > 0) dir = {dir.x / l, dir.y / l};
    cam.target.x += dir.x * v * dt; cam.target.z += dir.y * v * dt;
    if (settings.rotation) cam.yaw += rotationRate(nx, r) * dt;
    wake();
  }
  // keyboard nudge (Ctrl+arrows) / rotate (arrows)
  float kx = (right ? 1.f : 0.f) - (left ? 1.f : 0.f), ky = (down ? 1.f : 0.f) - (up ? 1.f : 0.f);
  if (kx != 0 || ky != 0) {
    if (ctrl) {
      vec2 dir = glideDirection(kx, ky, offset.x, offset.z);
      cam.target.x += dir.x * cam.distance * 0.6f * dt; cam.target.z += dir.y * cam.distance * 0.6f * dt;
    } else { cam.yaw -= kx * 1.2f * dt; cam.pitch = std::fmax(radians(5), std::fmin(radians(89), cam.pitch + ky * 0.8f * dt)); }
    wake();
  }
  // jump animation (smoothstep)
  if (jump_.active) {
    jump_.u = std::fmin(1.f, jump_.u + dt / COMPASS_JUMP_SECONDS);
    float s = jump_.u * jump_.u * (3 - 2 * jump_.u);
    cam.target = lerp(jump_.from, jump_.to, s);
    if (jump_.u >= 1) jump_.active = false;
  }
  cam.target.y = ground_(cam.target.x, cam.target.z);
  (void)dist;
  if (time_ > fadeAt_) opacity_ = std::fmax(0.f, opacity_ - dt / 0.5f);
  return rightDown || gliding_;
}

int Compass::azimuth(const OrbitCamera& cam) const { vec3 f = cam.target - cam.position(); return azimuthDegrees(f.x, f.z); }

void Compass::draw(ModelRenderer& r, const OrbitCamera& cam) {
  if (!settings.show || opacity_ <= 0.01f) return;
  mat_.baseColor.w = opacity_ * 0.9f;
  vec3 p = cam.target + vec3{0, 0.25f, 0};
  float scale = std::fmax(1.f, cam.distance / 250);   // keep the reticle readable when zoomed out
  mat4 base = mat4::translation(p) * mat4::scale({scale, scale, scale});
  r.drawMesh(ring_, mat_, {}, base);
  r.drawMesh(cross_, mat_, {}, base);
  r.drawMesh(north_, mat_, {}, base * mat4::translation({0, 0, -7.5f}));
  vec3 f = cam.target - cam.position(); float yaw = std::atan2(f.x, f.z);   // arrow points along the view direction
  r.drawMesh(viewArrow_, mat_, {}, base * mat4::rotationY(yaw + PI) * mat4::translation({0, 0, -3.5f}));
}

} // namespace eng

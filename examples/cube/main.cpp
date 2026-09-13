// Example 1: spinning cube. Proves the foundation (math + RHI + window) works.
#include "engine/core/orbit_camera.h"
#include "engine/core/window.h"
#include "engine/math/math.h"
#include "engine/rhi/rhi.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace eng;

static const char* VS = R"(
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
uniform mat4 uMVP, uModel;
out vec3 vNormal, vWorldPos;
void main() {
  vNormal = mat3(uModel) * aNormal;
  vWorldPos = (uModel * vec4(aPos, 1.0)).xyz;
  gl_Position = uMVP * vec4(aPos, 1.0);
})";

static const char* FS = R"(
in vec3 vNormal, vWorldPos;
uniform vec3 uLightDir, uColor, uEye;
out vec4 oColor;
void main() {
  vec3 n = normalize(vNormal);
  float diffuse = max(dot(n, uLightDir), 0.0);
  vec3 v = normalize(uEye - vWorldPos);
  vec3 h = normalize(uLightDir + v);
  float spec = pow(max(dot(n, h), 0.0), 32.0) * 0.3;
  vec3 c = uColor * (0.15 + 0.85 * diffuse) + spec;
  oColor = vec4(pow(c, vec3(1.0/2.2)), 1.0);
})";

struct Vertex { vec3 pos, normal; };

static rhi::Mesh makeCube() {
  std::vector<Vertex> v; std::vector<uint32_t> idx;
  const vec3 N[6] = {{0,0,1},{0,0,-1},{1,0,0},{-1,0,0},{0,1,0},{0,-1,0}};
  for (int s = 0; s < 6; ++s) {
    vec3 n = N[s], u = (s < 4) ? vec3{0,1,0} : vec3{1,0,0}, r = cross(u, n); u = cross(n, r);
    uint32_t base = (uint32_t)v.size();
    v.push_back({(n - r - u) * 0.5f, n}); v.push_back({(n + r - u) * 0.5f, n});
    v.push_back({(n + r + u) * 0.5f, n}); v.push_back({(n - r + u) * 0.5f, n});
    for (uint32_t i : {0u,1u,2u, 0u,2u,3u}) idx.push_back(base + i);
  }
  const rhi::Attribute layout[] = {{0, 3, sizeof(Vertex), 0}, {1, 3, sizeof(Vertex), sizeof(vec3)}};
  return rhi::createMesh(std::as_bytes(std::span(v)), layout, idx);
}

int main() {
  Window win;
  if (!win.open(1024, 720, "engine — cube")) return 1;
  rhi::init();

  rhi::Program prog = rhi::createProgram(VS, FS);
  int uMVP = rhi::uniformLocation(prog, "uMVP"), uModel = rhi::uniformLocation(prog, "uModel");
  int uLight = rhi::uniformLocation(prog, "uLightDir"), uColor = rhi::uniformLocation(prog, "uColor");
  int uEye = rhi::uniformLocation(prog, "uEye");
  rhi::Mesh cube = makeCube();
  OrbitCamera cam; cam.distance = 4;

  double mxPrev = 0, myPrev = 0; bool dragging = false;
  vec3 light = normalize(vec3{0.4f, 1.0f, 0.6f});
  int frame = 0;

  while (win.isOpen()) {
    win.pollEvents();
    double mx, my; win.mousePos(mx, my);
    if (win.mouseButton(0)) { if (dragging) cam.rotate((float)(mx - mxPrev), (float)(my - myPrev)); dragging = true; }
    else dragging = false;
    mxPrev = mx; myPrev = my;
    if (win.scroll != 0) { cam.zoom((float)win.scroll); win.scroll = 0; }

    int w, h; win.framebufferSize(w, h);
    rhi::setViewport(w, h);
    rhi::clear(0.07f, 0.08f, 0.10f, 1);

    float t = (float)win.time();
    mat4 model = mat4::rotationY(t * 0.8f) * mat4::rotationX(t * 0.5f);
    mat4 mvp = cam.projection((float)w / (float)h) * cam.view() * model;

    rhi::useProgram(prog);
    rhi::setUniform(uMVP, mvp.data());
    rhi::setUniform(uModel, model.data());
    rhi::setUniform(uLight, light.x, light.y, light.z);
    rhi::setUniform(uColor, 0.85f, 0.45f, 0.2f);
    vec3 e = cam.position(); rhi::setUniform(uEye, e.x, e.y, e.z);
    rhi::drawMesh(cube);

    if (frame++ == 0) rhi::checkErrors("first frame");
    if (const char* cap = std::getenv("ENG_CAPTURE"); cap && frame == 30) {
      rhi::captureFramebuffer(cap, w, h); std::printf("captured -> %s\n", cap); break;
    }
    win.swapBuffers();
  }
  rhi::destroyMesh(cube); rhi::destroyProgram(prog); win.close();
  return 0;
}

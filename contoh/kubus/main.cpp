// Contoh 1: kubus berputar. Bukti fondasi (math + RHI + jendela) jalan.
#include "mesin/inti/jendela.h"
#include "mesin/inti/kameraOrbit.h"
#include "mesin/matek/matek.h"
#include "mesin/rhi/rhi.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace mesin;

static const char* VS = R"(
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
uniform mat4 uMVP, uModel;
out vec3 vNormal, vPosDunia;
void main() {
  vNormal = mat3(uModel) * aNormal;
  vPosDunia = (uModel * vec4(aPos, 1.0)).xyz;
  gl_Position = uMVP * vec4(aPos, 1.0);
})";

static const char* FS = R"(
in vec3 vNormal, vPosDunia;
uniform vec3 uArahCahaya, uWarna, uMata;
out vec4 oWarna;
void main() {
  vec3 n = normalize(vNormal);
  float difus = max(dot(n, uArahCahaya), 0.0);
  vec3 v = normalize(uMata - vPosDunia);
  vec3 h = normalize(uArahCahaya + v);
  float spek = pow(max(dot(n, h), 0.0), 32.0) * 0.3;
  vec3 w = uWarna * (0.15 + 0.85 * difus) + spek;
  oWarna = vec4(pow(w, vec3(1.0/2.2)), 1.0);
})";

struct Verteks { vec3 pos, normal; };

static rhi::Mesh buatKubus() {
  std::vector<Verteks> v; std::vector<uint32_t> idx;
  const vec3 N[6] = {{0,0,1},{0,0,-1},{1,0,0},{-1,0,0},{0,1,0},{0,-1,0}};
  for (int s = 0; s < 6; ++s) {
    vec3 n = N[s], u = (s < 4) ? vec3{0,1,0} : vec3{1,0,0}, r = silang(u, n); u = silang(n, r);
    uint32_t dasar = (uint32_t)v.size();
    v.push_back({(n - r - u) * 0.5f, n}); v.push_back({(n + r - u) * 0.5f, n});
    v.push_back({(n + r + u) * 0.5f, n}); v.push_back({(n - r + u) * 0.5f, n});
    for (uint32_t i : {0u,1u,2u, 0u,2u,3u}) idx.push_back(dasar + i);
  }
  const rhi::Atribut layout[] = {{0, 3, sizeof(Verteks), 0}, {1, 3, sizeof(Verteks), sizeof(vec3)}};
  return rhi::buatMesh(std::as_bytes(std::span(v)), layout, idx);
}

int main() {
  Jendela j;
  if (!j.buka(1024, 720, "mesin — kubus")) return 1;
  rhi::mulai();

  rhi::Program prog = rhi::buatProgram(VS, FS);
  int uMVP = rhi::lokasiUniform(prog, "uMVP"), uModel = rhi::lokasiUniform(prog, "uModel");
  int uCahaya = rhi::lokasiUniform(prog, "uArahCahaya"), uWarna = rhi::lokasiUniform(prog, "uWarna");
  int uMata = rhi::lokasiUniform(prog, "uMata");
  rhi::Mesh kubus = buatKubus();
  KameraOrbit kam; kam.jarak = 4;

  double mxLama = 0, myLama = 0; bool seret = false;
  vec3 cahaya = normal(vec3{0.4f, 1.0f, 0.6f});
  int frame = 0;

  while (j.masihBuka()) {
    j.ambilPeristiwa();
    double mx, my; j.posisiMouse(mx, my);
    if (j.tombolMouse(0)) { if (seret) kam.putar((float)(mx - mxLama), (float)(my - myLama)); seret = true; }
    else seret = false;
    mxLama = mx; myLama = my;
    if (j.gulir != 0) { kam.zoom((float)j.gulir); j.gulir = 0; }

    int w, h; j.ukuranFramebuffer(w, h);
    rhi::ukuranLayar(w, h);
    rhi::bersihkan(0.07f, 0.08f, 0.10f, 1);

    float t = (float)j.waktu();
    mat4 model = mat4::rotasiY(t * 0.8f) * mat4::rotasiX(t * 0.5f);
    mat4 mvp = kam.proyeksi((float)w / (float)h) * kam.pandang() * model;

    rhi::pakaiProgram(prog);
    rhi::uniformMat4(uMVP, mvp.data());
    rhi::uniformMat4(uModel, model.data());
    rhi::uniformVec3(uCahaya, cahaya.x, cahaya.y, cahaya.z);
    rhi::uniformVec3(uWarna, 0.85f, 0.45f, 0.2f);
    vec3 m = kam.posisi(); rhi::uniformVec3(uMata, m.x, m.y, m.z);
    rhi::gambarMesh(kubus);

    if (frame++ == 0) rhi::periksaGalat("frame pertama");
    if (const char* tk = std::getenv("MESIN_TANGKAP"); tk && frame == 30) {
      rhi::tangkapLayar(tk, w, h); std::printf("tangkapan → %s\n", tk); break;
    }
    j.tukarBuffer();
  }
  rhi::hapusMesh(kubus); rhi::hapusProgram(prog); j.tutup();
  return 0;
}

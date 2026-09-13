#include "mesin/rhi/rhi.h"
#include <cstdio>
#include <string>
#include <vector>

#if defined(MESIN_GL_DESKTOP)
  #include <OpenGL/gl3.h>
  static const char* KEPALA_SHADER = "#version 410 core\n";
#elif defined(MESIN_GL_ES)
  #include <GLES3/gl3.h>
  static const char* KEPALA_SHADER = "#version 300 es\nprecision highp float;\n";
#endif

namespace mesin::rhi {

void mulai() {
  glEnable(GL_DEPTH_TEST);
  glEnable(GL_CULL_FACE);
  glCullFace(GL_BACK);
  std::printf("[rhi] %s | %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
}

void ukuranLayar(int w, int h) { glViewport(0, 0, w, h); }

void bersihkan(float r, float g, float b, float a, bool kedalaman) {
  glClearColor(r, g, b, a);
  glClear(GL_COLOR_BUFFER_BIT | (kedalaman ? GL_DEPTH_BUFFER_BIT : 0));
}

Buffer buatBuffer(JenisBuffer jenis, std::span<const std::byte> data) {
  GLenum sasaran = jenis == JenisBuffer::Verteks ? GL_ARRAY_BUFFER : GL_ELEMENT_ARRAY_BUFFER;
  Buffer b; glGenBuffers(1, &b.id);
  glBindBuffer(sasaran, b.id);
  glBufferData(sasaran, (GLsizeiptr)data.size(), data.data(), GL_STATIC_DRAW);
  return b;
}
void hapusBuffer(Buffer b) { if (b.id) glDeleteBuffers(1, &b.id); }

Mesh buatMesh(std::span<const std::byte> verteks, std::span<const Atribut> layout,
              std::span<const uint32_t> indeks) {
  Mesh m;
  glGenVertexArrays(1, &m.vao);
  glBindVertexArray(m.vao);
  m.vb = buatBuffer(JenisBuffer::Verteks, verteks);
  for (const Atribut& a : layout) {
    glEnableVertexAttribArray(a.lokasi);
    glVertexAttribPointer(a.lokasi, a.komponen, GL_FLOAT, a.normalisasi ? GL_TRUE : GL_FALSE,
                          a.stride, (const void*)(intptr_t)a.offset);
  }
  m.ib = buatBuffer(JenisBuffer::Indeks, std::as_bytes(indeks));
  m.jumlahIndeks = (uint32_t)indeks.size();
  glBindVertexArray(0);
  return m;
}
void hapusMesh(Mesh& m) {
  hapusBuffer(m.vb); hapusBuffer(m.ib);
  if (m.vao) glDeleteVertexArrays(1, &m.vao);
  m = {};
}
void gambarMesh(const Mesh& m) {
  glBindVertexArray(m.vao);
  glDrawElements(GL_TRIANGLES, (GLsizei)m.jumlahIndeks, GL_UNSIGNED_INT, nullptr);
}

static GLuint kompilasi(GLenum jenis, std::string_view src) {
  std::string penuh = std::string(KEPALA_SHADER) + std::string(src);
  const char* p = penuh.c_str();
  GLuint s = glCreateShader(jenis);
  glShaderSource(s, 1, &p, nullptr);
  glCompileShader(s);
  GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[2048]; glGetShaderInfoLog(s, sizeof log, nullptr, log);
    std::fprintf(stderr, "[rhi] gagal kompilasi shader (%s):\n%s\n",
                 jenis == GL_VERTEX_SHADER ? "verteks" : "fragmen", log);
  }
  return s;
}

Program buatProgram(std::string_view vs, std::string_view fs) {
  GLuint v = kompilasi(GL_VERTEX_SHADER, vs), f = kompilasi(GL_FRAGMENT_SHADER, fs);
  Program p; p.id = glCreateProgram();
  glAttachShader(p.id, v); glAttachShader(p.id, f);
  glLinkProgram(p.id);
  GLint ok = 0; glGetProgramiv(p.id, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[2048]; glGetProgramInfoLog(p.id, sizeof log, nullptr, log);
    std::fprintf(stderr, "[rhi] gagal link program:\n%s\n", log);
  }
  glDeleteShader(v); glDeleteShader(f);
  return p;
}
void hapusProgram(Program p) { if (p.id) glDeleteProgram(p.id); }
void pakaiProgram(Program p) { glUseProgram(p.id); }
int  lokasiUniform(Program p, const char* nama) { return glGetUniformLocation(p.id, nama); }
void uniformMat4(int lok, const float* m) { glUniformMatrix4fv(lok, 1, GL_FALSE, m); }
void uniformVec3(int lok, float x, float y, float z) { glUniform3f(lok, x, y, z); }
void uniformFloat(int lok, float v) { glUniform1f(lok, v); }
void uniformInt(int lok, int v) { glUniform1i(lok, v); }

Tekstur buatTekstur(int w, int h, Format f, std::span<const std::byte> piksel, bool mipmap) {
  GLenum fmt = f == Format::RGBA8 ? GL_RGBA : f == Format::RGB8 ? GL_RGB : f == Format::RG8 ? GL_RG : GL_RED;
  GLint internal = f == Format::RGBA8 ? GL_RGBA8 : f == Format::RGB8 ? GL_RGB8 : f == Format::RG8 ? GL_RG8 : GL_R8;
  Tekstur t; glGenTextures(1, &t.id);
  glBindTexture(GL_TEXTURE_2D, t.id);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, fmt, GL_UNSIGNED_BYTE, piksel.data());
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mipmap ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
  if (mipmap) glGenerateMipmap(GL_TEXTURE_2D);
  return t;
}
void hapusTekstur(Tekstur t) { if (t.id) glDeleteTextures(1, &t.id); }
void ikatTekstur(int slot, Tekstur t) { glActiveTexture(GL_TEXTURE0 + slot); glBindTexture(GL_TEXTURE_2D, t.id); }

void periksaGalat(const char* tempat) {
  for (GLenum e; (e = glGetError()) != GL_NO_ERROR;)
    std::fprintf(stderr, "[rhi] galat GL 0x%x di %s\n", e, tempat);
}

} // namespace mesin::rhi

namespace mesin::rhi {
bool tangkapLayar(const char* jalur, int w, int h) {
  std::vector<unsigned char> px((size_t)w * h * 3);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px.data());
  FILE* f = std::fopen(jalur, "wb"); if (!f) return false;
  std::fprintf(f, "P6\n%d %d\n255\n", w, h);
  for (int y = h - 1; y >= 0; --y) std::fwrite(&px[(size_t)y * w * 3], 1, (size_t)w * 3, f);
  std::fclose(f); return true;
}
}

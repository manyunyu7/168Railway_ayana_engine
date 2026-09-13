#include "engine/rhi/rhi.h"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <string>
#include <vector>

#if defined(ENG_GL_DESKTOP)
  #include <OpenGL/gl3.h>
  static const char* SHADER_HEADER = "#version 410 core\n";
#elif defined(ENG_GL_ES)
  #include <GLES3/gl3.h>
  static const char* SHADER_HEADER = "#version 300 es\nprecision highp float;\n";
#endif

#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#endif

namespace eng::rhi {

static float g_maxAniso = 0, g_aniso = 1;   // 0 = extension absent

void init() {
  g_maxAniso = 0; g_aniso = 1;
  GLint n = 0; glGetIntegerv(GL_NUM_EXTENSIONS, &n);
  for (GLint i = 0; i < n; ++i) {
    const char* e = (const char*)glGetStringi(GL_EXTENSIONS, (GLuint)i);
    if (e && !std::strcmp(e, "GL_EXT_texture_filter_anisotropic")) glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &g_maxAniso);
  }
  glEnable(GL_DEPTH_TEST);
  glEnable(GL_CULL_FACE);
  glCullFace(GL_BACK);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  std::printf("[rhi] %s | %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
}

void setViewport(int w, int h) { glViewport(0, 0, w, h); }

void clear(float r, float g, float b, float a, bool depth) {
  glClearColor(r, g, b, a);
  glClear(GL_COLOR_BUFFER_BIT | (depth ? GL_DEPTH_BUFFER_BIT : 0));
}
void setDepthWrite(bool on) { glDepthMask(on ? GL_TRUE : GL_FALSE); }
void setBlend(bool on) { if (on) glEnable(GL_BLEND); else glDisable(GL_BLEND); }
void setBlendAdditive(bool on) { glBlendFunc(GL_SRC_ALPHA, on ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA); }
void setCullFace(bool on) { if (on) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE); }
void setFrontFaceCCW(bool ccw) { glFrontFace(ccw ? GL_CCW : GL_CW); }
void setDepthTestEnabled(bool on) { if (on) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST); }

Buffer createBuffer(BufferKind kind, std::span<const std::byte> data) {
  GLenum target = kind == BufferKind::Vertex ? GL_ARRAY_BUFFER : GL_ELEMENT_ARRAY_BUFFER;
  Buffer b; glGenBuffers(1, &b.id);
  glBindBuffer(target, b.id);
  glBufferData(target, (GLsizeiptr)data.size(), data.data(), GL_STATIC_DRAW);
  return b;
}
void destroyBuffer(Buffer b) { if (b.id) glDeleteBuffers(1, &b.id); }

Mesh createMesh(std::span<const std::byte> vertices, std::span<const Attribute> layout,
                std::span<const uint32_t> indices) {
  Mesh m;
  glGenVertexArrays(1, &m.vao);
  glBindVertexArray(m.vao);
  m.vb = createBuffer(BufferKind::Vertex, vertices);
  for (const Attribute& a : layout) {
    glEnableVertexAttribArray(a.location);
    glVertexAttribPointer(a.location, a.components, GL_FLOAT, a.normalized ? GL_TRUE : GL_FALSE,
                          a.stride, (const void*)(intptr_t)a.offset);
  }
  m.ib = createBuffer(BufferKind::Index, std::as_bytes(indices));
  m.indexCount = (uint32_t)indices.size();
  glBindVertexArray(0);
  return m;
}
void destroyMesh(Mesh& m) {
  destroyBuffer(m.vb); destroyBuffer(m.ib);
  if (m.vao) glDeleteVertexArrays(1, &m.vao);
  m = {};
}
void drawMesh(const Mesh& m) {
  glBindVertexArray(m.vao);
  glDrawElements(GL_TRIANGLES, (GLsizei)m.indexCount, GL_UNSIGNED_INT, nullptr);
}

Buffer createDynamicBuffer(size_t bytes) {
  Buffer b; glGenBuffers(1, &b.id);
  glBindBuffer(GL_ARRAY_BUFFER, b.id);
  glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)bytes, nullptr, GL_DYNAMIC_DRAW);
  return b;
}
void updateBuffer(Buffer b, std::span<const std::byte> data) {
  glBindBuffer(GL_ARRAY_BUFFER, b.id);
  glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)data.size(), data.data());
}
void attachInstances(const Mesh& m, Buffer instances) {
  glBindVertexArray(m.vao);
  glBindBuffer(GL_ARRAY_BUFFER, instances.id);
  for (int c = 0; c < 4; ++c) {
    glEnableVertexAttribArray(3 + c);
    glVertexAttribPointer(3 + c, 4, GL_FLOAT, GL_FALSE, 64, (const void*)(intptr_t)(c * 16));
    glVertexAttribDivisor(3 + c, 1);
  }
  glBindVertexArray(0);
}
void drawMeshInstanced(const Mesh& m, uint32_t count) {
  glBindVertexArray(m.vao);
  glDrawElementsInstanced(GL_TRIANGLES, (GLsizei)m.indexCount, GL_UNSIGNED_INT, nullptr, (GLsizei)count);
}

static GLuint compile(GLenum kind, std::string_view src) {
  std::string full = std::string(SHADER_HEADER) + std::string(src);
  const char* p = full.c_str();
  GLuint s = glCreateShader(kind);
  glShaderSource(s, 1, &p, nullptr);
  glCompileShader(s);
  GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[2048]; glGetShaderInfoLog(s, sizeof log, nullptr, log);
    std::fprintf(stderr, "[rhi] %s shader compile failed:\n%s\n",
                 kind == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
  }
  return s;
}

Program createProgram(std::string_view vs, std::string_view fs) {
  GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
  Program p; p.id = glCreateProgram();
  glAttachShader(p.id, v); glAttachShader(p.id, f);
  glLinkProgram(p.id);
  GLint ok = 0; glGetProgramiv(p.id, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[2048]; glGetProgramInfoLog(p.id, sizeof log, nullptr, log);
    std::fprintf(stderr, "[rhi] program link failed:\n%s\n", log);
  }
  glDeleteShader(v); glDeleteShader(f);
  return p;
}
void destroyProgram(Program p) { if (p.id) glDeleteProgram(p.id); }
void useProgram(Program p) { glUseProgram(p.id); }
int  uniformLocation(Program p, const char* name) { return glGetUniformLocation(p.id, name); }
void setUniform(int loc, const float* m) { glUniformMatrix4fv(loc, 1, GL_FALSE, m); }
void setUniform(int loc, float x, float y) { glUniform2f(loc, x, y); }
void setUniform(int loc, float x, float y, float z) { glUniform3f(loc, x, y, z); }
void setUniform(int loc, float x, float y, float z, float w) { glUniform4f(loc, x, y, z, w); }
void setUniform(int loc, float v) { glUniform1f(loc, v); }
void setUniform(int loc, int v) { glUniform1i(loc, v); }

float setAnisotropy(float level) {
  g_aniso = g_maxAniso > 0 ? std::min(std::max(level, 1.f), g_maxAniso) : 1.f;
  return g_aniso;
}

Texture createTexture(int w, int h, Format f, std::span<const std::byte> pixels, bool mipmap, bool srgb, Wrap wrapS, Wrap wrapT) {
  auto wrapMode = [](Wrap w) { return w == Wrap::Clamp ? GL_CLAMP_TO_EDGE : w == Wrap::Mirror ? GL_MIRRORED_REPEAT : GL_REPEAT; };
  GLenum fmt = f == Format::RGBA8 ? GL_RGBA : f == Format::RGB8 ? GL_RGB : f == Format::RG8 ? GL_RG : GL_RED;
  GLint internal = f == Format::RGBA8 ? (srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8)
                 : f == Format::RGB8  ? (srgb ? GL_SRGB8 : GL_RGB8)
                 : f == Format::RG8   ? GL_RG8 : GL_R8;
  Texture t; glGenTextures(1, &t.id);
  glBindTexture(GL_TEXTURE_2D, t.id);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, fmt, GL_UNSIGNED_BYTE, pixels.data());
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrapMode(wrapS));
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrapMode(wrapT));
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mipmap ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
  if (mipmap) glGenerateMipmap(GL_TEXTURE_2D);
  if (mipmap && g_aniso > 1) glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, g_aniso);
  return t;
}
void destroyTexture(Texture t) { if (t.id) glDeleteTextures(1, &t.id); }
void bindTexture(int slot, Texture t) { glActiveTexture(GL_TEXTURE0 + slot); glBindTexture(GL_TEXTURE_2D, t.id); }

void checkErrors(const char* where) {
  for (GLenum e; (e = glGetError()) != GL_NO_ERROR;)
    std::fprintf(stderr, "[rhi] GL error 0x%x at %s\n", e, where);
}

bool captureFramebuffer(const char* path, int w, int h) {
  std::vector<unsigned char> px((size_t)w * h * 3);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px.data());
  FILE* f = std::fopen(path, "wb"); if (!f) return false;
  std::fprintf(f, "P6\n%d %d\n255\n", w, h);
  for (int y = h - 1; y >= 0; --y) std::fwrite(&px[(size_t)y * w * 3], 1, (size_t)w * 3, f);
  std::fclose(f); return true;
}

} // namespace eng::rhi

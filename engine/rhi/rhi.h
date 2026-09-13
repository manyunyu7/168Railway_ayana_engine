// RHI = thin layer over the GPU API. The engine only talks to the GPU through this.
// First backend: OpenGL (ES 3.0 on Android, 4.1 core on macOS). Metal later.
#pragma once
#include <cstdint>
#include <span>
#include <string_view>

namespace eng::rhi {

struct Buffer  { uint32_t id = 0; };
struct Program { uint32_t id = 0; };
struct Texture { uint32_t id = 0; };
struct Mesh    { uint32_t vao = 0; Buffer vb, ib; uint32_t indexCount = 0; };

enum class BufferKind { Vertex, Index };
enum class Format { RGBA8, RGB8, RG8, R8 };

// One vertex attribute (mesh layout description).
struct Attribute { int location; int components; int stride; int offset; bool normalized = false; };

void init();                        // once, after a GL context exists
void setViewport(int w, int h);
void clear(float r, float g, float b, float a, bool depth = true);
void setDepthWrite(bool on);
void setBlend(bool on);
void setCullFace(bool on);

Buffer createBuffer(BufferKind kind, std::span<const std::byte> data);
void   destroyBuffer(Buffer b);

Mesh   createMesh(std::span<const std::byte> vertices, std::span<const Attribute> layout,
                  std::span<const uint32_t> indices);
void   destroyMesh(Mesh& m);
void   drawMesh(const Mesh& m);

Program createProgram(std::string_view vs, std::string_view fs);  // GLSL source without #version
void    destroyProgram(Program p);
void    useProgram(Program p);
int     uniformLocation(Program p, const char* name);
void    setUniform(int loc, const float* mat4);
void    setUniform(int loc, float x, float y);
void    setUniform(int loc, float x, float y, float z);
void    setUniform(int loc, float x, float y, float z, float w);
void    setUniform(int loc, float v);
void    setUniform(int loc, int v);

Texture createTexture(int w, int h, Format f, std::span<const std::byte> pixels, bool mipmap = true, bool srgb = false);
void    destroyTexture(Texture t);
void    bindTexture(int slot, Texture t);

void checkErrors(const char* where);
bool captureFramebuffer(const char* path, int w, int h);  // PPM, for automated screenshots

} // namespace eng::rhi

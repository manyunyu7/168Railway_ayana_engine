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
// Uncompressed formats always exist; block-compressed ones depend on the GPU/extension (see supports()).
enum class Format { RGBA8, RGB8, RG8, R8, ETC2_RGB, ETC2_RGBA, BC1, BC3, BC7 };

// One vertex attribute (mesh layout description). `type` = the data in the buffer; it always reaches the
// shader as float (U8 unnormalized gives 0..255 — how skinning joint indices travel).
enum class AttrType : uint8_t { Float, U8 };
struct Attribute { int location; int components; int stride; int offset; bool normalized = false; AttrType type = AttrType::Float; };
// Optional per-vertex brightness (float, location 7): meshes that omit it read the generic value 1.0 set in init().
// The PBR shader multiplies the lit colour by it (tunnel interiors, ballast skirts).
constexpr int ATTR_SHADE = 7;
// Skinning attributes (engine/render: skinned vertex format): joint indices as unnormalized u8x4 and
// weights as f32x4. 3..6 belong to instancing, 7 to shade, so the skinned pair sits above them.
constexpr int ATTR_JOINTS = 8, ATTR_WEIGHTS = 9;

void init();                        // once, after a GL context exists
void setViewport(int w, int h);
void clear(float r, float g, float b, float a, bool depth = true);
void setDepthWrite(bool on);
void setBlend(bool on);
void setBlendAdditive(bool on);   // true: src*alpha + dst (light coronas); false: normal alpha blend
void setCullFace(bool on);
void setFrontFaceCCW(bool ccw);   // false for mirrored (negative-determinant) transforms so culling stays correct
void setDepthTestEnabled(bool on);

Buffer createBuffer(BufferKind kind, std::span<const std::byte> data);
void   destroyBuffer(Buffer b);

Mesh   createMesh(std::span<const std::byte> vertices, std::span<const Attribute> layout,
                  std::span<const uint32_t> indices);
void   destroyMesh(Mesh& m);
void   drawMesh(const Mesh& m);

// Instancing: a dynamic vertex buffer of per-instance mat4 (column-major, 64 B) attached to a mesh's
// VAO at attribute locations 3..6 with divisor 1; drawMeshInstanced draws `count` copies.
Buffer createDynamicBuffer(size_t bytes);                      // vertex buffer, contents undefined
void   updateBuffer(Buffer b, std::span<const std::byte> data); // rewrites from offset 0 (≤ created size)
void   attachInstances(const Mesh& m, Buffer instances);
void   drawMeshInstanced(const Mesh& m, uint32_t count);

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
void    setUniformMat4Array(int loc, const float* mats, int count);   // mat4 uJoints[N] (skinning palette)

// Anisotropic filtering level applied to textures created afterwards (clamped to the hardware maximum;
// no-op when GL_EXT_texture_filter_anisotropic is missing). Returns the level in effect.
float   setAnisotropy(float level);
enum class Wrap : uint8_t { Repeat, Clamp, Mirror };   // matches Image::wrapS/T encoding
Texture createTexture(int w, int h, Format f, std::span<const std::byte> pixels, bool mipmap = true, bool srgb = false,
                      Wrap wrapS = Wrap::Repeat, Wrap wrapT = Wrap::Repeat);
// Block-compressed upload (ETC2/BC): every mip level is given by the caller (glCompressedTexImage2D per
// level; nothing is generated). Returns id 0 when the format is unsupported — check supports() first.
struct MipData { int width, height; std::span<const std::byte> data; };
bool    supports(Format f);          // valid after init(); uncompressed formats are always true
Texture createTextureCompressed(Format f, std::span<const MipData> mips, bool srgb = false,
                                Wrap wrapS = Wrap::Repeat, Wrap wrapT = Wrap::Repeat);
// Rewrites the whole level 0 of an RGBA8 texture created with createTexture (same size); regenerates the mip
// chain when the texture has one (dynamic canvases: the in-world meja board).
void    updateTextureRGBA(Texture t, int w, int h, std::span<const std::byte> pixels, bool mipmap = true);
void    destroyTexture(Texture t);
void    bindTexture(int slot, Texture t);

void checkErrors(const char* where);
bool captureFramebuffer(const char* path, int w, int h);  // PPM, for automated screenshots

// Offscreen render target: RGBA8 colour texture + depth renderbuffer (palette thumbnails, eng_thumbnail).
// bindRenderTarget({}) returns to the default framebuffer; the caller restores the viewport itself.
struct RenderTarget { uint32_t fbo = 0, depth = 0; Texture color; int w = 0, h = 0; };
RenderTarget createRenderTarget(int w, int h);
void         bindRenderTarget(const RenderTarget& rt);   // rt.fbo 0 = the window / canvas framebuffer
void         destroyRenderTarget(RenderTarget& rt);
// Reads the bound framebuffer as RGBA8 (row 0 = bottom, GL convention) into `out` (w*h*4 bytes).
void         readPixels(int x, int y, int w, int h, uint8_t* out);
// Depth compare: false = GL_LESS (default), true = GL_LEQUAL (a second pass over the same geometry wins).
void         setDepthLessEqual(bool on);

} // namespace eng::rhi

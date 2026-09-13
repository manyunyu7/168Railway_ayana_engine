// RHI = lapisan tipis di atas GPU API. Engine hanya bicara lewat sini;
// backend pertama OpenGL (ES 3.0 di Android, 4.1 core di macOS). Metal menyusul.
#pragma once
#include <cstdint>
#include <span>
#include <string_view>

namespace mesin::rhi {

struct Buffer  { uint32_t id = 0; };
struct Program { uint32_t id = 0; };
struct Tekstur { uint32_t id = 0; };
struct Mesh    { uint32_t vao = 0; Buffer vb, ib; uint32_t jumlahIndeks = 0; };

enum class JenisBuffer { Verteks, Indeks };
enum class Format { RGBA8, RGB8, RG8, R8 };

// Deskripsi satu atribut verteks (untuk layout mesh).
struct Atribut { int lokasi; int komponen; int stride; int offset; bool normalisasi = false; };

void mulai();                       // sekali setelah konteks GL ada
void ukuranLayar(int w, int h);
void bersihkan(float r, float g, float b, float a, bool kedalaman = true);

Buffer buatBuffer(JenisBuffer jenis, std::span<const std::byte> data);
void   hapusBuffer(Buffer b);

Mesh   buatMesh(std::span<const std::byte> verteks, std::span<const Atribut> layout,
                std::span<const uint32_t> indeks);
void   hapusMesh(Mesh& m);
void   gambarMesh(const Mesh& m);

Program buatProgram(std::string_view vs, std::string_view fs);  // sumber GLSL tanpa #version
void    hapusProgram(Program p);
void    pakaiProgram(Program p);
int     lokasiUniform(Program p, const char* nama);
void    uniformMat4(int lok, const float* m);
void    uniformVec3(int lok, float x, float y, float z);
void    uniformFloat(int lok, float v);
void    uniformInt(int lok, int v);

Tekstur buatTekstur(int w, int h, Format f, std::span<const std::byte> piksel, bool mipmap = true);
void    hapusTekstur(Tekstur t);
void    ikatTekstur(int slot, Tekstur t);

void periksaGalat(const char* tempat);

} // namespace mesin::rhi

namespace mesin::rhi {
// Simpan isi framebuffer ke berkas PPM (untuk uji/tangkapan otomatis).
bool tangkapLayar(const char* jalur, int w, int h);
}

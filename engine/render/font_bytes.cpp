#include "engine/render/font_bytes.h"
#include <fstream>

namespace eng {
namespace { std::vector<uint8_t> g_font; }

void setFontBytes(const uint8_t* bytes, int len) {
  g_font.clear();
  if (bytes && len > 0) g_font.assign(bytes, bytes + len);
}

bool readFontFile(const std::string& path, std::vector<uint8_t>& out) {
  if (!g_font.empty()) { out = g_font; return true; }
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return !out.empty();
}

} // namespace eng

// Where an .efnt comes from. On desktop/web the bitmap font is a file (the checkout, or Emscripten's
// preloaded /assets/font.efnt); on Android there is no source tree and no readable path, so the host
// hands the bytes over once (eng_set_font) and every font loader reads them from here instead.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace eng {

// Install (or clear, with null/0) the host's copy of the font. Not thread-safe: call it before the
// render thread starts, or from the render thread itself.
void setFontBytes(const uint8_t* bytes, int len);

// The host override when one is installed, otherwise the file at `path`. false = neither available.
bool readFontFile(const std::string& path, std::vector<uint8_t>& out);

} // namespace eng

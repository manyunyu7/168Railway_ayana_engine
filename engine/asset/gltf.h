// GLB (binary glTF 2.0) parser. Meshes/materials/nodes; images are kept encoded.
#pragma once
#include "engine/asset/model.h"
#include <span>
#include <string>

namespace eng {

bool loadGlb(std::span<const uint8_t> bytes, Model& out, std::string& error);
bool loadGlbFile(const std::string& path, Model& out, std::string& error);

} // namespace eng

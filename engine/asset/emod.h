// Engine's own binary model format (".emod"). Produced offline by tools/convert from GLB;
// the runtime never decodes PNG/JPEG. Little-endian, versioned.
//
//   magic "EMOD" u32 version
//   u32 nImages  { u16 w, u16 h, u8 channels(4), u8 pad[3], bytes[w*h*4] }
//   u32 nMaterials { name, vec4 baseColor, f metallic, f roughness, vec3 emissive,
//                    i32 baseColorTex, metalRoughTex, normalTex, emissiveTex, occlusionTex,
//                    u8 alphaMode, f alphaCutoff, u8 doubleSided }
//   u32 nMeshes { name, u32 nPrims { i32 material, u32 nVerts, Vertex[], u32 nIdx, u32[], vec3 min, vec3 max } }
//   u32 nNodes { name, i32 mesh, i32 parent, u32 nChildren, i32[], mat4 local, u16 nExtras { key, value } (v2+) }
//   u32 nRoots i32[]   vec3 boundsMin boundsMax
// strings: u16 length + bytes
#pragma once
#include "engine/asset/model.h"
#include <string>

namespace eng {

constexpr uint32_t EMOD_VERSION = 3;   // v1 (no node extras) and v2 (TEXCOORD_0 only) files still load; v3 = material UV set

bool saveEmod(const Model& m, const std::string& path, std::string& error);
bool loadEmod(const std::string& path, Model& out, std::string& error);

} // namespace eng

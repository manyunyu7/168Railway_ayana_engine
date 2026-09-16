// Engine's own binary model format (".emod"). Produced offline by tools/convert from GLB;
// the runtime never decodes PNG/JPEG. Little-endian, versioned.
//
//   magic "EMOD" u32 version
//   u32 nImages  { u16 w, u16 h, u8 channels(4), u8 wrapS, u8 wrapT, u8 linear (v4+; were pad), i16 source (v6+),
//                  v1-v4: bytes[w*h*4] (RGBA8)
//                  v5+:   u8 nVariants { u8 format (TexFormat), u8 nMips { u16 w, u16 h, u32 bytes, data } }
//                         nVariants = 0 is a placeholder (v6, `convert --textures external`): w/h/wrap/linear are hints,
//                         the pixels come from image `source` of an external texture file (web: KTX2, transcoded in JS) }
//   u32 nMaterials { name, vec4 baseColor, f metallic, f roughness, vec3 emissive,
//                    i32 baseColorTex, metalRoughTex, normalTex, emissiveTex, occlusionTex,
//                    u8 alphaMode, f alphaCutoff, u8 doubleSided }
//   u32 nMeshes { name, u32 nPrims { i32 material, u32 nVerts, Vertex[], u32 nIdx, u32[], vec3 min, vec3 max,
//                 u32 nSkinVerts, VertexSkin[] (v8+; 0 = not skinned) } }
//   u32 nNodes { name, i32 mesh, i32 parent, u32 nChildren, i32[], mat4 local, u16 nExtras { key, value } (v2+),
//                vec3 translation, quat rotation, vec3 scale (v7+), i32 skin (v8+) }
//   u32 nRoots i32[]   vec3 boundsMin boundsMax
//   v7+: u32 nAnimations { name, f duration, u32 nSamplers { u8 comps, u8 step, u32 nKeys, f times[nKeys], f values[nKeys*comps] },
//                          u32 nChannels { i32 sampler, i32 node, u8 path (0 T, 1 R, 2 S) } }
//   v8+: u32 nSkins { name, i32 skeleton, u32 nJoints i32[], u32 nInverseBind mat4[] }
// strings: u16 length + bytes
//
// Standalone image files (".eimg", terrain satellite tiles): magic "EIMG" u32 version (1 = v5 record, 2 = v6) + one image record.
#pragma once
#include "engine/asset/model.h"
#include <span>
#include <string>

namespace eng {

constexpr uint32_t EMOD_VERSION = 8;   // v1 (no node extras), v2 (TEXCOORD_0 only), v3 (material UV set), v4 (image
                                       // wrap/colour-space flags, emissive strength, UV transforms baked), v5 (images as
                                       // GPU-format variants with mip chains: ETC2 / BC / RGBA8) still load;
                                       // v6 (image `source` index + placeholder images, textures streamed separately);
                                       // v7 = node TRS + animation clips (door / pantograph rigs);
                                       // v8 = skins (joints + inverse bind matrices) and per-vertex joints/weights (avatars)

bool saveEmod(const Model& m, const std::string& path, std::string& error);
bool loadEmod(const std::string& path, Model& out, std::string& error);
bool loadEmod(std::span<const uint8_t> bytes, Model& out, std::string& error);   // e.g. after fetchFile

bool saveImageFile(const Image& im, const std::string& path, std::string& error);
bool loadImageFile(std::span<const uint8_t> bytes, Image& out, std::string& error);

} // namespace eng

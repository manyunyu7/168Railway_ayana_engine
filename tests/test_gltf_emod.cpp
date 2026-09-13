// GLB built in memory -> loadGlb -> saveEmod -> loadEmod: exact round trip of vertices, indices,
// materials, nodes and extras; KHR_texture_transform baked into UVs; TEXCOORD_n selection.
#include "engine/asset/emod.h"
#include "engine/asset/gltf.h"
#include "tests/check.h"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

using namespace eng;

namespace {
// Little helper that accumulates a BIN chunk and the bufferView/accessor JSON for it.
struct GlbBuilder {
  std::vector<uint8_t> bin; std::string views, accessors; int nViews = 0, nAcc = 0;
  template <class T> int view(const std::vector<T>& v) {
    while (bin.size() % 4) bin.push_back(0);
    size_t off = bin.size(); bin.resize(off + v.size() * sizeof(T)); std::memcpy(bin.data() + off, v.data(), v.size() * sizeof(T));
    views += (nViews ? "," : "") + std::string("{\"buffer\":0,\"byteOffset\":") + std::to_string(off) + ",\"byteLength\":" + std::to_string(v.size() * sizeof(T)) + "}";
    return nViews++;
  }
  int accessor(int view, int count, const char* type, int compType, const char* extra = "") {
    accessors += (nAcc ? "," : "") + std::string("{\"bufferView\":") + std::to_string(view) + ",\"componentType\":" + std::to_string(compType) +
                 ",\"count\":" + std::to_string(count) + ",\"type\":\"" + type + "\"" + extra + "}";
    return nAcc++;
  }
  std::vector<uint8_t> finish(const std::string& body) {   // body = the rest of the glTF JSON (without the leading '{')
    std::string json = "{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"byteLength\":" + std::to_string(bin.size()) + "}],"
                       "\"bufferViews\":[" + views + "],\"accessors\":[" + accessors + "]," + body + "}";
    while (json.size() % 4) json += ' ';
    while (bin.size() % 4) bin.push_back(0);
    std::vector<uint8_t> out; auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) out.push_back((uint8_t)(v >> (8 * i))); };
    out.insert(out.end(), {'g', 'l', 'T', 'F'}); u32(2); u32((uint32_t)(12 + 8 + json.size() + 8 + bin.size()));
    u32((uint32_t)json.size()); u32(0x4E4F534A); out.insert(out.end(), json.begin(), json.end());
    u32((uint32_t)bin.size()); u32(0x004E4942); out.insert(out.end(), bin.begin(), bin.end());
    return out;
  }
};

bool sameMat(const mat4& a, const mat4& b) { return std::memcmp(a.data(), b.data(), 64) == 0; }
}

TEST_MAIN({
  // ---- model 1: one triangle, two UV sets, one material (no texture), two nodes with extras
  std::vector<uint8_t> glb;
  {
    GlbBuilder b;
    std::vector<float> pos{0, 0, 0, 1, 0, 0, 0, 1, 0};
    std::vector<float> nrm{0, 0, 1, 0, 0, 1, 0, 0, 1};
    std::vector<float> uv0{0, 0, 1, 0, 0, 1};
    std::vector<float> uv1{0.5f, 0.5f, 0.75f, 0.5f, 0.5f, 0.75f};
    std::vector<uint16_t> idx{0, 1, 2};
    int aPos = b.accessor(b.view(pos), 3, "VEC3", 5126, ",\"min\":[0,0,0],\"max\":[1,1,0]");
    int aNrm = b.accessor(b.view(nrm), 3, "VEC3", 5126);
    int aUv0 = b.accessor(b.view(uv0), 3, "VEC2", 5126);
    int aUv1 = b.accessor(b.view(uv1), 3, "VEC2", 5126);
    int aIdx = b.accessor(b.view(idx), 3, "SCALAR", 5123);
    std::string body =
      "\"materials\":[{\"name\":\"paint\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.25,0.5,0.75,1],\"metallicFactor\":0.125,\"roughnessFactor\":0.625},"
      "\"emissiveFactor\":[0.1,0.2,0.3],\"extensions\":{\"KHR_materials_emissive_strength\":{\"emissiveStrength\":2}},\"alphaMode\":\"MASK\",\"alphaCutoff\":0.3,\"doubleSided\":true}],"
      "\"meshes\":[{\"name\":\"tri\",\"primitives\":[{\"attributes\":{\"POSITION\":" + std::to_string(aPos) + ",\"NORMAL\":" + std::to_string(aNrm) +
      ",\"TEXCOORD_0\":" + std::to_string(aUv0) + ",\"TEXCOORD_1\":" + std::to_string(aUv1) + "},\"indices\":" + std::to_string(aIdx) + ",\"material\":0}]}],"
      "\"nodes\":[{\"name\":\"root\",\"children\":[1],\"translation\":[1,2,3],\"extras\":{\"kelas\":3,\"nama\":\"bogie\",\"flip\":true,\"ratio\":0.5,\"ignored\":[1,2]}},"
      "{\"name\":\"leaf\",\"mesh\":0,\"scale\":[2,2,2],\"rotation\":[0,0.7071068,0,0.7071068]}],"
      "\"scenes\":[{\"nodes\":[0]}],\"scene\":0";
    glb = b.finish(body);
  }
  Model m; std::string err;
  CHECK_MSG(loadGlb(glb, m, err), err);
  CHECK(m.materials.size() == 1); CHECK(m.meshes.size() == 1); CHECK(m.nodes.size() == 2); CHECK(m.roots.size() == 1 && m.roots[0] == 0);
  const Material& mt = m.materials[0];
  CHECK(mt.name == "paint"); CHECK(mt.baseColor.x == 0.25f && mt.baseColor.y == 0.5f && mt.baseColor.z == 0.75f && mt.baseColor.w == 1);
  CHECK(mt.metallic == 0.125f && mt.roughness == 0.625f);
  CHECK_NEAR(mt.emissive.x, 0.2, 1e-6); CHECK_NEAR(mt.emissive.z, 0.6, 1e-6);   // emissive strength applied
  CHECK(mt.alphaMode == AlphaMode::Mask); CHECK(mt.alphaCutoff == 0.3f); CHECK(mt.doubleSided); CHECK(!mt.unlit);
  CHECK(mt.baseColorTex == -1 && mt.baseColorUv == 0);
  CHECK(m.meshes[0].primitives.size() == 1);
  const Primitive& p = m.meshes[0].primitives[0];
  CHECK(p.material == 0); CHECK(p.vertices.size() == 3); CHECK(p.indices.size() == 3);
  CHECK(p.vertices[1].pos.x == 1 && p.vertices[2].pos.y == 1);
  CHECK(p.vertices[0].normal.z == 1);
  CHECK(p.vertices[1].uv.x == 1 && p.vertices[2].uv.y == 1);   // TEXCOORD_0 (no texture -> set 0)
  CHECK(p.indices[0] == 0 && p.indices[1] == 1 && p.indices[2] == 2);
  CHECK(p.boundsMin.x == 0 && p.boundsMax.x == 1 && p.boundsMax.z == 0);
  const Node &root = m.nodes[0], &leaf = m.nodes[1];
  CHECK(root.name == "root" && root.mesh == -1 && root.parent == -1 && root.children.size() == 1 && root.children[0] == 1);
  CHECK(leaf.name == "leaf" && leaf.mesh == 0 && leaf.parent == 0 && leaf.children.empty());
  CHECK(root.local.m[3][0] == 1 && root.local.m[3][1] == 2 && root.local.m[3][2] == 3);
  CHECK(root.extras.size() == 4);   // arrays dropped
  CHECK(root.extraNumber("kelas", -1) == 3); CHECK(root.extraNumber("ratio", -1) == 0.5f);
  CHECK(root.extras.at("nama") == "bogie"); CHECK(root.extras.at("flip") == "true");
  CHECK(root.extraNumber("nama", -1) == -1); CHECK(root.extraNumber("absent", 7) == 7);
  // leaf local = R(90° about Y) * S(2): +X -> (0,0,-2)
  vec3 lx = leaf.local.transformDir({1, 0, 0}); CHECK_NEAR(lx.x, 0, 1e-5); CHECK_NEAR(lx.z, -2, 1e-5);
  // model bounds: triangle rotated (+x -> -z) and scaled 2 under translation (1,2,3): x in [1,1], y in [2,4], z in [1,3]
  CHECK_NEAR(m.boundsMin.x, 1, 1e-5); CHECK_NEAR(m.boundsMax.y, 4, 1e-5); CHECK_NEAR(m.boundsMin.z, 1, 1e-5); CHECK_NEAR(m.boundsMax.z, 3, 1e-5);

  // ---- round trip through EMOD
  std::string path = (std::filesystem::temp_directory_path() / "engine_test_roundtrip.emod").string();
  CHECK_MSG(saveEmod(m, path, err), err);
  Model r;
  CHECK_MSG(loadEmod(path, r, err), err);
  std::filesystem::remove(path);
  CHECK(r.images.size() == m.images.size()); CHECK(r.materials.size() == 1); CHECK(r.meshes.size() == 1); CHECK(r.nodes.size() == 2);
  const Material& rm = r.materials[0];
  CHECK(rm.name == mt.name); CHECK(std::memcmp(&rm.baseColor, &mt.baseColor, sizeof(vec4)) == 0);
  CHECK(rm.metallic == mt.metallic && rm.roughness == mt.roughness);
  CHECK(std::memcmp(&rm.emissive, &mt.emissive, sizeof(vec3)) == 0);
  CHECK(rm.baseColorTex == mt.baseColorTex && rm.metalRoughTex == mt.metalRoughTex && rm.normalTex == mt.normalTex && rm.emissiveTex == mt.emissiveTex && rm.occlusionTex == mt.occlusionTex);
  CHECK(rm.alphaMode == mt.alphaMode && rm.alphaCutoff == mt.alphaCutoff && rm.doubleSided == mt.doubleSided && rm.unlit == mt.unlit);
  const Primitive& rp = r.meshes[0].primitives[0];
  CHECK(r.meshes[0].name == "tri"); CHECK(rp.material == p.material);
  CHECK(rp.vertices.size() == p.vertices.size() && std::memcmp(rp.vertices.data(), p.vertices.data(), p.vertices.size() * sizeof(Vertex)) == 0);
  CHECK(rp.indices == p.indices);
  CHECK(std::memcmp(&rp.boundsMin, &p.boundsMin, sizeof(vec3)) == 0 && std::memcmp(&rp.boundsMax, &p.boundsMax, sizeof(vec3)) == 0);
  for (size_t i = 0; i < 2; ++i) {
    const Node &a = m.nodes[i], &b = r.nodes[i];
    CHECK(a.name == b.name && a.mesh == b.mesh && a.parent == b.parent && a.children == b.children);
    CHECK(sameMat(a.local, b.local)); CHECK(a.extras == b.extras);
  }
  CHECK(r.roots == m.roots);
  CHECK(std::memcmp(&r.boundsMin, &m.boundsMin, sizeof(vec3)) == 0 && std::memcmp(&r.boundsMax, &m.boundsMax, sizeof(vec3)) == 0);
  // bad input
  CHECK(!loadEmod("/nonexistent/x.emod", r, err)); CHECK(!err.empty());
  std::vector<uint8_t> junk(glb.begin(), glb.begin() + 40);
  CHECK(!loadGlb(junk, r, err));
  std::vector<uint8_t> notGlb{'x', 'y', 'z', 'w', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  CHECK(!loadGlb(notGlb, r, err) && err == "not a GLB file");

  // ---- model 2: KHR_texture_transform (offset + scale + rotation) baked into UVs, texture sampling TEXCOORD_1,
  //      one image kept encoded (no decoding in the engine), sampler wrap modes, linear MR map, unlit
  {
    GlbBuilder b;
    std::vector<float> pos{0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0};
    std::vector<float> uv0{0, 0, 1, 0, 0, 1, 1, 1};
    std::vector<float> uv1{0, 0, 2, 0, 0, 2, 2, 2};   // distinct from set 0 so the selection is observable
    std::vector<uint16_t> idx{0, 1, 2, 2, 1, 3};
    std::vector<uint8_t> fakePng{0x89, 'P', 'N', 'G', 1, 2, 3, 4, 5};
    int aPos = b.accessor(b.view(pos), 4, "VEC3", 5126);
    int aUv0 = b.accessor(b.view(uv0), 4, "VEC2", 5126);
    int aUv1 = b.accessor(b.view(uv1), 4, "VEC2", 5126);
    int aIdx = b.accessor(b.view(idx), 6, "SCALAR", 5123);
    int vImg = b.view(fakePng);
    std::string body =
      "\"images\":[{\"mimeType\":\"image/png\",\"bufferView\":" + std::to_string(vImg) + "},{\"mimeType\":\"image/png\",\"bufferView\":" + std::to_string(vImg) + "}],"
      "\"samplers\":[{\"wrapS\":33071,\"wrapT\":33648},{\"wrapS\":10497,\"wrapT\":10497}],"
      "\"textures\":[{\"source\":0,\"sampler\":0},{\"source\":1,\"sampler\":1}],"
      "\"materials\":["
      "{\"name\":\"xf\",\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0,\"texCoord\":1,\"extensions\":{\"KHR_texture_transform\":{\"offset\":[0.5,0.25],\"scale\":[2,3],\"rotation\":1.5707963267948966}}},"
      "\"metallicRoughnessTexture\":{\"index\":1}},\"extensions\":{\"KHR_materials_unlit\":{}}},"
      "{\"name\":\"set1\",\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0,\"texCoord\":1}}},"
      "{\"name\":\"set0\",\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}},"
      "{\"name\":\"set9\",\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0,\"texCoord\":9}}}],"
      "\"meshes\":[{\"name\":\"q\",\"primitives\":["
      "{\"attributes\":{\"POSITION\":" + std::to_string(aPos) + ",\"TEXCOORD_0\":" + std::to_string(aUv0) + ",\"TEXCOORD_1\":" + std::to_string(aUv1) + "},\"indices\":" + std::to_string(aIdx) + ",\"material\":0},"
      "{\"attributes\":{\"POSITION\":" + std::to_string(aPos) + ",\"TEXCOORD_0\":" + std::to_string(aUv0) + ",\"TEXCOORD_1\":" + std::to_string(aUv1) + "},\"indices\":" + std::to_string(aIdx) + ",\"material\":1},"
      "{\"attributes\":{\"POSITION\":" + std::to_string(aPos) + ",\"TEXCOORD_0\":" + std::to_string(aUv0) + ",\"TEXCOORD_1\":" + std::to_string(aUv1) + "},\"indices\":" + std::to_string(aIdx) + ",\"material\":2},"
      "{\"attributes\":{\"POSITION\":" + std::to_string(aPos) + ",\"TEXCOORD_0\":" + std::to_string(aUv0) + ",\"TEXCOORD_1\":" + std::to_string(aUv1) + "},\"indices\":" + std::to_string(aIdx) + ",\"material\":3},"
      "{\"attributes\":{\"POSITION\":" + std::to_string(aPos) + "},\"mode\":1}"   // lines: skipped
      "]}],\"nodes\":[{\"mesh\":0}]";
    glb = b.finish(body);
  }
  Model t;
  CHECK_MSG(loadGlb(glb, t, err), err);
  CHECK(t.images.size() == 2); CHECK(t.images[0].encoded.size() == 9 && t.images[0].encoded[1] == 'P'); CHECK(t.images[0].pixels.empty());
  CHECK(t.images[0].wrapS == 1 && t.images[0].wrapT == 2); CHECK(t.images[1].wrapS == 0 && t.images[1].wrapT == 0);
  CHECK(!t.images[0].linear); CHECK(t.images[1].linear);   // metal/rough map flagged linear
  CHECK(t.materials.size() == 4); CHECK(t.materials[0].unlit); CHECK(t.materials[0].baseColorTex == 0 && t.materials[0].metalRoughTex == 1);
  CHECK(t.materials[0].baseColorUv == 1); CHECK(t.materials[1].baseColorUv == 1); CHECK(t.materials[2].baseColorUv == 0); CHECK(t.materials[3].baseColorUv == 9);
  CHECK(t.materials[0].uvScale.x == 2 && t.materials[0].uvScale.y == 3 && t.materials[0].uvOffset.x == 0.5f);
  CHECK(t.meshes[0].primitives.size() == 4);   // line primitive skipped
  // prim 0: TEXCOORD_1 (2,2) at vertex 3, transform: scale -> (4,6), rotate 90° -> (-6,4), offset -> (-5.5, 4.25)
  const Vertex& v3 = t.meshes[0].primitives[0].vertices[3];
  CHECK_NEAR(v3.uv.x, -5.5, 1e-5); CHECK_NEAR(v3.uv.y, 4.25, 1e-5);
  const Vertex& v0 = t.meshes[0].primitives[0].vertices[0];
  CHECK_NEAR(v0.uv.x, 0.5, 1e-6); CHECK_NEAR(v0.uv.y, 0.25, 1e-6);   // uv (0,0) -> offset only
  // prim 1: TEXCOORD_1 selected, untransformed
  CHECK(t.meshes[0].primitives[1].vertices[3].uv.x == 2 && t.meshes[0].primitives[1].vertices[3].uv.y == 2);
  // prim 2: TEXCOORD_0
  CHECK(t.meshes[0].primitives[2].vertices[3].uv.x == 1 && t.meshes[0].primitives[2].vertices[3].uv.y == 1);
  // prim 3: TEXCOORD_9 missing -> falls back to TEXCOORD_0
  CHECK(t.meshes[0].primitives[3].vertices[3].uv.x == 1);
  // no NORMAL -> flat normals from winding (CCW in XY plane -> +Z)
  CHECK_NEAR(t.meshes[0].primitives[2].vertices[0].normal.z, 1, 1e-6);
  // no scenes -> parentless nodes are roots
  CHECK(t.roots.size() == 1 && t.roots[0] == 0);

  // EMOD refuses undecoded images; after a fake decode the flags round-trip
  CHECK(!saveEmod(t, path, err)); CHECK(err.find("not decoded") != std::string::npos);
  for (Image& im : t.images) { im.width = 2; im.height = 1; im.channels = 4; im.pixels = {1, 2, 3, 4, 5, 6, 7, 8}; im.encoded.clear(); }
  CHECK_MSG(saveEmod(t, path, err), err);
  Model t2; CHECK_MSG(loadEmod(path, t2, err), err); std::filesystem::remove(path);
  CHECK(t2.images.size() == 2 && t2.images[0].width == 2 && t2.images[0].height == 1 && t2.images[0].channels == 4 && t2.images[0].pixels == t.images[0].pixels);
  CHECK(t2.images[0].wrapS == 1 && t2.images[0].wrapT == 2 && !t2.images[0].linear && t2.images[1].linear);
  CHECK(t2.materials[0].unlit && t2.materials[0].baseColorTex == 0 && t2.materials[0].metalRoughTex == 1);
  CHECK(t2.meshes[0].primitives.size() == 4);
  CHECK(std::memcmp(t2.meshes[0].primitives[0].vertices.data(), t.meshes[0].primitives[0].vertices.data(), 4 * sizeof(Vertex)) == 0);
  CHECK(t2.meshes[0].primitives[0].indices == t.meshes[0].primitives[0].indices);

  // v5 variants: a compressed chain plus an RGBA8 fallback round-trip; wrong level sizes are refused
  Image& v5 = t.images[0]; v5.width = 8; v5.height = 4; v5.pixels.clear();
  ImageVariant etc; etc.format = TexFormat::ETC2_RGB;
  etc.mips = {{8, 4, std::vector<uint8_t>(16, 0xAB)}, {4, 2, std::vector<uint8_t>(8, 0xCD)}, {2, 1, std::vector<uint8_t>(8, 1)}, {1, 1, std::vector<uint8_t>(8, 2)}};
  ImageVariant fb; fb.format = TexFormat::RGBA8; fb.mips = {{2, 1, std::vector<uint8_t>(8, 7)}};
  v5.variants = {etc, fb};
  CHECK_MSG(saveEmod(t, path, err), err);
  Model t3; CHECK_MSG(loadEmod(path, t3, err), err); std::filesystem::remove(path);
  CHECK(t3.images[0].variants.size() == 2 && t3.images[0].variants[0].format == TexFormat::ETC2_RGB && t3.images[0].variants[0].mips.size() == 4);
  CHECK(t3.images[0].variants[0].mips[1].width == 4 && t3.images[0].variants[0].mips[1].data == etc.mips[1].data);
  CHECK(t3.images[0].variants[1].format == TexFormat::RGBA8 && t3.images[0].variants[1].mips[0].data == fb.mips[0].data);
  CHECK(t3.images[0].pixels.empty() && t3.images[1].pixels.size() == 8);   // plain RGBA8 records are still exposed as pixels
  v5.variants[0].mips[0].data.resize(15);
  CHECK(!saveEmod(t, path, err)); CHECK(err.find("mip level") != std::string::npos);
  CHECK(texLevelBytes(TexFormat::ETC2_RGBA, 5, 5) == 64 && texLevelBytes(TexFormat::BC1, 1, 1) == 8);
  // standalone image file (terrain tiles)
  std::string ipath = (std::filesystem::temp_directory_path() / "eng_test.eimg").string();
  CHECK_MSG(saveImageFile(t.images[1], ipath, err), err);
  std::ifstream fi(ipath, std::ios::binary); std::vector<uint8_t> ib((std::istreambuf_iterator<char>(fi)), {}); fi.close(); std::filesystem::remove(ipath);
  Image im2; CHECK_MSG(loadImageFile(ib, im2, err), err);
  CHECK(im2.width == 2 && im2.linear && im2.pixels == t.images[1].pixels);
  CHECK(!loadImageFile(std::span<const uint8_t>(ib.data(), 10), im2, err));
  // v6 placeholder: hints + source index only, no pixel data (textures streamed by the host)
  v5.variants.clear(); v5.pixels.clear(); v5.source = 3; v5.width = 4096; v5.height = 2048;
  CHECK(v5.placeholder());
  CHECK_MSG(saveEmod(t, path, err), err);
  Model t4; CHECK_MSG(loadEmod(path, t4, err), err); std::filesystem::remove(path);
  CHECK(t4.images[0].placeholder() && t4.images[0].source == 3 && t4.images[0].width == 4096 && t4.images[0].wrapS == 1);
  CHECK(!t4.images[1].placeholder() && t4.images[1].source == -1);

  // ---- model 3: animations (v7). Two nodes: `door` translated by a LINEAR clip, `arm` rotated by a STEP
  //      clip plus a CUBICSPLINE scale sampler (tangents dropped); a matrix-form node is decomposed to TRS.
  {
    GlbBuilder b;
    std::vector<float> times{0, 1, 2};
    std::vector<float> trans{0, 0, 0, 0, 0, 1, 0, 0, 3};               // z: 0 -> 1 -> 3
    std::vector<float> rot{0, 0, 0, 1, 0, 0.7071068f, 0, 0.7071068f, 0, 1, 0, 0};   // identity, 90 deg Y, 180 deg Y
    std::vector<float> cub{0, 0, 0, 1, 1, 1, 0, 0, 0, 0, 0, 0, 2, 2, 2, 0, 0, 0, 0, 0, 0, 4, 4, 4, 0, 0, 0};   // (in, value, out) x 3 keys
    int aT = b.accessor(b.view(times), 3, "SCALAR", 5126, ",\"min\":[0],\"max\":[2]");
    int aTr = b.accessor(b.view(trans), 3, "VEC3", 5126);
    int aRot = b.accessor(b.view(rot), 3, "VEC4", 5126);
    int aCub = b.accessor(b.view(cub), 9, "VEC3", 5126);
    std::string body =
      "\"nodes\":[{\"name\":\"door\",\"translation\":[5,0,0],\"children\":[1]},"
      "{\"name\":\"arm\",\"matrix\":[2,0,0,0, 0,2,0,0, 0,0,2,0, 1,2,3,1]}],"
      "\"scenes\":[{\"nodes\":[0]}],"
      "\"animations\":[{\"name\":\"pintu-kiri\",\"samplers\":[{\"input\":" + std::to_string(aT) + ",\"output\":" + std::to_string(aTr) + "}],"
      "\"channels\":[{\"sampler\":0,\"target\":{\"node\":0,\"path\":\"translation\"}}]},"
      "{\"name\":\"panto-naik\",\"samplers\":[{\"input\":" + std::to_string(aT) + ",\"output\":" + std::to_string(aRot) + ",\"interpolation\":\"STEP\"},"
      "{\"input\":" + std::to_string(aT) + ",\"output\":" + std::to_string(aCub) + ",\"interpolation\":\"CUBICSPLINE\"}],"
      "\"channels\":[{\"sampler\":0,\"target\":{\"node\":1,\"path\":\"rotation\"}},{\"sampler\":1,\"target\":{\"node\":1,\"path\":\"scale\"}},"
      "{\"sampler\":0,\"target\":{\"node\":1,\"path\":\"weights\"}}]}]";
    glb = b.finish(body);
  }
  Model an;
  CHECK_MSG(loadGlb(glb, an, err), err);
  CHECK(an.nodes[0].translation.x == 5 && an.nodes[0].scale.y == 1);
  CHECK_NEAR(an.nodes[1].translation.z, 3, 1e-6); CHECK_NEAR(an.nodes[1].scale.x, 2, 1e-6); CHECK_NEAR(an.nodes[1].rotation.w, 1, 1e-6);
  CHECK(an.animations.size() == 2);
  CHECK(an.animations[0].name == "pintu-kiri" && an.animations[0].channels.size() == 1 && an.animations[0].samplers.size() == 1);
  CHECK_NEAR(an.animations[0].duration, 2, 1e-6);
  CHECK(an.animations[1].channels.size() == 2);   // weights channel dropped
  CHECK(an.animations[1].samplers[0].step && !an.animations[1].samplers[1].step);
  CHECK(an.animations[1].samplers[1].values.size() == 9 && an.animations[1].samplers[1].values[3] == 2 && an.animations[1].samplers[1].values[6] == 4);
  // scrub: door z at t = 1.5 -> 2 (linear between 1 and 3), clamped past the end; arm STEP holds 90 deg until t = 2
  std::vector<mat4> local, world;
  restPose(an.nodes, local); scrubAnimation(an.nodes, an.animations[0], 1.5f, local);
  CHECK_NEAR(local[0].m[3][2], 2, 1e-5); CHECK_NEAR(local[0].m[3][0], 0, 1e-5);   // translation replaced (x no longer 5)
  scrubAnimation(an.nodes, an.animations[0], 9, local); CHECK_NEAR(local[0].m[3][2], 3, 1e-5);
  scrubAnimation(an.nodes, an.animations[1], 1.9f, local);
  { vec3 ax = local[1].transformDir({1, 0, 0}); CHECK_NEAR(ax.x, 0, 1e-4); CHECK_NEAR(ax.z, -3.8, 1e-4); }   // 90 deg about Y (STEP), scale 2 -> 4 lerped at 0.9
  scrubAnimation(an.nodes, an.animations[1], 0.5f, local);
  { vec3 ax = local[1].transformDir({1, 0, 0}); CHECK_NEAR(ax.x, 1.5, 1e-4); }   // STEP rotation held at key 0, scale lerps 1 -> 2
  computeWorld(an.nodes, an.roots, local, world);
  CHECK_NEAR(world[1].m[3][2], 3 + 3, 1e-4);   // parent door at z = 3 (clamped clip) + own z 3
  // EMOD v7 round trip of TRS + clips
  CHECK_MSG(saveEmod(an, path, err), err);
  Model an2; CHECK_MSG(loadEmod(path, an2, err), err); std::filesystem::remove(path);
  CHECK(an2.animations.size() == 2 && an2.animations[1].name == "panto-naik");
  CHECK(an2.animations[0].samplers[0].times == an.animations[0].samplers[0].times && an2.animations[0].samplers[0].values == an.animations[0].samplers[0].values);
  CHECK(an2.animations[1].samplers[0].step && an2.animations[1].channels[1].path == AnimPath::Scale && an2.animations[1].channels[1].node == 1);
  CHECK(an2.nodes[1].scale.x == an.nodes[1].scale.x && an2.nodes[0].translation.x == 5 && an2.nodes[1].rotation.w == an.nodes[1].rotation.w);
})

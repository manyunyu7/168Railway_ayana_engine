#include "engine/world/garis_visual.h"
#include "engine/world/rolling_stock.h"
#include "engine/world/spline.h"
#include "engine/world/sun.h"
#include <cstdio>

namespace eng {

using namespace garis;

namespace { constexpr float RAD = PI / 180; }

// uji3dSpline.ts protoProsedural: boxes in tile space (long axis +X, centred in x/z, floor at y = 0 —
// except the platform whose skirt goes below zero). `rough` maps the MeshStandardMaterial roughness.
bool GarisVisuals::procParts(const std::string& name, std::vector<ProcPart>& out) {
  auto box = [&](float lx, float ly, float lz, unsigned color, float rough, float x = 0, float y = 0, float z = 0, float rotX = 0) {
    out.push_back({{x, y, z}, {lx, ly, lz}, color, rough, rotX});
  };
  auto badanPeron = [&](float panjang) {
    float tinggiBadan = PERON_TINGGI + PERON_ROK;
    box(panjang, tinggiBadan, PERON_LEBAR, WARNA_DINDING, 0.95f, 0, PERON_TINGGI - tinggiBadan / 2, 0);
    box(panjang, 0.06f, PERON_LEBAR - 0.02f, WARNA_LANTAI, 0.95f, 0, PERON_TINGGI - 0.02f, 0);   // floor as its own slab for the colour
    for (float s : {-1.f, 1.f}) {
      box(panjang, 0.08f, 0.45f, WARNA_DINDING, 0.95f, 0, PERON_TINGGI + 0.01f, s * (PERON_LEBAR / 2 - 0.225f));   // edge strip
      box(panjang, 0.02f, GARIS_KUNING_LEBAR, WARNA_KUNING, 0.7f, 0, PERON_TINGGI + 0.05f, s * (PERON_LEBAR / 2 - GARIS_KUNING_DARI_TEPI));   // safety line, both edges (island platform)
    }
  };
  if (name == "tembok-beton") {
    box(4.96f, 2.1f, 0.12f, 0xb8b4ac, 0.95f, 0, 1.15f, 0);
    box(5, 0.2f, 0.2f, 0x9a968e, 0.95f, 0, 0.1f, 0);
    box(5, 0.1f, 0.18f, 0x9a968e, 0.95f, 0, 2.25f, 0);
    for (float s : {-1.f, 1.f}) box(0.22f, 2.3f, 0.22f, 0x9a968e, 0.95f, s * 2.5f, 1.15f, 0);   // pilasters at both ends
    return true;
  }
  if (name == "peron") { badanPeron(PERON_UBIN); return true; }
  if (name == "peron-kanopi") {
    badanPeron(PERON_UBIN);
    float puncak = PERON_TINGGI + KANOPI_TINGGI;
    box(PERON_UBIN, 0.24f, 0.3f, 0x7e858d, 0.6f, 0, puncak + 0.12f, 0);   // ridge beam
    for (float s : {-1.f, 1.f}) {   // shallow gable: two ±8° panes meeting at the ridge
      float miring = s * 8 * RAD;
      box(PERON_UBIN, 0.08f, KANOPI_LEBAR / 2, 0xccd1d6, 0.55f, 0, puncak + 0.30f - std::fabs(std::sin(miring)) * KANOPI_LEBAR / 4, s * KANOPI_LEBAR / 4, miring);
    }
    return true;
  }
  if (name == "peron-tiang") {   // canopy post + cross rafters, planted every jarakTiang
    float puncak = PERON_TINGGI + KANOPI_TINGGI;
    box(0.2f, KANOPI_TINGGI, 0.2f, 0x7e858d, 0.6f, 0, PERON_TINGGI + KANOPI_TINGGI / 2, 0);
    for (float s : {-1.f, 1.f}) box(0.14f, 0.14f, KANOPI_LEBAR / 2, 0x7e858d, 0.6f, 0, puncak + 0.06f, s * KANOPI_LEBAR / 4);
    return true;
  }
  return false;
}

bool GarisVisuals::walkFloor(const GarisEntry& k) {
  if (k.datar) return true;
  for (const char* kat : {"peron", "jalan", "jembatan", "tanggul", "sawah", "rel"}) if (k.kategori == kat) return true;
  return false;
}

void GarisVisuals::placeProc(const std::string& name, const std::vector<mat4>& mats, bool walk, bool peron) {
  std::vector<ProcPart> parts;
  if (!procParts(name, parts)) { ++stats.missing; std::printf("[garis] no procedural prototype '%s'\n", name.c_str()); return; }
  MeshBuilder walkMesh;
  for (const ProcPart& p : parts) {
    MeshBuilder unit; unit.box(p.size * -0.5f, p.size * 0.5f);
    mat4 local = mat4::translation(p.centre) * (p.rotX != 0 ? mat4::rotationX(p.rotX) : mat4::identity());
    unsigned key = (p.color & 0xffffffu) | ((unsigned)(p.rough * 255) << 24);
    MeshBuilder& mb = procBuild_[key];
    for (const mat4& m : mats) mb.append(unit, m * local);
    if (walk && p.size.y >= 0.05f) for (const mat4& m : mats) walkMesh.append(unit, m * local);   // paint-thin strips (safety line) skipped
  }
  if (walk && !walkMesh.empty()) {
    ProcWalk w; w.peron = peron; w.idx = walkMesh.indices; w.pos.reserve(walkMesh.vertices.size());
    for (const Vertex& v : walkMesh.vertices) w.pos.push_back(v.pos);
    procWalk_.push_back(std::move(w));
  }
}

void GarisVisuals::placeGlb(const std::string& catalogId, AssetCatalog& catalog, const std::vector<mat4>& mats, bool walk, bool peron) {
  GpuModel* m = catalog.model(catalogId);
  if (!m) { ++stats.missing; return; }   // pilot GLB absent: skipped silently (as the web)
  ModelSlot& slot = slots_[catalogId];
  if (!slot.model) { slot.model = m; slot.norm = RollingStock::normalizeTransform(*m, true); slot.walk = walk; slot.peron = peron; }   // long axis +X, centred, base y = 0
  AABB local = m->bounds.transformed(slot.norm);
  for (const mat4& mm : mats) { slot.mats.push_back(mm * slot.norm); slot.bounds.expand(local.transformed(mm)); }
}

void GarisVisuals::collectWalk(WalkCollider& out, const AssetCatalog&) const {
  for (const ProcWalk& w : procWalk_) out.addMesh(w.pos, w.idx, mat4::identity(), w.peron, true);
  for (const auto& [id, slot] : slots_) {
    if (!slot.walk || !slot.model) continue;
    const GpuModel& m = *slot.model;
    for (size_t ni = 0; ni < m.nodes.size(); ++ni) {
      const Node& n = m.nodes[ni];
      if (n.mesh < 0 || n.mesh >= (int)m.meshes.size()) continue;
      for (const GpuPrimitive& prim : m.meshes[(size_t)n.mesh].primitives)
        for (const mat4& mm : slot.mats) out.addMesh(prim.collisionPos, prim.collisionIdx, mm * m.world[ni], slot.peron, true);
    }
  }
}

void GarisVisuals::build(const Json& hiasan, AssetCatalog& catalog, const WorldOrigin& origin, const GroundFn& ground) {
  destroy();
  stats = {};
  auto height = [&](float x, float z) { return ground(x + origin.ox, z + origin.oz); };
  for (const Json& g : hiasan["garis"].arr) {
    const GarisEntry* k = catalog.findGaris(g["kelas"].stringOr(""));
    if (!k) { ++stats.missing; std::printf("[garis] unknown class '%s'\n", g["kelas"].stringOr("").c_str()); continue; }
    std::vector<vec2> pts;
    for (const Json& t : g["titik"].arr) { vec3 p = origin.toScene(t["x"].numberOr(0), t["y"].numberOr(0), 0); pts.push_back({p.x, p.z}); }
    CatmullRom curve;
    if (!curve.set(pts)) continue;
    float naik = (float)g["naik"].numberOr(k->naik);
    ++stats.lines;
    auto frames = [&](float step, bool centred) {
      SplineFrameOptions o; o.step = step; o.height = height; o.centred = centred; o.flat = k->datar;
      std::vector<SplineFrame> fr = splineFrames(curve, o);
      std::vector<mat4> mats; mats.reserve(fr.size());
      for (const SplineFrame& f : fr) mats.push_back(splineFrameMatrix(f, naik));
      stats.tiles += mats.size();
      return mats;
    };
    const bool walk = walkFloor(*k), peron = k->kategori == "peron";
    auto put = [&](const std::string& berkasKey, const std::string& prosedural, const std::vector<mat4>& mats) {
      if (mats.empty()) return;
      if (!prosedural.empty()) placeProc(prosedural, mats, walk, peron); else placeGlb(berkasKey, catalog, mats, walk, peron);
    };
    if (!k->berkas.empty() || !k->prosedural.empty()) put("garis:" + k->id, k->prosedural, frames(k->langkah, true));
    if ((!k->tiang.empty() || !k->tiangProsedural.empty()) && k->jarakTiang > 0) put("garis:" + k->id + ":tiang", k->tiangProsedural, frames(k->jarakTiang, false));
    for (size_t i = 0; i < k->slot.size(); ++i)
      if (k->slot[i].jarak > 0) put("garis:" + k->id + ":slot" + std::to_string(i), k->slot[i].prosedural, frames(k->slot[i].jarak, false));
  }
  for (auto& [id, slot] : slots_) {
    if (slot.mats.empty()) continue;
    slot.instances = rhi::createDynamicBuffer(slot.mats.size() * sizeof(mat4));
    rhi::updateBuffer(slot.instances, std::as_bytes(std::span(slot.mats)));
  }
  stats.glbSlots = slots_.size();
  for (auto& [key, mb] : procBuild_) {
    if (mb.empty()) continue;
    ProcMesh pm; pm.mesh = mb.upload(); pm.bounds = mb.bounds;
    vec3 c = rgb(key & 0xffffffu); pm.mat.baseColor = {c.x, c.y, c.z, 1}; pm.mat.metallic = 0; pm.mat.roughness = (float)(key >> 24) / 255.f;
    proc_.push_back(pm);
  }
  procBuild_.clear();
  stats.procMeshes = proc_.size();
}

void GarisVisuals::draw(ModelRenderer& r, const Frustum* frustum) {
  for (auto& [id, slot] : slots_) {
    if (slot.mats.empty() || (frustum && !frustum->contains(slot.bounds))) continue;
    if (!slot.model->textured()) continue;   // textures still streaming
    r.drawInstanced(*slot.model, mat4::identity(), (uint32_t)slot.mats.size(), slot.instances, slot.bounds.center());
  }
  for (const ProcMesh& pm : proc_) {
    if (frustum && !frustum->contains(pm.bounds)) continue;
    r.drawMesh(pm.mesh, pm.mat, {}, mat4::identity());
  }
}

void GarisVisuals::destroy() {
  for (auto& [id, slot] : slots_) if (slot.instances.id) rhi::destroyBuffer(slot.instances);
  slots_.clear();
  for (ProcMesh& pm : proc_) rhi::destroyMesh(pm.mesh);
  proc_.clear(); procBuild_.clear(); procWalk_.clear();
}

} // namespace eng

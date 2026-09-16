#include "engine/render/texture_cache.h"
#include "engine/render/model_renderer.h"
#include <span>
#include <unordered_map>

namespace eng {
namespace {

struct Entry { rhi::Texture tex; unsigned refs; uint64_t bytes; };
std::unordered_map<uint64_t, Entry>& entries() { static std::unordered_map<uint64_t, Entry> e; return e; }
std::unordered_map<uint32_t, uint64_t>& byId() { static std::unordered_map<uint32_t, uint64_t> m; return m; }   // texture id -> key
TextureCacheStats& stats() { static TextureCacheStats s; return s; }

uint64_t fnv(uint64_t h, const void* data, size_t n) {
  const uint8_t* p = (const uint8_t*)data;
  for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
  return h;
}
template <class T> uint64_t fnvValue(uint64_t h, T v) { return fnv(h, &v, sizeof v); }

// The variant uploadImage will use (first supported one), or nullptr for a plain `pixels` image.
const ImageVariant* chosenVariant(const Image& im) {
  static const rhi::Format map[] = {rhi::Format::RGBA8, rhi::Format::ETC2_RGB, rhi::Format::ETC2_RGBA, rhi::Format::BC1, rhi::Format::BC3, rhi::Format::BC7};
  for (const ImageVariant& v : im.variants) if (!v.mips.empty() && rhi::supports(map[(int)v.format])) return &v;
  return nullptr;
}

// Key + upload size of an image; false for placeholders and images with nothing to upload.
bool keyOf(const Image& im, uint64_t& key, uint64_t& bytes) {
  if (im.placeholder()) return false;
  uint64_t h = 14695981039346656037ull;
  h = fnvValue(h, im.wrapS); h = fnvValue(h, im.wrapT); h = fnvValue(h, (uint8_t)im.linear);
  bytes = 0;
  if (im.variants.empty()) {
    if (im.pixels.empty()) return false;
    h = fnvValue(h, (int32_t)-1); h = fnvValue(h, (int32_t)im.width); h = fnvValue(h, (int32_t)im.height);
    h = fnvValue(h, (uint64_t)im.pixels.size()); h = fnv(h, im.pixels.data(), im.pixels.size());
    bytes = (uint64_t)im.pixels.size() * 4 / 3;   // generated mip chain
    key = h; return true;
  }
  const ImageVariant* v = chosenVariant(im);
  if (!v) return false;
  h = fnvValue(h, (int32_t)v->format); h = fnvValue(h, (uint64_t)v->mips.size());
  for (const MipLevel& l : v->mips) {
    h = fnvValue(h, (int32_t)l.width); h = fnvValue(h, (int32_t)l.height); h = fnvValue(h, (uint64_t)l.data.size());
    h = fnv(h, l.data.data(), l.data.size());
    bytes += l.data.size();
  }
  if (v->format == TexFormat::RGBA8) bytes = bytes * 4 / 3;   // only mip 0 is uploaded, the chain is generated
  key = h; return true;
}
}

rhi::Texture acquireTexture(const Image& im) {
  uint64_t key = 0, bytes = 0;
  if (!keyOf(im, key, bytes)) return uploadImage(im);
  auto& e = entries();
  if (auto it = e.find(key); it != e.end()) {
    ++it->second.refs; ++stats().references; stats().bytesShared += it->second.bytes;
    return it->second.tex;
  }
  rhi::Texture t = uploadImage(im);
  if (!t.id) return t;
  e[key] = {t, 1, bytes}; byId()[t.id] = key;
  ++stats().entries; ++stats().references; stats().bytes += bytes;
  return t;
}

void releaseTexture(rhi::Texture t) {
  if (!t.id) return;
  auto& ids = byId();
  auto idIt = ids.find(t.id);
  if (idIt == ids.end()) { rhi::destroyTexture(t); return; }
  auto& e = entries();
  auto it = e.find(idIt->second);
  --stats().references;
  if (it == e.end()) { ids.erase(idIt); rhi::destroyTexture(t); return; }
  if (--it->second.refs > 0) return;
  stats().bytes -= it->second.bytes; --stats().entries;
  rhi::destroyTexture(it->second.tex);
  e.erase(it); ids.erase(idIt);
}

TextureCacheStats textureCacheStats() { return stats(); }

} // namespace eng

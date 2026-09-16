// Content-addressed GPU texture cache: models that carry byte-identical images (the same atlas exported into
// several GLBs — houses, shops, station props) share one rhi::Texture instead of uploading it once per model.
// The key hashes the dimensions, wrap/colour-space flags and the uploaded bytes (the variant uploadImage
// would pick, or `pixels`); placeholders (streamed later) are never cached.
// Ownership is counted: every acquire needs a release; release on a texture the cache does not know
// destroys it outright, so callers can route every model texture through release().
#pragma once
#include "engine/asset/model.h"
#include "engine/rhi/rhi.h"
#include <cstdint>

namespace eng {

rhi::Texture acquireTexture(const Image& im);   // upload or share; id 0 when nothing is usable (see uploadImage)
void releaseTexture(rhi::Texture t);            // drop one reference; the GPU texture dies with the last one

struct TextureCacheStats {
  unsigned entries = 0;        // distinct textures alive through the cache
  unsigned references = 0;     // acquisitions still held (>= entries)
  uint64_t bytes = 0;          // GPU bytes of the distinct textures (mip chains as stored)
  uint64_t bytesShared = 0;    // bytes that a second (or later) acquisition of the same image did not upload again
};
TextureCacheStats textureCacheStats();

} // namespace eng

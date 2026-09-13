#include "engine/core/fetch.h"
#include <fstream>
#ifdef __EMSCRIPTEN__
#include <emscripten/fetch.h>
#include <cstring>
#endif

namespace eng {

#ifdef __EMSCRIPTEN__
namespace {
struct Pending { FetchCallback cb; std::string path; };
void done(emscripten_fetch_t* f) {
  Pending* p = (Pending*)f->userData;
  FetchResult r; r.ok = true; r.bytes.assign((const uint8_t*)f->data, (const uint8_t*)f->data + f->numBytes);
  emscripten_fetch_close(f);
  p->cb(r); delete p;
}
void failed(emscripten_fetch_t* f) {
  Pending* p = (Pending*)f->userData;
  FetchResult r; r.error = p->path + ": HTTP " + std::to_string(f->status);
  emscripten_fetch_close(f);
  p->cb(r); delete p;
}
}
void fetchFile(const std::string& path, FetchCallback cb) {
  emscripten_fetch_attr_t attr; emscripten_fetch_attr_init(&attr);
  std::strcpy(attr.requestMethod, "GET");
  attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY;
  attr.onsuccess = done; attr.onerror = failed;
  attr.userData = new Pending{std::move(cb), path};
  emscripten_fetch(&attr, path.c_str());
}
#else
void fetchFile(const std::string& path, FetchCallback cb) {
  FetchResult r;
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) { r.error = "cannot open " + path; cb(r); return; }
  std::streamsize n = f.tellg(); f.seekg(0); r.bytes.resize((size_t)n);
  if (n > 0 && !f.read((char*)r.bytes.data(), n)) { r.error = "read error " + path; r.bytes.clear(); cb(r); return; }
  r.ok = true; cb(r);
}
#endif

} // namespace eng

// Asynchronous file loading. Native: reads the file synchronously and calls back before returning.
// Emscripten: emscripten_fetch (XHR) — the callback runs later on the main loop, so callers must keep
// their state alive and treat every load as pending until `ok` arrives. Paths are file paths natively
// and URLs (relative to the page) on the web.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace eng {

struct FetchResult { bool ok = false; std::string error; std::vector<uint8_t> bytes; };
using FetchCallback = std::function<void(FetchResult&)>;

void fetchFile(const std::string& path, FetchCallback cb);

} // namespace eng

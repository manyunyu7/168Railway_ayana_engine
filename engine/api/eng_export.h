// Keeps the `eng_*` C ABI visible in the shared build. Emscripten drops anything not KEEPALIVE'd;
// Android builds libayana.so with -fvisibility=hidden, so the exported entry points must say so.
#pragma once
#if defined(__EMSCRIPTEN__)
  #include <emscripten.h>
  #define ENG_EXPORT EMSCRIPTEN_KEEPALIVE
#elif defined(__GNUC__) || defined(__clang__)
  #define ENG_EXPORT __attribute__((visibility("default"), used))
#else
  #define ENG_EXPORT
#endif

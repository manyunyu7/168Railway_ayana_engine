# Flutter / Android plan — the engine inside the 168Railway app

Status: plan, 2026-09-18. Nothing on Android exists yet (PARITY.md row "GLES3 backend + Android EGL,
Flutter `Texture` plugin" = ❌). This document is the order of work; each milestone ends with something
visible on a real phone.

## Where we start from

**Engine (this repo).** The whole world renderer is already a C library: `engine/api/engine_api.h`
(`eng_*`, ~150 `extern "C"` functions, single-threaded, host drives every fetch). The RHI already
compiles for GLES 3.0 (`ENG_GL_ES`, used by the WebGL2 build) and the root CMake already has an
`ANDROID` branch linking `GLESv3 EGL log android`. Emscripten-specific code is small and isolated:
`eng_init` creating a WebGL context on `#ayana-canvas`, the `api_request` JS hook, the `KEEP`
macro, `engine/core/fetch.cpp` (viewer only) and `engine/core/window.cpp` (GLFW, desktop only).
Texture compression for Android is already in the converter (`convert --target android` = ETC2
embedded in the `.emod`).

**App (`~/Developer/168Railway/mobile`, package `railway_168`).** Flutter 3.44 / Dart 3.12, AGP
8.7.2, Kotlin 2.2, minSdk 24, NDK 28.2 installed, only `MainActivity.kt` as native code. PPKA lives
in `lib/pages/ppka/`: `PpkaGamePage` is a full-screen `flutter_inappwebview` 6 WebView on
`pk.168railway.com` (landscape, immersive, renderer-crash rebuild, boot fragment
`wadah=webview`), `PpkaAsetService` caches every `https://168railway.space/pk/<path>` on disk
(`ApplicationSupport/pk-aset/`) and serves it to the WebView through `shouldInterceptRequest`.
Today the 3D view on the phone is the Wasm build of this engine running **inside** that WebView.

**Game (`~/Developer/ppka-wannabe-2`).** The simulation is TypeScript (`src/engine`, 12.7 k lines,
C++ port in `docs/port-cpp.md` NOT started) and the UI is DOM (~30 k lines). The web adapter
`src/tiga-ayana/` (7.9 k lines) drives the engine: per frame `eng_set_state` (tens of KB JSON),
`eng_frame`, then reads for the DOM overlays (`eng_train_screen`, `eng_signal_screen`,
`eng_station_screen`, `eng_camera_json`, avatars). The surveyor tools (`tataAyana`, `objekRelAyana`,
`ukurAyana`, `garisAyana`, `kuasAyana`, `tinggiAyana`, ~4 k lines) use dozens of synchronous picks.

## The decision

**Hybrid: native engine under a transparent WebView.** The engine becomes `libayana.so` drawn into
a Flutter `Texture`; the WebView keeps the UI and the simulation (unchanged TypeScript) and sits
on top with a transparent background. This is the direction Henry chose on 2026-09-14 and it is
the only one that reuses everything: no Dart UI rewrite, no sim port as a prerequisite.

What it fixes compared with today: the Wasm heap (256 MB+), the GPU textures and the terrain no
longer live inside the WebView's renderer process, which is what Android kills under memory
pressure; when the WebView renderer dies the world survives in the native process and only the DOM
is rebuilt; the engine renders at vsync from a native thread instead of a WebGL context capped by
the WebView's quality tier (`hemat`).

What it does not fix: audio decoded to PCM inside the WebView (ppka-wannabe-2
`docs/app-mobile-ppka.md` §7) is a separate problem and stays one.

Not chosen: native Dart UI + QuickJS sim (`docs/flutter-port.md` in ppka-wannabe-2) = 19–30 k lines
of UI rewritten; full native (C++ sim port + Dart UI) = the same plus the port. Both can come later
on top of this plan because the engine side is identical.

## Shape of the code

```
168Railway app (Flutter)
├─ PpkaGamePage (exists)                 WebView, transparent, on top: DOM UI + sim TS
│     └─ callHandler('ayana', batch)     one message per frame (JS → Dart), reply = snapshot
├─ AyanaView (new Dart widget)            Texture(textureId) under the WebView, GestureDetector
│     └─ dart:ffi → libayana.so           eng_* through a command queue
└─ plugin `ayana` (new, lives in THIS repo at flutter/ayana/, path dependency)
      ├─ android/src/main/kotlin          TextureRegistry → SurfaceTexture → Surface → JNI once
      ├─ android/CMakeLists.txt           add_subdirectory(engine root) + engine/api/android/*.cpp
      └─ lib/ayana.dart                   FFI bindings, asset request listener, snapshot types
```

`engine/api/android/ayana_android.cpp` (new): owns the EGL display/context (GLES 3.0, window surface
on the `ANativeWindow` from the Java `Surface`) and a **render thread** that is the only thread ever
calling `eng_*`. Every host call goes through a queue: fire-and-forget for writes (`eng_set_state`,
`eng_pointer`, …), blocking for reads (`eng_pick`, `eng_train_screen`, …); the thread services the
queue between frames and while waiting for vsync, so a blocking read answers within one frame.
`request(kind, path)` (the asset hook) pushes into an outgoing queue that Dart drains through a
`NativeCallable.listener`. No JNI apart from `ANativeWindow_fromSurface`.

**Asset flow on Android** (the host fetches, as on the web): terrain index + DEM Terrarium PNGs +
satellite tiles from `tiles.168railway.com` decoded by Android (`BitmapFactory` in the plugin, or the
Dart `image` package) → `eng_terrain_tile_rgba`; city JSON, `model.json` and models through
`PpkaAsetService` (same disk cache the WebView uses). Models: **`convert --target android`** `.emod`
with ETC2 textures embedded, uploaded next to the web ones (`ayana/models-android/`) — no runtime
KTX2 transcoder needed. Cost: ~10× the download of the KTX2 twins, once, cached on disk
(Mojokerto ≈ 50 MB instead of 5). A basis-universal transcoder inside the plugin (host side, like
`ktx2.js` is host side on the web) is the later optimisation, not a prerequisite.

**Sim ↔ engine over the WebView bridge.** `callHandler` is asynchronous, so the TypeScript side
gets a second transport next to the Wasm `Module`: a **snapshot proxy**. Every write `_eng_*`
appends to the frame's batch; the batch is sent once per frame with the state JSON; the reply is
one JSON with everything the overlays read (`camera`, `trains`, `signals`, `stations`, `avatars`,
`stats`), and the read functions return it synchronously from the last reply (one frame late,
which the DOM labels cannot tell). Rare reads with a result the caller needs now (`eng_pick` on a
tap, `eng_pick_ground`) become `await`. The surveyor tools are **not supported in the app in v1**
(mobile players do not edit maps; they keep working on web/desktop), which shrinks the transport
to ~40 player-facing functions. Touch: the transparent `#canvas-wrap` div keeps receiving the
pointer events `sentuhAyana.ts` already handles; they go out in the batch as `eng_pointer` /
`eng_orbit` / `eng_zoom`.

Boot param `wadah=flutter` (next to `webview`) tells the game to use the proxy transport and to
skip loading `ayana.wasm`.

## Milestones (one step, one visible result)

| # | Work | Visible result |
|---|---|---|
| **M1** | Plugin skeleton: `flutter/ayana/` with `android/CMakeLists.txt` building the engine for arm64 (`ENG_GL_ES`, drop `window.cpp` / `sim_process.cpp` on Android, `-Werror` clean under NDK clang), `ayana_android.cpp` with EGL + render thread + queue, Kotlin `TextureRegistry` glue, `eng_init`/`eng_resize`/`eng_frame`. Font and the `ENG_SOURCE_DIR` path assumptions (`asset_catalog.cpp`, `text.cpp`) get an "bytes from the host" variant. Dev entry in the app: Profil → Aset PPKA → "Uji mesin 3D". | The engine's sky gradient + an `.emod` model (cc203) spinning in a Flutter `Texture` on Henry's phone, fps in a corner. |
| **M2** | World load from Dart: `eng_load_world` with a bundled Mojokerto save + summary + `model.json`; asset requests answered from `PpkaAsetService` / tile server; Android-target `.emod` uploaded; `GestureDetector` → orbit/zoom/compass. | Mojokerto terrain, rails, signals, station and trees on the phone, orbit by touch. No trains yet. |
| **M3** | The bridge: `wadah=flutter`, snapshot proxy in `src/tiga-ayana/` (transport interface: Wasm `Module` vs Flutter), `AyanaView` under the existing `PpkaGamePage` with `transparentBackground`, per-frame batch through `callHandler`, replies as snapshots. Measure: ms per round trip, batch size, fps at 60 vs 30 Hz state. | The full PPKA meja layan on the phone: trains move in the native 3D under the DOM HUD, tap a signal → route, camera modes (kabin/samping/…), labels and bubbles anchored. |
| **M4** | Hardening: lifecycle (pause/resume, EGL context loss, `onTrimMemory` → `eng_set_quality`), WebView crash rebuild keeping the world, DPR/quality tiers from the device, corridor prefetch of `.emod` when 3D is switched on, memory telemetry, disk cap on `pk-aset/`. | A 30-minute dinas on a 4 GB phone without a kill; Play internal test build. |
| later | State extrapolation in the engine (vehicles carry `seg/s` + speed) so the state can go at 30 Hz; KTX2 transcoder in the plugin; iOS (Metal RHI, separate plan). | |

The Mac stays the fast loop: everything in `engine/api/android/` except EGL is portable, and the
snapshot proxy is testable in a desktop browser against the Wasm build (the proxy can wrap the
Wasm module too, which is how M3 is developed before touching the phone).

## Risks and what to check first

* **Disk.** The Mac has ~3.9 GB free. An Android emulator image alone is 2+ GB → **a real phone
  over USB is required** (none connected on 2026-09-18). NDK object files for the engine are small
  (tens of MB); Gradle caches for the app already exist.
* **CMake.** The engine asks for CMake ≥ 3.28; the Android SDK ships 3.22.1 and Homebrew has 4.4.
  Nothing in the engine needs 3.28 (`CMAKE_CXX_SCAN_FOR_MODULES OFF` is harmless on older versions,
  presets are Mac-only), so the plugin's CMake either points Gradle at the Homebrew binary
  (`cmake.path`) or the minimum is lowered to 3.22 — decide at M1.
* **Bridge latency** is the one real unknown (M3 measures it). Fallback if `callHandler` at 60 Hz
  is too slow: state at 30 Hz + engine extrapolation, or a Kotlin `@JavascriptInterface` on the
  WebView calling the .so directly (synchronous, bypasses Dart) if `flutter_inappwebview` exposes
  its `WebView` instance to another plugin.
* **Single-threaded ABI.** `eng_*` assumes one caller; the queue in `ayana_android.cpp` is the only
  door. Dart code never calls an `eng_*` symbol directly.
* **Third-party code.** The engine rule stays: none at runtime in the engine. The KTX2 transcoder,
  if ever, is host code in the plugin, like `ktx2.js` is host code on the web.
* **Two repos move together.** M2 touches ppka-wannabe-2 assets (Android `.emod` upload), M3 its
  `src/tiga-ayana/` (transport interface) and `src/bootParam.ts` (`wadah=flutter`); the app repo
  gets the plugin path dependency and `AyanaView`. Commit each repo by explicit paths (other sessions
  edit them concurrently).

# Flutter / Android plan — the engine inside the 168Railway app

Status: **M1 done on the Mac, 2026-09-18** (everything but the pixels — no phone was available, and no
emulator: the disk cannot hold one). M2 onwards is still plan. Each milestone ends with something visible
on a real phone; see "M1: what is proven" below for exactly where the line falls.

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
| **M1** ✅ | Plugin skeleton: `flutter/ayana/` with `android/CMakeLists.txt` building the engine for arm64 (`ENG_GL_ES`, drop `window.cpp` / `sim_process.cpp` on Android, `-Werror` clean under NDK clang), `ayana_android.cpp` with EGL + render thread + queue, Kotlin `TextureRegistry` glue, `eng_init`/`eng_resize`/`eng_frame`. Font and the `ENG_SOURCE_DIR` path assumptions (`asset_catalog.cpp`, `text.cpp`) get an "bytes from the host" variant. Dev entry in the app: Profil → Aset PPKA → "Uji mesin 3D". | The engine's sky gradient + an `.emod` model (cc203) spinning in a Flutter `Texture` on Henry's phone, fps in a corner. |
| **M2** | World load from Dart: `eng_load_world` with a bundled Mojokerto save + summary + `model.json`; asset requests answered from `PpkaAsetService` / tile server; Android-target `.emod` uploaded; `GestureDetector` → orbit/zoom/compass. | Mojokerto terrain, rails, signals, station and trees on the phone, orbit by touch. No trains yet. |
| **M3** | The bridge: `wadah=flutter`, snapshot proxy in `src/tiga-ayana/` (transport interface: Wasm `Module` vs Flutter), `AyanaView` under the existing `PpkaGamePage` with `transparentBackground`, per-frame batch through `callHandler`, replies as snapshots. Measure: ms per round trip, batch size, fps at 60 vs 30 Hz state. | The full PPKA meja layan on the phone: trains move in the native 3D under the DOM HUD, tap a signal → route, camera modes (kabin/samping/…), labels and bubbles anchored. |
| **M4** | Hardening: lifecycle (pause/resume, EGL context loss, `onTrimMemory` → `eng_set_quality`), WebView crash rebuild keeping the world, DPR/quality tiers from the device, corridor prefetch of `.emod` when 3D is switched on, memory telemetry, disk cap on `pk-aset/`. | A 30-minute dinas on a 4 GB phone without a kill; Play internal test build. |
| later | State extrapolation in the engine (vehicles carry `seg/s` + speed) so the state can go at 30 Hz; KTX2 transcoder in the plugin; iOS (Metal RHI, separate plan). | |

## M1: what is proven, and what still waits for a phone

Proven on the Mac, reproducible:

```
cmake --preset android-arm64 && cmake --build --preset android-arm64
  -> build/android-arm64/libayana.so   (39 MB with debug info, 2.4 MB stripped)
llvm-nm -D --defined-only libayana.so | grep ' T eng_' | wc -l   -> 104
llvm-nm -u libayana.so | grep -iE 'glfw|fork'                    -> nothing
ctest --preset mac-debug                                         -> 15/15 (test_host_queue is new)
cd ~/Developer/168Railway/mobile && flutter build apk --debug --target-platform android-arm64
unzip -l build/app/outputs/flutter-apk/app-debug.apk | grep libayana
  -> lib/arm64-v8a/libayana.so + packages/ayana/assets/{cc203.emod,font.efnt}
flutter analyze (plugin, and the two touched app files)           -> clean
```

What that means concretely:

* `cmake_minimum_required` is **3.22** now (the CMake the Android SDK ships), and the root CMakeLists
  uses `ENG_ROOT` instead of `CMAKE_SOURCE_DIR` because Gradle `add_subdirectory()`s it. Nothing needed
  3.28; `CMakePresets.json` v6 only constrains the *running* binary, so the Mac presets are unaffected
  and Gradle needs no `cmake.path` override.
* On Android the engine drops `engine/core/window.cpp` (GLFW) and `engine/sim/sim_process.cpp` (fork),
  and builds one extra target, `ayana` (SHARED) = the `eng_*` ABI + `engine/api/android/ayana_android.cpp`.
  Visibility is hidden by default; `engine/api/eng_export.h` (`ENG_EXPORT`) replaced the per-file `KEEP`
  macro and marks the exports for both Emscripten and the NDK. The NDK's clang produced no new warnings,
  so `-Wall -Wextra -Werror` is untouched.
* `engine/api/host/host_queue.h` is the platform-neutral half and compiles everywhere: `post` for writes,
  `callInt` / `callFloat` / `callString` / `run` for blocking reads, the asset outbox that the engine's
  `request(kind, path)` hook now writes into off Emscripten, a `FramePacer`, and a `shutdown()` that
  releases every blocked caller instead of deadlocking. `tests/test_host_queue` exercises all of it
  (ordering under 4 concurrent caller threads, values back, outbox drain, no deadlock after shutdown).
* The font no longer needs a source tree: `eng_set_font(bytes, len)` installs the `.efnt` and both
  readers (`TextRenderer`, the CPU board-atlas font) go through `engine/render/font_bytes.h`. The plugin
  ships `assets/font.efnt` and hands it over before `eng_init`.
* `eng_request_poll(kind, kindCap, path, pathCap)` is the C-side drain of the outbox; Dart uses the
  `ayana_poll_request` wrapper (polled at 10 Hz — asset requests come in bursts after `eng_load_world`,
  never per frame, so a `NativeCallable.listener` would buy nothing and put a Dart API in the C code).

Still waiting for a phone (nothing of it is testable here):

* that EGL picks a config and `eglMakeCurrent` succeeds on a Flutter `SurfaceTexture`;
* that `eng_init` gets a live GLES 3.0 context and the clear colour reaches the `Texture` widget;
* that `ayana_model_begin` accepts the ETC2 `.emod` on a real GPU.

Note on the milestone's "spinning cc203": without `eng_load_world` the engine draws a clear colour only
(`eng_frame` returns early until `eng_ready`), so the M1 page shows the clear + the fps counter and
confirms the model entered the catalog. Geometry on screen arrives with M2's world load, which is one
`eng_load_world` call away.

### First run when a phone is plugged in

1. `cd ~/Developer/168Railway/mobile && flutter devices` (USB debugging on).
2. `flutter run --debug --target-platform android-arm64` — free ~2 GB first, the Gradle+NDK build needs it.
3. In the app: Profil → **Aset PPKA** → **"Uji mesin 3D"**.
4. Expect: a dark blue-grey `Texture` filling the page, "0 fps" turning into 55–60, and the line
   "model cc203.emod masuk katalog" underneath.
5. `adb logcat -s ayana` shows `attached <w>x<h> dpr=... engine=1`. `engine=0` or `eglCreateWindowSurface
   failed` = the EGL path; `eng_init failed` = the GL context (check `ANDROID_PLATFORM`/GLES 3.0).
6. Rotating/resizing goes through `AyanaPlugin.resize` → `setDefaultBufferSize` + `eng_resize`; leaving
   the page calls `dispose` → `nativeDetach` (blocking, on the render thread) → `nativeStop` → join.

## Shape of the code, as built

```
game-engine-experiment/
├─ engine/api/host/host_queue.{h,cpp}   platform-neutral: render-thread queue + asset outbox + pacer
├─ engine/api/android/ayana_android.cpp EGL + render thread + JNI (Java_com_ayana_engine_AyanaPlugin_*)
│                                        + plain C for Dart FFI (ayana_*)
├─ engine/api/eng_export.h              ENG_EXPORT: what stays visible in libayana.so / the wasm module
├─ engine/render/font_bytes.{h,cpp}     the .efnt from the host instead of from a path
└─ flutter/ayana/                       the plugin (path dependency of the app)
   ├─ android/CMakeLists.txt            add_subdirectory(<repo root>) -> target `ayana`
   ├─ android/src/main/kotlin/.../AyanaPlugin.kt   TextureRegistry -> SurfaceTexture -> Surface -> JNI
   ├─ lib/ayana.dart                    FFI bindings, asset-request stream, AyanaView widget
   └─ assets/{font.efnt, cc203.emod}    cc203 = convert --target android --max-texture 128 (2.6 MB)
```

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

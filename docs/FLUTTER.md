# Flutter / Android plan — the engine inside the 168Railway app

Status: **M1 and M2 run on a real phone, 2026-09-18** (Infinix Note 30, Helio G99 / Mali-G57,
Android 14). M3 is started: the native and Dart halves are in, the page's half is a skeleton.
The Mac is still the only machine here — no emulator, no adb — so everything below says which side
of that line it was proven on. Each milestone ends with something visible
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
| **M2** ✅ | World load from Dart: `eng_load_world` with a bundled Mojokerto save + summary + `model.json`; asset requests answered from `PpkaAsetService` / tile server; Android-target `.emod` uploaded; `GestureDetector` → orbit/zoom/compass. | Mojokerto terrain, rails, signals, station and trees on the phone, orbit by touch. No trains yet. |
| **M3** 🟡 | The bridge: `wadah=flutter`, snapshot proxy in `src/tiga-ayana/` (transport interface: Wasm `Module` vs Flutter), `AyanaView` under the existing `PpkaGamePage` with `transparentBackground`, per-frame batch through `callHandler`, replies as snapshots. Measure: ms per round trip, batch size, fps at 60 vs 30 Hz state. | The full PPKA meja layan on the phone: trains move in the native 3D under the DOM HUD, tap a signal → route, camera modes (kabin/samping/…), labels and bubbles anchored. |
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

## M2: what is proven, and what still waits for a phone

Proven on the Mac:

```
build/mac-release/test_host_world          # the Android host path without Android
  -> tiles 126 (0 failed), models 6, resident 100, patches 60
     34.7 km track, 16 points, 18 signals, rails 329466 tris, 350728 trees; 140 checks, 0 failures
cd flutter/ayana && flutter test            # 19 tests: tile math, index, Terrarium decode, routing
cmake --build --preset android-arm64        # 27 ayana_* wrappers exported from libayana.so
ctest --preset mac-debug                    # 15/15
web/build-models-android.sh pohon-05 sttd-stasiun-mojokerto
  -> stasiun 24.0 MB raw -> 5.0 MB gzipped (penuh, 1024 px) / 11.9 MB -> 3.0 MB (hemat, 512 px)
```

* **`tests/test_host_world`** is the engine-side proof and needs no Android at all: a second thread
  loads Mojokerto through the render-thread queue exactly the way `package:ayana` does over FFI (never
  calling `eng_*` directly) and answers the outbox from `assets/terrain/mojokerto` and
  `build/wasm/models/*.emod`. It asserts `eng_ready()`, an empty `eng_last_error()` and that the stats
  report resident tiles, patches and the rails.
* **The wrappers** (`engine/api/android/ayana_android.cpp`): `load_world` (the four big strings copied
  to the heap — never a stack buffer), `terrain_index`, `terrain_tile`, `terrain_tile_rgba`,
  `terrain_tile_fail`, `city_json`, `model_fail`, `model_textures_unavailable`, `set_state`, `pointer`,
  `compass_click`, `camera_mode`, `set_quality`, `set_sky_time`, `last_error`.
* **The Dart loader** (`flutter/ayana/lib/src/`) mirrors `ppka-wannabe-2/src/tiga-ayana`: the same tile
  proxy (`tiles.168railway.com/terrarium/{z}/{x}/{y}.png`, `/satellite/{z}/{x}/{y}.png`, with the S3 and
  Esri fallbacks — Esri takes **z/y/x** and no extension), the same 6-deep FIFO fetch pool, and the same
  composition arithmetic: a layer's `px` decides everything, `k = log2(px/256)`, so a 512 px layer is
  four tiles at `zoom + 1` drawn into one image with the missing cells left `#606060`. The z in the
  request path is ignored for `sat/<i>` — the index layer's own zoom is authoritative. DEM goes to the
  engine as **raw Terrarium RGBA**; the engine does `R*256 + G + B/256 - 32768` itself.
* **The plugin stays app-agnostic**: `AyanaWorldLoader(resolve: ...)` takes a callback
  `(kind, path, assetPath) -> bytes?`. The app wires `PpkaAsetService.ambilAtauUnduh` into it, so the
  engine's models come out of the very same disk cache the WebView uses.
* **Model files on Android**: `web/build-models-android.sh` writes two variants, both
  `convert --target android` with ETC2 embedded and both **gzipped on disk** (Cloudflare will not
  compress `application/octet-stream`, so the object has to be stored compressed with
  `Content-Encoding: gzip` — see `docs/TEXTURE-FORMAT.md`):
  | directory | flag | atlas | station model |
  |---|---|---|---|
  | `ayana/models-android` (`AyanaModelVariant.penuh`, the default) | `--max-texture 512` | 1024 px | 24.0 MB -> **5.0 MB** |
  | `ayana/models-android-hemat` (`AyanaModelVariant.hemat`) | `--max-texture 256` | 512 px | 11.9 MB -> **3.0 MB** |
  512 px alone is visibly soft in the kabin/samping cameras, which is why `penuh` is the default and
  `hemat` is what a low-memory device gets (the app will pick it from device memory, like the web
  quality tiers). The real fix for "sharp near, cheap far" is **mip streaming** — uploading only the
  mips a model's on-screen size needs and filling in the finer ones as it comes closer — and that is a
  later milestone, not a prerequisite. The Dart loader inflates gzip itself when the HTTP stack did not,
  so a mis-tagged object still works.
* **The `.emod` reader is bounded now**: every u32 count must fit the bytes left in the file
  (`engine/asset/emod.cpp`, `Reader::count`), because these files now arrive over a phone network.
  `tests/test_gltf_emod` sweeps every u32 of a real file with `0xFFFFFFF0` and asserts nothing parses
  into an absurd model.

Still waiting for a phone: everything that needs a GPU and a network on the device — that EGL binds,
that the tile fetches and the ETC2 uploads keep up, and what the world actually looks like. The engine
side of all of it is exercised on the Mac by `test_host_world`.

## M2 on the phone: what it actually did

Infinix Note 30 (Helio G99, Mali-G57), Android 14, 540x899 Texture:

| | |
|---|---|
| world built | 1.7 s |
| decor build | **11 213 ms** — 1 hiasan, 59 302 trees, render thread frozen, Texture frozen |
| steady state | 62 fps |

Three things came out of that and are fixed:

* **The UI isolate must never wait on the render thread.** `ayana_ready` / `ayana_last_error` /
  `ayana_stats` were blocking calls polled at 10 Hz; during the 9 s decor build the Dart isolate hung
  with it and Android offered to kill the app. They are per-frame snapshots now, and only
  `ayana_load_world` still waits.
* **`SurfaceTexture` ignores the swap interval**, so the render thread is paced at 60 fps by the host's
  `FramePacer` instead of by vsync.
* **The decor build is sliced** (`eng_set_decor_budget`, below).

### Decor in slices

`eng_set_decor_budget(ms)` (0 = all at once, which is what the desktop tools and the tests want; the
plugin installs 8 ms) spreads the tree work over frames, and a late vegetation model no longer throws
the forest away. Measured by `tests/test_host_world`, which loads Mojokerto twice and times every slice
of the render thread:

```
budget 0 ms: worst slice 159 ms (queue 157, draw 6), decor 162 ms, 350728 trees
budget 8 ms: worst slice  49 ms (queue  48, draw 10), decor  71 ms, 350728 trees (streaming)
```

Same forest, a third of the stall, on a Mac. What is left in that 48 ms is **`eng_model_begin`** — the
`.emod` parse plus a one-shot GPU upload — which is not sliced yet and is the obvious next step: on the
phone that one is the 24 MB ETC2 station, and it is the largest remaining freeze.

`eng_set_quality` and `eng_set_decor_budget` work **before `eng_init`** now (the value is remembered),
because the tier has to be in force when the world is built — setting it afterwards does nothing until
the next build. `AyanaView(quality: …)` installs both before the engine starts.

## M3: where it stands

The transport is "one message per frame in, one snapshot out". Three of its four pieces exist.

**Native + Dart (done, `flutter test` 23/23, 34 `ayana_*` exported):**
* `ayana_snapshot_enable` / `ayana_snapshot` / `ayana_snapshot_frame` — the render thread builds one
  JSON string after each frame (`{frame, ready, camera, trains, signals, stations, stats}`); Dart copies
  it out of a mutex and never waits. Off until a page asks for it.
* `ayana_cmd(name, a, b, c, str)` + `ayana_pointer4` — one posted door for the ~25 player-facing
  commands instead of forty wrappers. Unknown names are ignored, so an older engine cannot break a
  newer page.
* `AyanaBridge` (`flutter/ayana/lib/src/bridge.dart`) turns a batch into those calls and returns the
  snapshot, with `AyanaLatency` recording p50/p95.

**The page (`ppka-wannabe-2/src/tiga-ayana/mesin/`, done):**
* `mesin.ts` — the `Mesin` interface. Deliberately **not** the Emscripten module shape: no heap
  pointers, strings stay strings. The editing ABI is not in it, because the surveyor tools are not
  supported in the app in v1 and they are exactly what needs the dozens of synchronous reads a
  snapshot cannot answer.
* `wasm.ts` — `MesinWasm`, today's module; all the heap handling stops in this one file.
* `flutter.ts` — `MesinFlutter`, the snapshot proxy. Writes pile into a per-frame batch, `frame()`
  sends it once, reads answer from the previous reply, `pick`/`pickGround` become async, and a failed
  send puts the commands back in the queue instead of dropping them.
* `loopback.ts` + `uji-proxy.mts` — `npx tsx src/tiga-ayana/mesin/uji-proxy.mts` → **14 checks, 0
  failures** (one send per frame, order kept, snapshot one frame late, async pick, nothing lost on a
  broken bridge, `load_world` carried in the batch).
* `bootParam.ts` takes `wadah=flutter`; `konstInti.diFlutter()`.

**`duniaAyana` / `hudAyana` moved onto it.** Nothing a *player* touches calls the Emscripten module
any more: init, world load, `set_panel`, per-frame `set_state` / `frame`, the whole camera and input
surface, every setting, and the reads (`stats`, `camera_json`, `train_screen`, `signal_screen`,
`station_screen`) which now come from the snapshot. `hudAyana` draws its signal plates from the
snapshot's `signals` array and the station boards from `stations` instead of calling `eng_project`
per element; `gelembungAyana` and `mejaApungAyana` read `camera` the same way. What is still on the
raw module is exactly what is wasm-only by design: the surveyor tools, the asset serving, `eng_project`
(it writes into the heap) and the avatar ABI — and `wadah=flutter` does not wire any of them.

**Proven in a desktop browser, no phone** (`npx vite --port 5199`, then the Playwright script
`playtest/_cek-mesin-transport.mjs`, which is local like the other `_cek-*`): the world loads, then the
snapshot proxy is wrapped **around the live wasm module** (loopback) and a few seconds are allowed to
pass.

```
WASM   jenis wasm,    siap, 60 fps, 6 sinyal tampak, pick(468,346) = signal:t765
PROXY  jenis flutter, siap, 60 fps, 6 sinyal tampak, pick(468,346) = signal:t765
       jembatan: 240 batch / 240 perintah, round trip 1.5 ms (max 10.2 ms)
world built in 71 ms: 34.7 km track, 16 points, 18 signals ...     errors: tidak ada
```

The existing smoke tests are clean on the wasm path after the move: `_cek-ayana.mjs` (27 MB loaded,
click a signal → route `t765`, camera glide 6948 m, 346 k trees, 60 fps), `_cek-ayana-label.mjs`, and
`_cek-hud-ayana.mjs` (2 anchored plates, plate click → route `t768`, station bubble → fly-to, floating
desk).

**The app (done, opt-in):** a dev switch "Mesin 3D natif" on Aset PPKA (default off) makes
`PpkaGamePage` send `wadah=flutter`, put `AyanaView` under a **transparent** WebView, and register the
`ayana` JavaScript handler. The page sends `load_world` in the batch (it owns the save); `AyanaBridge`
hands it to the engine and starts an `AyanaWorldLoader` whose resolver is `PpkaAsetService` — the same
disk cache the WebView uses. With the switch off nothing about a normal dinas changes.

**Still awaiting a phone:** that the transparent WebView really composites over the `Texture`, what
`callHandler` costs at 60 Hz on the device (the Mac loopback is 1.5 ms, which is a floor, not a
prediction), and whether 30 Hz state + engine extrapolation turns out to be needed. The p50/p95 of the
round trip is logged every 300 frames under the `ayana` tag.

### First run when a phone is plugged in

1. `cd ~/Developer/168Railway/mobile && flutter devices` (USB debugging on).
2. `flutter run --debug --target-platform android-arm64` — free ~2 GB first, the Gradle+NDK build needs it.
3. In the app: Profil → **Aset PPKA** → **"Uji mesin 3D"**.
4. Expect: a dark blue-grey `Texture` filling the page, "0 fps" turning into 55–60, and the line
   "mesin hidup, cc203.emod masuk katalog" underneath.
4b. Tap **"Muat Mojokerto"**. The status line counts assets answered / failed / in flight and flips to
   `siap ya`; the view fills with terrain, rails, signals, the station and trees, and one-finger drag
   orbits, two fingers zoom. **This needs the Android `.emod` files to be on R2 first** (below):
   without them every model fails its slot and the world is rails and terrain with boxes for decor.
5. `adb logcat -s ayana` shows `attached <w>x<h> dpr=... engine=1`. `engine=0` or `eglCreateWindowSurface
   failed` = the EGL path; `eng_init failed` = the GL context (check `ANDROID_PLATFORM`/GLES 3.0).
6. Mojokerto's Android models are on R2 already (`ayana/models-android[-hemat]`, gzip, uploaded
   2026-09-18). For another map, Henry must upload the two model directories (`web/build-models-android.sh --all`
   then the `aws s3 sync` lines the script prints, or two entries in ppka-wannabe-2
   `tools/unggah-r2.mjs` next to `ayana`, **with `Content-Encoding: gzip`**). Nothing else is needed:
   the terrain index is bundled in the plugin and the tiles come from `tiles.168railway.com`.
7. Rotating/resizing goes through `AyanaPlugin.resize` → `setDefaultBufferSize` + `eng_resize`; leaving
   the page calls `dispose` → `nativeDetach` (blocking, on the render thread) → `nativeStop` → join.

## Shape of the code, as built

```
game-engine-experiment/
├─ engine/api/host/host_queue.{h,cpp}   platform-neutral: render-thread queue + asset outbox + pacer
├─ engine/api/android/ayana_android.cpp EGL + render thread + JNI (Java_com_ayana_engine_AyanaPlugin_*)
│                                        + plain C for Dart FFI (ayana_*)
├─ engine/api/eng_export.h              ENG_EXPORT: what stays visible in libayana.so / the wasm module
├─ engine/render/font_bytes.{h,cpp}     the .efnt from the host instead of from a path
├─ tests/test_host_world.cpp            the whole host path on Mojokerto, no Android needed
├─ web/build-models-android.sh          the two gzipped ETC2 model builds
└─ flutter/ayana/                       the plugin (path dependency of the app)
   ├─ android/CMakeLists.txt            add_subdirectory(<repo root>) -> target `ayana`
   ├─ android/src/main/kotlin/.../AyanaPlugin.kt   TextureRegistry -> SurfaceTexture -> Surface -> JNI
   ├─ lib/ayana.dart                    FFI bindings, asset-request stream, AyanaView widget
   ├─ lib/src/terrain_index.dart        index.json -> the sat layers' zoom/px (authoritative)
   ├─ lib/src/tile_urls.dart            request path -> URLs, the 2x2 composition, model variants
   ├─ lib/src/tile_images.dart          PNG -> RGBA8 and the composition, dart:ui only
   ├─ lib/src/tile_http.dart            6 in flight, 20 s deadline, primary + fallback URL
   ├─ lib/src/world_loader.dart         AyanaWorldLoader: routes every request, reports progress
   ├─ test/ayana_test.dart              19 tests, no device
   └─ assets/                           font.efnt, cc203.emod, mojokerto/{world,summary,model,index}.json
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

# Texture and model transport for Android (and later iOS)

*What format the phone should download, decided on measurements from our own models, September 2026.*

## Recommendation

**For M2, ship exactly what `docs/FLUTTER.md` already plans — `convert --target android` `.emod`
with ETC2 embedded — but with two cheap changes: cap textures with `--max-texture 256`
(= 512 px per atlas side) for the phone build, and upload the files **pre-compressed with
gzip** (`.emod.gz`, `Content-Encoding: gzip`) instead of raw.** Those two changes together take a
coach from 33.7 MB to **2.2 MB** on the wire and from 28 MB to 1.7 MB of GPU memory, which is
*smaller* than the KTX2 files the web downloads today — with zero new code in the engine, no
third-party decoder in the app, and no new crash surface. A host-side Basis Universal transcoder
(KTX2 ETC1S, like `ktx2.js` on the web) stays the **later** optimisation it is called in
`docs/FLUTTER.md`: it buys quality per byte, not size, and it costs ~250 KB of APK, a new
attack surface, and per-model transcode time on a slow CPU. ASTC is *not* worth it for Android
now (not guaranteed on GLES 3.0, needs new engine code, and encodes bigger than ETC2 on the
wire), but it becomes the obvious format the day we do iOS/Metal — and the clean way to get
there is UASTC KTX2 in the host transcoder, not a second ASTC `.emod` build.

One caveat that must not be skipped: **Cloudflare will not compress `.emod` for us**. Its
automatic gzip/brotli only applies to known text content types; `application/octet-stream`
is served as-is. Option (b) in the brief ("let Cloudflare inflate it for free") therefore only
works if the uploader stores the object already compressed with the right
`Content-Encoding` header — see *What changes in the pipeline*.

---

## The measured table

Three real models from `ppka-wannabe-2/public/model3d/`, converted with
`build/mac-release/convert --target android`, then `gzip -9` and `brotli -q 11`.
All numbers are bytes of the **whole `.emod`** (geometry + textures) unless the row says otherwise.

| Model | variant | texture cap | raw | gzip ‑9 | brotli ‑11 | gzip ratio |
|---|---|---|---|---|---|---|
| **pilot-cc203** (loco, 40 k tris, 14 atlases) | `--max-texture 2048` (today's default) | 1024 px | 9 110 685 | 2 694 835 | 1 811 546 | 0.30 |
| | `--max-texture 512` | 1024 px | 9 110 685 | 2 694 834 | 1 811 546 | 0.30 |
| | **`--max-texture 256`** | **512 px** | **3 867 693** | **1 363 228** | **818 800** | 0.35 |
| | geometry only (`--textures external`) | — | 2 118 585 | 744 248 | 335 538 | 0.35 |
| | KTX2 twin (ETC1S+zstd, what the web fetches) | 1024 px | 1 117 000 | 1 065 740 | 997 696 | 0.95 |
| **anr-k1-taksaka-badan** (coach, 63 k tris, 10 atlases) | `--max-texture 2048` | 2048 px | 33 714 290 | 5 125 617 | 3 178 495 | 0.15 |
| | `--max-texture 512` | 1024 px | 12 742 690 | 2 856 912 | 1 785 348 | 0.22 |
| | **`--max-texture 256`** | **512 px** | **7 499 730** | **2 199 507** | **1 378 587** | 0.29 |
| | geometry only | — | 5 751 150 | 1 823 529 | 1 117 097 | 0.32 |
| | KTX2 twin | 2048 px | 1 661 104 | 1 634 285 | 1 622 244 | 0.98 |
| **pilot-bn-peron-stasiun** (trackside, 2 small atlases) | any cap (textures already ≤ 512 px) | 128–512 px | 46 185 | 13 759 | 11 355 | 0.30 |
| | geometry only | — | 2 269 | 654 | 545 | 0.29 |
| | KTX2 twin | — | 17 536 | 13 193 | 12 723 | 0.75 |

Reading it:

* **Raw ETC2 in `.emod` compresses far better than the folklore says.** The "ETC2 is
  incompressible, that's why the web uses KTX2" line in `README.md` is not what these files do:
  gzip takes 70–85 % off. Our liveries are large flat colour fields, and the block data repeats.
* **`--max-texture` is the single biggest lever**, bigger than the choice of container.
  Careful: the flag is a *budget*, not a cap — `convert` allows sides up to `2 × max-texture`.
  So `--max-texture 2048`, `1024` and `512` all produce identical files for these models;
  you have to go to **`--max-texture 256`** before anything shrinks. (For the coach, `512`
  does bite, because its atlases are 2048².)
* **gzip vs brotli:** brotli is another 30–40 % smaller, and Android's HTTP stack (OkHttp /
  Cronet) accepts `br` on https. Worth doing if the uploader can produce it; gzip alone
  already gets us where we need to be, and gzip decodes ~2× faster.
* **Compressed ETC2 lands in the same size class as the KTX2 twin.** Coach: 2.2 MB
  (gzip, 512 px) vs 1.66 MB KTX2 at 2048 px. Loco: 1.36 MB vs 1.12 MB. That is the whole
  argument for not shipping a transcoder yet. The KTX2 files themselves are already zstd
  inside, so gzipping them again gains nothing (ratio 0.95–0.98) — do not waste CPU on that.

### Geometry on the wire

Yes, compress it. The geometry-only `.emod` is our own uncompressed binary (float vertex
attributes, 32-bit indices) and gzips to **0.29–0.35** of its size, brotli to **0.16–0.32**.
For the loco that is 2.12 MB → 744 KB → 336 KB. Since the same `Content-Encoding` trick covers
the whole file, geometry comes along for free — no separate work.

### Caveat found while measuring

For two of the three models, the KTX2 twin was **rejected** by the converter:

```
KTX2 twin .../ktx2/pilot-cc203.glb has 13 images, GLB 14 (ignored)
KTX2 twin .../ktx2/anr-k1-taksaka-badan.glb has 9 images, GLB 10 (ignored)
```

The twins are stale relative to the GLBs (one image added since). The converter then silently
falls back to `tools/texcomp`'s own ETC1 encoder. So the numbers above are *our* encoder, not
Basis — which partly explains why they gzip so well (our encoder's output is more repetitive)
and means the on-screen quality of `--target android` today is a little below the web's.
**Regenerating the KTX2 twins is worth doing regardless of which option we pick**, because
`--target web` and `--target desktop` silently degrade the same way.

---

## GPU memory

Compressed textures stay compressed in VRAM — the GPU samples the blocks directly. So
"bytes on the GPU" is just bytes/pixel × pixels, summed over the mip chain (mips add 33 %).

| Format | bytes per pixel | relative to ETC2 RGB | on our GLES 3.0 target |
|---|---|---|---|
| RGBA8 (uncompressed) | 4.00 | 8× | always works, never ship it |
| **ETC2 RGB** | **0.50** | 1× | guaranteed on every GLES 3.0 device |
| **ETC2 RGBA + EAC** | **1.00** | 2× | guaranteed |
| BC1 / BC3 (desktop) | 0.50 / 1.00 | 1× / 2× | desktop only, not on phones |
| ASTC 4×4 | 1.00 | 2× | not guaranteed on GLES 3.0 |
| ASTC 6×6 | 0.44 | 0.89× | not guaranteed |
| ASTC 8×8 | 0.25 | 0.5× | not guaranteed |

Per model (measured texture payload; mip chains included):

| Model | textures @ 2048 px | @ 1024 px | @ 512 px (`--max-texture 256`) | ASTC 8×8 @ 512 px (would be) |
|---|---|---|---|---|
| pilot-cc203 | — (sources are 1024) | 7.0 MB | **1.7 MB** | 0.9 MB |
| anr-k1-taksaka-badan | 28.0 MB | 7.0 MB | **1.7 MB** | 0.9 MB |
| pilot-bn-peron-stasiun | 0.04 MB | 0.04 MB | 0.04 MB | 0.02 MB |

This is the number that actually matters on a 3–4 GB phone. `docs/app-mobile-ppka.md` §7 is
explicit that what kills the tab is resident RAM, not downloads, and that a WebView's practical
budget is ≈ 500 MB. A Kroya-sized scene with ~30 distinct vehicles at 2048 px would be
**~840 MB of texture alone** — instant death. At 512 px it is **~50 MB**. That single decision
matters more than every container question on this page put together.

Note that dropping to 512 px is not a quality catastrophe here: the converter's own comment
says 512-px *atlases* were the point where loco numbers stopped being readable, and
`--max-texture 256` gives exactly 512 px sides for a 4096² atlas. If a specific hero loco looks
wrong, raise the cap for that one model — the flag is per-file.

---

## CPU decode cost on a phone

| Path | What the CPU does per model | Measured / estimated |
|---|---|---|
| **raw ETC2 `.emod`** | nothing — blocks go straight to `glCompressedTexImage2D` | **0 ms** |
| **gzip'd `.emod`** | inflate, in the HTTP stack, streaming, in native code | 12.7 MB inflated in ≈ 10 ms on this Mac → budget **50–100 ms** on a mid-range phone, overlapped with the download |
| **brotli'd `.emod`** | brotli decode | ≈ 2× gzip → **100–200 ms** on a phone |
| **KTX2 ETC1S → ETC2** | Basis transcode, per mip level, per atlas | `README.md` measures **83 ms** for the whole CC203 (five 4096² atlases) in headless Chromium on desktop; ARM phone cores are roughly 3–5× slower here → **250–400 ms per loco**, on the CPU, per cold load |
| **KTX2 UASTC → ETC2** | re-encode (not a copy) — UASTC is a subset of BC7/ASTC, *not* of ETC2 | several times slower than ETC1S on the ETC2 path; only cheap when the target is ASTC or BC7 |

Two things follow. First, gzip inflate happens inside the download, on a thread we do not own,
and costs nothing we can perceive. Transcoding happens after the download, on our thread,
and is the kind of thing that stutters when a train spawns. Second, **UASTC is the wrong shape
for Android**: its whole advantage is that it transcodes almost for free to ASTC and BC7,
which are exactly the two formats our GLES 3.0 target does not have. UASTC becomes interesting
on iOS/Metal, not before.

(The 83 ms figure is from the project's own headless-Chromium run, already in `README.md`;
I did not re-run Playwright for this document. The phone multiplier is an estimate, marked as
such — the honest version is "somewhere between 200 ms and half a second per loco".)

---

## Coverage, and what each option means for iOS

**ETC2/EAC** is part of the OpenGL ES 3.0 core specification. Every device that can run our
GLES 3.0 build supports it, by definition — there is no fallback path to write and no device
matrix to maintain. With `minSdk 24` this is 100 % of our target.

**ASTC** is mandatory only from **GLES 3.2**; on 3.0/3.1 it is the optional extension
`KHR_texture_compression_astc_ldr`. In practice on 2026 Android:

* Present on essentially all **Mali** (Midgard T6xx onward), **Adreno 4xx and newer**, and
  **PowerVR Rogue 6XT and newer** — which is the large majority of live devices.
* **Missing on Adreno 3xx.** This is the part that matters for us: Adreno 3xx includes the
  **Adreno 308 in Snapdragon 425/430**, which shipped in cheap handsets well past 2018 and is
  still alive in Indonesia. Various older/odd Vivante and entry Mali-400 parts are GLES 2 and
  out of scope anyway.
* Google's own Play asset-delivery guidance still names ETC2 as the safe baseline and treats
  ASTC as the format you *add* with a fallback — i.e. shipping ASTC means shipping **two**
  texture builds, not one.

So ASTC on Android costs: a new `TexFormat` and GL enum path in `engine/rhi/rhi_gl.cpp` and
`engine/asset/model.h` (they know `ETC2_RGB, ETC2_RGBA, BC1, BC3, BC7` and nothing else today),
an ASTC encoder in `tools/`, a second set of files in R2, and a runtime probe with an ETC2
fallback — to save maybe 40 % of GPU memory over ETC2 at 6×6, while being *larger* on the wire
(ASTC blocks are high-entropy and gzip poorly, typically only 10–30 % off versus the 70 % we
measured on ETC2). Bad trade today.

**iOS/Metal later.** Metal has no ETC2 on Apple Silicon-class GPUs in practice — ASTC is *the*
format there, and it is universal on every iOS device we would support. So iOS forces the ASTC
work eventually, whichever option we pick now. The cheapest route at that point is
**KTX2 UASTC decoded host-side in the plugin** (UASTC → ASTC is close to a copy, so the transcode
cost objection above disappears on iOS), with the engine gaining an `ASTC_4x4`/`ASTC_6x6`
format enum and nothing more. Choosing raw ETC2 `.emod` now does not block that: the `.emod`
image record already stores *variants in preference order*, so an ASTC variant can simply be
appended for the iOS build, and old players keep loading the ETC2 one.

---

## Safety

### Our own parser, on a truncated or corrupt download

`engine/asset/emod.cpp` is in reasonable shape. Its `Reader` bounds-checks **every** read
(`get<T>()` and `bytes()` both compare against `buf.size()` and set `ok = false`), mip levels are
validated against the format's expected size (`n != texLevelBytes(...)` → `"bad mip level"`),
the format byte is range-checked, and the function ends with `if (!r.ok) { err = "truncated
EMOD"; ...}`. A truncated file fails cleanly. **There is no out-of-bounds read here.**

There is one real weakness: **unbounded `resize()` on attacker-visible counts**. These lines
allocate before any byte of payload has been proven to exist —

```cpp
m.meshes.resize(r.get<uint32_t>());
p.vertices.resize(r.get<uint32_t>());   // Vertex is dozens of bytes
p.indices.resize(r.get<uint32_t>());
m.nodes.resize(r.get<uint32_t>());
n.children.resize(r.get<uint32_t>());
m.roots.resize(r.get<uint32_t>());
```

A file whose bytes got scrambled in transit (or truncated right before a count) can name
4 billion vertices and make the process try to allocate tens of GB. On a 3–4 GB phone the OS
kills the app — a hang or a crash, not a security hole, but exactly the failure mode
`docs/app-mobile-ppka.md` §7 is about. Note that the newer sections already got this right:
animations cap at `1 << 24` samples, skins at 255 joints. **Fix: one guard in `Reader` — refuse
a count whose element size × count exceeds the bytes remaining in the buffer.** Ten lines,
engine-side, no third-party code, and it makes every count in the file safe at once. This
should be done for M2 whatever else we choose; it is the cheapest robustness we will ever buy.

### What a host-side transcoder would add

A Basis transcoder in the Flutter plugin is *new parsing of a binary format in C++, in the app
process*. `basis_universal`'s transcoder is well-audited and fuzzed by now, and our assets come
from our own R2 bucket — but the bytes still arrive over a network that a hostile Wi-Fi can
interfere with, and a corrupt KTX2 goes into a decoder, not just into a bounds-checked reader.
It also adds a JNI/FFI boundary carrying pointer + length pairs, which is the classic place for
an off-by-one. None of this is a reason never to do it; it *is* a reason not to do it in M2 when
the measured benefit is ~0.5 MB per model.

### Licences and APK cost

| Component | Licence | Attribution needed | APK cost (arm64) |
|---|---|---|---|
| Basis Universal transcoder (ETC1S only) | Apache-2.0 | yes, NOTICE file | ≈ 150–250 KB |
| Basis Universal transcoder (+UASTC) | Apache-2.0 | yes | ≈ 300–400 KB |
| ARM `astc-encoder` (would live in `tools/` only, never in the app) | Apache-2.0 | offline tool, no APK cost | 0 |
| KTX-Software (`libktx`) — **not recommended**, we already parse GLB ourselves | Apache-2.0 | yes | ≈ 700 KB+ |
| gzip / brotli inflate | already in Android's HTTP stack | no | **0** |

All three are Apache-2.0: compatible, permissive, requires keeping the licence text and a NOTICE.
Nothing here is copyleft. And the rule in `README.md` ("everything the runtime executes is our
code") is respected in every option — the transcoder would sit in the Flutter plugin, exactly
where `ktx2.js` sits on the web, not inside `engine/`.

---

## Options, scored

| Option | Wire size (coach) | GPU mem | Phone CPU | New code | iOS later | Verdict |
|---|---|---|---|---|---|---|
| (a) raw ETC2 `.emod` | 33.7 MB | 28 MB | none | none | neutral | too big, but only because of the texture cap |
| (b) **(a) + gzip, 512 px** | **2.2 MB** | **1.7 MB** | inflate only | **uploader only** | neutral | ✅ **ship this for M2** |
| (b′) same with brotli | 1.4 MB | 1.7 MB | ~2× inflate | uploader only | neutral | do it if the uploader can |
| (c) KTX2 ETC1S + zstd | 1.66 MB | 7 MB @2048 | 250–400 ms/model | plugin transcoder + host protocol | good stepping stone | later; best quality-per-byte |
| (d) KTX2 UASTC + zstd | ~4–6 MB | 7 MB | slowest on ETC2 target | as (c) | **best** | only worth it once iOS is real |
| (e) ASTC raw / gzip | bigger than (b) | 0.9 MB @8×8 | none | RHI + model.h + encoder + fallback build | required eventually | not for Android now |
| (f) lower `--max-texture` | — | — | — | none | neutral | **the biggest single lever; use with anything** |
| (g) zstd `.emod` host-side | 2.0 MB | 1.7 MB | fast | zstd in plugin (BSD/GPL dual) | neutral | ~10 % better than gzip for a new dependency — not worth it |

(Option (g) measured: `zstd -19` on the 512-px coach gives 2 020 930 bytes vs gzip's 2 856 912
and brotli's 1 785 348. Brotli wins on size *and* needs no new library, because Android already
speaks it. There is no case for adding zstd.)

---

## What changes in the pipeline

1. **Converter invocation for Android** — add `--max-texture 256` to the Android model build
   (a phone-specific `web/build-models.sh`-style script writing to `ayana/models-android/`).
   Consider a per-model override list for hero locos that need 1024 px.
2. **Regenerate the KTX2 twins** in `ppka-wannabe-2/public/model3d/ktx2/` — at least for the
   models whose image count no longer matches; the converter is silently falling back to our own
   encoder for them on *all* targets.
3. **Compress at upload, not at the edge.** `tools/unggah-r2.mjs` should gzip (or brotli) each
   `.emod` before the PUT and set `Content-Encoding: gzip` (+ `Content-Type:
   application/octet-stream`) on the object. Cloudflare does *not* auto-compress binary types, so
   this is on us. The stored key keeps the name `<slot>.emod`; nothing in `duniaAyana.ts` or in
   the plugin's URL handling changes.
4. **Plugin fetch path:** OkHttp/Cronet inflate `Content-Encoding` transparently, so the disk
   cache in `pk-aset/` stores the *inflated* bytes and the existing `shouldInterceptRequest`
   logic keeps working unchanged. If we ever want the cache to hold compressed bytes, that is a
   separate decision — remember §5's warning that disk and RAM are different problems.
5. **Engine, one small commit:** add the allocation guard to `Reader` in `engine/asset/emod.cpp`
   (count × element size vs bytes remaining). Nothing else in the engine changes for M2.
6. **Explicitly deferred:** the Basis transcoder in the plugin, ASTC in the RHI, and the iOS
   texture build. When iOS arrives, revisit with UASTC KTX2 — that is the point at which the
   host-side transcoder pays for itself twice.

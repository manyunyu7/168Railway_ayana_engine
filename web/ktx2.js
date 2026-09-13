// Texture streaming for geometry-only EMODs (convert --textures external): downloads the reference project's KTX2
// GLB (Basis Universal ETC1S, zstd supercompressed — ~10x smaller than raw ETC2 blocks), transcodes every image in a
// Web Worker (web/ktx2-worker.js, third-party transcoder lives only on the JS side) to the best block format the
// GPU supports and hands the mip chains to the engine through the viewer's C ABI:
//   viewer_supports(format), viewer_image_count/source/flags(i),
//   viewer_texture_begin(image, w, h, format, mips, srgb, wrapS, wrapT), viewer_texture_mip(level, ptr, bytes),
//   viewer_texture_end(), viewer_alloc/viewer_free (Wasm heap), Module.heap() (current HEAPU8 view).
// Plain ES module, no bundler.

export const TexFormat = { RGBA8: 0, ETC2_RGB: 1, ETC2_RGBA: 2, BC1: 3, BC3: 4, BC7: 5 };   // eng::TexFormat
const Transcoder = { ETC1_RGB: 0, ETC2_RGBA: 1, BC1_RGB: 2, BC3_RGBA: 3, RGBA32: 13 };         // basist::transcoder_texture_format

// Images of a GLB as raw byte views (the KTX2 twins are Draco + KHR_texture_basisu; only the image blobs matter).
export function glbImages(buffer) {
  const dv = new DataView(buffer), u8 = new Uint8Array(buffer);
  if (buffer.byteLength < 20 || dv.getUint32(0, true) !== 0x46546C67) throw new Error('not a GLB');
  const jsonLen = dv.getUint32(12, true);
  if (dv.getUint32(16, true) !== 0x4E4F534A) throw new Error('GLB: first chunk is not JSON');
  const json = JSON.parse(new TextDecoder().decode(u8.subarray(20, 20 + jsonLen)));
  const binOff = 20 + jsonLen + 8, binLen = dv.getUint32(20 + jsonLen, true);
  if (dv.getUint32(20 + jsonLen + 4, true) !== 0x004E4942 || binOff + binLen > buffer.byteLength) throw new Error('GLB: bad BIN chunk');
  return (json.images || []).map(im => {
    const bv = json.bufferViews[im.bufferView];
    const off = bv.byteOffset || 0;
    if (off + bv.byteLength > binLen) throw new Error('GLB: image outside BIN chunk');
    return { mime: im.mimeType, bytes: u8.subarray(binOff + off, binOff + off + bv.byteLength) };
  });
}

// Best block format pair (opaque, alpha) the GPU accepts: ETC2 (mobile / Chromium everywhere) -> BC (desktop) -> RGBA8.
export function pickFormat(Module) {
  const ok = f => Module._viewer_supports(f) !== 0;
  if (ok(TexFormat.ETC2_RGB) && ok(TexFormat.ETC2_RGBA)) return { name: 'ETC2', rgb: [TexFormat.ETC2_RGB, Transcoder.ETC1_RGB], rgba: [TexFormat.ETC2_RGBA, Transcoder.ETC2_RGBA] };
  if (ok(TexFormat.BC1) && ok(TexFormat.BC3)) return { name: 'BC1/BC3', rgb: [TexFormat.BC1, Transcoder.BC1_RGB], rgba: [TexFormat.BC3, Transcoder.BC3_RGBA] };
  return { name: 'RGBA8', rgb: [TexFormat.RGBA8, Transcoder.RGBA32], rgba: [TexFormat.RGBA8, Transcoder.RGBA32] };
}

let worker = null, nextId = 1; const jobs = new Map();
function transcodeInWorker(ktx2, rgb, rgba) {
  if (!worker) {
    worker = new Worker('ktx2-worker.js');
    worker.onmessage = e => { const j = jobs.get(e.data.id); jobs.delete(e.data.id); if (!j) return; e.data.error ? j.reject(new Error(e.data.error)) : j.resolve(e.data); };
  }
  return new Promise((resolve, reject) => {
    const id = nextId++; jobs.set(id, { resolve, reject });
    const copy = ktx2.slice();   // own buffer so it can be transferred
    worker.postMessage({ id, ktx2: copy.buffer, rgb, rgba }, [copy.buffer]);
  });
}

function uploadTexture(Module, image, fmt, srgb, flags, mips) {
  const wrapS = flags & 0xff, wrapT = (flags >> 8) & 0xff;
  // levels must halve from the top; stop at the first that does not (the engine checks byte counts per level)
  let w = mips[0].width, h = mips[0].height, n = 0;
  for (const m of mips) { if (m.width !== w || m.height !== h) break; ++n; w = Math.max(1, w >> 1); h = Math.max(1, h >> 1); }
  if (!Module._viewer_texture_begin(image, mips[0].width, mips[0].height, fmt, n, srgb ? 1 : 0, wrapS, wrapT)) throw new Error('viewer_texture_begin refused image ' + image);
  for (let i = 0; i < n; ++i) {
    const d = mips[i].data, ptr = Module._viewer_alloc(d.byteLength);
    if (!ptr) throw new Error('out of Wasm memory');
    Module.heap().set(d, ptr);
    const ok = Module._viewer_texture_mip(i, ptr, d.byteLength);
    Module._viewer_free(ptr);
    if (!ok) throw new Error('viewer_texture_mip refused level ' + i + ' of image ' + image);
  }
  if (!Module._viewer_texture_end()) throw new Error('texture upload failed for image ' + image);
}

// Streams every placeholder image of the loaded model from the KTX2 GLB at `url`. Resolves with statistics:
// { bytes, count, format, transcodeMs (worker CPU time), uploadMs, fetchMs }. onProgress(text) is optional; stillWanted()
// lets the page cancel the uploads when another model was loaded meanwhile.
export async function streamTextures(Module, url, onProgress = () => {}, stillWanted = () => true) {
  const t0 = performance.now();
  const res = await fetch(url);
  if (!res.ok) throw new Error(url + ': HTTP ' + res.status);
  const buffer = await res.arrayBuffer();
  const fetchMs = performance.now() - t0;
  const images = glbImages(buffer);
  const fmt = pickFormat(Module);
  const count = Module._viewer_image_count();
  let transcodeMs = 0, uploadMs = 0, done = 0;
  const jobsOut = [];
  for (let i = 0; i < count; ++i) {
    const flags = Module._viewer_image_flags(i), source = Module._viewer_image_source(i);
    if (!(flags & 0x20000) || source < 0) continue;   // stored in the EMOD already
    if (source >= images.length) { console.warn('image', i, 'source', source, 'not in', url); continue; }
    if (images[source].mime !== 'image/ktx2') { console.warn('image', source, 'of', url, 'is', images[source].mime, 'not KTX2'); continue; }
    const srgb = !(flags & 0x10000);
    jobsOut.push(transcodeInWorker(images[source].bytes, fmt.rgb[1], fmt.rgba[1]).then(r => {
      transcodeMs += r.ms;
      if (!stillWanted()) return;
      const tu = performance.now();
      uploadTexture(Module, i, r.hasAlpha ? fmt.rgba[0] : fmt.rgb[0], srgb, flags, r.mips);
      uploadMs += performance.now() - tu;
      onProgress('textures ' + (++done) + '/' + jobsOut.length);
    }));
  }
  await Promise.all(jobsOut);
  return { bytes: buffer.byteLength, count: jobsOut.length, format: fmt.name, transcodeMs, uploadMs, fetchMs };
}

// Web Worker: transcodes KTX2 (Basis Universal ETC1S/UASTC) images to the GPU block format the engine asked for
// with the BinomialLLC transcoder shipped by three.js (web/vendor/basis_transcoder.{js,wasm}, Apache 2.0).
// Messages in:  { id, ktx2: ArrayBuffer, rgb, rgba }  (transcoder format for opaque / alpha images; the file's DFD decides)
// Messages out: { id, width, height, hasAlpha, format, ms, mips: [{ width, height, data: Uint8Array }] } or { id, error }
// Transcoder format codes (basist::transcoder_texture_format): 0 ETC1_RGB, 1 ETC2_RGBA, 2 BC1_RGB, 3 BC3_RGBA, 13 RGBA32.
'use strict';
importScripts('vendor/basis_transcoder.js');

const ready = BASIS({ locateFile: p => 'vendor/' + p }).then(m => { m.initializeBasis(); return m; });

function transcode(m, buffer, rgb, rgba) {
  const file = new m.KTX2File(new Uint8Array(buffer));
  try {
    if (!file.isValid()) throw new Error('invalid KTX2');
    if (!file.isETC1S() && !file.isUASTC()) throw new Error('unsupported Basis encoding');
    const width = file.getWidth(), height = file.getHeight(), levels = file.getLevels(), hasAlpha = file.getHasAlpha();
    if (!width || !height || !levels) throw new Error('empty texture');
    if (!file.startTranscoding()) throw new Error('startTranscoding failed');
    const format = hasAlpha ? rgba : rgb;
    const mips = [], transfer = [];
    const only0 = format === 13;   // RGBA32: the engine generates the mip chain itself
    for (let mip = 0; mip < (only0 ? 1 : levels); ++mip) {
      const info = file.getImageLevelInfo(mip, 0, 0);
      const data = new Uint8Array(file.getImageTranscodedSizeInBytes(mip, 0, 0, format));
      if (!file.transcodeImage(data, mip, 0, 0, format, 0, -1, -1)) throw new Error('transcodeImage failed at level ' + mip);
      mips.push({ width: info.origWidth, height: info.origHeight, data }); transfer.push(data.buffer);
    }
    return { result: { width, height, hasAlpha, format, mips }, transfer };
  } finally { file.close(); file.delete(); }
}

self.onmessage = e => {
  const { id, ktx2, rgb, rgba } = e.data;
  ready.then(m => {
    try { const t0 = performance.now(); const { result, transfer } = transcode(m, ktx2, rgb, rgba); self.postMessage({ id, ms: performance.now() - t0, ...result }, transfer); }
    catch (err) { self.postMessage({ id, error: String(err.message || err) }); }
  }, err => self.postMessage({ id, error: 'transcoder failed to load: ' + err }));
};

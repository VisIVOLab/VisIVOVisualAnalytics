// The spectator view's JPEG encoder (issue #281), off the page's thread: the
// XR frame never pays for it. A message {id, buf, w, h, quality} holds the
// RGBA pixels of one capture as GL reads them (bottom row first); the answer
// {id, jpeg, buf, ms} hands the pixel buffer back for the next capture, or
// {id, error, buf}, or {id, unsupported: true, buf, w, h, quality} when this
// worker cannot make a JPEG (no OffscreenCanvas with convertToBlob): the page
// encodes on its own canvas then.
//
// flipRows is the page's (spectator.js), repeated here so the worker loads
// nothing else; spectator.test.mjs checks that both give the same rows.

/** The rows of an RGBA image of `w`×`h` upside down, from `src` into `dst`. */
export function flipRows(src, w, h, dst) {
  const row = w * 4;
  for (let y = 0; y < h; y++) dst.set(src.subarray((h - 1 - y) * row, (h - y) * row), y * row);
  return dst;
}

let canvas = null;
let ctx2d = null;
let image = null;

async function encode({ id, buf, w, h, quality }) {
  if (typeof OffscreenCanvas === 'undefined' || !OffscreenCanvas.prototype.convertToBlob) {
    self.postMessage({ id, unsupported: true, buf, w, h, quality }, [buf]);
    return;
  }
  const t0 = performance.now();
  try {
    if (!canvas || canvas.width !== w || canvas.height !== h) {
      canvas = new OffscreenCanvas(w, h);
      ctx2d = canvas.getContext('2d');
      image = new ImageData(w, h);
    }
    flipRows(new Uint8Array(buf), w, h, new Uint8Array(image.data.buffer));
    ctx2d.putImageData(image, 0, 0);
    const blob = await canvas.convertToBlob({ type: 'image/jpeg', quality });
    const jpeg = await blob.arrayBuffer();
    self.postMessage({ id, jpeg, buf, ms: performance.now() - t0 }, [jpeg, buf]);
  } catch (err) {
    self.postMessage({ id, error: String(err && err.message ? err.message : err), buf }, [buf]);
  }
}

// In a worker only (node's tests import this file for flipRows).
if (typeof WorkerGlobalScope !== 'undefined' && typeof self !== 'undefined' && self instanceof WorkerGlobalScope) {
  self.onmessage = (e) => { encode(e.data || {}); };
}

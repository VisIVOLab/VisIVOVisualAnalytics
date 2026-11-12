// 2-D products in the headset (issue #191): a map (moment) or a diagram (PV)
// drawn on a canvas with its axes and colour bar, uploaded as the texture of a
// plane in the room. The pure parts (colour tables, colouring, ranges, ticks,
// units, base64 decoding) are tested by node (backend/tests/vr/tools-moment.test.mjs).

import { niceTicks } from './volume.js';

// The desktop's maps by kind (vtkWindowCube::applyMomentMapResult): inferno
// for an intensity, a diverging cool-warm for a velocity, viridis for a width.
const STOPS = {
  inferno: ['#000004', '#1b0c41', '#4a0c6b', '#781c6d', '#a52c60', '#cf4446', '#ed6925', '#fb9b06', '#fcffa4'],
  viridis: ['#440154', '#472d7b', '#3b528b', '#2c728e', '#21918c', '#28ae80', '#5ec962', '#addc30', '#fde725'],
  coolwarm: ['#3b4cc0', '#6282ea', '#8db0fe', '#b8d0f9', '#dddddd', '#f5c4ac', '#f49a7b', '#de604d', '#b40426'],
};

/** 256 RGB entries (Uint8Array of 768) of a named map. */
export function lut(name) {
  const stops = (STOPS[name] || STOPS.viridis).map((c) => [1, 3, 5].map((i) => parseInt(c.slice(i, i + 2), 16)));
  const out = new Uint8Array(256 * 3);
  const n = stops.length - 1;
  for (let i = 0; i < 256; i++) {
    const x = (i / 255) * n, k = Math.min(n - 1, Math.floor(x)), f = x - k;
    for (let ch = 0; ch < 3; ch++) out[i * 3 + ch] = Math.round(stops[k][ch] + (stops[k + 1][ch] - stops[k][ch]) * f);
  }
  return out;
}

/** Float32 values from base64 (little-endian, as the backend's _b64f32). */
export function f32FromBase64(b64) {
  const bytes = bytesFromBase64(b64);
  return new Float32Array(bytes.buffer, bytes.byteOffset, bytes.byteLength / 4);
}

/** Float64 values from base64 (little-endian). */
export function f64FromBase64(b64) {
  const bytes = bytesFromBase64(b64);
  return new Float64Array(bytes.buffer, bytes.byteOffset, bytes.byteLength / 8);
}

export function bytesFromBase64(b64) {
  if (!b64) return new Uint8Array(0);
  const bin = atob(b64);
  const out = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
  return out;
}

/** Qt's qCompress: a 4-byte big-endian length, then zlib. Inflated with the
 *  platform's DecompressionStream (browsers and node 18+). */
export async function qtUncompress(bytes) {
  if (bytes.length < 4) throw new Error('compressed payload too short');
  const n = ((bytes[0] << 24) >>> 0) + (bytes[1] << 16) + (bytes[2] << 8) + bytes[3];
  const stream = new Blob([bytes.subarray(4)]).stream().pipeThrough(new DecompressionStream('deflate'));
  const out = new Uint8Array(await new Response(stream).arrayBuffer());
  if (out.length !== n) throw new Error(`inflated ${out.length} bytes, expected ${n}`);
  return out;
}

/** Finite min and max, or null. */
export function finiteRange(values) {
  let lo = Infinity, hi = -Infinity;
  for (const v of values) if (Number.isFinite(v)) { if (v < lo) lo = v; if (v > hi) hi = v; }
  return lo <= hi ? [lo, hi] : null;
}

/** The desktop's range for a velocity field (robustSymmetricMomentRange):
 *  the median ± 4 robust sigma (1.4826 MAD), or null with fewer than 8 values. */
export function robustSymmetricRange(values) {
  const v = [];
  for (const x of values) if (Number.isFinite(x)) v.push(x);
  if (v.length < 8) return null;
  const median = (a) => { a.sort((p, q) => p - q); const m = a.length; return m % 2 ? a[m >> 1] : 0.5 * (a[m / 2 - 1] + a[m / 2]); };
  const med = median(v);
  const sigma = 1.4826 * median(v.map((x) => Math.abs(x - med)));
  if (!(sigma > 0)) return null;
  return [med - 4 * sigma, med + 4 * sigma];
}

/**
 * The unit shown on the plane: a velocity in m/s reads in km/s (the page's
 * unit for velocities), the numbers scaled by 1e-3; anything else as the
 * backend gives it. Returns {unit, scale}.
 */
export function displayUnit(unit) {
  const u = (unit || '').trim();
  // m/s, m s-1, m.s-1, m s^-1, m.s**-1 (FITS forms), alone or after an intensity.
  const v = /(^|[\s*.])m\s*(?:\/\s*s|[\s.]s(?:\^|\*\*)?-1)$/i.exec(u);
  if (v) return { unit: `${u.slice(0, v.index + v[1].length)}km/s`, scale: 1e-3 };
  const sq = /(^|[\s*.])(?:\(m\/s\)\^2|m(?:\^|\*\*)?2[\s.]s(?:\^|\*\*)?-2)$/i.exec(u);
  if (sq) return { unit: `${u.slice(0, sq.index + sq[1].length)}(km/s)^2`, scale: 1e-6 };
  return { unit: u, scale: 1 };
}

/**
 * RGBA pixels of a w × h image (row-major, row 0 first in `values`), drawn
 * with row 0 at the BOTTOM (FITS: y up), or rows in `rowOrder` from the
 * bottom up when given (a PV diagram's channels in coordinate order). NaN is
 * transparent. range [lo, hi] maps to the table's ends, clamped, or (with
 * `outsideTransparent`, the desktop's clampOutliers for a velocity field)
 * values outside it transparent.
 */
export function colourise(values, w, h, range, table, rowOrder = null, outsideTransparent = false) {
  const out = new Uint8ClampedArray(w * h * 4);
  const [lo, hi] = range;
  const k = hi > lo ? 255 / (hi - lo) : 0;
  for (let r = 0; r < h; r++) {
    const src = rowOrder ? rowOrder[r] : r;     // the r-th row from the bottom
    const dstRow = h - 1 - r;                   // canvas rows go down
    for (let x = 0; x < w; x++) {
      const v = values[src * w + x];
      const o = (dstRow * w + x) * 4;
      if (!Number.isFinite(v)) continue;
      if (outsideTransparent && (v < lo || v > hi)) continue;
      const i = Math.max(0, Math.min(255, Math.round((v - lo) * k)));
      out[o] = table[i * 3]; out[o + 1] = table[i * 3 + 1]; out[o + 2] = table[i * 3 + 2]; out[o + 3] = 255;
    }
  }
  return out;
}

/** A crop [x0, x1] × [y0, y1] (inclusive) of a w × h row-major image. */
export function crop(values, w, x0, x1, y0, y1) {
  const cw = x1 - x0 + 1, ch = y1 - y0 + 1;
  const out = new Float32Array(cw * ch);
  for (let y = 0; y < ch; y++) out.set(values.subarray((y0 + y) * w + x0, (y0 + y) * w + x0 + cw), y * cw);
  return out;
}

export function fmt(v, digits = 4) {
  if (v === null || v === undefined || !Number.isFinite(v)) return 'n/a';
  const a = Math.abs(v);
  if (a !== 0 && (a < 1e-3 || a >= 1e5)) return v.toExponential(digits - 1);
  return Number(v.toPrecision(digits)).toString();
}

/** Ticks [{f, text}] at nice values of a row axis: `rows` the value of each
 *  row centre from the bottom (monotonic, not necessarily linear, such as an
 *  optical velocity on a frequency axis); f is interpolated between the rows. */
export function rowTicks(rows, n = 4) {
  const k = rows ? rows.length : 0;
  if (k < 2 || !Number.isFinite(rows[0]) || !Number.isFinite(rows[k - 1])) return [];
  const up = rows[k - 1] > rows[0];
  const out = [];
  for (const v of niceTicks(rows[0], rows[k - 1], n)) {
    for (let i = 0; i + 1 < k; i++) {
      const a = rows[i], b = rows[i + 1];
      if (!Number.isFinite(a) || !Number.isFinite(b)) continue;      // a row without a value
      if ((up ? v >= a && v <= b : v <= a && v >= b) && a !== b) {
        out.push({ f: (i + 0.5 + (v - a) / (b - a)) / k, text: fmt(v, 4) });
        break;
      }
    }
  }
  return out;
}

/** Ticks [{f, text}] along a span [a, b] (f: 0..1 from a). */
export function ticks(a, b, n = 4) {
  if (!(Number.isFinite(a) && Number.isFinite(b)) || a === b) return [];
  const lo = Math.min(a, b), hi = Math.max(a, b);
  return niceTicks(lo, hi, n).filter((v) => v >= lo && v <= hi).map((v) => ({ f: (v - a) / (b - a), text: fmt(v, 4) }));
}

/** Canvas pixels of a product plane, and its size in metres: 1600 px/m, so
 *  its 17-19 px labels are 1.1-1.2 cm high and its title 1.6 cm (the planes
 *  hang a little farther than the panels, about 0.85 m). */
export const PLANE_PX = [768, 576];
export const PLANE_SIZE = [PLANE_PX[0] / 1600, PLANE_PX[1] / 1600];

function makeCanvas(w, h) {
  if (typeof OffscreenCanvas !== 'undefined') return new OffscreenCanvas(w, h);
  if (typeof document !== 'undefined') return Object.assign(document.createElement('canvas'), { width: w, height: h });
  return null;
}

/**
 * Draws a product on canvas `cv` (PLANE_PX): title and lines at the top, the
 * image in a frame keeping its aspect (`aspect`: displayed width / height of
 * one image pixel), x and y ticks with their labels, a colour bar with its
 * unit on the right. p: {title, lines, rgba, w, h, aspect, xTicks, yTicks,
 * xLabel, yLabel, table, bar: {lo, hi, unit}, marks: [{x, y, color}] (image
 * pixel coordinates, y up)}.
 */
export function drawProduct(cv, p) {
  const c = cv.getContext('2d');
  const W = cv.width, H = cv.height;
  c.clearRect(0, 0, W, H);
  c.fillStyle = 'rgba(12, 16, 24, 0.94)';
  c.fillRect(0, 0, W, H);
  c.textBaseline = 'alphabetic';
  c.fillStyle = '#9fd0ff';
  c.font = 'bold 26px -apple-system, system-ui, sans-serif';
  c.fillText(p.title || '', 16, 32);
  c.font = '19px ui-monospace, Menlo, monospace';
  c.fillStyle = '#e6e9ef';
  (p.lines || []).slice(0, 2).forEach((l, i) => c.fillText(l, 16, 58 + i * 24));
  if (!p.rgba) return;
  // The frame: left margin for y labels, right for the colour bar.
  const fx0 = 92, fy0 = 112, fx1 = W - 150, fy1 = H - 64;
  const fw = fx1 - fx0, fh = fy1 - fy0;
  const ar = (p.w * (p.aspect || 1)) / p.h;
  let iw = fw, ih = fw / ar;
  if (ih > fh) { ih = fh; iw = fh * ar; }
  const ix = fx0 + (fw - iw) / 2, iy = fy0 + (fh - ih) / 2;
  c.fillStyle = '#05070b';
  c.fillRect(ix, iy, iw, ih);
  const img = makeCanvas(p.w, p.h);
  if (img) {
    const ic = img.getContext('2d');
    const id = ic.createImageData(p.w, p.h);
    id.data.set(p.rgba);
    ic.putImageData(id, 0, 0);
    c.imageSmoothingEnabled = false;
    c.drawImage(img, ix, iy, iw, ih);
  }
  c.strokeStyle = '#59627a';
  c.lineWidth = 1;
  c.strokeRect(ix, iy, iw, ih);
  for (const m of p.marks || []) {
    const mx = ix + ((m.x + 0.5) / p.w) * iw, my = iy + ih - ((m.y + 0.5) / p.h) * ih;
    c.strokeStyle = m.color || '#ffd33d';
    c.lineWidth = 2;
    c.beginPath(); c.moveTo(mx - 8, my); c.lineTo(mx + 8, my); c.moveTo(mx, my - 8); c.lineTo(mx, my + 8); c.stroke();
  }
  c.fillStyle = '#9aa3b2';
  c.font = '17px ui-monospace, Menlo, monospace';
  c.textAlign = 'center';
  for (const t of p.xTicks || []) {
    const x = ix + t.f * iw;
    c.fillRect(x, iy + ih, 1, 6);
    c.fillText(t.text, x, iy + ih + 22);
  }
  c.fillText(p.xLabel || '', ix + iw / 2, iy + ih + 46);
  c.textAlign = 'right';
  for (const t of p.yTicks || []) {
    const y = iy + ih - t.f * ih;
    c.fillRect(ix - 6, y, 6, 1);
    c.fillText(t.text, ix - 8, y + 6);
  }
  c.save();
  c.translate(18, iy + ih / 2);
  c.rotate(-Math.PI / 2);
  c.textAlign = 'center';
  c.fillText(p.yLabel || '', 0, 6);
  c.restore();
  // The colour bar: bottom = lo, top = hi.
  if (p.bar && p.table) {
    const bx = W - 128, bw = 26;
    for (let i = 0; i < ih; i++) {
      const k = Math.round((1 - i / ih) * 255);
      c.fillStyle = `rgb(${p.table[k * 3]},${p.table[k * 3 + 1]},${p.table[k * 3 + 2]})`;
      c.fillRect(bx, iy + i, bw, 1.5);
    }
    c.strokeRect(bx, iy, bw, ih);
    c.textAlign = 'left';
    c.fillStyle = '#e6e9ef';
    for (const t of ticks(p.bar.lo, p.bar.hi, 4)) {
      const y = iy + ih - t.f * ih;
      c.fillRect(bx + bw, y, 5, 1);
      c.fillText(t.text, bx + bw + 7, y + 6);
    }
    c.fillStyle = '#9fd0ff';
    c.textAlign = 'center';
    c.fillText(p.bar.unit || '', bx + bw / 2 + 20, iy - 10);
  }
  c.textAlign = 'left';
}

export { makeCanvas };

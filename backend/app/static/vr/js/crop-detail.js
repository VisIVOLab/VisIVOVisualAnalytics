// The crop in more detail (issue #328): when the spatial crop has settled, the
// page asks the backend for the cropped region at the finest level of detail
// whose region fits the fixed capacity (#235), quantises it on the whole
// cube's codes (same scale, threshold and colour range), writes it into the
// capacity's spare bank (texSubImage3D only: no 3-D allocation in VR, #229)
// and draws it in the crop's place. Outside the crop nothing is drawn, as with
// any crop. Clearing the crop, or moving it out of the region, shows the
// coarse cube again (still in its bank). Pure functions: node --test.

/** How long the crop must stay still before its region is asked for (ms). */
export const SETTLE_MS = 700;

/** The shapes of the levels as [x, y, z] (pyramid_status gives [z, y, x], level 0 first). */
export function levelShapesXYZ(shapesZYX) {
  return (shapesZYX || []).map((s) => [s[2], s[1], s[0]]);
}

/** Whether the crop is the whole cube (no detail then). */
export function wholeCrop(clip) {
  return !clip || (clip.min.every((v) => v <= 0) && clip.max.every((v) => v >= 1));
}

/**
 * The region to ask for: the crop `clip` ({min, max}, box fractions, x first)
 * of a cube `fullShape` [x, y, z], at the finest level (level l halves the
 * cube l times, rounded up: `levels` [x, y, z] per level) whose box fits
 * `capDims` [x, y, z] (each side and in all). {level, box: {x0, x1, y0, y1,
 * z0, z1} (inclusive, voxels of the level), dims [x, y, z], lo, hi (the box's
 * extent in box fractions: level voxel i covers cube voxels [i 2^l, (i + 1)
 * 2^l)), step (2^l)}; null when the crop is the whole cube, nothing fits, or
 * the level is not finer than the coarse cube's binning `factors` on any axis
 * (no gain).
 */
export function planRegion({ clip, fullShape, levels, capDims, factors }) {
  if (wholeCrop(clip) || !fullShape || !levels || !levels.length || !capDims) return null;
  const capVoxels = capDims[0] * capDims[1] * capDims[2];
  for (let l = 0; l < levels.length; l++) {
    const step = 2 ** l;
    const box = {}, dims = [], lo = [], hi = [];
    let ok = true;
    for (let i = 0; i < 3 && ok; i++) {
      const n = fullShape[i], nl = levels[l][i];
      const a = Math.max(0, Math.min(n - 1, Math.floor(clip.min[i] * n)));
      const b = Math.max(a, Math.min(n - 1, Math.ceil(clip.max[i] * n) - 1));
      const al = Math.floor(a / step), bl = Math.min(nl - 1, Math.floor(b / step));
      const k = 'xyz'[i];
      box[`${k}0`] = al; box[`${k}1`] = bl;
      dims.push(bl - al + 1);
      lo.push((al * step) / n);
      hi.push(Math.min((bl + 1) * step, n) / n);
      if (dims[i] > capDims[i]) ok = false;
    }
    if (!ok || dims[0] * dims[1] * dims[2] > capVoxels) continue;
    // Finer on one axis at least, coarser on none (a spectral axis left
    // unbinned is not halved inside the crop).
    const f = factors || [1, 1, 1];
    const gain = f.some((v) => step < v) && f.every((v) => step <= v);
    return gain ? { level: l, box, dims, lo, hi, step } : null;
  }
  return null;
}

/** Whether the region `r` still holds the crop `clip` (its detail can stay on show). */
export function regionHolds(r, clip) {
  if (!r || wholeCrop(clip)) return false;
  const eps = 1e-9;
  return [0, 1, 2].every((i) => clip.min[i] >= r.lo[i] - eps && clip.max[i] <= r.hi[i] + eps);
}

/** The region's place for the volume shader: q = (p - lo) * scale maps a box
 *  coordinate p of the whole cube to the region's own, and `voxRatio` (the
 *  region's voxel over the coarse cube's, per axis averaged) keeps the
 *  opacity per unit length the coarse cube's. `texScale`: covered cube voxels
 *  over (region voxels * step), the partial last block as for the cube. */
export function regionDraw(r, fullShape, coarseVoxelSize) {
  const scale = [0, 1, 2].map((i) => 1 / Math.max(1e-12, r.hi[i] - r.lo[i]));
  const texScale = [0, 1, 2].map((i) => ((r.hi[i] - r.lo[i]) * fullShape[i]) / (r.dims[i] * r.step));
  const cv = coarseVoxelSize || [1, 1, 1];
  const voxRatio = Math.cbrt([0, 1, 2].reduce((acc, i) => acc * (r.step / cv[i]), 1));
  return { lo: r.lo.slice(), scale, texScale, voxRatio };
}

// ── Quantisation on the whole cube's codes (cube_reduce.quantise) ───────────

const F32 = new Float32Array(1);
const U32 = new Uint32Array(F32.buffer);
/** IEEE half-float bits of `v` (round to nearest even; overflow to infinity). */
export function toHalf(v) {
  F32[0] = v;
  const x = U32[0];
  const sign = (x >>> 16) & 0x8000;
  const exp = (x >>> 23) & 0xff;
  let mant = x & 0x7fffff;
  if (exp === 0xff) return sign | 0x7c00 | (mant ? 0x200 : 0);
  let e = exp - 127 + 15;
  if (e >= 0x1f) return sign | 0x7c00;
  if (e <= 0) {
    if (e < -10) return sign;
    mant |= 0x800000;
    const shift = 14 - e;
    let h = mant >>> shift;
    const rem = mant & ((1 << shift) - 1), half = 1 << (shift - 1);
    if (rem > half || (rem === half && (h & 1))) h++;
    return sign | h;
  }
  let h = (e << 10) | (mant >>> 13);
  const rem = mant & 0x1fff;
  if (rem > 0x1000 || (rem === 0x1000 && (h & 1))) h++;
  return sign | h;
}

/** Rounding half to even, as numpy's rint. */
export function rint(x) {
  const r = Math.round(x);
  return Math.abs(x - Math.trunc(x)) === 0.5 && r % 2 !== 0 ? r - 1 : r;
}

/**
 * Codes of `values` (Float32Array, NaN blank) on the cube's `lo`, `hi` and
 * `scale` (log: codes of log10, values <= 0 blank), as the backend makes them
 * for the reduced cube: uint8 0 blank, 1..255 over lo..hi (clamped); float16
 * the normalised value, -1 blank. In place into `out` from `start` to `end`
 * (a slice at a time keeps the page responsive). Returns `out`.
 */
export function quantise(values, lo, hi, scale, encoding, out, start = 0, end = values.length) {
  let span = hi - lo;
  if (!Number.isFinite(span) || span <= 0) span = 1;
  const log = scale === 'log';
  const u8 = encoding === 'uint8';
  const blankHalf = toHalf(-1);
  // As numpy does it: float32 arithmetic (log10 of a float32), rounding half to even (np.rint).
  const lo32 = Math.fround(lo), span32 = Math.fround(span);
  for (let i = start; i < end; i++) {
    let v = values[i];
    if (log) v = v > 0 ? Math.fround(Math.log10(v)) : NaN;
    if (!Number.isFinite(v)) { out[i] = u8 ? 0 : blankHalf; continue; }
    const n = Math.fround(Math.fround(Math.fround(v) - lo32) / span32);
    if (u8) out[i] = 1 + Math.min(254, Math.max(0, rint(Math.fround(n * 254))));
    else out[i] = toHalf(log ? Math.max(0, n) : n);
  }
  return out;
}

/**
 * When to ask for the crop's detail: `note(key, now)` with the crop's key at
 * every frame (it restarts the wait when the key changed); `due(now)` is the
 * key to ask for once it has stayed SETTLE_MS, once per key (a key asked for,
 * or the one on show, is not asked again until it changes).
 */
export class DetailGate {
  constructor(settleMs = SETTLE_MS) { this.settleMs = settleMs; this.key = ''; this.since = 0; this.asked = ''; }
  note(key, now) {
    if (key !== this.key) { this.key = key; this.since = now; }
  }
  due(now) {
    if (!this.key || this.key === this.asked || now - this.since < this.settleMs) return '';
    this.asked = this.key;
    return this.key;
  }
  /** Asked for again at the next settle (a failed request, the detail dropped). */
  reset() { this.asked = ''; }
}

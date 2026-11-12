// Picking and selections of the VR page (issue #171), without DOM or GL so
// node runs the tests: a ray through the reduced cube to the sample the
// renderer draws first, the mapping of a point of the box to the FULL cube's
// pixel and channel (where the backend computes), the reduction of a
// full-resolution selection mask to the drawn grid, and the sums that turn
// per-channel region statistics into the statistics of a 3-D box.
//
// Coordinates. A box coordinate u (0..1 per axis) spans the cube's
// full-resolution pixels, pixel p's centre at (p + 0.5) / N_full: the
// renderer's `p`. Its texture coordinate is u * texScale (texScale =
// N_full / (blocks * factor), below 1 when the last block of an axis is
// partial), texel i's centre at (i + 0.5) / N_reduced, filtered trilinearly
// with CLAMP_TO_EDGE, as WebGL samples the 3-D texture.

import { codeToNorm, halfToFloat, boxToTex, texToWorld, axisTexToWorld } from './volume.js';

/** Ray (origin o, direction d) against the box [lo, hi]: the entry and exit
 *  parameters with t >= 0, or null when it misses. */
export function rayBox(o, d, lo, hi) {
  let tn = 0, tf = Infinity;
  for (let i = 0; i < 3; i++) {
    if (Math.abs(d[i]) < 1e-12) {
      if (o[i] < lo[i] || o[i] > hi[i]) return null;
      continue;
    }
    let a = (lo[i] - o[i]) / d[i], b = (hi[i] - o[i]) / d[i];
    if (a > b) [a, b] = [b, a];
    tn = Math.max(tn, a);
    tf = Math.min(tf, b);
  }
  return tf > tn ? { tn, tf } : null;
}

/** The normalised value the shader reads at box coordinate p (normAt in the
 *  fragment shader): trilinear over the texels, blank < 0. `vol` =
 *  {data, dims [w, h, d], encoding, texScale}. */
export function sampleNorm(vol, p) {
  const { data, dims, encoding } = vol;
  const ts = vol.texScale || [1, 1, 1];
  const [w, h] = dims;
  const idx = [0, 1, 2].map((i) => {
    const x = p[i] * ts[i] * dims[i] - 0.5;
    const i0 = Math.floor(x);
    const f = x - i0;
    const n = dims[i];
    return [Math.min(n - 1, Math.max(0, i0)), Math.min(n - 1, Math.max(0, i0 + 1)), f];
  });
  const at = (x, y, z) => data[(z * h + y) * w + x];
  // An 8-bit texture is filtered on its stored codes and decoded after
  // (s * 255 - 1) / 254; a float16 one is filtered on its values.
  const val = encoding === 'uint8' ? (x, y, z) => at(x, y, z) : (x, y, z) => halfToFloat(at(x, y, z));
  const [x0, x1, fx] = idx[0], [y0, y1, fy] = idx[1], [z0, z1, fz] = idx[2];
  const lerp = (a, b, f) => a + (b - a) * f;
  const c00 = lerp(val(x0, y0, z0), val(x1, y0, z0), fx);
  const c10 = lerp(val(x0, y1, z0), val(x1, y1, z0), fx);
  const c01 = lerp(val(x0, y0, z1), val(x1, y0, z1), fx);
  const c11 = lerp(val(x0, y1, z1), val(x1, y1, z1), fx);
  const s = lerp(lerp(c00, c10, fy), lerp(c01, c11, fy), fz);
  return encoding === 'uint8' ? (s - 1) / 254 : s;
}

/**
 * The sample a ray draws first, and the brightest of the emission it enters.
 *
 * Emission mode (0): the first sample at or above `thr` along the ray inside
 * the clipping box, where the renderer starts to composite (the same
 * threshold, the same values), then the brightest sample of the run of
 * samples above `thr` that starts there: the object the ray entered, not one
 * behind it. Maximum-intensity mode (1): the brightest sample of the whole ray,
 * which is what that mode draws, when it is at or above `thr`.
 *
 * Samples are `stepVox` reduced voxels apart (finer than the renderer's, with
 * no jitter, so a pick does not depend on the frame). Returns null when the
 * ray draws nothing, else {hit, peak}, each {t, p, value}.
 */
export function pickRay(vol, o, d, { thr, boxMin = [0, 0, 0], boxMax = [1, 1, 1], mode = 0,
  stepVox = 0.25 } = {}) {
  const span = rayBox(o, d, boxMin, boxMax);
  if (!span) return null;
  const ts = vol.texScale || [1, 1, 1];
  const vox = [0, 1, 2].map((i) => vol.dims[i] * ts[i]);   // reduced voxels per box unit
  const len = Math.hypot(d[0] * vox[0], d[1] * vox[1], d[2] * vox[2]);
  if (!(len > 0)) return null;
  const dt = stepVox / len;
  const pointAt = (t) => [o[0] + d[0] * t, o[1] + d[1] * t, o[2] + d[2] * t];
  let hit = null, peak = null;
  for (let t = span.tn; t <= span.tf; t += dt) {
    const p = pointAt(t);
    const v = sampleNorm(vol, p);
    if (mode === 1) {
      if (v >= thr && (!peak || v > peak.value)) peak = { t, p, value: v };
      continue;
    }
    if (v >= thr) {
      if (!hit) hit = { t, p, value: v };
      if (!peak || v > peak.value) peak = { t, p, value: v };
    } else if (hit) {
      break;            // the end of the first run of emission
    }
  }
  if (mode === 1) return peak ? { hit: peak, peak } : null;
  return hit ? { hit, peak } : null;
}

// ── Box coordinate <-> full cube ────────────────────────────────────────────

/** The full-resolution pixel (x, y, channel), 0-based, holding box point p. */
export function boxToFullPixel(p, fullShape) {
  return [0, 1, 2].map((i) => Math.min(fullShape[i] - 1, Math.max(0, Math.floor(p[i] * fullShape[i]))));
}

/** The box coordinate of a full-resolution pixel's centre. */
export function fullPixelToBox(px, fullShape) {
  return [0, 1, 2].map((i) => (px[i] + 0.5) / fullShape[i]);
}

/** The reduced voxel holding full pixel px (integer division by the factor). */
export function reducedVoxelOf(px, factors) {
  return [0, 1, 2].map((i) => Math.floor(px[i] / factors[i]));
}

/** The full-resolution pixels [first, last] of reduced voxel r along an axis
 *  (the last block of an axis may be partial). */
export function blockRange(r, factor, nFull) {
  const a = r * factor;
  return [a, Math.min(a + factor, nFull) - 1];
}

/** The block of full-resolution voxels behind reduced voxel r: [[x0, x1],
 *  [y0, y1], [z0, z1]], inclusive (the subvolume_bin convention). */
export function blockOfReduced(r, factors, fullShape) {
  return [0, 1, 2].map((i) => blockRange(r[i], factors[i], fullShape[i]));
}

/** The brightest finite voxel of a block read at full resolution (float32,
 *  x fastest, dims [w, h, d], origin [x, y, z]): [x, y, z], or null when the
 *  block is all blank. Ties keep the first voxel. */
export function brightestInBlock(values, dims, origin) {
  const [w, h] = dims;
  let best = -Infinity, at = -1;
  for (let i = 0; i < values.length; i++) {
    const v = values[i];
    if (Number.isFinite(v) && v > best) { best = v; at = i; }
  }
  if (at < 0) return null;
  const z = Math.floor(at / (w * h)), rem = at - z * w * h;
  const y = Math.floor(rem / w), x = rem - y * w;
  return [origin[0] + x, origin[1] + y, origin[2] + z];
}

/** Blocks of more voxels than this snap to their brightest 3 x 3 spatial mean,
 *  not to their brightest voxel: in a large block (8 x 8 x 4 = 256 voxels) the
 *  brightest voxel is often a noise spike at its edge. */
export const SMOOTH_SNAP_ABOVE = 27;

/**
 * The block's voxel with the brightest 3 x 3 spatial mean (finite neighbours
 * only, each in its own channel). `values` (float32, x fastest, dims [w, h, d],
 * origin) hold the block widened by up to one pixel on x and y so the means
 * at its edges see their neighbours; `block` = [[x0, x1], [y0, y1], [z0, z1]]
 * is the block itself, inside which the seed is chosen. Ties keep the first.
 */
export function brightestSmoothed(values, dims, origin, block) {
  const [w, h, d] = dims;
  let best = -Infinity, at = null;
  for (let z = block[2][0]; z <= block[2][1]; z++) {
    const lz = z - origin[2];
    if (lz < 0 || lz >= d) continue;
    for (let y = block[1][0]; y <= block[1][1]; y++) {
      for (let x = block[0][0]; x <= block[0][1]; x++) {
        const lx = x - origin[0], ly = y - origin[1];
        if (!Number.isFinite(values[(lz * h + ly) * w + lx])) continue;
        let s = 0, n = 0;
        for (let dy = -1; dy <= 1; dy++) {
          const yy = ly + dy;
          if (yy < 0 || yy >= h) continue;
          for (let dx = -1; dx <= 1; dx++) {
            const xx = lx + dx;
            if (xx < 0 || xx >= w) continue;
            const v = values[(lz * h + yy) * w + xx];
            if (Number.isFinite(v)) { s += v; n++; }
          }
        }
        const m = s / n;
        if (m > best) { best = m; at = [x, y, z]; }
      }
    }
  }
  return at;
}

// ── A full-resolution mask on the drawn grid ────────────────────────────────

/**
 * A selection mask (values > 0.5 selected; x fastest; dims [w, h, d] at full
 * resolution, origin [x, y, z]) reduced to the drawn grid with the cube's
 * binning: a reduced voxel is marked when ANY full voxel of its block is
 * selected, so nothing selected disappears from the drawing. Returns
 * {data: Uint8Array (255 marked, 0 not), origin, dims} over the reduced
 * voxels the mask touches, and the number marked.
 */
export function reduceMask(mask, dims, origin, factors, reducedShape) {
  const [w, h, d] = dims;
  const lo = [0, 1, 2].map((i) => Math.floor(origin[i] / factors[i]));
  const hi = [0, 1, 2].map((i) => Math.min(reducedShape[i] - 1,
    Math.floor((origin[i] + dims[i] - 1) / factors[i])));
  const rd = [0, 1, 2].map((i) => Math.max(0, hi[i] - lo[i] + 1));
  const out = new Uint8Array(rd[0] * rd[1] * rd[2]);
  let count = 0;
  for (let z = 0; z < d; z++) {
    const rz = Math.floor((origin[2] + z) / factors[2]) - lo[2];
    if (rz < 0 || rz >= rd[2]) continue;
    for (let y = 0; y < h; y++) {
      const ry = Math.floor((origin[1] + y) / factors[1]) - lo[1];
      if (ry < 0 || ry >= rd[1]) continue;
      const row = (z * h + y) * w;
      for (let x = 0; x < w; x++) {
        if (!(mask[row + x] > 0.5)) continue;
        const rx = Math.floor((origin[0] + x) / factors[0]) - lo[0];
        if (rx < 0 || rx >= rd[0]) continue;
        const k = (rz * rd[1] + ry) * rd[0] + rx;
        if (!out[k]) { out[k] = 255; count++; }
      }
    }
  }
  return { data: out, origin: lo, dims: rd, count };
}

/** Selected voxels of a float32 0/1 mask, and its channels with any selected. */
export function maskSummary(mask, dims, origin) {
  const [w, h, d] = dims;
  let n = 0, z0 = Infinity, z1 = -Infinity;
  for (let z = 0; z < d; z++) {
    let any = false;
    const base = z * w * h;
    for (let i = 0; i < w * h; i++) if (mask[base + i] > 0.5) { n++; any = true; }
    if (any) { z0 = Math.min(z0, origin[2] + z); z1 = Math.max(z1, origin[2] + z); }
  }
  return { voxels: n, channels: n ? [z0, z1] : null };
}

// ── A 3-D box ───────────────────────────────────────────────────────────────

/** Full-resolution box from two box points (either order): [[x0, x1], [y0, y1],
 *  [z0, z1]] inclusive, the pixels whose centres the two corners enclose (at
 *  least the pixel each corner is in). */
export function boxFromCorners(a, b, fullShape) {
  const pa = boxToFullPixel(a, fullShape), pb = boxToFullPixel(b, fullShape);
  return [0, 1, 2].map((i) => [Math.min(pa[i], pb[i]), Math.max(pa[i], pb[i])]);
}

/** The full-resolution channels a clipping range [z0, z1] (box coordinates)
 *  shows: those whose centres lie inside it, at least one. */
export function channelsOfClip(zClip, nFull) {
  let a = Math.ceil(zClip[0] * nFull - 0.5), b = Math.floor(zClip[1] * nFull - 0.5);
  a = Math.max(0, Math.min(nFull - 1, a));
  b = Math.max(0, Math.min(nFull - 1, b));
  return b >= a ? [a, b] : [a, a];
}

/** A box placed by two rays (the mouse, or a controller pointing from outside
 *  the cube), which fix a position on the sky but no depth: x and y from the
 *  corners, the channels of the velocity slab (the clipping range on z). */
export function boxFromRayCorners(a, b, fullShape, zClip) {
  const r = boxFromCorners(a, b, fullShape);
  r[2] = channelsOfClip(zClip, fullShape[2]);
  return r;
}

// ── World coordinates of a full-resolution pixel ────────────────────────────

/**
 * World coordinates of full pixel px from /v1/cube/reduced_info: the sky axes
 * interpolated between the reduced voxels' centres (read along the central
 * row and column, as the axis labels are; the exact ICRS position comes from
 * /v1/datasets/pixel_to_sky), the velocity of the channel itself from
 * `full_velocity_kms` when the backend gives it.
 */
export function worldOfPixel(info, px) {
  const ax = info.axes, f = info.factors, n = info.full_shape;
  const along = (i, world) => texToWorld(world, boxToTex((px[i] + 0.5) / n[i], n[i], f[i]));
  // RA (a longitude) across 0° is interpolated on its unwrapped line.
  const sky = (i, axis) => axisTexToWorld(axis, axis.world, boxToTex((px[i] + 0.5) / n[i], n[i], f[i]));
  const out = { x: sky(0, ax.x), y: sky(1, ax.y), xName: ax.x.name, yName: ax.y.name,
    xUnit: ax.x.unit, yUnit: ax.y.unit, velocity: null, spectral: along(2, ax.z.world), spectralUnit: ax.z.unit };
  const fv = ax.z.full_velocity_kms;
  if (Array.isArray(fv) && fv.length === n[2]) out.velocity = fv[px[2]];
  else if (Array.isArray(ax.z.velocity_kms)) out.velocity = along(2, ax.z.velocity_kms);
  return out;
}

/** Velocity (km/s) of full channel z, or null. */
export function channelVelocity(info, z) {
  const fv = info && info.axes && info.axes.z.full_velocity_kms;
  return Array.isArray(fv) && z >= 0 && z < fv.length ? fv[z] : null;
}

function sexagesimal(v, decimals) {
  const sign = v < 0 ? '-' : '';
  let a = Math.abs(v);
  let d = Math.floor(a); a = (a - d) * 60;
  let m = Math.floor(a); let s = (a - m) * 60;
  const scale = 10 ** decimals;
  s = Math.round(s * scale) / scale;
  if (s >= 60) { s -= 60; m += 1; }
  if (m >= 60) { m -= 60; d += 1; }
  const ss = s.toFixed(decimals).padStart(decimals ? decimals + 3 : 2, '0');
  return `${sign}${String(d).padStart(2, '0')}:${String(m).padStart(2, '0')}:${ss}`;
}

/** Right ascension in degrees as hh:mm:ss.ss. */
export function formatRA(deg) {
  return sexagesimal((((deg % 360) + 360) % 360) / 15, 2);
}

/** Declination in degrees as ±dd:mm:ss.s. */
export function formatDec(deg) {
  const s = sexagesimal(deg, 1);
  return deg < 0 ? s : `+${s}`;
}

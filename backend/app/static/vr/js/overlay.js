// Two or three variables of a simulation volume overlaid in one view (issue
// #357): density with temperature, or the abundances of O, Si and Fe.
//
// The fields come from the backend in one request
// (/v1/cube/reduced_fields_bin), on the cube on show's grid (the same binning
// factors), interleaved: one multi-channel 3-D texture (RG8 for two fields,
// RGB8 for three) instead of one sampler per field. Each field keeps its own
// codes (its own range and scale, log or linear), colour map (a row of a LUT
// atlas), threshold, colour range, stretch and opacity. The empty-space skip
// grid holds each field's maximum in its own channel (RGBA32F), so a cell is
// skipped only when every field is below its own threshold there.
//
// Compositing (renderer.js VOL_OV_FS): by default the fields are emitting and
// absorbing media that share the ray: their extinctions add up, and the
// colour of a step is their colours weighted by each field's share of the
// extinction (one field alone draws exactly as the single-field cube does).
// MIP takes, along the ray, the brightest field value in its own colour
// window (the field and its colour map that won).
//
// The storage is allocated with the capacity, outside VR (no 3-D allocation
// during a session, #229/#235). While an overlay is written the single cube
// is drawn: the storage is never on show while it changes.
//
// Pure: no DOM, no GL. Unit-tested in backend/tests/vr/overlay.test.mjs.

import { buildMacrocells, normQuantile, normToData, dataToNorm, halfToFloat } from './volume.js';
import { colormapTable } from './colormaps.js';

/** At most this many fields overlaid (one RGB8 texel). */
export const OVERLAY_MAX = 3;
/** The compositing rules: summed emission-absorption, maximum intensity. */
export const COMPOSITES = ['sum', 'mip'];
export const COMPOSITE_CODE = { sum: 0, mip: 1 };
/** The colour maps given to the fields in turn: red, green, blue, the
 *  astronomer's three-colour composite. */
export const OVERLAY_DEFAULT_MAPS = ['red', 'green', 'blue'];
/** Rows of the LUT atlas (a 256 × 4 RGBA8 2-D texture; row i is field i's map). */
export const LUT_ROWS = 4;
/** The colour range's top a field starts with: this quantile of its values above its threshold. */
export const OVERLAY_CMAX_QUANTILE = 0.99;

/** The storage's channels for a source with `n` fields: two fields need RG8,
 *  three or more RGB8 (a pair of them then sits in an RGB8 storage, the third
 *  channel zero). */
export function storageChannels(n) {
  return n >= 3 ? 3 : 2;
}

/** The largest answer the backend gives for an overlay (all channels, the
 *  padding included): /v1/cube/reduced_fields_bin's OVERLAY_MAX_BYTES. */
export const OVERLAY_MAX_BYTES = 128 * 1024 * 1024;

/** Why fields on a grid of `dims` (`channels` a voxel, uint8) cannot be
 *  overlaid ('' when they can): the answer would pass OVERLAY_MAX_BYTES. */
export function tooLarge(dims, channels) {
  const bytes = dims[0] * dims[1] * dims[2] * channels;
  if (bytes <= OVERLAY_MAX_BYTES) return '';
  return `The fields of a ${dims.join('×')} cube are ${(bytes / 2 ** 20).toFixed(0)} MiB, more than the ${OVERLAY_MAX_BYTES / 2 ** 20} MiB `
    + 'an overlay can take: choose a smaller Max side.';
}

/** Bytes a voxel of a storage of `channels` takes on the GPU: RGB8 is counted
 *  as four (drivers pad it), RG8 as two; float16 doubles them. */
export function bytesPerVoxel(channels, encoding = 'uint8') {
  const c = channels >= 3 ? 4 : 2;
  return encoding === 'float16' ? 2 * c : c;
}

/**
 * GPU bytes of an overlay storage of `dims` voxels and `channels`: one
 * multi-channel texture and its skip grid (RGBA32F, one cell a `cell`^3).
 * {volumeBytes, macroBytes, totalBytes}.
 */
export function overlayBytes(dims, channels, encoding = 'uint8', cell = 8) {
  const nvox = dims[0] * dims[1] * dims[2];
  const volumeBytes = nvox * bytesPerVoxel(channels, encoding);
  const macroBytes = dims.reduce((a, v) => a * Math.ceil(v / cell), 1) * 16;
  return { volumeBytes, macroBytes, totalBytes: volumeBytes + macroBytes };
}

/**
 * The fields a press on `name` leaves in the overlay `list`: taken out when
 * in it, else added at the end while there is room (OVERLAY_MAX). The
 * order is the channels' order (field i takes channel i and colour map i).
 */
export function toggleField(list, name, max = OVERLAY_MAX) {
  const out = list.filter((n) => n !== name);
  if (out.length < list.length) return out;
  return out.length < max ? [...out, name] : out;
}

/** The overlay a source starts with: the variable on show first, then the
 *  next ones in the source's order, up to three. */
export function defaultFields(names, current) {
  if (!Array.isArray(names) || names.length < 2) return [];
  const i = Math.max(0, names.indexOf(current));
  const out = [];
  for (let k = 0; k < names.length && out.length < Math.min(OVERLAY_MAX, names.length); k++) out.push(names[(i + k) % names.length]);
  return out;
}

/**
 * The per-field descriptions of an answer's X-Visivo-Fields header (JSON),
 * as the page reads one cube's (api.js reducedScale): {name, path, scale,
 * lo, hi (the domain its codes span: log10 of the values on a log scale),
 * valueMin, valueMax, threshold (a value, or null), nonpositive, bunit,
 * source, pyramidLevel, fileRevision}. Throws on a malformed header.
 */
export function parseFields(text, count) {
  let list;
  try { list = JSON.parse(String(text || '')); } catch { throw new Error('the overlay came without a readable X-Visivo-Fields header'); }
  if (!Array.isArray(list) || list.length !== count) throw new Error(`the overlay came with ${Array.isArray(list) ? list.length : 0} field descriptions, not ${count}`);
  return list.map((f) => {
    const log = f && f.scale === 'log';
    const num = (v) => (typeof v === 'number' && Number.isFinite(v) ? v : null);
    const lo = log ? num(f.log_min) : num(f.range_min), hi = log ? num(f.log_max) : num(f.range_max);
    if (lo === null || hi === null) throw new Error(`the overlay's field ${f && f.name} has no value range`);
    return {
      name: String(f.name || ''), path: String(f.path || ''), scale: log ? 'log' : 'linear', lo, hi,
      valueMin: num(f.range_min), valueMax: num(f.range_max), threshold: num(f.threshold),
      nonpositive: Number(f.nonpositive_voxels) || 0, bunit: String(f.bunit || ''), source: String(f.source || 'cube'),
      pyramidLevel: Number.isInteger(f.pyramid_level) ? f.pyramid_level : null, fileRevision: String(f.file_revision || ''),
      note: String(f.source_note || ''),
    };
  });
}

/** One channel of interleaved voxels (`C` a voxel) as an array of its own. */
export function channelOf(data, C, c) {
  const n = Math.floor(data.length / C);
  const out = new data.constructor(n);
  for (let i = 0, j = c; i < n; i++, j += C) out[i] = data[j];
  return out;
}

/**
 * Everything an overlay needs computed from its voxels before it is written
 * (the cube-prep worker runs it, cube-prep.js prepareCube): `macro` the skip
 * grid, four floats a cell (field i's largest normalised value, widened by a
 * voxel as volume.js buildMacrocells; -1 in the channels of no field),
 * `apron` (apronBlocksC, for the storage `store`), `quantiles` (each field's
 * OVERLAY_CMAX_QUANTILE of its values above `aboves[i]`) and `ms`.
 */
export function prepareOverlay(data, dims, encoding, { cell = 8, channels, fields, aboves = [], store = null } = {}) {
  const t0 = Date.now();
  const C = channels, nf = Math.min(fields || C, C);
  let cdims = null, out = null;
  const quantiles = [];
  for (let c = 0; c < nf; c++) {
    const one = channelOf(data, C, c);
    const m = buildMacrocells(one, dims, encoding, cell);
    if (!out) { cdims = m.dims; out = new Float32Array(m.data.length * 4).fill(-1); }
    for (let k = 0; k < m.data.length; k++) out[4 * k + c] = m.data[k];
    quantiles.push(normQuantile(one, encoding, OVERLAY_CMAX_QUANTILE, 2_000_000, Number.isFinite(aboves[c]) ? aboves[c] : 0));
  }
  const apron = store ? apronBlocksC(data, dims, store, C) : null;
  return { macro: { data: out, dims: cdims, channels: 4 }, apron, quantiles, store: store ? store.slice() : null, cell, channels: C, ms: Date.now() - t0 };
}

/**
 * volume.js apronBlocks for interleaved voxels of `C` channels: the plane
 * just past the cube's last voxel on each axis where the storage is longer,
 * a copy of that last voxel (all its channels). [{offset, size, data}],
 * `data` C values a voxel.
 */
export function apronBlocksC(data, dims, store, C) {
  const [w, h, d] = dims;
  const ex = w < store[0] ? 1 : 0, ey = h < store[1] ? 1 : 0, ez = d < store[2] ? 1 : 0;
  const Arr = data.constructor;
  const put = (a, k, x, y, z) => {
    const s = ((Math.min(z, d - 1) * h + Math.min(y, h - 1)) * w + Math.min(x, w - 1)) * C;
    for (let c = 0; c < C; c++) a[k * C + c] = data[s + c];
  };
  const out = [];
  if (ex) {
    const a = new Arr(h * d * C);
    for (let z = 0; z < d; z++) for (let y = 0; y < h; y++) put(a, z * h + y, w - 1, y, z);
    out.push({ offset: [w, 0, 0], size: [1, h, d], data: a });
  }
  if (ey) {
    const W = w + ex;
    const a = new Arr(W * d * C);
    for (let z = 0; z < d; z++) for (let x = 0; x < W; x++) put(a, z * W + x, x, h - 1, z);
    out.push({ offset: [0, h, 0], size: [W, 1, d], data: a });
  }
  if (ez) {
    const W = w + ex, H = h + ey;
    const a = new Arr(W * H * C);
    for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) put(a, y * W + x, x, y, d - 1);
    out.push({ offset: [0, 0, d], size: [W, H, 1], data: a });
  }
  return out;
}

/** The LUT atlas: LUT_ROWS rows of 256 RGBA8 texels, row i field i's map
 *  (the rows of no field are black). */
export function lutAtlas(maps) {
  const out = new Uint8Array(256 * 4 * LUT_ROWS);
  maps.slice(0, LUT_ROWS).forEach((m, i) => { if (m) out.set(colormapTable(m), i * 256 * 4); });
  return out;
}

/** A field's texture coordinate row in the atlas (the texel row's centre). */
export const lutRow = (i) => (i + 0.5) / LUT_ROWS;

/**
 * The look a field starts with: colour map i of OVERLAY_DEFAULT_MAPS, the
 * threshold the backend gave it (its default: the 99th percentile for a
 * simulation volume), the colour range up to `cmaxNorm` (the 99 % of its
 * values above that threshold), the stretch a cube of its scale starts with
 * (on log codes Linear, the desktop's Log; else Sqrt), opacity 0.4.
 */
export function defaultLook(field, i, { cmaxNorm = 1, stretch = 'sqrt' } = {}) {
  const thr = Number.isFinite(field.threshold) ? dataToNorm(field.threshold, field.lo, field.hi, field.scale) : 0.1;
  const thrNorm = clamp01(Number.isFinite(thr) ? thr : 0);
  return {
    colormap: OVERLAY_DEFAULT_MAPS[i % OVERLAY_DEFAULT_MAPS.length], thrNorm,
    cmaxNorm: Math.max(clamp01(cmaxNorm), Math.min(1, thrNorm + 0.01)),
    stretch: field.scale === 'log' ? 'linear' : stretch, opacity: 0.4, scale: 'auto', on: true,
  };
}

const clamp01 = (v) => Math.max(0, Math.min(1, v));

/**
 * A look kept for a field (as data values, so a field reloaded on another
 * scale or range comes back as it was) put on `field`'s codes: {thrNorm,
 * cmaxNorm} clamped, the colour max above the threshold; null when the kept
 * values are not finite.
 */
export function lookOnField(kept, field) {
  if (!kept || !Number.isFinite(kept.threshold) || !Number.isFinite(kept.colourMax)) return null;
  const norm = (v) => { const n = dataToNorm(v, field.lo, field.hi, field.scale); return Number.isFinite(n) ? clamp01(n) : 0; };
  const thrNorm = norm(kept.threshold);
  let cmaxNorm = norm(kept.colourMax);
  if (cmaxNorm <= thrNorm) cmaxNorm = Math.min(1, thrNorm + 0.01);
  return { thrNorm, cmaxNorm };
}

/** A field's look as kept (data values in place of the codes' 0..1). */
export function keptLook(look, field) {
  return { ...look, threshold: normToData(look.thrNorm, field.lo, field.hi, field.scale),
    colourMax: normToData(Math.max(look.cmaxNorm, look.thrNorm + 1e-3), field.lo, field.hi, field.scale) };
}

/**
 * The draw's per-field uniforms: vec3s (x, y, z for fields 1-3) of the
 * threshold, colour window, stretch parameter and opacity, an ivec3 of the
 * stretch codes, and the field count. A field switched off (`on` false), or
 * a channel with no field, has a threshold of 2: never drawn, never keeping
 * a skip-grid cell. `stretchCode(name)` and `stretchA(look, field)` come
 * from levels.js (main.js passes them).
 */
export function overlayUniforms(looks, fields, { stretchCode, stretchA }) {
  const thr = [2, 2, 2], cmin = [0, 0, 0], cmax = [1, 1, 1], a = [1, 1, 1], op = [0, 0, 0], st = [0, 0, 0];
  for (let i = 0; i < Math.min(3, fields.length); i++) {
    const L = looks[i];
    if (!L) continue;
    const t = clamp01(L.thrNorm);
    if (L.on !== false) { thr[i] = t; op[i] = L.opacity; }
    cmin[i] = t;
    cmax[i] = Math.max(L.cmaxNorm, t + 1e-3);
    st[i] = stretchCode(L.stretch) | 0;
    const sa = stretchA(L, fields[i]);
    a[i] = Number.isFinite(sa) ? sa : 1;
  }
  return { thr, cmin, cmax, stretchA: a, opacity: op, stretch: st, n: fields.length };
}

/** Is the overlay drawable: two fields at least, one of them switched on? */
export function anyOn(looks, n) {
  for (let i = 0; i < Math.min(n, looks.length); i++) if (looks[i] && looks[i].on !== false) return true;
  return false;
}

/**
 * The headset hub's word on the overlay (the `overlay` key of a `look`
 * event): {composite, fields: [{name, path, colormap, scale, threshold,
 * colour_max (values), opacity, stretch, on}]}, and its one-line summary.
 */
export function hubOverlay(fields, looks, composite) {
  const out = fields.map((f, i) => {
    const L = looks[i] || {};
    const k = keptLook(L, f);
    const fin = (v) => (Number.isFinite(v) ? v : null);
    return { name: f.name, path: f.path, colormap: L.colormap || '', scale: f.scale, threshold: fin(k.threshold),
      colour_max: fin(k.colourMax), opacity: fin(L.opacity), stretch: L.stretch || '', on: L.on !== false };
  });
  const names = out.map((f) => `${f.name}${f.on ? '' : ' (off)'} ${f.colormap}${f.scale === 'log' ? ' log' : ''}`);
  return { payload: { composite, fields: out }, summary: `Overlay (${composite === 'mip' ? 'MIP' : 'emission'}): ${names.join(', ')}`.slice(0, 280) };
}

/**
 * Each field's value at box point `p` (0..1 over the cube), filtered as the
 * GPU filters the overlay's texture (trilinear on the stored codes, as
 * selection.js sampleNorm does for one cube): [{name, value (a data value,
 * NaN where blank), unit}]. `ov` {data (interleaved), dims, channels,
 * encoding, fields}. The readout's lines with the overlay on.
 */
export function sampleFields(ov, p, texScale = [1, 1, 1]) {
  const { data, dims, channels: C, encoding, fields } = ov;
  const [w, h] = dims;
  const idx = [0, 1, 2].map((i) => {
    const x = p[i] * texScale[i] * dims[i] - 0.5;
    const i0 = Math.floor(x);
    const n = dims[i];
    return [Math.min(n - 1, Math.max(0, i0)), Math.min(n - 1, Math.max(0, i0 + 1)), x - i0];
  });
  const [x0, x1, fx] = idx[0], [y0, y1, fy] = idx[1], [z0, z1, fz] = idx[2];
  const lerp = (a, b, f) => a + (b - a) * f;
  return fields.map((f, c) => {
    const v = (x, y, z) => {
      const k = data[((z * h + y) * w + x) * C + c];
      return encoding === 'uint8' ? k : halfToFloat(k);
    };
    const s = lerp(lerp(lerp(v(x0, y0, z0), v(x1, y0, z0), fx), lerp(v(x0, y1, z0), v(x1, y1, z0), fx), fy),
      lerp(lerp(v(x0, y0, z1), v(x1, y0, z1), fx), lerp(v(x0, y1, z1), v(x1, y1, z1), fx), fy), fz);
    const n = encoding === 'uint8' ? (s - 1) / 254 : s;
    return { name: f.name, value: n >= 0 && Number.isFinite(n) ? normToData(n, f.lo, f.hi, f.scale) : NaN, unit: f.bunit || '' };
  });
}

// A label mask over the cube (issue #282): the sources of a SoFiA-2 or
// desktop mask as the backend sends them (/v1/cube/mask_layer_bin: each
// source's sorted linear indices of the full grid, x fastest), drawn on the
// page's reduced grid, picked by pointing, refined with the Kinematic Lasso
// (New / Add / Subtract, with undo) and saved back (/v1/cube/mask_layer_save:
// the edited sources' voxel lists).
//
// Drawing: the mask bank of the capacity (one byte a reduced voxel, the
// selection's storage, written with texSubImage3D, never allocated in VR,
// #229) holds ENC values: 0 none, INSIDE a voxel of a source, SHELL one on a
// source's surface (a 6-neighbour outside every source), SELECTED the source
// being refined (and the lasso's selection, which the renderer adds).
// Pure: tested by node (backend/tests/vr/mask-layer.test.mjs).

import { pixelToModel } from './catalogue.js';

export const MODES = ['off', 'outline', 'only', 'residual'];
export const MODE_NAMES = { off: 'Off', outline: 'Outline', only: 'Mask only', residual: 'Residuals' };
/** The shader's numbers for the modes (renderer.js uMaskMode). */
export const MODE_CODE = { off: 0, outline: 1, only: 2, residual: 3 };
export const ENC = { none: 0, inside: 64, shell: 112, selected: 255 };
export const MAX_LABELS = 60;
export const UNDO_DEPTH = 32;

/**
 * The backend's answer: uint32 little-endian JSON length, the JSON (padded to
 * 4 bytes), the indices (uint32). Returns {shape, maxId, nVoxels, meta, sources:
 * [{id, count, bbox, centroid, idx (a view, sorted)}]}.
 */
export function parseLayer(buf) {
  const ab = buf instanceof ArrayBuffer ? buf : buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength);
  if (ab.byteLength < 4) throw new Error('mask layer: too short');
  const n = new DataView(ab).getUint32(0, true);
  if (n > ab.byteLength - 4) throw new Error('mask layer: bad header length');
  const head = JSON.parse(new TextDecoder().decode(new Uint8Array(ab, 4, n)));
  const pos = 4 + n + ((4 - ((4 + n) % 4)) % 4);
  const all = new Uint32Array(ab, pos, Math.max(0, (ab.byteLength - pos) >> 2));
  if (all.length !== head.n_voxels) throw new Error(`mask layer: ${all.length} indices for ${head.n_voxels} voxels`);
  const sources = head.sources.map((s) => ({ id: s.id, count: s.count, bbox: s.bbox, centroid: s.centroid,
    idx: all.subarray(s.offset, s.offset + s.count) }));
  const meta = { ...head };
  delete meta.sources;
  return { shape: head.shape, maxId: head.max_id, nVoxels: head.n_voxels, meta, sources };
}

/** Is `v` in the sorted `a`? */
export function sortedHas(a, v) {
  let lo = 0, hi = a.length - 1;
  while (lo <= hi) {
    const m = (lo + hi) >> 1;
    const x = a[m];
    if (x === v) return true;
    if (x < v) lo = m + 1; else hi = m - 1;
  }
  return false;
}

/** The sorted linear indices of a lasso mask (float32 > 0.5, x fastest, `dims`
 *  [w, h, d] at full resolution from `origin`) on a grid of `shape`. */
export function growIndices(mask, dims, origin, shape) {
  const [w, h, d] = dims, [nx, ny] = shape;
  const out = [];
  for (let z = 0; z < d; z++) {
    for (let y = 0; y < h; y++) {
      const row = (z * h + y) * w;
      const base = ((origin[2] + z) * ny + origin[1] + y) * nx + origin[0];
      for (let x = 0; x < w; x++) if (mask[row + x] > 0.5) out.push(base + x);
    }
  }
  return Uint32Array.from(out);       // ascending: z, then y, then x
}

function bboxOf(idx, shape) {
  const [nx, ny] = shape;
  if (!idx.length) return null;
  const b = [Infinity, -Infinity, Infinity, -Infinity, Infinity, -Infinity];
  let sx = 0, sy = 0, sz = 0;
  for (let i = 0; i < idx.length; i++) {
    const v = idx[i], x = v % nx, r = (v - x) / nx, y = r % ny, z = (r - y) / ny;
    if (x < b[0]) b[0] = x; if (x > b[1]) b[1] = x;
    if (y < b[2]) b[2] = y; if (y > b[3]) b[3] = y;
    if (z < b[4]) b[4] = z; if (z > b[5]) b[5] = z;
    sx += x; sy += y; sz += z;
  }
  return { bbox: b, centroid: [sx / idx.length, sy / idx.length, sz / idx.length] };
}

/** Sorted union, difference and intersection of sorted Uint32Arrays. */
export function union(a, b) {
  const out = new Uint32Array(a.length + b.length);
  let i = 0, j = 0, k = 0;
  while (i < a.length && j < b.length) {
    if (a[i] < b[j]) out[k++] = a[i++];
    else if (a[i] > b[j]) out[k++] = b[j++];
    else { out[k++] = a[i++]; j++; }
  }
  while (i < a.length) out[k++] = a[i++];
  while (j < b.length) out[k++] = b[j++];
  return out.slice(0, k);
}
/** `a[0..n)` without the elements of the sorted `b`, compacted in place;
 *  returns the new length. */
export function minusInto(a, n, b) {
  let i = 0, j = 0, k = 0;
  while (i < n) {
    while (j < b.length && b[j] < a[i]) j++;
    if (j < b.length && b[j] === a[i]) { i++; continue; }
    a[k++] = a[i++];
  }
  return k;
}
export function minus(a, b) {
  const out = new Uint32Array(a.length);
  let i = 0, j = 0, k = 0;
  while (i < a.length) {
    while (j < b.length && b[j] < a[i]) j++;
    if (j < b.length && b[j] === a[i]) { i++; continue; }
    out[k++] = a[i++];
  }
  return out.slice(0, k);
}

/**
 * The mask's sources and their edits. `parsed`: parseLayer's answer.
 * Edits never take a voxel from another source: New and Add claim free
 * voxels (and, for New, the source's own); Subtract gives voxels back.
 */
export class MaskLayer {
  constructor(parsed) {
    this.shape = parsed.shape.slice();
    this.meta = parsed.meta || {};
    this.origMaxId = parsed.maxId;
    this.maxId = parsed.maxId;
    this.byId = new Map();
    for (const s of parsed.sources) {
      this.byId.set(s.id, { id: s.id, idx: s.idx, bbox: s.bbox, centroid: s.centroid, count: s.count, edited: false, isNew: false });
    }
    this.undoStack = [];
    this.version = 0;
  }
  /** The sources with voxels, by ID. */
  ids() { return [...this.byId.values()].filter((s) => s.count > 0).map((s) => s.id).sort((a, b) => a - b); }
  get(id) { return this.byId.get(id) || null; }
  /** The source holding full voxel `v` (linear index), or 0. */
  idAt(v) {
    const [nx, ny] = this.shape;
    const x = v % nx, r = (v - x) / nx, y = r % ny, z = (r - y) / ny;
    for (const s of this.byId.values()) {
      const b = s.bbox;
      if (!s.count || x < b[0] || x > b[1] || y < b[2] || y > b[3] || z < b[4] || z > b[5]) continue;
      if (sortedHas(s.idx, v)) return s.id;
    }
    return 0;
  }
  /**
   * `grow` without the voxels of every source but `except`: only the sources
   * whose box meets the grow's are looked at, and they are taken out of one
   * copy of the grow, in place (a mask of thousands of sources stays cheap).
   */
  free(grow, except = 0) {
    if (!grow.length) return grow;
    const g = bboxOf(grow, this.shape).bbox;
    let out = null, n = grow.length;
    for (const s of this.byId.values()) {
      if (s.id === except || !s.count) continue;
      const b = s.bbox;
      if (b[1] < g[0] || b[0] > g[1] || b[3] < g[2] || b[2] > g[3] || b[5] < g[4] || b[4] > g[5]) continue;
      if (!out) out = Uint32Array.from(grow);
      n = minusInto(out, n, s.idx);
      if (!n) break;
    }
    return out ? out.subarray(0, n) : grow;
  }
  _set(s, idx) {
    s.idx = idx;
    s.count = idx.length;
    const bc = bboxOf(idx, this.shape);
    s.bbox = bc ? bc.bbox : [0, -1, 0, -1, 0, -1];
    s.centroid = bc ? bc.centroid : s.centroid;
    s.edited = true;
    this.version++;
  }
  /**
   * The lasso's grow (sorted indices) applied to source `id` (null: a new
   * source) by `op` ('new' | 'add' | 'carve'). Returns {id, changed (voxels
   * that changed hands)}; the step can be undone.
   */
  apply(id, grow, op) {
    let s = id ? this.byId.get(id) : null;
    if (id && !s) throw new Error(`no source ${id} in the mask`);
    if (!s) {
      if (op === 'carve') return { id: 0, changed: 0 };
      const nid = this.maxId + 1;
      s = { id: nid, idx: new Uint32Array(0), bbox: [0, -1, 0, -1, 0, -1], centroid: [0, 0, 0], count: 0, edited: true, isNew: true };
      this.byId.set(nid, s);
      this.maxId = nid;
      this.undoStack.push({ id: nid, before: null, created: true });
    } else {
      this.undoStack.push({ id: s.id, before: s.idx, created: false, edited: s.edited });
    }
    if (this.undoStack.length > UNDO_DEPTH) this.undoStack.shift();
    const before = s.count;
    let next;
    if (op === 'carve') next = minus(s.idx, grow);
    else if (op === 'add') next = union(s.idx, this.free(grow, s.id));
    else next = this.free(grow, s.id);
    this._set(s, next);
    return { id: s.id, changed: Math.abs(next.length - before) || (op === 'new' ? next.length : 0) };
  }
  canUndo() { return this.undoStack.length > 0; }
  /** The last step taken back; returns the source's ID (0 when nothing). */
  undo() {
    const u = this.undoStack.pop();
    if (!u) return 0;
    if (u.created) {
      this.byId.delete(u.id);
      if (u.id === this.maxId) this.maxId = Math.max(this.origMaxId, ...[...this.byId.keys()]);
      this.version++;
      return u.id;
    }
    const s = this.byId.get(u.id);
    this._set(s, u.before);
    s.edited = u.edited;
    return u.id;
  }
  /** What a save sends: the edited sources with voxels, and the original ones emptied. */
  changes() {
    const edits = [], removed = [];
    for (const s of this.byId.values()) {
      if (!s.edited) continue;
      if (s.count) edits.push(s);
      else if (!s.isNew) removed.push(s.id);
    }
    edits.sort((a, b) => a.id - b.id);
    return { edits, removed: removed.sort((a, b) => a - b) };
  }
  /**
   * The body of /v1/cube/mask_layer_save: uint32 JSON length, the JSON
   * ({...meta, edits: [{id, count, new}], removed}), padding to 4 bytes, each
   * edited source's indices in the order of `edits`.
   */
  saveBody(meta = {}) {
    const { edits, removed } = this.changes();
    const head = { ...meta, edits: edits.map((s) => ({ id: s.id, count: s.count, new: s.isNew })), removed };
    let text = new TextEncoder().encode(JSON.stringify(head));
    const pad = (4 - ((4 + text.length) % 4)) % 4;
    const total = 4 + text.length + pad + 4 * edits.reduce((n, s) => n + s.count, 0);
    const out = new Uint8Array(total);
    new DataView(out.buffer).setUint32(0, text.length, true);
    out.set(text, 4);
    out.fill(32, 4 + text.length, 4 + text.length + pad);
    let pos = 4 + text.length + pad;
    for (const s of edits) {
      const view = new Uint8Array(s.idx.buffer, s.idx.byteOffset, s.idx.byteLength);
      out.set(view, pos);
      pos += s.idx.byteLength;
    }
    text = null;
    return { body: out, edits: edits.length, removed: removed.length };
  }
}

/**
 * The layer on the reduced grid (`factors`, `reduced` its shape): {data
 * (Uint8Array over the box), origin, dims} with ENC values, `selected` the
 * source drawn as SELECTED; null when the mask is empty. A reduced voxel is
 * the mask's when any of its full voxels is (as reduceMask does).
 */
export function encodeLayer(layer, factors, reduced, selected = 0) {
  const lo = [Infinity, Infinity, Infinity], hi = [-1, -1, -1];
  const srcs = [...layer.byId.values()].filter((s) => s.count);
  if (!srcs.length) return null;
  for (const s of srcs) {
    for (let a = 0; a < 3; a++) {
      lo[a] = Math.min(lo[a], Math.floor(s.bbox[2 * a] / factors[a]));
      hi[a] = Math.max(hi[a], Math.min(reduced[a] - 1, Math.floor(s.bbox[2 * a + 1] / factors[a])));
    }
  }
  const dims = [0, 1, 2].map((a) => hi[a] - lo[a] + 1);
  const occ = new Uint8Array(dims[0] * dims[1] * dims[2]);
  const [nx, ny] = layer.shape;
  const mark = (s, value) => {
    const idx = s.idx;
    for (let i = 0; i < idx.length; i++) {
      const v = idx[i], x = v % nx, r = (v - x) / nx, y = r % ny, z = (r - y) / ny;
      const rx = Math.floor(x / factors[0]) - lo[0], ry = Math.floor(y / factors[1]) - lo[1], rz = Math.floor(z / factors[2]) - lo[2];
      if (rx < 0 || ry < 0 || rz < 0 || rx >= dims[0] || ry >= dims[1] || rz >= dims[2]) continue;
      occ[(rz * dims[1] + ry) * dims[0] + rx] = value;
    }
  };
  for (const s of srcs) mark(s, 1);
  const data = new Uint8Array(occ.length);
  const [w, h, d] = dims;
  for (let z = 0; z < d; z++) {
    for (let y = 0; y < h; y++) {
      for (let x = 0; x < w; x++) {
        const k = (z * h + y) * w + x;
        if (!occ[k]) continue;
        const out = x === 0 || !occ[k - 1] || x === w - 1 || !occ[k + 1] || y === 0 || !occ[k - w] || y === h - 1 || !occ[k + w]
          || z === 0 || !occ[k - w * h] || z === d - 1 || !occ[k + w * h];
        data[k] = out ? ENC.shell : ENC.inside;
      }
    }
  }
  const sel = selected ? layer.get(selected) : null;
  if (sel && sel.count) {
    const tmp = occ.fill(0);
    mark(sel, 1);
    for (let k = 0; k < tmp.length; k++) if (tmp[k]) data[k] = ENC.selected;
  }
  return { data, origin: lo, dims };
}

/** The layer of a mask with no voxel left: one empty voxel, so the renderer
 *  keeps the layer (and its mode: Mask only draws nothing) rather than none. */
export function emptyEncoding() {
  return { data: new Uint8Array(1), origin: [0, 0, 0], dims: [1, 1, 1] };
}

/** The skip grid's cells (`macroDims`, `cell` reduced voxels a side) holding
 *  any of an encoded box: 1 there (never skipped), 0 elsewhere. */
export function layerCells(enc, macroDims, cell) {
  const out = new Uint8Array(macroDims[0] * macroDims[1] * macroDims[2]);
  if (!enc) return out;
  const [w, h, d] = enc.dims, [ox, oy, oz] = enc.origin;
  for (let z = 0; z < d; z++) {
    const cz = Math.min(macroDims[2] - 1, Math.floor((oz + z) / cell));
    for (let y = 0; y < h; y++) {
      const cy = Math.min(macroDims[1] - 1, Math.floor((oy + y) / cell));
      const row = (z * h + y) * w;
      for (let x = 0; x < w; x++) {
        if (!enc.data[row + x]) continue;
        out[(cz * macroDims[1] + cy) * macroDims[0] + Math.min(macroDims[0] - 1, Math.floor((ox + x) / cell))] = 1;
      }
    }
  }
  return out;
}

/**
 * The first source along a box-space ray (o, d in 0..1 box coordinates)
 * inside the clipping box `clip`: {id, voxel [x, y, z], t} or null. Steps
 * of half a full voxel.
 */
export function pickSource(layer, o, d, clip = { min: [0, 0, 0], max: [1, 1, 1] }) {
  const n = layer.shape;
  let tn = -Infinity, tf = Infinity;
  for (let a = 0; a < 3; a++) {
    if (Math.abs(d[a]) < 1e-12) { if (o[a] < clip.min[a] || o[a] > clip.max[a]) return null; continue; }
    const t1 = (clip.min[a] - o[a]) / d[a], t2 = (clip.max[a] - o[a]) / d[a];
    tn = Math.max(tn, Math.min(t1, t2)); tf = Math.min(tf, Math.max(t1, t2));
  }
  tn = Math.max(tn, 0);
  if (!(tf > tn)) return null;
  const len = Math.hypot(d[0] * n[0], d[1] * n[1], d[2] * n[2]);
  const dt = 0.5 / len;
  let last = -1;
  for (let t = tn; t <= tf; t += dt) {
    const v = [0, 1, 2].map((a) => Math.min(n[a] - 1, Math.max(0, Math.floor((o[a] + d[a] * t) * n[a]))));
    const lin = (v[2] * n[1] + v[1]) * n[0] + v[0];
    if (lin === last) continue;
    last = lin;
    const id = layer.idAt(lin);
    if (id) return { id, voxel: v, t };
  }
  return null;
}

/** Labels of the sources at their centroids (model space), at most MAX_LABELS,
 *  the selected one first; inside the clipping box only. */
export function layerLabels(layer, extent, clip, selected = 0) {
  const n = layer.shape, out = [];
  const ids = layer.ids();
  ids.sort((a, b) => (a === selected ? -1 : b === selected ? 1 : a - b));
  for (const id of ids) {
    if (out.length >= MAX_LABELS) break;
    const c = layer.get(id).centroid;
    const u = [0, 1, 2].map((a) => (c[a] + 0.5) / n[a]);
    if (u.some((v, a) => v < clip.min[a] || v > clip.max[a])) continue;
    out.push({ pos: [0, 1, 2].map((a) => pixelToModel(c[a], n[a], extent[a])), text: `${id}${layer.get(id).isNew ? ' (new)' : ''}`,
      height: id === selected ? 0.03 : 0.022, anchor: 'left', color: id === selected ? 'rgb(120,255,150)' : 'rgb(255,214,120)' });
  }
  return out;
}

/** A source as a row the visit takes (source-list.js rowModel): its centroid. */
export function sourceRow(layer, id) {
  const s = layer.get(id);
  if (!s) return null;
  return { key: `m:${id}`, label: `Source ${id}`, x: s.centroid[0], y: s.centroid[1], z: s.centroid[2] };
}

/** An ellipse-like size for the visit: the source's sky box as a closed outline. */
export function sourceOutline(layer, id) {
  const s = layer.get(id);
  if (!s || !s.count) return null;
  const b = s.bbox;
  return { ellipse: [[b[0], b[2]], [b[1], b[2]], [b[1], b[3]], [b[0], b[3]], [b[0], b[2]]] };
}

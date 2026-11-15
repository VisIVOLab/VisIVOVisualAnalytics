// The source list of a catalogue over a cube (issue #279): the sources the
// backend placed in the cube's pixels (/v1/cube/catalogue_pixels, catalogue.js)
// as rows with a name, a sky position and a velocity, sorted and paged; the
// flags and notes the user gives them; the sources found by eye (the user's
// list); the visit (a source brought to the centre of the view at a
// comfortable size); and what goes to the desktop (a VOTable written by
// /v1/catalogue/classified_votable, a `classification` product on the hub).
//
// Positions: a source's x, y, z are the backend's 0-based full-resolution
// pixels (pixel i spans [i - 0.5, i + 0.5]); its lon/lat are the catalogue's
// own (in `source_frame`); the velocity is the cube's at z, from reduced_info's
// per-channel full_velocity_kms (the readout's numbers), interpolated between
// channels. Pure: tested by node (backend/tests/vr/source-list.test.mjs).

import * as M from './mat4.js';
import { pixelToModel } from './catalogue.js';
import { formatRA, formatDec } from './selection.js';

/** The flags and notes offered when nothing else is configured. */
export const DEFAULT_FLAGS = ['real', 'doubtful', 'spurious'];
export const DEFAULT_NOTES = ['blended', 'edge', 'RFI', 'extended', 'faint', 'check'];
export const MAX_FLAGS = 4;
export const MAX_NOTES = 6;
export const MAX_SET_LABEL = 12;
/** The colours of the first flags (the default set's: green, amber, red), then a palette. */
export const FLAG_COLOURS = [[0.35, 1, 0.45], [1, 0.78, 0.25], [1, 0.35, 0.35], [0.55, 0.7, 1]];
export const CURRENT_COLOUR = [1, 1, 1];
export const USER_COLOUR = [0.3, 0.95, 1];
/** At most this many markers drawn (the nearest to the focus), and labels. */
export const MAX_DRAWN = 3000;
export const MAX_LABELS = 40;
/** The bytes of the sources sent in the hub event (its payload is 16 KiB at most). */
export const HUB_SOURCES_BYTES = 11000;

/**
 * A flag or note set from a comma-separated text (the page's settings):
 * trimmed, without repeats, at most `max` entries of at most MAX_SET_LABEL
 * characters; `defaults` when nothing usable is left.
 */
export function parseSet(text, defaults, max) {
  const seen = new Set();
  const out = [];
  for (const raw of String(text ?? '').split(',')) {
    const t = raw.trim().slice(0, MAX_SET_LABEL);
    if (!t || seen.has(t.toLowerCase())) continue;
    seen.add(t.toLowerCase());
    out.push(t);
    if (out.length >= max) break;
  }
  return out.length ? out : defaults.slice(0, max);
}

/** The cube's velocity (km/s) at full channel z (fractional), or null. */
export function velocityAt(info, z) {
  const fv = info && info.axes && info.axes.z && info.axes.z.full_velocity_kms;
  if (!Array.isArray(fv) || !fv.length || z === null || z === undefined || !Number.isFinite(z)) return null;
  if (z <= 0) return fv[0];
  if (z >= fv.length - 1) return fv[fv.length - 1];
  const i = Math.floor(z), f = z - i;
  return fv[i] + f * (fv[i + 1] - fv[i]);
}

/** A source's key: its catalogue row ('c:12'), else its place in the overlay ('o:3'). */
export function sourceKey(s, i) {
  return Number.isInteger(s.row) ? `c:${s.row}` : `o:${i}`;
}

/**
 * The rows of the list: the overlay's sources (catalogue_pixels' answer) then
 * the user's found sources. Each {key, index (in ov.sources, -1 for a found
 * one), label, x, y, z, lon, lat, frame, velocity, origin ('catalogue' |
 * 'overlay' | 'user'), row, band}.
 */
export function sourceRows(ov, info, user = []) {
  const rows = [];
  if (ov && ov.valid) {
    const frame = ov.source_frame || ov.frame || 'icrs';
    (ov.sources || []).forEach((s, i) => {
      const z = s.z === null || s.z === undefined ? null : s.z;
      rows.push({ key: sourceKey(s, i), index: i, label: String(s.label || `#${i}`), x: s.x, y: s.y, z,
        lon: Number.isFinite(s.lon) ? s.lon : null, lat: Number.isFinite(s.lat) ? s.lat : null, frame,
        velocity: velocityAt(info, z), origin: Number.isInteger(s.row) ? 'catalogue' : 'overlay',
        row: Number.isInteger(s.row) ? s.row : null, band: s.band || '' });
    });
  }
  for (const u of user) rows.push({ ...u, index: -1, origin: 'user', row: null, band: '' });
  return rows;
}

export const SORT_KEYS = ['name', 'lon', 'lat', 'velocity', 'flag'];

/**
 * The rows sorted by `key` (SORT_KEYS) in direction `dir` (1 up, -1 down);
 * `flagOf(key)` the row's flag. Rows with no value go last either way, ties
 * keep the list's order. A new array.
 */
export function sortRows(rows, key, dir = 1, flagOf = () => '', flagSet = DEFAULT_FLAGS) {
  const val = (r) => {
    if (key === 'name') return r.label;
    if (key === 'flag') { const f = flagOf(r.key); const i = flagSet.indexOf(f); return f ? (i < 0 ? flagSet.length : i) : null; }
    const v = r[key];
    return v === null || v === undefined || !Number.isFinite(v) ? null : v;
  };
  const coll = typeof Intl !== 'undefined' ? new Intl.Collator('en', { numeric: true, sensitivity: 'base' }) : null;
  return rows.map((r, i) => ({ r, i, v: val(r) })).sort((a, b) => {
    if (a.v === null && b.v === null) return a.i - b.i;
    if (a.v === null) return 1;
    if (b.v === null) return -1;
    const c = typeof a.v === 'string' ? (coll ? coll.compare(a.v, b.v) : a.v.localeCompare(b.v)) : a.v - b.v;
    return c ? dir * c : a.i - b.i;
  }).map((x) => x.r);
}

/** Pages of `per` rows for `n` rows (at least 1). */
export const pageCount = (n, per) => Math.max(1, Math.ceil(n / per));

/** The page holding the row of `key` (0 when it is not listed). */
export function pageOfKey(rows, key, per) {
  const i = rows.findIndex((r) => r.key === key);
  return i < 0 ? 0 : Math.floor(i / per);
}

/** The key `dir` (+1 next, -1 previous) rows from `key` in the sorted rows,
 *  wrapping at the ends; the first row's when `key` is not listed; null for none. */
export function stepKey(rows, key, dir) {
  if (!rows.length) return null;
  const i = rows.findIndex((r) => r.key === key);
  if (i < 0) return rows[dir < 0 ? rows.length - 1 : 0].key;
  return rows[(i + dir + rows.length) % rows.length].key;
}

/** The two position texts of a row: RA/Dec sexagesimal (ICRS), else Galactic degrees. */
export function positionText(r) {
  if (r.lon === null || r.lat === null || r.lon === undefined) return ['—', '—'];
  if (r.frame === 'galactic') return [`l ${r.lon.toFixed(4)}`, `b ${r.lat >= 0 ? '+' : ''}${r.lat.toFixed(4)}`];
  return [formatRA(r.lon), formatDec(r.lat)];
}

/** The velocity's text (km/s, one decimal), '—' when the source has none. */
export function velocityText(v) {
  return v === null || v === undefined || !Number.isFinite(v) ? '—' : v.toFixed(1);
}

/** The column names of the position for a frame. */
export const positionNames = (frame) => (frame === 'galactic' ? ['GLON', 'GLAT'] : ['RA', 'Dec']);

// ── The work kept in the browser ────────────────────────────────────────────

/**
 * A fingerprint of an overlay's sources: their count and an FNV-1a hash of
 * their labels and positions (pixels to 0.01), so the flags kept for one
 * catalogue are never read back onto another of the same name.
 */
export function overlayFingerprint(ov) {
  const srcs = ov && ov.valid ? ov.sources || [] : [];
  let h = 0x811c9dc5;
  const mix = (text) => {
    for (let i = 0; i < text.length; i++) { h ^= text.charCodeAt(i); h = Math.imul(h, 0x01000193) >>> 0; }
  };
  for (const s of srcs) {
    const z = s.z === null || s.z === undefined ? '-' : Number(s.z).toFixed(2);
    mix(`${s.label || ''}|${Number(s.x).toFixed(2)}|${Number(s.y).toFixed(2)}|${z};`);
  }
  return `${srcs.length}:${h.toString(16).padStart(8, '0')}`;
}

/** The storage key of the work on overlay `ov` over the cube at `cubePath` (its full path). */
export function storageKey(cubePath, ov, prefix = 'visivo-vr-sources:') {
  return `${prefix}${cubePath || ''}|${ov ? overlayFingerprint(ov) : 'none'}`;
}

// ── Classification ──────────────────────────────────────────────────────────

/**
 * The flags and notes given (a source keeps one flag and one note; pressing
 * the one it has takes it back). `flagSet`, `noteSet`: the offered sets.
 */
export class Classification {
  constructor(flagSet = DEFAULT_FLAGS, noteSet = DEFAULT_NOTES) {
    this.flagSet = flagSet.slice();
    this.noteSet = noteSet.slice();
    this.marks = new Map();       // key -> {flag, note}
    this.version = 0;             // bumped on every change (what was sent is compared to it)
  }
  get(key) { return this.marks.get(key) || { flag: '', note: '' }; }
  flagOf(key) { return this.get(key).flag; }
  _set(key, m) {
    if (!m.flag && !m.note) this.marks.delete(key); else this.marks.set(key, m);
    this.version++;
  }
  /** Gives `flag` to `key`, or takes it back when it has it. Returns the new flag. */
  toggleFlag(key, flag) {
    const m = this.get(key);
    const next = m.flag === flag ? '' : flag;
    this._set(key, { ...m, flag: next });
    return next;
  }
  toggleNote(key, note) {
    const m = this.get(key);
    const next = m.note === note ? '' : note;
    this._set(key, { ...m, note: next });
    return next;
  }
  /** Forgets a key (a found source removed). */
  forget(key) { if (this.marks.delete(key)) this.version++; }
  /** {flag: count} over the set, and how many are flagged; `only(key)`
   *  false leaves a key out (the found sources, counted apart). */
  counts(only = null) {
    const out = Object.fromEntries(this.flagSet.map((f) => [f, 0]));
    let flagged = 0;
    for (const [k, m] of this.marks.entries()) {
      if (!m.flag || (only && !only(k))) continue;
      flagged++;
      out[m.flag] = (out[m.flag] || 0) + 1;
    }
    return { byFlag: out, flagged };
  }
  /** A JSON-able copy (kept by the page across a reload). */
  toJSON() { return { flags: this.flagSet, notes: this.noteSet, marks: [...this.marks.entries()] }; }
  /** Marks read back (only of keys still listed, with flags and notes still offered). */
  restore(saved, keys = null) {
    if (!saved || !Array.isArray(saved.marks)) return 0;
    let n = 0;
    for (const [k, m] of saved.marks) {
      if (keys && !keys.has(k)) continue;
      const flag = this.flagSet.includes(m && m.flag) ? m.flag : '';
      const note = this.noteSet.includes(m && m.note) ? m.note : '';
      if (flag || note) { this.marks.set(k, { flag, note }); n++; }
    }
    if (n) this.version++;
    return n;
  }
}

/** The colour of a flag: its place in the set (green, amber, red, blue). */
export function flagColour(flag, flagSet) {
  const i = flagSet.indexOf(flag);
  return i < 0 ? null : FLAG_COLOURS[i % FLAG_COLOURS.length];
}

const r6 = (v) => (v === null || v === undefined || !Number.isFinite(v) ? null : Math.round(v * 1e6) / 1e6);
const r2 = (v) => (v === null || v === undefined || !Number.isFinite(v) ? null : Math.round(v * 100) / 100);

/** A row as the backend's ClassifiedSource. */
export function classifiedSource(r, mark) {
  const s = { origin: r.origin, label: String(r.label || '').slice(0, 200), lon: r6(r.lon), lat: r6(r.lat),
    x: r2(r.x), y: r2(r.y), z: r2(r.z), velocity_kms: r2(r.velocity), flag: mark.flag || '', note: mark.note || '' };
  if (r.frame === 'icrs' || r.frame === 'galactic') s.frame = r.frame;
  if (r.origin === 'catalogue') s.row = r.row;
  return s;
}

/**
 * The request of /v1/catalogue/classified_votable: the classified rows and
 * every found source; for an overlay with no catalogue behind it (VLKB, a
 * cross-match), every row (the table is then made of them).
 */
export function votableRequest({ datasetId, catalogueId = '', sourceFrame = 'icrs', rows, cls, flagColumn = 'vr_flag', basename = '' }) {
  const sources = [];
  for (const r of rows) {
    const m = cls.get(r.key);
    const keep = r.origin === 'user' || (r.origin === 'overlay' && !catalogueId) || m.flag || m.note;
    if (keep) sources.push(classifiedSource(r, m));
  }
  return { dataset_id: datasetId, catalogue_id: catalogueId, source_frame: sourceFrame === 'galactic' ? 'galactic' : 'icrs',
    flag_set: cls.flagSet.slice(), flag_column: flagColumn, sources, output_basename: basename };
}

/**
 * The `classification` part of the hub event (the desktop's catalogue table):
 * the classified and found sources (label, sky position in its frame, pixel,
 * velocity, flag, note), at most HUB_SOURCES_BYTES of JSON (`truncated` then
 * says the VOTable holds the rest), the counts of the catalogue's sources
 * per flag (`flagged` of them), the found ones (`found`), the sets and the catalogue.
 */
export function hubClassification({ rows, cls, catalogue = {}, flagColumn = 'vr_flag', maxBytes = HUB_SOURCES_BYTES }) {
  const out = [];
  let bytes = 2, truncated = false;
  const enc = typeof TextEncoder !== 'undefined' ? new TextEncoder() : null;
  for (const r of rows) {
    const m = cls.get(r.key);
    if (r.origin !== 'user' && !m.flag && !m.note) continue;
    const s = { k: r.key, n: String(r.label || '').slice(0, 60), lon: r6(r.lon), lat: r6(r.lat), f: r.frame === 'galactic' ? 'g' : 'i',
      x: r2(r.x), y: r2(r.y), z: r2(r.z), v: r2(r.velocity), flag: m.flag || '', note: m.note || '', o: r.origin[0] };
    if (r.origin === 'catalogue') s.row = r.row;
    const text = JSON.stringify(s);
    const b = (enc ? enc.encode(text).length : text.length) + 1;
    if (bytes + b > maxBytes) { truncated = true; break; }
    bytes += b;
    out.push(s);
  }
  // The catalogue's sources counted; the found ones are counted apart.
  const c = cls.counts((k) => !k.startsWith('u:'));
  return {
    catalogue: { name: String(catalogue.name || '').slice(0, 120), path: String(catalogue.path || '').slice(0, 500),
      dataset_id: String(catalogue.dataset_id || ''), rows: rows.filter((r) => r.origin !== 'user').length },
    flag_set: cls.flagSet.slice(), note_set: cls.noteSet.slice(), column: flagColumn,
    counts: c.byFlag, flagged: c.flagged, found: rows.filter((r) => r.origin === 'user').length,
    sources: out, truncated,
  };
}

/** The line the desktop shows for it (at most 300 characters). */
export function classificationSummary(h, file = '') {
  const parts = h.flag_set.map((f) => `${h.counts[f] || 0} ${f}`);
  let s = `Classified ${h.flagged} of ${h.catalogue.rows} sources${h.catalogue.name ? ` of ${h.catalogue.name}` : ''} (${parts.join(', ')})`;
  if (h.found) s += `, ${h.found} found by eye`;
  if (file) s += ` → ${file}`;
  return s.slice(0, 300);
}

// ── Found sources ───────────────────────────────────────────────────────────

/**
 * A source found by eye: `n` its number (VR-n), `px` its full pixel
 * [x, y, z], `sky` the backend's pixel_to_sky answer (or null: no sky
 * position), `how` 'lasso' or 'probe', `extra` ({voxels, flux}).
 */
export function foundSource(n, px, info, sky, how, extra = {}) {
  const ok = sky && sky.valid;
  return { key: `u:${n}`, label: `VR-${n}`, x: px[0], y: px[1], z: px[2],
    lon: ok ? sky.ra_deg : null, lat: ok ? sky.dec_deg : null, frame: 'icrs',
    velocity: velocityAt(info, px[2]), how, ...extra };
}

/** The voxel a lasso selection is found at: its brightest voxel when the
 *  statistics give one, else the centre of its box (full pixels). */
export function lassoVoxel(lasso) {
  const pv = lasso && lasso.stats && lasso.stats.peak_voxel;
  if (Array.isArray(pv) && pv.length === 3 && pv.every(Number.isFinite)) return pv.map((v) => Math.round(v));
  if (!lasso || !lasso.origin || !lasso.dims) return null;
  return [0, 1, 2].map((i) => Math.round(lasso.origin[i] + (lasso.dims[i] - 1) / 2));
}

// ── Visit ───────────────────────────────────────────────────────────────────

/** A row's position in the cube's model space; one with no channel sits in
 *  the middle of the velocity range shown (`clip` box fractions). */
export function rowModel(r, shape, extent, clip = { min: [0, 0, 0], max: [1, 1, 1] }) {
  const z = r.z === null || r.z === undefined ? ((clip.min[2] + clip.max[2]) / 2) * shape[2] - 0.5 : r.z;
  return [pixelToModel(r.x, shape[0], extent[0]), pixelToModel(r.y, shape[1], extent[1]), pixelToModel(z, shape[2], extent[2])];
}

/**
 * The size (model units) a visit frames: 2.5 times the source's ellipse when
 * it has one, else 8 % of the sky side (at least 6 pixels).
 */
export function rowSizeModel(source, shape, extent) {
  const px = extent[0] / shape[0];
  const e = source && source.ellipse;
  if (Array.isArray(e) && e.length > 2) {
    let x0 = Infinity, x1 = -Infinity, y0 = Infinity, y1 = -Infinity;
    for (const [x, y] of e) { x0 = Math.min(x0, x); x1 = Math.max(x1, x); y0 = Math.min(y0, y); y1 = Math.max(y1, y); }
    return Math.max(6, 2.5 * Math.max(x1 - x0, y1 - y0)) * px;
  }
  return Math.max(6 * px, 0.08 * Math.max(extent[0], extent[1]));
}

/** Scale of a model matrix (its first column's length: the page's models are uniform). */
export const modelScale = (m) => Math.hypot(m[0], m[1], m[2]);

/**
 * The model that brings model point `p` to world point `target` with the
 * region `size` (model units) `targetSize` metres across, the orientation
 * kept. The scale stays within [minScale, maxScale].
 */
export function visitModel(model, p, size, target, targetSize = 0.25, minScale = 0.02, maxScale = 40) {
  const s0 = modelScale(model);
  const s1 = Math.max(minScale, Math.min(maxScale, targetSize / Math.max(size, 1e-9)));
  const k = s1 / s0;
  const out = new Float32Array(16);
  for (let i = 0; i < 12; i++) out[i] = (i % 4 === 3) ? 0 : model[i] * k;
  out[15] = 1;
  const q = M.transformPoint(out, p);       // where p lands before the translation is fixed
  out[12] = target[0] - (q[0] - 0); out[13] = target[1] - (q[1] - 0); out[14] = target[2] - (q[2] - 0);
  return out;
}

/** In the headset: where a visited source goes, `dist` metres ahead of the
 *  eyes along the gaze's horizontal direction and a little below them. */
export function visitTarget(head, dist = 0.55, drop = 0.06) {
  const pos = [head[12], head[13], head[14]];
  let f = [-head[8], 0, -head[10]];
  const n = Math.hypot(f[0], f[2]);
  f = n < 1e-6 ? [0, 0, -1] : [f[0] / n, 0, f[2] / n];
  return [pos[0] + dist * f[0], pos[1] - drop, pos[2] + dist * f[2]];
}

/** The velocity range (box fractions) moved to hold channel z, its width kept;
 *  null when it holds it already (or the source has no channel). */
export function slabFor(z, nz, clip) {
  if (z === null || z === undefined || !Number.isFinite(z)) return null;
  const w = (z + 0.5) / nz;
  if (w >= clip.min[2] && w <= clip.max[2]) return null;
  const half = (clip.max[2] - clip.min[2]) / 2;
  let lo = w - half, hi = w + half;
  if (lo < 0) { hi -= lo; lo = 0; }
  if (hi > 1) { lo -= hi - 1; hi = 1; }
  return [Math.max(0, lo), Math.min(1, hi)];
}

// ── Drawing over the cube ───────────────────────────────────────────────────

/**
 * The keys of the rows nearest to `focus` (full pixels [x, y, z]; z of a row
 * with no channel ignored), at most `n`: the markers and labels drawn of a
 * large catalogue. Null when all fit.
 */
export function nearestKeys(rows, focus, n) {
  if (rows.length <= n) return null;
  const d = rows.map((r) => {
    const dz = r.z === null || r.z === undefined || !focus ? 0 : r.z - focus[2];
    return { k: r.key, d: focus ? (r.x - focus[0]) ** 2 + (r.y - focus[1]) ** 2 + dz * dz : 0 };
  });
  d.sort((a, b) => a.d - b.d);
  return new Set(d.slice(0, n).map((x) => x.k));
}

/**
 * The marks of the list in model space: a bracket of four corners around the
 * current source (white), and a diamond on each found source (cyan; white
 * when current). `rows`, `current` (key), `shape`, `extent`, `clip`.
 */
export function listSegments(rows, current, shape, extent, clip) {
  const segs = [];
  const size = 0.012 * Math.max(...extent);
  for (const r of rows) {
    const isCur = r.key === current;
    if (r.origin !== 'user' && !isCur) continue;
    const [X, Y, Z] = rowModel(r, shape, extent, clip);
    if (r.origin === 'user') {
      const c = [...(isCur ? CURRENT_COLOUR : USER_COLOUR), 0.95];
      const d = size * 1.2;
      const pts = [[X + d, Y, Z], [X, Y + d, Z], [X - d, Y, Z], [X, Y - d, Z]];
      for (let i = 0; i < 4; i++) segs.push({ a: pts[i], b: pts[(i + 1) % 4], color: c });
      segs.push({ a: [X, Y, Z - d], b: [X, Y, Z + d], color: c });
    }
    if (isCur) {
      const c = [...CURRENT_COLOUR, 1];
      const h = size * 3, l = size * 1.3;
      for (const sx of [-1, 1]) {
        for (const sy of [-1, 1]) {
          const cx = X + sx * h, cy = Y + sy * h;
          segs.push({ a: [cx, cy, Z], b: [cx - sx * l, cy, Z], color: c });
          segs.push({ a: [cx, cy, Z], b: [cx, cy - sy * l, Z], color: c });
        }
      }
    }
  }
  return segs;
}

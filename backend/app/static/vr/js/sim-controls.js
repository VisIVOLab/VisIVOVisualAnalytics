// The headset's controls for simulation data (issue #259), as pure functions:
// the snapshot series (the files beside the table, app/snapshot_series.py),
// the X/Y/Z columns, each particle's splat sized by its smoothing length, the
// box selection as a backend filter, and the colour bar's unit. No DOM, no
// GL: node runs the tests (backend/tests/vr/sim-controls.test.mjs).

const Q = 65535;

// ── Snapshots ───────────────────────────────────────────────────────────────

/** The datasets a burst of snapshot steps leaves behind, to close (#305): the
 *  table on show when the burst began (`carry.from`, unless `withFrom` is
 *  false) and the steps' own tables dropped while on show (`carry.left`).
 *  Only tables the page opened itself (`own`), each id once, and never the
 *  table on show now (`shown`) nor one with its id. */
export function burstLeftovers(carry, shown, withFrom = true) {
  if (!carry) return [];
  const ids = [];
  for (const t of [withFrom ? carry.from : null, ...(carry.left || [])]) {
    if (!t || !t.own || !t.datasetId || t === shown || (shown && t.datasetId === shown.datasetId)) continue;
    if (!ids.includes(t.datasetId)) ids.push(t.datasetId);
  }
  return ids;
}

/** Resolves once the promise `get()` returns has settled and no newer one
 *  replaced it meanwhile (the refiner's newest switch, #305). Never rejects. */
export async function settled(get, maxRounds = 8) {
  for (let i = 0; i < maxRounds; i++) {
    const p = get();
    try { await p; } catch { /* a failed switch has ended too */ }
    if (get() === p) return;
  }
}

/** The file `dir` steps (-1 back, +1 on) from a series {paths, index, count},
 *  or null at either end or without a series. */
export function seriesStep(series, dir) {
  if (!series || !Array.isArray(series.paths)) return null;
  const i = series.index + (dir > 0 ? 1 : -1);
  return i >= 0 && i < series.paths.length ? { index: i, path: series.paths[i] } : null;
}

/** 'snap_002.bin (3 of 12)', or '' without a series of more than one. */
export function seriesLabel(series) {
  if (!series || !(series.count > 1)) return '';
  const name = String(series.paths[series.index] || '').split('/').pop().replace(/\.head$/, '');
  return `${name} (${series.index + 1} of ${series.count})`;
}

/**
 * What a new snapshot keeps of the one before (`prev`: {axes, colourField,
 * sizeField}) given its `fields`: each only when the new table has that
 * column (a snapshot of the same simulation does), else the new table's own.
 */
export function carryOver(prev, fields, defaults) {
  const has = (f) => !!f && fields.includes(f);
  const axes = prev.axes && prev.axes.every(has) ? prev.axes.slice() : defaults.axes.slice();
  return {
    axes,
    // One colour (null) is a choice, kept as such.
    colourField: prev.colourField === null ? null : has(prev.colourField) ? prev.colourField : defaults.colourField,
    sizeField: has(prev.sizeField) ? prev.sizeField : smoothingField(fields),
  };
}

// ── Columns ─────────────────────────────────────────────────────────────────

/** The columns that can be an axis or a size: the table's own (a sky
 *  catalogue's computed @x/@y/@z included: they are its positions). */
export const axisFields = (fields) => fields.slice();

/** The column after (`dir` +1) or before (-1) `current` among `fields`
 *  (cyclic), passing over those in `skip` (a Set); the first usable one when
 *  `current` is not among them; null when none is usable. */
export function cycleField(fields, current, dir = 1, skip = null) {
  const ok = (f) => !(skip && skip.has(f));
  if (!fields.some(ok)) return null;
  const i = fields.indexOf(current);
  if (i < 0) return fields.find(ok);
  const n = fields.length;
  for (let k = 1; k <= n; k++) {
    const f = fields[(i + (dir < 0 ? -k : k) + n * k) % n];
    if (ok(f)) return f;
  }
  return null;
}

/** A box in fractions of `bounds` [xmin, xmax, ymin, ymax, zmin, zmax] as
 *  table units ({min, max}). */
export function toData(box, bounds) {
  const at = (i, f) => bounds[2 * i] + f * (bounds[2 * i + 1] - bounds[2 * i]);
  return { min: [0, 1, 2].map((i) => at(i, box.min[i])), max: [0, 1, 2].map((i) => at(i, box.max[i])) };
}

/** A box in table units as fractions of `bounds`, clamped to the box (a flat
 *  axis: all of it); null when the region lies wholly outside the new box on
 *  an axis. The crop and the box selection keep their place in space from
 *  one snapshot to the next this way (#259 review), not their fraction of a
 *  box whose bounds changed. */
export function toFrac(data, bounds) {
  const min = [0, 0, 0], max = [1, 1, 1];
  for (let i = 0; i < 3; i++) {
    const lo = bounds[2 * i], span = bounds[2 * i + 1] - lo;
    if (!(span > 0)) continue;
    min[i] = Math.max(0, Math.min(1, (data.min[i] - lo) / span));
    max[i] = Math.max(0, Math.min(1, (data.max[i] - lo) / span));
    if (!(max[i] > min[i])) return null;
  }
  return { min, max };
}

/** A smoothing-length column by its usual names (h, hsml, sml,
 *  SmoothingLength as Gadget/SWIFT write it, smoothing_length), or null. */
export function smoothingField(fields) {
  const re = /^(h|hsml|sml|smoothing_?lengths?)$/i;
  return fields.find((f) => re.test(String(f).trim())) || null;
}

// ── Sizes ───────────────────────────────────────────────────────────────────

/** The size header of a sample ({field, range: [lo, hi]}), or null. */
export function sizeInfo(get) {
  const field = get('X-Visivo-Size-Field');
  if (!field) return null;
  const range = String(get('X-Visivo-Size-Range') || '').split(',').map(Number);
  return range.length === 2 && range.every((v) => v > 0 && Number.isFinite(v)) ? { field, range } : null;
}

/** The sizes' codes in a sample's body: after the positions (6 bytes a row)
 *  and the colours (2 bytes a row, when there are colours). */
export function sizeArray(buf, count, hasColour) {
  const off = (hasColour ? 8 : 6) * count;
  return buf.byteLength >= off + 2 * count ? new Uint16Array(buf, off, count) : null;
}

/** The size (data units) a code stands for over `range` (log10 codes). */
export function codeSize(code, range) {
  const a = Math.log10(range[0]), b = Math.log10(range[1]);
  return 10 ** (a + (code / Q) * (b - a));
}

/**
 * The shader's numbers for splats sized by a smoothing length: each splat's
 * Gaussian σ is h / √2, the ray-traced renderer's kernel exp(-d²/h²)
 * (app/particle_raytrace.py), times the size step (`splatSize`) and the
 * sample's growth ∛(rows / sample rows), as the uniform splats grow. `radius`
 * is the uniform splat radius (data units) the page would draw: the shader
 * multiplies it by f = h · mul, and conserves each splat's light by 1 / f²
 * (clamped). Returns {log: [log10 lo, log10 hi - log10 lo], mul}.
 */
export function sizedSplat(range, { splatSize = 1, grow = 1, radius = 1 } = {}) {
  const lo = Math.log10(range[0]), hi = Math.log10(range[1]);
  return { log: [lo, hi - lo], mul: (splatSize * grow) / Math.SQRT2 / Math.max(radius, 1e-30) };
}

/**
 * The light factor of a sized splat (the shader's vLight): the uniform
 * splat's light over this one's, each its sprite's area (the pixel size
 * clamped to 1..maxPx, as drawn) times the sub-pixel factor max(min(px, 1),
 * 0.25), so a particle gives the light of the uniform splat whatever its
 * smoothing length and however its sprite is clamped. No floor, no cap.
 * `pxU`: the uniform splat's size (pixels), `pxD`: this one's.
 */
export function lightFactor(pxU, pxD, maxPx) {
  const c = (px) => Math.min(maxPx, Math.max(1, px));
  const sub = (px) => Math.max(Math.min(px, 1), 0.25);
  return (c(pxU) ** 2 * sub(pxU)) / (c(pxD) ** 2 * sub(pxD));
}

// ── Box selection ───────────────────────────────────────────────────────────

/** The selection's modes: every particle, only those inside the box, all but those. */
export const SELECT_MODES = ['all', 'inside', 'outside'];

/**
 * The backend filter of a box `box` ({min, max} fractions over the sample's
 * `bounds` [xmin, xmax, ymin, ymax, zmin, zmax] on the columns `axes`), for
 * `mode` 'inside' or 'outside'; [] for 'all' or no box.
 */
export function boxFilters(axes, bounds, box, mode) {
  if (!box || (mode !== 'inside' && mode !== 'outside')) return [];
  const at = (i, f) => bounds[2 * i] + f * (bounds[2 * i + 1] - bounds[2 * i]);
  return [{ box: { fields: axes.slice(0, 3), min: [0, 1, 2].map((i) => at(i, box.min[i])),
    max: [0, 1, 2].map((i) => at(i, box.max[i])) }, op: mode }];
}

/** The box's data ranges as text: 'X −1.2 to 3.4, Y …'. */
export function boxText(axes, bounds, box) {
  if (!box) return '';
  const f = (v) => (Math.abs(v) >= 1e4 || (Math.abs(v) < 1e-2 && v !== 0) ? v.toExponential(2) : String(+v.toPrecision(4)));
  return [0, 1, 2].map((i) => {
    const a = bounds[2 * i] + box.min[i] * (bounds[2 * i + 1] - bounds[2 * i]);
    const b = bounds[2 * i] + box.max[i] * (bounds[2 * i + 1] - bounds[2 * i]);
    return `${axes[i]} ${f(a)} to ${f(b)}`;
  }).join(', ');
}

// ── The colour bar ──────────────────────────────────────────────────────────

/** A column's unit from the table's `units` ({field: unit}) or, failing
 *  that, from its name ('mass [Msun]', 'T (K)', 'rho_g_cm3' is not read). */
export function unitOf(units, field) {
  if (!field) return '';
  if (units && typeof units[field] === 'string' && units[field].trim()) return units[field].trim();
  const m = /[[(]([^\])]+)[\])]\s*$/.exec(String(field));
  return m ? m[1].trim() : '';
}

/** The colour bar's title: 'Mass [Msun]', 'Mass (no unit in the file)'; log
 *  scales say so. */
export function colourBarTitle(field, unit, log = false) {
  if (!field) return '';
  const u = unit ? ` [${unit}]` : ' (no unit in the file)';
  return `${log ? 'log ' : ''}${field}${u}`;
}

/** The colour bar's tick values over [a, b] (data values; on a log scale
 *  the decades between them, else nice linear ticks). */
export function colourBarTicks(a, b, log, niceTicks) {
  if (!(Number.isFinite(a) && Number.isFinite(b)) || a === b) return [a];
  if (log && a > 0 && b > 0) {
    const out = [];
    for (let e = Math.ceil(Math.log10(Math.min(a, b))); e <= Math.floor(Math.log10(Math.max(a, b))); e++) out.push(10 ** e);
    return out.length >= 2 ? out : [Math.min(a, b), Math.max(a, b)];
  }
  return niceTicks(a, b, 4);
}

// ── The variables of a simulation volume (issue #317) ──────────────────────

/** The variables the hand menu offers for the cube on show, from
 *  reduced_info's `fields` ([{name, path}]) and `field_name`: {names, paths
 *  ({name: path}), current (null when the one on show is not listed)}; null
 *  with fewer than two (the picker is hidden). A name listed twice counts
 *  once (its first path). */
export function variablesOf(info) {
  const list = info && Array.isArray(info.fields) ? info.fields : [];
  const names = [], paths = {};
  for (const f of list) {
    if (!f || typeof f.name !== 'string' || !f.name || typeof f.path !== 'string' || !f.path) continue;
    if (Object.prototype.hasOwnProperty.call(paths, f.name)) continue;
    names.push(f.name);
    paths[f.name] = f.path;
  }
  if (names.length < 2) return null;
  const current = names.includes(info.field_name) ? info.field_name : null;
  return { names, paths, current };
}

/** The variables' chips on the menu's row: all of them when they fit
 *  `perPage`, else the page of `perPage - 1` holding `current` (the first
 *  page without one) and `more` (the last cell steps to the next variable,
 *  nextVariable, and the page follows it). */
export function variableChips(names, current, perPage = 5) {
  if (names.length <= perPage) return { chips: names.slice(), more: false, pages: 1, page: 0 };
  const per = perPage - 1, pages = Math.ceil(names.length / per);
  const i = Math.max(0, names.indexOf(current));
  const page = Math.floor(i / per);
  return { chips: names.slice(page * per, page * per + per), more: true, pages, page };
}

/** The variable after (`dir` +1) or before (-1) `current`, cyclic; the first
 *  when `current` is not among them (cycleField). */
export function nextVariable(names, current, dir = 1) {
  return cycleField(names, current, dir);
}

/**
 * Each variable's look, by its path (a field reference), so a variable shown
 * again comes back as it was left: {colormap, stretch, gamma, opacity, mode,
 * scale (the reduced cube's request: auto, linear, log), threshold and
 * colourMax as data values}. At most `max` variables are kept (the oldest
 * goes first).
 */
export class VariableLooks {
  constructor(max = 64) { this.max = max; this.map = new Map(); }
  save(path, look) {
    if (!path || !look) return;
    this.map.delete(path);
    this.map.set(path, { ...look });
    while (this.map.size > this.max) this.map.delete(this.map.keys().next().value);
  }
  get(path) { const l = this.map.get(path); return l ? { ...l } : null; }
  clear() { this.map.clear(); }
  get size() { return this.map.size; }
}

/**
 * The threshold and colour max (0..1 of the cube's codes) a saved look gives
 * the cube `cube` ({lo, hi, scale}), through `toNorm` (volume.js
 * dataToNorm): its data values put back on the cube's codes, clamped to
 * 0..1, the colour max kept above the threshold. null when the look has no
 * finite values (the cube's own defaults then).
 */
export function lookNorms(look, cube, toNorm) {
  if (!look || !cube || !Number.isFinite(look.threshold) || !Number.isFinite(look.colourMax)) return null;
  const clamp = (n) => (Number.isFinite(n) ? Math.max(0, Math.min(1, n)) : 0);
  const thrNorm = clamp(toNorm(look.threshold, cube.lo, cube.hi, cube.scale));
  let cmaxNorm = clamp(toNorm(look.colourMax, cube.lo, cube.hi, cube.scale));
  if (cmaxNorm <= thrNorm) cmaxNorm = Math.min(1, thrNorm + 0.01);
  return { thrNorm, cmaxNorm };
}

/** The data file a field reference reads ('…/rho.0000.flt' for
 *  '…/rho.0000.flt::pluto-var=vx1'); any other path as it is. */
export function fieldSource(path) {
  return String(path || '').replace(/::(pluto-var|vbt-field)=.*$/, '');
}

/** The headset hub's word on a cube switch (issue #317): for another
 *  variable of the source shown before (`prev`, the path on show until now:
 *  one of the new cube's fields, or their data file opened by itself),
 *  {summary 'Variable vx1 of rho.0000.flt', field, from}; null for an
 *  ordinary switch. `picked`: the switch was made with the variable picker
 *  (the paths may then differ only by how they were spelled: /var and
 *  /private/var on macOS). */
export function variableSwitch(path, prev, info, fileName, picked = false) {
  const v = variablesOf(info);
  if (!v || !v.current || !prev || prev === path) return null;
  const source = fieldSource(path);
  if (!picked && !Object.values(v.paths).includes(prev) && fieldSource(prev) !== source) return null;
  return { summary: `Variable ${v.current} of ${fileName(source)}`, field: v.current, from: prev };
}

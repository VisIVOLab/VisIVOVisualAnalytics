// Point tables in the headset (issue #190): an even sample of the table's
// rows within a budget (/v1/points/sample_bin, 8 bytes a row), drawn on the
// headset's own GPU as points or additive splats, coloured by a column through
// the page's colour maps; while the view is still the backend's point render
// session draws every row for the eyes (render_session/_stereo_points.py) and
// the page blends that frame in, as for a cube.
//
// Model space, as for a cube: a box centred on the origin, its longest side 1,
// over the sample's bounding box (dataToModel). The backend gets the same map
// with every pose (data_to_model), so its frames land on the page's points.
// The pure functions are unit-tested (backend/tests/vr/layers.test.mjs).

import * as M from './mat4.js';

/** Rows drawn, per budget: the headset's (a Quest 3 at 72-90 Hz in stereo) and
 *  a desktop browser's. Measured in docs/user/reference/vr.md. */
export const BUDGETS = {
  'quest-low': 500_000, quest: 1_000_000, 'quest-high': 2_000_000,
  desktop: 5_000_000, 'desktop-high': 10_000_000,
};

const Q = 65535;

/**
 * The rows to draw next frame: the sample arrives in a low-discrepancy (van
 * der Corput) order, every prefix spread over the whole sample (app/
 * point_sample.py prefix_order: no gap wider than 2 n / m), so when the
 * GPU time of a frame (`gpuMs`, both eyes) passes `share` of the frame period
 * fewer rows are drawn, and more again when there is room, never below
 * `min` nor above the sample. A negative gpuMs (no timer query) keeps the count.
 */
export function governRows(current, sampleRows, gpuMs, targetHz, { share = 0.6, min = 100_000 } = {}) {
  if (!(gpuMs > 0) || !(targetHz > 0)) return Math.min(current, sampleRows);
  const budget = (1000 / targetHz) * share;
  const f = Math.max(0.6, Math.min(1.1, budget / gpuMs));
  return Math.round(Math.max(Math.min(min, sampleRows), Math.min(sampleRows, current * f)));
}

/**
 * Splats a headset starts with (issue #295): measured on a Quest 3 (Adreno
 * 740) splats are fill-rate bound, and at size 1 about 200 k hold 72 Hz on
 * the half-float target (50 k on float32), where points hold a million. The
 * sample stays the budget's (points, refinement); the splats draw a prefix of
 * it, which the row governor then moves.
 */
export const HEADSET_SPLAT_ROWS = 200_000;

/**
 * The frame intervals of the last `windowMs` (the newest at the end of
 * `intervals`): {n, meanMs, medianMs, missed (share above 1.5 periods),
 * slow (share above 1.1 periods)}.
 * Pauses longer than 250 ms (a load, a dialog, entering VR) are not frames,
 * as for the cube's governor: one of them must not read as a slow scene.
 */
export function recentFrames(intervals, targetHz, windowMs = 500) {
  const period = 1000 / targetHz;
  const v = [];
  let sum = 0, missed = 0, slow = 0;
  for (let i = intervals.length - 1; i >= 0 && sum < windowMs; i--) {
    const x = intervals[i];
    if (!(x > 0) || x > 250) continue;
    v.push(x); sum += x;
    if (x > period * 1.5) missed++;
    if (x > period * 1.1) slow++;
  }
  const n = v.length;
  v.sort((a, b) => a - b);
  return { n, meanMs: n ? sum / n : 0, medianMs: n ? v[(n - 1) >> 1] : 0,
    missed: n ? missed / n : 0, slow: n ? slow / n : 0 };
}

/**
 * The row governor without a GPU timer (issue #295: the Quest Browser has no
 * EXT_disjoint_timer_query_webgl2, and governRows kept its count): from the
 * frame intervals, as the cube's samples-per-voxel governor works. Late
 * frames are intervals over 1.5 periods ("missed"), but a GPU-bound Quest 3
 * also paces frames between one and 1.5 periods (63 fps at 72 Hz with under
 * 1 % missed), so the median interval and the share of slow frames (over 1.1
 * periods) count too. More than 15 % missed, or a median above 1.12 periods,
 * cuts the rows at once, by the period over the slower of mean and median
 * (0.5 to 0.85); more than 5 % missed or 10 % slow cuts them by 8 %. A cut
 * remembers the count that was too many (the ceiling) and holds for
 * `holdMs`; clean windows (at most 2 % missed and 3 % slow, the median within
 * add 12 % a quarter second, up to 90 % of the ceiling, which rises by a
 * half for every `ceilingRiseMs` of clean frames. A cut soon after a rise
 * (within 3 s) doubles that wait (up to a minute), and every `decayMs`
 * without such a cut halves it again: the count settles just under where it
 * missed instead of probing it every few seconds, and a later cut that no
 * probe caused (a zoom into the box) is not climbed back from at a wait
 * left long by probes minutes ago. Clean time is earned by
 * frames that drew the splats only: a gap of more than `gapMs` between
 * updates (splats not drawn under a refined frame, a paused page) or an
 * `interrupted()` call restarts it. Never below `min` nor above the sample.
 */
export class FrameRowGovernor {
  constructor({ min = 20_000, holdMs = 1500, ceilingRiseMs = 1500, maxRiseMs = 60_000, gapMs = 500,
    decayMs = 20_000 } = {}) {
    this.min = min; this.holdMs = holdMs; this.ceilingRiseMs = ceilingRiseMs; this.maxRiseMs = maxRiseMs;
    this.gapMs = gapMs; this.decayMs = decayMs;
    this.reset();
  }

  reset() {
    this.ceiling = Infinity; this.holdUntil = 0; this.cleanSince = null;
    this.riseMs = this.ceilingRiseMs; this.lastRiseAt = -Infinity; this.lastUpdate = -Infinity;
    this.lastBackoffAt = -Infinity;
  }

  /** The splats were not drawn for a while (a refined frame over them): the
   *  clean run restarts, and the first frames drawn again are waited for. */
  interrupted(now) {
    this.cleanSince = null;
    this.holdUntil = Math.max(this.holdUntil, now + 750);
  }

  /** The rows to draw from `current`; `frame`: recentFrames; `now`: ms.
   *  `riseCeiling` false (still under refined splat frames, #300): no clean
   *  time is earned and the ceiling does not rise, so the count does not
   *  probe above where it last missed. */
  update(current, sampleRows, frame, targetHz, now, { riseCeiling = true } = {}) {
    const floor = Math.min(this.min, sampleRows);
    const clampRows = (v) => Math.round(Math.max(floor, Math.min(sampleRows, v)));
    if (now - this.lastUpdate > this.gapMs) this.cleanSince = null;   // not drawn meanwhile
    this.lastUpdate = now;
    if (this.riseMs > this.ceilingRiseMs && now - this.lastBackoffAt >= this.decayMs) {
      this.riseMs = Math.max(this.ceilingRiseMs, this.riseMs / 2);
      this.lastBackoffAt = now;
    }
    if (!(targetHz > 0) || !frame || frame.n < 4) return clampRows(current);
    const period = 1000 / targetHz;
    const median = frame.medianMs || frame.meanMs;
    const slow = frame.slow ?? 0;
    const hard = frame.missed > 0.15 || median > 1.12 * period;
    if (hard || frame.missed > 0.05 || slow > 0.1) {
      if (now < this.holdUntil && !hard) return clampRows(current);   // a gentle cut takes effect first
      if (now - this.lastRiseAt < 3000) {
        this.riseMs = Math.min(this.maxRiseMs, 2 * this.riseMs);
        this.lastBackoffAt = now;
      }
      this.ceiling = current;
      this.holdUntil = now + this.holdMs;
      this.cleanSince = null;
      const f = hard ? Math.max(0.5, Math.min(0.85, period / Math.max(frame.meanMs, median))) : 0.92;
      return clampRows(current * f);
    }
    if (frame.missed > 0.02 || slow > 0.03 || median > 1.04 * period) { this.cleanSince = null; return clampRows(current); }
    if (!riseCeiling) this.cleanSince = null;
    else if (this.cleanSince === null) this.cleanSince = now;
    if (riseCeiling && Number.isFinite(this.ceiling) && now - this.cleanSince >= this.riseMs) {
      this.ceiling *= 1.5;
      this.cleanSince = now;
      this.lastRiseAt = now;
    }
    if (now < this.holdUntil) return clampRows(current);
    const cap = Number.isFinite(this.ceiling) ? Math.max(current, 0.9 * this.ceiling) : Infinity;
    return clampRows(Math.min(cap, current * 1.12));
  }
}

/**
 * Which row governor runs: 'gpu' (GPU time, where there is a timer), 'frames'
 * (the frame intervals: only in a VR session once its display rate has
 * settled, issue #295: a desktop browser without a timer, Firefox or Safari,
 * may run at 30 or 50 Hz against an assumed 60, and a headset settles from
 * 90 to 72 Hz in its first second), or 'none' (the count is kept).
 */
export function rowGovernorKind({ gpuMs, inXR, rateSettled }) {
  if (gpuMs > 0) return 'gpu';
  return inXR && rateSettled ? 'frames' : 'none';
}

/**
 * The rows drawn for a governor's count: half-octave steps (…, 141 k, 200 k,
 * 283 k, 400 k, …), or the whole sample. The splats are sized and refined for
 * the rows drawn, so a count that moved every quarter second would make
 * every refined frame stale; a step changes only when the governor crosses
 * it. With `prev` (the step drawn now) the step is kept until the count is
 * more than 0.4 octave from it (0.15 past the midpoint to the next step), so
 * a count hovering at a midpoint does not flip between two steps.
 */
export function quantizeRows(n, sampleRows, prev = null) {
  if (!(n > 0)) return Math.min(sampleRows, 1);
  if (prev > 0 && prev <= sampleRows && Math.abs(Math.log2(Math.min(n, sampleRows) / prev)) <= 0.4) return prev;
  const q = Math.round(2 ** (Math.round(2 * Math.log2(n)) / 2));
  return Math.max(1, Math.min(sampleRows, q));
}

/** The page's box for the bounds [xmin, xmax, ymin, ymax, zmin, zmax]:
 *  {centre, side, extent (box sides), matrix: data -> model, column-major}. */
export function dataToModel(bounds) {
  const lo = [bounds[0], bounds[2], bounds[4]], hi = [bounds[1], bounds[3], bounds[5]];
  const span = [0, 1, 2].map((i) => hi[i] - lo[i]);
  const side = Math.max(...span, 1e-30);
  const centre = [0, 1, 2].map((i) => 0.5 * (lo[i] + hi[i]));
  const s = 1 / side;
  const matrix = new Float64Array([s, 0, 0, 0, 0, s, 0, 0, 0, 0, s, 0,
    -centre[0] * s, -centre[1] * s, -centre[2] * s, 1]);
  return { centre, side, extent: span.map((v) => Math.max(v * s, 1e-6)), matrix, lo, hi };
}

/** Headers of a sample (a fetch Response's, or a plain object of them). */
export function sampleInfo(get) {
  const num = (k, d = 0) => { const v = get(k); return v === null || v === undefined || v === '' ? d : Number(v); };
  const list = (k) => (get(k) || '').split(',').filter((v) => v !== '').map(Number);
  const field = get('X-Visivo-Color-Field') || null;
  const totalRows = num('X-Visivo-Total-Rows');
  return {
    count: num('X-Visivo-Count'), totalRows, step: num('X-Visivo-Step', 1),
    // The rows the sample was drawn from (a sky catalogue's unplaced ones
    // left out, #265): the splat size counts these. An older backend: all.
    placedRows: num('X-Visivo-Placed-Rows', totalRows),
    bounds: list('X-Visivo-Bounds'), axes: (get('X-Visivo-Axes') || 'X,Y,Z').split(','),
    colourField: field, colourRange: field ? list('X-Visivo-Color-Range') : null,
    colourLog: get('X-Visivo-Color-Log') === '1', dataRange: field ? list('X-Visivo-Data-Range') : null,
  };
}

/** The sample's arrays over its body: positions (3 per row) then colours. */
export function sampleArrays(buf, info) {
  const n = info.count;
  const positions = new Uint16Array(buf, 0, 3 * n);
  const colour = info.colourField && buf.byteLength >= 8 * n ? new Uint16Array(buf, 6 * n, n) : null;
  return { positions, colour };
}

/** Data coordinates of row i (a check of the quantisation, and the picks). */
export function rowPosition(positions, i, bounds) {
  return [0, 1, 2].map((a) => bounds[2 * a] + (positions[3 * i + a] / Q) * (bounds[2 * a + 1] - bounds[2 * a]));
}

/** The data value a colour code stands for (log: 10 ** the log value). */
export function codeValue(code, range, log) {
  const [a, b] = log ? range.map(Math.log10) : range;
  const v = a + (code / Q) * (b - a);
  return log ? 10 ** v : v;
}

/**
 * The page's colours as the point session's `set_color` points: `lut`, the
 * 256 x RGBA8 table; the codes' range `range` [cmin, cmax] (data values) with
 * the page's own window over it, [c0, c1] fractions; log as the sample's.
 * Points sit at data values, evenly in the log of the value on a log scale
 * (the session interpolates them on that scale, set_color's log).
 */
export function colourCommand(field, range, log, lut, c0 = 0, c1 = 1, n = 32) {
  if (!field || !range) return { type: 'set_color', field: null };
  const [a, b] = log ? range.map(Math.log10) : range;
  const at = (f) => { const v = a + f * (b - a); return log ? 10 ** v : v; };
  const lo = Math.min(c0, c1 - 1e-6), hi = Math.max(c1, lo + 1e-6);
  const lutAt = (L) => {
    const x = Math.max(0, Math.min(255, L * 255));
    const i = Math.min(254, Math.floor(x)), t = x - i;
    return [0, 1, 2].map((c) => ((1 - t) * lut[i * 4 + c] + t * lut[(i + 1) * 4 + c]) / 255);
  };
  const points = [];
  for (let j = 0; j <= n; j++) {
    const L = j / n;
    points.push([at(lo + L * (hi - lo)), ...lutAt(L)]);
  }
  return { type: 'set_color', field, min: at(lo), max: at(hi), points, log };
}

/**
 * How the page draws its splats (issue #263), by what they are added into;
 * the point session draws its refined frames by the same rule
 * (app/point_splat.py SPLAT_RULES), and says which in each frame:
 * - 'v2-f32': float32, opacity over size², tone-mapped;
 * - 'v2-f16': half floats, the same with the opacity at least
 *   F16_OPACITY_FLOOR (fainter additions are dropped there);
 * - 'v2-8bit': no float target: the desktop's opacity, clamped (over size²
 *   a size-12 splat would add less than half an 8-bit step: none at all).
 * 'v1' is the rule of a page from before #263 (as 'v2-8bit').
 */
export const SPLAT_RULES = ['v1', 'v2-8bit', 'v2-f16', 'v2-f32'];
export const F16_OPACITY_FLOOR = 1 / 512;

/** The desktop's opacity as `rule` draws it (point_splat.rule_opacity). */
export function ruleOpacity(opacity, size, rule) {
  if (rule === 'v2-f32') return opacity / (size * size);
  if (rule === 'v2-f16') return Math.max(opacity / (size * size), F16_OPACITY_FLOOR);
  return opacity;
}

/** True when a refined frame was drawn by the page's own splat rule (its
 *  header's splat_rule): a backend from before #263 says none and draws
 *  the old look, so its frames are not shown over tone-mapped splats. */
export function acceptFrame(header, style, rule) {
  return !isSplats(style) || (!!header && header.splat_rule === rule);
}

/**
 * Splat radius (data units) and opacity, as the desktop and the point session
 * size them (render_session/points.py gaussian_splat): spacing ∛(volume / N)
 * of the box the points fill, radius = spacing × size / 8; opacity is
 * 1.5 / D at the standard intensity, clamped to 0.005..0.25, with D the
 * longest side / spacing, as the
 * splat `rule` draws it (ruleOpacity: over size² in a float target, issue
 * #263, so the size step changes the size, not the brightness; the
 * desktop's at size 1, the default). N is the table's rows: a sample of
 * fewer rows is drawn with splats larger by ∛(rows per sample row), so it
 * covers the same volume.
 */
export function splatStyle(bounds, totalRows, sampleRows, size, intensity, rule = 'v1') {
  const dims = [bounds[1] - bounds[0], bounds[3] - bounds[2], bounds[5] - bounds[4]];
  const longest = Math.max(...dims, 1e-6);
  const volume = dims.reduce((p, d) => p * Math.max(d, longest * 1e-3), 1);
  const spacing = Math.cbrt(volume / Math.max(1, totalRows));
  const radius = (spacing * size) / 8;
  const depth = Math.max(longest / spacing, 1);
  const opacity = ruleOpacity(Math.min(Math.max((1.5 / depth) * intensity / 35, 0.005), 0.25),
    size, rule);
  const grow = Math.cbrt(Math.max(1, totalRows / Math.max(1, sampleRows)));
  return { radius, opacity, sampleRadius: radius * grow };
}

/**
 * The styles of a point table: points, additive splats, and ray-traced
 * splats (issue #264), which the page draws as splats while the view moves
 * and the backend's ray-traced particle renderer refines when it is still,
 * with the depth the light comes from.
 */
export const STYLES = ['points', 'splats', 'raytraced'];

/** True for the styles the page draws as additive splats. */
export function isSplats(style) { return style === 'splats' || style === 'raytraced'; }

/** The style a table opens with: ray-traced for a simulation's particles (a
 *  VBT point table, a snapshot's particle type: /v1/points/info says
 *  `particles`), points for a catalogue, whose rows are objects, not a
 *  medium. An older backend says only the format. */
export function defaultStyle(format, particles = format === 'vbt') {
  return particles ? 'raytraced' : 'points';
}

/**
 * The column a table is coloured by when it opens (issue #263): a density
 * (`*dens*`, `*rho*`, `LogD`, ...) where there is one, as a snapshot's
 * structure reads best by it; else the first column that is neither a
 * position, a velocity, a sky coordinate nor an identifier (a catalogue's
 * redshift or magnitude rather than its RA); else none.
 */
export function defaultColourField(fields, axes = []) {
  const usable = fields.filter((f) => !axes.includes(f) && !f.startsWith('@'));
  const dens = usable.find((f) => /dens|rho|^log_?d$|^ld$/i.test(f));
  if (dens) return dens;
  const skip = /^(v_?[xyz]|vel_?[xyz]|ra|dec|ra_?deg|dec_?deg|ra_?j2000|de_?j2000|dej2000|raj2000|glon|glat|l|b|elon|elat|lon|lat|id|.*_id|name|index|row|[xyz])$/i;
  return usable.find((f) => !skip.test(f)) || null;
}

/** The box's axis titles: a sky catalogue's computed positions (`@x`, `@y`,
 *  `@z`, comoving distances, app/catalogue_positions.py) say their unit,
 *  Mpc; a table's own columns keep their names. */
export function axisTitles(axes) {
  return axes.map((a) => (/^@[xyz]$/i.test(a) ? `${a.slice(1)} (Mpc)` : a));
}

/** The commands that give the backend's point session the page's look
 *  (`splatRule`: how the page draws its splats, PointCloud.splatRule). A sky
 *  catalogue's placement (`p.sky`, the desktop window's, issue #260) goes
 *  first, so the refined frame places the sources as the sample does. */
export function pointLook(p) {
  const splats = isSplats(p.style);
  const sky = p.sky ? [{ type: 'set_sky_positions', ...p.sky }] : [];
  return [
    ...sky,
    { type: 'set_point_fields', x: p.axes[0], y: p.axes[1], z: p.axes[2] },
    colourCommand(p.colourField, p.colourRange, p.colourLog, p.lut, p.c0, p.c1),
    { type: 'set_point_style', mode: splats ? 'gaussian' : 'points',
      size: splats ? p.splatSize : Math.max(1, Math.round(p.pointPx)), intensity: p.intensity,
      stereo_renderer: p.style === 'raytraced' ? 'raytrace' : 'splat',
      splat_rule: p.splatRule || 'v1' },
    { type: 'set_show_outline', visible: false },
    // The box selection (issue #259), as the sample's filters.
    { type: 'set_filters', filters: p.filters || [] },
  ];
}

/**
 * A sky catalogue's placement for a status line, from /v1/points/info's
 * `sky_settings`: "supergalactic · comoving · Planck18 · 26 unplaced"
 * (the unplaced part only when there are any), or "fk5 · celestial sphere
 * (no distances)" for a catalogue of positions only; '' for none.
 */
export function skyLabel(st) {
  if (!st || !st.frame) return '';
  // Positions only (no redshift or distance column): the celestial sphere.
  if (st.sky_sphere) return `${st.frame} · celestial sphere (no distances)`;
  const parts = [st.frame, st.distance_mode === 'redshift_space' ? 'redshift space' : 'comoving', st.cosmology];
  if (st.unplaced > 0) parts.push(`${st.unplaced} unplaced`);
  return parts.join(' · ');
}

/**
 * What a refined frame's pose adds for a point table (`eyes`: the page's
 * eyes, {width, height}): the page's box (data_to_model), and for splats the
 * sample's rows and the eye's size in page pixels, which the session's
 * splats take (app/point_splat.py, issue #262) so the refined frame has the
 * preview's size and brightness. While the governor draws only part of the
 * sample the preview is dimmer (fewer rows, each larger by less than their
 * light), so the refined frame is then brighter than it, not darker.
 */
export function refinePoseExtra(box, style, sampleRows, eyes) {
  const out = { data_to_model: Array.from(box.matrix) };
  if (isSplats(style)) {
    if (sampleRows > 0) out.sample_rows = Math.round(sampleRows);
    const w = Math.round(Math.max(0, ...eyes.map((e) => e.width || 0)));
    const h = Math.round(Math.max(0, ...eyes.map((e) => e.height || 0)));
    // The session draws at most 4096 pixels a side (render_session/_stereo_points.py).
    if (w >= 16 && h >= 16 && w <= 4096 && h <= 4096) out.viewport_size = [w, h];
  }
  return out;
}

/** Box edges and ticks in data units ({segs, labels}) for a box `d` (dataToModel). */
export function boxDecorations(d, axes, niceTicks) {
  const e = d.extent, h = e.map((v) => v / 2);
  const segs = [], labels = [];
  const col = [0.55, 0.6, 0.7, 0.45];
  const c = (x, y, z) => [x ? h[0] : -h[0], y ? h[1] : -h[1], z ? h[2] : -h[2]];
  const E = [[0, 0, 0, 1, 0, 0], [0, 1, 0, 1, 1, 0], [0, 0, 1, 1, 0, 1], [0, 1, 1, 1, 1, 1],
    [0, 0, 0, 0, 1, 0], [1, 0, 0, 1, 1, 0], [0, 0, 1, 0, 1, 1], [1, 0, 1, 1, 1, 1],
    [0, 0, 0, 0, 0, 1], [1, 0, 0, 1, 0, 1], [0, 1, 0, 0, 1, 1], [1, 1, 0, 1, 1, 1]];
  for (const [a, b, g, x, y, z] of E) segs.push({ a: c(a, b, g), b: c(x, y, z), color: col });
  const L = 0.02, H = 0.03, tick = [0.85, 0.88, 0.95, 0.9];
  const fmt = (v) => (Math.abs(v) >= 1e4 || (Math.abs(v) < 1e-2 && v !== 0) ? v.toExponential(1) : String(+v.toPrecision(4)));
  const toModel = (i, v) => (v - d.centre[i]) / d.side;
  for (const i of [0, 1, 2]) {
    const nt = Math.max(2, Math.min(5, Math.round(e[i] / 0.25)));
    for (const v of niceTicks(d.lo[i], d.hi[i], nt)) {
      const m = toModel(i, v);
      if (m < -h[i] - 1e-9 || m > h[i] + 1e-9) continue;
      const p = i === 0 ? [m, -h[1], h[2]] : i === 1 ? [-h[0], m, h[2]] : [h[0], -h[1], m];
      const q = i === 0 ? [m, -h[1] - L, h[2]] : i === 1 ? [-h[0] - L, m, h[2]] : [h[0] + L, -h[1], m];
      segs.push({ a: p, b: q, color: tick });
      const lp = i === 0 ? [m, -h[1] - 2.2 * L, h[2]] : i === 1 ? [-h[0] - 1.6 * L, m, h[2]] : [h[0] + 1.6 * L, -h[1], m];
      labels.push({ pos: lp, text: fmt(v), height: H, anchor: i === 1 ? 'right' : i === 2 ? 'left' : 'center', ...(i === 2 ? { face: 'side' } : {}) });
    }
  }
  const title = [[0, -h[1] - 4.5 * L, h[2]], [-h[0], h[1] + 2.2 * L, h[2]], [h[0] + 1.6 * L, -h[1] - 3 * L, 0]];
  axes.forEach((name, i) => labels.push({ pos: title[i], text: name, height: H * 1.1,
    anchor: i === 0 ? 'center' : 'left', color: '#9fd0ff', ...(i === 2 ? { face: 'side' } : {}) }));
  return { segs, labels };
}

// ── GL ─────────────────────────────────────────────────────────────────────
//
// Points are point sprites with the depth test. Splats (issue #263) are
// instanced quads, so a splat near the head keeps growing up to MAX_SPLAT_PX
// (point sprites stopped at 64 px), added into a float target (float32 where
// it can be blended, else half floats) and then
// tone-mapped onto the frame (toneMap): dense halos keep their colour instead
// of burning out at the 8-bit target's 1, and faint splats are no longer
// rounded away one addition at a time. Without a renderable float target
// the splats are added onto the frame directly, clamped, as before.

/** The widest splat, page pixels (the backend's point_splat.MAX_PX). */
export const MAX_SPLAT_PX = 256;

/** Rec. 709 luminance weights, as the tone curve and the backend use them. */
export const LUMA = [0.2126, 0.7152, 0.0722];

/**
 * The tone curve of the splats' light (RGB, any size): the luminance Y goes
 * to 1 - exp(-Y), the three channels follow it (the hue is kept), and a
 * channel still above 1 scales the three down. Linear for faint light (the
 * picture of a sparse region is as before), never above 1. Mirrored by the
 * resolve shader and by the backend (app/point_splat.py tone_map).
 */
export function toneMap(rgb) {
  const y = LUMA[0] * rgb[0] + LUMA[1] * rgb[1] + LUMA[2] * rgb[2];
  if (!(y > 0)) return [0, 0, 0];
  const f = -Math.expm1(-y) / y;
  const out = rgb.map((c) => c * f);
  const m = Math.max(...out);
  return m > 1 ? out.map((c) => c / m) : out;
}

const VS = `#version 300 es
precision highp float;
in vec3 aPos;          // 0..1 over the bounds (normalised uint16)
in float aVal;         // 0..1 colour code
uniform mat4 uMV;
uniform mat4 uP;
uniform vec3 uLo;      // model space of code 0 and the span to code 1
uniform vec3 uSpan;
uniform float uPx;     // pixels
uniform float uC0;
uniform float uC1;
uniform vec3 uClipMin; // the spatial crop (#257), box fractions: aPos itself
uniform vec3 uClipMax;
// The box selection (#259), box fractions as the crop: 1 only inside, 2 all but inside.
uniform vec3 uSelMin;
uniform vec3 uSelMax;
uniform int uSelMode;
out float vT;
void main() {
  bool inSel = all(greaterThanEqual(aPos, uSelMin)) && all(lessThanEqual(aPos, uSelMax));
  if (any(lessThan(aPos, uClipMin)) || any(greaterThan(aPos, uClipMax))
      || (uSelMode == 1 && !inSel) || (uSelMode == 2 && inSel)) {
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);   // outside the view volume: culled
    gl_PointSize = 1.0;
    vT = 0.0;
    return;
  }
  gl_Position = uP * (uMV * vec4(uLo + aPos * uSpan, 1.0));
  gl_PointSize = uPx;
  vT = clamp((aVal - uC0) / max(1e-6, uC1 - uC0), 0.0, 1.0);
}`;

const FS = `#version 300 es
precision mediump float;
in float vT;
uniform sampler2D uLut;
uniform float uSolid;
uniform int uRound;    // round points (a discard, which costs the early depth test)
out vec4 outColor;
void main() {
  vec3 c = uSolid > 0.5 ? vec3(0.8, 0.85, 1.0) : texture(uLut, vec2(vT, 0.5)).rgb;
  if (uRound == 1) { vec2 d = gl_PointCoord - 0.5; if (dot(d, d) > 0.25) discard; }
  outColor = vec4(c, 1.0);
}`;

// A splat: a quad clamp(px, 1, uMaxPx) pixels wide around its centre, in
// clip space so it is clipped by the near and far planes as a whole, as a
// point is (OpenGL ES 3.0 §2.17) and as the backend draws it.
const SPLAT_VS = `#version 300 es
precision highp float;
in vec2 aCorner;       // -1..1, the quad's corners
in vec3 aPos;          // per splat: 0..1 over the bounds (normalised uint16)
in float aVal;         // per splat: 0..1 colour code
in float aSize;        // per splat: 0..1 size code (#259: log10 of a smoothing length)
uniform mat4 uMV;
uniform mat4 uP;
uniform vec3 uLo;
uniform vec3 uSpan;
uniform float uRadius; // world radius (metres) * projection scale: px at 1 m
uniform float uMaxPx;
uniform vec2 uViewport;
uniform float uC0;
uniform float uC1;
uniform vec3 uClipMin;
uniform vec3 uClipMax;
uniform vec3 uSelMin;
uniform vec3 uSelMax;
uniform int uSelMode;
// Splats sized by a column (#259, sim-controls.js sizedSplat): x on, y log10
// of the smallest size, z the log span; uSizeMul turns a size into a factor of
// uRadius. Each splat gives the uniform splat's light (vLight).
uniform vec3 uSize;
uniform float uSizeMul;
out float vT;
out float vSplat;
out float vLight;
out vec2 vD;
void main() {
  bool inSel = all(greaterThanEqual(aPos, uSelMin)) && all(lessThanEqual(aPos, uSelMax));
  if (any(lessThan(aPos, uClipMin)) || any(greaterThan(aPos, uClipMax))
      || (uSelMode == 1 && !inSel) || (uSelMode == 2 && inSel)) {
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
    vT = 0.0; vSplat = 0.0; vLight = 0.0; vD = vec2(0.0);
    return;
  }
  vec4 eye = uMV * vec4(uLo + aPos * uSpan, 1.0);
  vec4 c = uP * eye;
  float f = uSize.x > 0.5 ? pow(10.0, uSize.y + aSize * uSize.z) * uSizeMul : 1.0;
  float pxU = uRadius / max(1e-4, -eye.z);
  float px = pxU * f;
  float sprite = clamp(px, 1.0, uMaxPx);
  // The uniform splat's light over this one's, as drawn (clamped sprites,
  // the sub-pixel factor): sim-controls.js lightFactor.
  float cu = clamp(pxU, 1.0, uMaxPx);
  vLight = uSize.x > 0.5 ? (cu * cu * max(min(pxU, 1.0), 0.25)) / (sprite * sprite * max(min(px, 1.0), 0.25)) : 1.0;
  // Half the width each side: sprite / 2 pixels = sprite / viewport in NDC.
  c.xy += aCorner * (sprite / uViewport) * c.w;
  gl_Position = c;
  vD = aCorner * 3.0;                          // out to 3 sigma, as the backend's splats
  vSplat = clamp(px, 0.0, 1.0);               // sub-pixel splats: their light, not a pixel's
  vT = clamp((aVal - uC0) / max(1e-6, uC1 - uC0), 0.0, 1.0);
}`;

const SPLAT_FS = `#version 300 es
precision highp float;
in float vT;
in float vSplat;
in float vLight;
in vec2 vD;
uniform sampler2D uLut;
uniform float uSolid;
uniform float uOpacity;
out vec4 outColor;
void main() {
  vec3 c = uSolid > 0.5 ? vec3(0.8, 0.85, 1.0) : texture(uLut, vec2(vT, 0.5)).rgb;
  float g = exp(-0.5 * dot(vD, vD)) * uOpacity * max(vSplat, 0.25) * vLight;
  outColor = vec4(c * g, g);
}`;

// The accumulated light onto the frame, tone-mapped (toneMap), added over
// what is there (the clear colour). highp: the light reaches tens, and the
// luminance of a faint pixel is far below fp16's resolution near 1.
const RESOLVE_VS = `#version 300 es
precision highp float;
void main() {
  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);   // one triangle over the viewport
}`;

const RESOLVE_FS = `#version 300 es
precision highp float;
precision highp int;
uniform highp sampler2D uAcc;
uniform ivec2 uOrigin;   // the viewport's corner in the frame
out vec4 outColor;
void main() {
  vec4 a = texelFetch(uAcc, ivec2(gl_FragCoord.xy) - uOrigin, 0);
  float y = dot(a.rgb, vec3(0.2126, 0.7152, 0.0722));
  // (1 - exp(-y)) / y, by its series where the subtraction would cancel.
  float f = y < 1e-3 ? 1.0 - 0.5 * y + y * y / 6.0 : (1.0 - exp(-y)) / y;
  vec3 rgb = y > 0.0 ? a.rgb * f : vec3(0.0);
  float m = max(rgb.r, max(rgb.g, rgb.b));
  if (m > 1.0) rgb /= m;
  outColor = vec4(rgb, 1.0 - exp(-max(a.a, 0.0)));
}`;

function compile(gl, vs, fs, attribs) {
  const p = gl.createProgram();
  for (const [type, src] of [[gl.VERTEX_SHADER, vs], [gl.FRAGMENT_SHADER, fs]]) {
    const s = gl.createShader(type);
    gl.shaderSource(s, src);
    gl.compileShader(s);
    if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) throw new Error(`points shader: ${gl.getShaderInfoLog(s)}`);
    gl.attachShader(p, s);
  }
  attribs.forEach((name, i) => gl.bindAttribLocation(p, i, name));
  gl.linkProgram(p);
  if (!gl.getProgramParameter(p, gl.LINK_STATUS)) throw new Error(`points link: ${gl.getProgramInfoLog(p)}`);
  const u = {};
  const n = gl.getProgramParameter(p, gl.ACTIVE_UNIFORMS);
  for (let i = 0; i < n; i++) { const info = gl.getActiveUniform(p, i); u[info.name] = gl.getUniformLocation(p, info.name); }
  return { p, u };
}

/** The sample on the GPU and its drawing. */
export class PointCloud {
  /** `target`: the splats' float target, 'auto' (float32 where it can be
   *  blended, else half floats), 'f16' (a headset's default, issue #295: half
   *  the fill cost of float32 on a Quest 3; still views are refined anyway),
   *  'f32', or '8bit' (none: added onto the frame). */
  constructor(gl, lutTexture, { target = 'auto' } = {}) {
    this.gl = gl;
    this.lut = lutTexture;
    this.prog = compile(gl, VS, FS, ['aPos', 'aVal']);
    this.splatProg = compile(gl, SPLAT_VS, SPLAT_FS, ['aPos', 'aVal', 'aCorner', 'aSize']);
    this.resolveProg = compile(gl, RESOLVE_VS, RESOLVE_FS, []);
    this.vao = gl.createVertexArray();          // points: one vertex a row
    this.splatVao = gl.createVertexArray();     // splats: a quad a row (instanced)
    this.posBuf = gl.createBuffer();
    this.valBuf = gl.createBuffer();
    this.sizeBuf = gl.createBuffer();            // splats' sizes (#259), when sent
    this.hasSize = false;
    this.cornerBuf = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, this.cornerBuf);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), gl.STATIC_DRAW);
    this.emptyVao = gl.createVertexArray();     // the resolve's triangle (gl_VertexID)
    this.count = 0;
    this.hasColour = false;
    this.maxPx = MAX_SPLAT_PX;
    this.bytes = 0;
    // The splats. float target (made on first use, at the viewport.s size).
    this.acc = null;
    const cbf = gl.getExtension('EXT_color_buffer_float');
    this.target = target;
    this.hdr = target !== '8bit' && !!(cbf || gl.getExtension('EXT_color_buffer_half_float'));
    // Float32 where it can be blended: a half float's 11 bits drop the
    // additions below half its step (at 1, under 0.0005), and large faint
    // splats (opacity over size²) are made of such additions: at size 4 the
    // preview was 15 % darker than the refined frame in half floats.
    this.float32 = target !== 'f16' && target !== '8bit' && !!(cbf && gl.getExtension('EXT_float_blend'));
  }

  /** True while splats are tone-mapped (a renderable float target). */
  get toneMapped() { return this.hdr; }

  /** The splat rule the page draws by now (SPLAT_RULES); the backend's
   *  refined frames follow it (set_point_style splat_rule). */
  get splatRule() { return this.hdr ? (this.float32 ? 'v2-f32' : 'v2-f16') : 'v2-8bit'; }

  /** Uploads a sample (`size`: its size codes, #259, or null); returns the
   *  milliseconds it took (CPU side). */
  upload(positions, colour, size = null) {
    const gl = this.gl, t0 = performance.now();
    gl.bindBuffer(gl.ARRAY_BUFFER, this.posBuf);
    gl.bufferData(gl.ARRAY_BUFFER, positions, gl.STATIC_DRAW);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.valBuf);
    gl.bufferData(gl.ARRAY_BUFFER, colour || new Uint16Array(positions.length / 3), gl.STATIC_DRAW);
    for (const [vao, divisor] of [[this.vao, 0], [this.splatVao, 1]]) {
      gl.bindVertexArray(vao);
      gl.bindBuffer(gl.ARRAY_BUFFER, this.posBuf);
      gl.enableVertexAttribArray(0);
      gl.vertexAttribPointer(0, 3, gl.UNSIGNED_SHORT, true, 6, 0);
      gl.vertexAttribDivisor(0, divisor);
      gl.bindBuffer(gl.ARRAY_BUFFER, this.valBuf);
      gl.enableVertexAttribArray(1);
      gl.vertexAttribPointer(1, 1, gl.UNSIGNED_SHORT, true, 2, 0);
      gl.vertexAttribDivisor(1, divisor);
      if (divisor) {
        gl.bindBuffer(gl.ARRAY_BUFFER, this.cornerBuf);
        gl.enableVertexAttribArray(2);
        gl.vertexAttribPointer(2, 2, gl.FLOAT, false, 8, 0);
        gl.vertexAttribDivisor(2, 0);
        // Each splat's size code (#259), per instance; without one, the constant 0.
        if (size) {
          gl.bindBuffer(gl.ARRAY_BUFFER, this.sizeBuf);
          gl.bufferData(gl.ARRAY_BUFFER, size, gl.STATIC_DRAW);
          gl.enableVertexAttribArray(3);
          gl.vertexAttribPointer(3, 1, gl.UNSIGNED_SHORT, true, 2, 0);
          gl.vertexAttribDivisor(3, 1);
        } else {
          gl.disableVertexAttribArray(3);
        }
      }
    }
    gl.bindVertexArray(null);
    if (!size) gl.vertexAttrib1f(3, 0);
    this.hasSize = !!size;
    this.count = positions.length / 3;
    this.hasColour = !!colour;
    this.bytes = positions.byteLength + (colour ? colour.byteLength : 2 * this.count) + (size ? size.byteLength : 0);
    return performance.now() - t0;
  }

  /** The float target at w x h (a 2-D texture: allowed in a VR session,
   *  issue #229 is about 3-D ones); false when it cannot be rendered to. */
  _target(w, h) {
    const gl = this.gl;
    if (this.acc && this.acc.w === w && this.acc.h === h) return true;
    if (!this.acc) this.acc = { tex: gl.createTexture(), fb: gl.createFramebuffer(), w: 0, h: 0 };
    const a = this.acc;
    gl.bindTexture(gl.TEXTURE_2D, a.tex);
    if (this.float32) gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA32F, w, h, 0, gl.RGBA, gl.FLOAT, null);
    else gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA16F, w, h, 0, gl.RGBA, gl.HALF_FLOAT, null);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    gl.bindFramebuffer(gl.FRAMEBUFFER, a.fb);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, a.tex, 0);
    let ok = gl.checkFramebufferStatus(gl.FRAMEBUFFER) === gl.FRAMEBUFFER_COMPLETE;
    if (!ok && this.float32) {
      // Float32 not renderable after all: half floats.
      this.float32 = false;
      gl.bindTexture(gl.TEXTURE_2D, a.tex);
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA16F, w, h, 0, gl.RGBA, gl.HALF_FLOAT, null);
      ok = gl.checkFramebufferStatus(gl.FRAMEBUFFER) === gl.FRAMEBUFFER_COMPLETE;
    }
    a.w = w; a.h = h;
    if (!ok) {
      // Not renderable here: splats are added onto the frame (the old look).
      gl.deleteFramebuffer(a.fb); gl.deleteTexture(a.tex);
      this.acc = null;
      this.hdr = false;
    }
    return ok;
  }

  /** Frees the GPU objects (a new cloud replaces this one). */
  dispose() {
    const gl = this.gl;
    for (const b of [this.posBuf, this.valBuf, this.cornerBuf, this.sizeBuf]) gl.deleteBuffer(b);
    for (const v of [this.vao, this.splatVao, this.emptyVao]) gl.deleteVertexArray(v);
    if (this.acc) { gl.deleteFramebuffer(this.acc.fb); gl.deleteTexture(this.acc.tex); this.acc = null; }
  }

  _common(u, view, model, p) {
    const gl = this.gl;
    gl.uniformMatrix4fv(u.uMV, false, M.multiply(view.view, model));
    gl.uniformMatrix4fv(u.uP, false, view.proj);
    const b = p.box;
    gl.uniform3fv(u.uLo, [0, 1, 2].map((i) => (b.lo[i] - b.centre[i]) / b.side));
    gl.uniform3fv(u.uSpan, [0, 1, 2].map((i) => (b.hi[i] - b.lo[i]) / b.side));
    gl.uniform1f(u.uC0, p.c0);
    gl.uniform1f(u.uC1, p.c1);
    gl.uniform3fv(u.uClipMin, p.clip ? p.clip.min : [0, 0, 0]);
    gl.uniform3fv(u.uClipMax, p.clip ? p.clip.max : [1, 1, 1]);
    // The box selection (#259): 1 only inside, 2 all but inside, 0 all.
    const sel = p.sel && p.sel.box ? p.sel : null;
    gl.uniform1i(u.uSelMode, sel ? (sel.mode === 'inside' ? 1 : sel.mode === 'outside' ? 2 : 0) : 0);
    gl.uniform3fv(u.uSelMin, sel ? sel.box.min : [0, 0, 0]);
    gl.uniform3fv(u.uSelMax, sel ? sel.box.max : [1, 1, 1]);
    gl.uniform1f(u.uSolid, this.hasColour ? 0 : 1);
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, this.lut);
    gl.uniform1i(u.uLut, 0);
  }

  /**
   * One eye, into the current framebuffer and viewport. `p`: {box
   * (dataToModel), bounds, style (STYLES), pointPx, radius (data units),
   * opacity, c0, c1, viewportH, count (rows to draw), clip ({min, max} box
   * fractions over the sample's bounds: the crop; none = all), size (splats
   * by a column, sim-controls.js sizedSplat: {log, mul}; none = uniform), sel
   * ({box, mode 'inside' | 'outside'}: the box selection, #259; none = all)}.
   */
  draw(view, model, p) {
    const gl = this.gl;
    if (!this.count) return;
    const n = Math.min(this.count, p.count ?? this.count);
    if (!isSplats(p.style)) {
      const { u } = this.prog;
      gl.useProgram(this.prog.p);
      this._common(u, view, model, p);
      gl.uniform1f(u.uPx, p.pointPx);
      // Squares up to 3 px (no discard: the early depth test stays on); discs above.
      gl.uniform1i(u.uRound, p.pointPx > 3 ? 1 : 0);
      gl.disable(gl.BLEND);
      gl.enable(gl.DEPTH_TEST);
      gl.depthFunc(gl.LESS);
      gl.depthMask(true);
      gl.bindVertexArray(this.vao);
      gl.drawArrays(gl.POINTS, 0, n);
      gl.bindVertexArray(null);
      gl.disable(gl.DEPTH_TEST);
      return;
    }
    const vp = gl.getParameter(gl.VIEWPORT);
    const [vx, vy, vw, vh] = vp;
    const frame = gl.getParameter(gl.FRAMEBUFFER_BINDING);
    const tried = this.hdr;
    const hdr = tried && this._target(vw, vh);
    if (hdr) {
      gl.bindFramebuffer(gl.FRAMEBUFFER, this.acc.fb);
      gl.viewport(0, 0, vw, vh);
      gl.clearColor(0, 0, 0, 0);
      gl.clear(gl.COLOR_BUFFER_BIT);
    } else if (tried) {
      gl.bindFramebuffer(gl.FRAMEBUFFER, frame);   // the failed target was bound
    }
    const { u } = this.splatProg;
    gl.useProgram(this.splatProg.p);
    this._common(u, view, model, p);
    // A splat's diameter in pixels: 2 r (model) x model scale x P11 x H / 2,
    // over the eye distance; drawn out to 3 sigma, as the session's (r = sigma).
    const scale = M.uniformScale(model);
    const projScale = view.proj[5] * p.viewportH * 0.5;
    gl.uniform1f(u.uRadius, 6 * (p.radius / p.box.side) * scale * projScale);
    gl.uniform1f(u.uMaxPx, this.maxPx);
    gl.uniform2f(u.uViewport, vw, vh);
    gl.uniform1f(u.uOpacity, p.opacity);
    const sized = !!(p.size && this.hasSize);
    gl.uniform3fv(u.uSize, sized ? [1, p.size.log[0], p.size.log[1]] : [0, 0, 0]);
    gl.uniform1f(u.uSizeMul, sized ? p.size.mul : 1);
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.ONE, gl.ONE);
    gl.disable(gl.DEPTH_TEST);
    gl.depthMask(false);
    gl.bindVertexArray(this.splatVao);
    gl.drawArraysInstanced(gl.TRIANGLE_STRIP, 0, 4, n);
    if (hdr) {
      gl.bindFramebuffer(gl.FRAMEBUFFER, frame);
      gl.viewport(vx, vy, vw, vh);
      gl.useProgram(this.resolveProg.p);
      gl.activeTexture(gl.TEXTURE0);
      gl.bindTexture(gl.TEXTURE_2D, this.acc.tex);
      gl.uniform1i(this.resolveProg.u.uAcc, 0);
      gl.uniform2i(this.resolveProg.u.uOrigin, vx, vy);
      gl.bindVertexArray(this.emptyVao);
      gl.drawArrays(gl.TRIANGLES, 0, 3);
    }
    gl.bindVertexArray(null);
    gl.depthMask(true);
  }
}

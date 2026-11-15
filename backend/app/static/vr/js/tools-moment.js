// A moment map of the selection in the headset (issue #191). The desktop's way:
// the lasso's mask isolates the selection into a new cube (mask_apply_bin,
// blank_outside: tools-lasso.js), and the moment is that cube's, through the
// desktop's moment route with the desktop's request (the whole channel range,
// no threshold mask: only the selection's voxels are left). Shown on a plane
// in the room with its colour bar and unit, cropped to the selection.
//
// Pure parts tested by node: backend/tests/vr/tools-moment.test.mjs.

import * as PC from './product-canvas.js';
import { applyStep, applyRecord, APPLY_MODES } from './tools-lasso.js';
import { channelsOfClip } from './selection.js';

/** The desktop's labels of the orders (MomentMapController::describeMomentOrder). */
export const ORDER_NAMES = {
  0: 'Moment 0 (Integrated intensity)',
  1: 'Moment 1 (Intensity-weighted coordinate)',
  2: 'Moment 2 (Spectral dispersion σ)',
};
/** The desktop's colour map of each order. */
export const ORDER_LUT = { 0: 'inferno', 1: 'coolwarm', 2: 'viridis' };

/** The body of POST /v1/products/moment (less dataset_id), under the
 *  fields the desktop's BackendClient::requestMoment / createMomentTask send:
 *  the whole channel range (the desktop dialog's default) and the threshold
 *  mask OFF. The dialog's default is an automatic ~3 sigma mask; on an
 *  isolated selection the selection is the mask, and a further cut would drop
 *  the faint margin the lasso grew into. */
export function momentRequest(order, depth) {
  return { moment_order: order, channel_start: 0, channel_end: Math.max(0, depth - 1),
    mask_enabled: false, threshold_value: 0, threshold_auto: false };
}

/** The body of a moment of the whole field (#257: no selection needed), over
 *  the channels [a, b] the velocity range shows, with the desktop moment
 *  dialog's defaults (MomentMapController::Config: the threshold mask on,
 *  its automatic threshold), as the desktop's remote moment sends them. */
/**
 * The spatial crop's pixels of the full cube, [x0, x1, y0, y1] (inclusive),
 * or null when x and y are not cropped (#277). `clip` the page's {min, max}
 * (box fractions); voxel i spans i/n..(i+1)/n, as the clip box the desktop
 * is told (headset.js voxelBox). A moment is a map of each pixel's own
 * spectrum, so the map of the cropped field is the whole field's map cut to
 * these pixels (the automatic mask's threshold stays the whole cube's).
 */
export function cropPixels(clip, fullShape) {
  if (!clip || !fullShape) return null;
  const r = [0, 1].map((i) => {
    const n = Math.max(1, Math.round(fullShape[i]));
    const lo = Math.min(n - 1, Math.max(0, Math.round(clip.min[i] * n)));
    const hi = Math.min(n - 1, Math.max(lo, Math.round(clip.max[i] * n) - 1));
    return [lo, hi, n];
  });
  if (r.every(([lo, hi, n]) => lo === 0 && hi === n - 1)) return null;
  return [r[0][0], r[0][1], r[1][0], r[1][1]];
}

/**
 * A whole-field map cut to the crop `clip` (#277): {box, cut, range,
 * summary, cropped, cropKey}. `values` the full map (width × height of the
 * full cube's pixels, else not cut).
 */
export function cutWhole(order, values, width, height, clip, fullShape) {
  const crop = wholeCrop(width, height, clip, fullShape);
  const box = crop || [0, width - 1, 0, height - 1];
  const cut = crop ? PC.crop(values, width, box[0], box[1], box[2], box[3]) : values;
  // One pass for the numbers shown and the exact finite range; a velocity
  // field's robust range from at most PC.RANGE_SAMPLE of its finite values,
  // the first cut as every re-cut (#300, #302): its two sorts stay a few ms.
  const st = mapStats(cut);
  const range = colourRange(order, cut, st);
  return { box, cut, range, summary: { pixels: st.pixels, sum: st.sum }, cropped: !!crop, cropKey: JSON.stringify(crop) };
}
/** At most n of the finite values of `a` (product-canvas.js finiteSample). */
export const finiteSample = PC.finiteSample;
/** The most values a robust range sorts (product-canvas.js). */
export const RANGE_SAMPLE = PC.RANGE_SAMPLE;
/** A re-cut waits for the crop to stay still this long (ms). */
export const RECUT_MS = 250;
/** The crop's pixels for a whole-field map of width × height, or null (cheap: the key of a cut). */
export function wholeCrop(width, height, clip, fullShape) {
  return fullShape && width === fullShape[0] && height === fullShape[1] ? cropPixels(clip, fullShape) : null;
}

export function wholeFieldRequest(order, [a, b]) {
  return { moment_order: order, channel_start: a, channel_end: Math.max(a, b),
    mask_enabled: true, threshold_value: 0, threshold_auto: true };
}

/** The Provenance of a whole-field moment, under the desktop's labels. */
export function wholeFieldParams(req, unit) {
  return {
    Order: ORDER_NAMES[req.moment_order] || `Moment ${req.moment_order}`,
    Channels: `${req.channel_start + 1}–${req.channel_end + 1}`,
    Mask: 'automatic threshold (the desktop dialog\'s default)',
    Scope: 'Remote full dataset',
    Unit: unit || '-',
    'Measured on': 'the whole field over the velocity range shown',
  };
}

/** The Provenance of the map, under the desktop's labels (vtkWindowCube's
 *  m_momentProvenanceParams), and where it was measured. */
export function momentParams(req, unit, isolatedFile, temporary = false) {
  return {
    Order: ORDER_NAMES[req.moment_order] || `Moment ${req.moment_order}`,
    Channels: `${req.channel_start + 1}–${req.channel_end + 1}`,
    Mask: 'none',
    Scope: 'Remote full dataset',
    Unit: unit || '-',
    'Measured on': `the Kinematic Lasso selection: its ${temporary ? 'temporary ' : ''}isolated cube${isolatedFile ? ` ${isolatedFile}` : ''} `
      + `(outside blanked${temporary ? '; deleted when the selection changes' : ''})`,
  };
}

/** The placeholder of the moment step's dataset: the isolate's answer. */
export const ISOLATED_ID = '{new_dataset_id}';

/** The replay: the steps that make the isolated cube (the grow and the
 *  isolate; or, for a received selection, what tools-lasso's applyRecord
 *  records: `steps` an array), then the moment of the cube they register. The chaining is in the request itself: dataset_id is the
 *  placeholder of the previous answer's field (as the desktop's
 *  momentDatasetSteps write {output_path}), so a replay cannot post the
 *  opened dataset's id there by accident. */
export function momentReplay(steps, req, handEdited = false) {
  const head = Array.isArray(steps) ? steps
    : [{ route: '/v1/cube/kinematic_select_bin', request: steps }, applyStep(APPLY_MODES.isolate, handEdited)];
  return [
    ...head,
    { route: '/v1/products/moment', request: { dataset_id: ISOLATED_ID, ...req },
      body: `none (JSON). dataset_id ${ISOLATED_ID}: the new_dataset_id of the previous call's answer, the isolated cube` },
  ];
}

/** Said with every moment recipe: what a replay must do, and that this is
 *  the page's own record (the desktop records no replay steps for a moment). */
export const MOMENT_CAVEAT = 'The last step measures the cube the step before it registers (mask_apply_bin): post it with that answer\'s '
  + `new_dataset_id in place of ${ISOLATED_ID}. With the opened dataset's id it would measure the whole cube, `
  + 'not the selection. Recorded by the VR page; the desktop records no replay steps for a moment.';

/** The crop shown: the selection's bounding box [x0, x1, y0, y1] grown by
 *  `pad` pixels (at least 3, a tenth of its larger side), within the map. */
export function cropBox(bbox, w, h, pad = null) {
  const [x0, x1, y0, y1] = bbox;
  const p = pad ?? Math.max(3, Math.round(0.1 * Math.max(x1 - x0 + 1, y1 - y0 + 1)));
  return [Math.max(0, x0 - p), Math.min(w - 1, x1 + p), Math.max(0, y0 - p), Math.min(h - 1, y1 + p)];
}

/** The colour range of an order: the desktop's robust symmetric range for a
 *  velocity field, the finite range otherwise. The finite range is always
 *  exact (one pass, no sort; `stats` mapStats of `values` when already
 *  made); the robust range is measured on at most PC.RANGE_SAMPLE finite
 *  values (#300, #302: all of them on a map with fewer), its median and MAD
 *  sorts being the slow part. No sort of the whole map, ever. */
export function colourRange(order, values, stats = null) {
  const st = stats || mapStats(values);
  if (order === 1) {
    const r = PC.robustSymmetricRange(values, st.pixels);
    if (r) return r;
  }
  return st.pixels ? [st.lo, st.hi] : [0, 1];
}

/** mapSummary and the finite range [lo, hi] in one pass (#300):
 *  {pixels, sum, lo, hi}, lo > hi when no pixel is finite. */
export function mapStats(values) {
  let n = 0, sum = 0, lo = Infinity, hi = -Infinity;
  for (let i = 0; i < values.length; i++) {
    const v = values[i];
    if (!Number.isFinite(v)) continue;
    n++; sum += v;
    if (v < lo) lo = v;
    if (v > hi) hi = v;
  }
  return { pixels: n, sum, lo, hi };
}

/** Map statistics shown under the title: the number of finite pixels and
 *  their sum (in the map's unit). */
export function mapSummary(values) {
  const { pixels, sum } = mapStats(values);
  return { pixels, sum };
}

/**
 * ctx: {selection, lassoTools, backend(), datasetId(), info(), onChange()}.
 */
export function createMomentTool(ctx) {
  const t = { busy: '', error: '', result: null, gen: 0, canvas: PC.makeCanvas(...PC.PLANE_PX), dirty: false,
    recipes: { lasso: null, ids: {} } };   // the recipe id of each order, for the selection they measure
  const changed = () => { t.dirty = true; ctx.onChange && ctx.onChange(); };
  // The crop's key of a whole-field map, kept with the six clip numbers it
  // was made from (#300): tick() runs every frame, and builds the key only
  // when one of them (or the map) changed. The page edits its clip in place.
  const memo = { result: null, clip: new Float64Array(6), none: true, key: '' };
  function cropKeyOf(R) {
    const c = ctx.clip();
    let same = memo.result === R && memo.none === !c;
    if (same && c) {
      for (let i = 0; i < 3 && same; i++) same = memo.clip[i] === c.min[i] && memo.clip[3 + i] === c.max[i];
    }
    if (same) return memo.key;
    memo.result = R;
    memo.none = !c;
    if (c) for (let i = 0; i < 3; i++) { memo.clip[i] = c.min[i]; memo.clip[3 + i] = c.max[i]; }
    memo.key = JSON.stringify(wholeCrop(R.width, R.height, c, R.fullShape));
    return memo.key;
  }

  /** The moment of the whole field over the velocity range shown (#257). */
  async function computeWhole(order, gen) {
    const info = ctx.info();
    const id = ctx.datasetId();
    const b = ctx.backend();
    if (!b || !id || !info) { t.error = 'A moment needs a cube from the backend.'; changed(); return; }
    const req = wholeFieldRequest(order, channelsOfClip(ctx.clipZ ? ctx.clipZ() : [0, 1], info.full_shape[2]));
    // The crop as it is when asked (#277 review), not when the answer comes.
    const clip0 = ctx.clip ? JSON.parse(JSON.stringify(ctx.clip())) : null;
    t.busy = `moment ${order} of the whole field…`;
    t.pending = 'whole';
    changed();
    try {
      const t0 = performance.now();
      const r = await b._json('POST', '/v1/products/moment', { dataset_id: id, ...req });
      if (gen !== t.gen) return;
      if (!r.valid) throw new Error(r.error || 'The moment failed.');
      const values = PC.f32FromBase64(r.data_base64);
      if (values.length !== r.width * r.height) throw new Error('bad moment payload');
      // A spatial crop (#277): the map shows, and its numbers count, the
      // cropped field only; the product saved for the desktop is the request's.
      const c = cutWhole(order, values, r.width, r.height, clip0, info.full_shape);
      t.result = { order, req, unit: r.moment_unit, bunit: r.bunit, width: r.width, height: r.height, fullShape: info.full_shape.slice(),
        values, ...c, iso: null, lasso: null, whole: true, ms: performance.now() - t0 };
      if (t.recipes.lasso !== 'whole') t.recipes = { lasso: 'whole', ids: {} };
      const rec = ctx.selection.record({ kind: 'moment', label: `Moment ${order}`,
        replay: [{ route: '/v1/products/moment', request: { dataset_id: id, ...req } }] },
      wholeFieldParams(req, r.moment_unit), null, t.recipes.ids[order] || '',
      { product: 'moment', dataset_id: id, request: req });
      t.recipes.ids[order] = rec.product.id;
      t.busy = '';
      t.pending = null;
      draw();
    } catch (e) {
      if (gen !== t.gen) return;
      t.busy = '';
      t.pending = null;
      t.error = String(e && e.message ? e.message : e);
    }
    changed();
  }

  async function compute(order) {
    const sel = ctx.selection.sel;
    const L = sel.lasso;
    const gen = ++t.gen;
    t.error = '';
    // On a lasso object, its mask; with no selection, the whole field (#257).
    if (!L || !(L.bytes || L.request)) { await computeWhole(order, gen); return; }
    t.busy = `moment ${order} of the selection…`;
    t.pending = L;
    changed();
    // The selection changed under the request: nothing to show, nor to keep busy.
    const dropped = () => {
      if (gen !== t.gen) return true;
      if (ctx.selection.sel.lasso !== L) { t.busy = ''; t.pending = null; changed(); return true; }
      return false;
    };
    try {
      const t0 = performance.now();
      // Made for the moment only: not recorded, deleted when the selection changes
      // (unless Isolate is pressed for it); one cube per selection, shared.
      const iso = await ctx.lassoTools.apply(APPLY_MODES.isolate, { record: false, temp: true });
      if (dropped()) return;
      const req = momentRequest(order, ctx.info().full_shape[2]);
      const b = ctx.backend();
      const r = await b._json('POST', '/v1/products/moment', { dataset_id: iso.id, ...req });
      if (dropped()) return;
      if (!r.valid) throw new Error(r.error || 'The moment failed.');
      const values = PC.f32FromBase64(r.data_base64);
      if (values.length !== r.width * r.height) throw new Error('bad moment payload');
      const bbox = L.stats && L.stats.bbox ? L.stats.bbox : [0, r.width - 1, 0, r.height - 1];
      const box = cropBox(bbox, r.width, r.height);
      const cut = PC.crop(values, r.width, box[0], box[1], box[2], box[3]);
      const range = colourRange(order, cut);
      t.result = { order, req, unit: r.moment_unit, bunit: r.bunit, width: r.width, height: r.height,
        values, box, cut, range, iso, lasso: L, ms: performance.now() - t0, summary: mapSummary(values) };
      // How the isolated cube is recorded (never a grow step without a request).
      const isoRec = applyRecord(L, APPLY_MODES.isolate, iso.trimBox || 0);
      // One product per order and selection: pressed again, it is recorded again in place.
      if (t.recipes.lasso !== L) t.recipes = { lasso: L, ids: {} };
      const rec = ctx.selection.record({ kind: 'moment', label: `Moment ${order}`, replay: momentReplay(isoRec.replay, req),
        replay_caveat: isoRec.caveat ? `${MOMENT_CAVEAT} ${isoRec.caveat}` : MOMENT_CAVEAT }, momentParams(req, r.moment_unit, iso.file, !!iso.temp), null, t.recipes.ids[order] || '',
        // Saved for the desktop from the isolated cube it was measured on.
        { product: 'moment', dataset_id: iso.id, request: req });
      t.recipes.ids[order] = rec.product.id;
      t.busy = '';
      t.pending = null;
      draw();
    } catch (e) {
      if (gen !== t.gen) return;
      t.busy = '';
      t.pending = null;
      t.error = String(e && e.message ? e.message : e);
    }
    changed();
  }

  function draw() {
    const R = t.result;
    if (!R || !t.canvas) return;
    const [x0, x1, y0, y1] = R.box;
    const w = x1 - x0 + 1, h = y1 - y0 + 1;
    const table = PC.lut(ORDER_LUT[R.order]);
    const du = PC.displayUnit(R.unit);
    const info = ctx.info();
    // Square sky pixels: |CDELT1| = |CDELT2| on the cubes the page shows; the
    // axes give pixels, the cube's own ticks give the sky.
    PC.drawProduct(t.canvas, {
      title: R.whole ? `Moment ${R.order} of the ${R.cropped ? 'cropped' : 'whole'} field` : `Moment ${R.order} of the selection`,
      lines: [
        `${ORDER_NAMES[R.order].replace(/^Moment \d /, '')}, channels ${R.req.channel_start + 1}–${R.req.channel_end + 1}`,
        R.whole ? `${R.summary.pixels} pixels${R.cropped ? ` in x ${R.box[0]}–${R.box[1]}, y ${R.box[2]}–${R.box[3]}` : ''}, ${R.ms.toFixed(0)} ms · automatic threshold mask`
          : `${R.summary.pixels} pixels, ${R.ms.toFixed(0)} ms · ${R.iso.file}`,
      ],
      // A velocity field: outside its robust range transparent, as the desktop
      // draws it (clampOutliers); the other orders clamp to the table's ends.
      rgba: PC.colourise(R.cut, w, h, R.range, table, null, R.order === 1), w, h, aspect: 1, table,
      xTicks: PC.ticks(x0 - 0.5, x1 + 0.5), yTicks: PC.ticks(y0 - 0.5, y1 + 0.5),
      xLabel: `x (pixel of ${info.full_shape[0]})`, yLabel: 'y (pixel)',
      bar: { lo: R.range[0] * du.scale, hi: R.range[1] * du.scale, unit: du.unit },
    });
  }

  function clear() { t.gen++; t.result = null; t.busy = t.error = ''; t.pending = null; t.recipes = { lasso: null, ids: {} }; changed(); }

  /** A moment of a selection no longer shown (a new grow, Clear) goes; a
   *  moment of the whole field stays. */
  function sync() {
    const L = ctx.selection.sel.lasso;
    if ((t.result && !t.result.whole && t.result.lasso !== L) || (t.pending && t.pending !== 'whole' && t.pending !== L)) clear();
  }

  function lines() {
    const out = [];
    if (t.error) out.push(t.error);
    if (t.busy) out.push(t.busy);
    if (t.result) {
      const du = PC.displayUnit(t.result.unit);
      out.push(`moment ${t.result.order}: ${PC.fmt(t.result.range[0] * du.scale)} to ${PC.fmt(t.result.range[1] * du.scale)} ${du.unit}`);
    }
    return out;
  }

  /** The crop changed since a whole-field map was cut: cut again from the
   *  full map it keeps (no new request; the product saved is the same). */
  function recut() {
    const R = t.result;
    if (!R || !R.whole || !ctx.clip) return false;
    if (cropKeyOf(R) === R.cropKey) return false;
    const c = cutWhole(R.order, R.values, R.width, R.height, ctx.clip(), R.fullShape);
    t.result = { ...R, ...c };
    t.recutPending = null;
    draw();
    changed();
    return true;
  }

  /**
   * Every frame (cheap: the crop's key only). A crop that changed is cut
   * again once it has stayed still for RECUT_MS, and only while the window
   * is `shown` (a hidden one is cut when it shows again). Returns whether it cut.
   */
  function tick(now, shown) {
    const R = t.result;
    if (!R || !R.whole || !ctx.clip) { t.recutPending = null; return false; }
    const key = cropKeyOf(R);
    if (key === R.cropKey) { t.recutPending = null; return false; }
    if (!t.recutPending || t.recutPending.key !== key) { t.recutPending = { key, at: now }; return false; }
    if (!shown || now - t.recutPending.at < RECUT_MS) return false;
    return recut();
  }

  return { state: t, compute, clear, sync, lines, draw, recut, tick };
}

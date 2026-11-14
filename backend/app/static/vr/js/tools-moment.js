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
 *  velocity field, the finite range otherwise. */
export function colourRange(order, values) {
  if (order === 1) {
    const r = PC.robustSymmetricRange(values);
    if (r) return r;
  }
  return PC.finiteRange(values) || [0, 1];
}

/** Map statistics shown under the title: the number of finite pixels and
 *  their sum (in the map's unit). */
export function mapSummary(values) {
  let n = 0, sum = 0;
  for (const v of values) if (Number.isFinite(v)) { n++; sum += v; }
  return { pixels: n, sum };
}

/**
 * ctx: {selection, lassoTools, backend(), datasetId(), info(), onChange()}.
 */
export function createMomentTool(ctx) {
  const t = { busy: '', error: '', result: null, gen: 0, canvas: PC.makeCanvas(...PC.PLANE_PX), dirty: false,
    recipes: { lasso: null, ids: {} } };   // the recipe id of each order, for the selection they measure
  const changed = () => { t.dirty = true; ctx.onChange && ctx.onChange(); };

  async function compute(order) {
    const sel = ctx.selection.sel;
    const L = sel.lasso;
    const gen = ++t.gen;
    t.error = '';
    if (!L || !(L.bytes || L.request)) { t.error = 'A moment of the selection: grow a Kinematic Lasso selection first.'; changed(); return; }
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
      title: `Moment ${R.order} of the selection`,
      lines: [
        `${ORDER_NAMES[R.order].replace(/^Moment \d /, '')}, channels ${R.req.channel_start + 1}–${R.req.channel_end + 1}`,
        `${R.summary.pixels} pixels, ${R.ms.toFixed(0)} ms · ${R.iso.file}`,
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

  /** A moment of a selection no longer shown (a new grow, Clear) goes. */
  function sync() {
    const L = ctx.selection.sel.lasso;
    if ((t.result && t.result.lasso !== L) || (t.pending && t.pending !== L)) clear();
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

  return { state: t, compute, clear, sync, lines, draw };
}

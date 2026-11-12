// The VR page's other data types (issue #190): point tables, velocity fields,
// and catalogue sources over a cube. Kept out of main.js, which calls a few
// hooks: the open by path or by pairing, the drawing of a frame (the layer's
// own scene when no cube is shown, the catalogue over a cube), the refinement
// of a point table, the HUD, the page's controls and a strip of buttons under
// the in-VR panel.

import * as M from './mat4.js';
import * as PT from './points.js';
import * as FL from './flow.js';
import * as CT from './catalogue.js';
import { colormapTable } from './colormaps.js';
import { niceTicks } from './volume.js';
import * as P from './vr-panel.js';

/** What a path opens as (`choice`: the page's selector, 'auto' by the name). */
export function kindOfPath(path, choice = 'auto') {
  if (choice && choice !== 'auto') return choice;
  const p = String(path || '').toLowerCase();
  if (p.endsWith('.bin.head') || p.endsWith('.head') || p.endsWith('.bin')) return 'points';
  if (/\.(csv|tsv|txt|xml|vot|votable|tbl|ipac|speck)$/.test(p)) return 'points';
  if (/(velocity|_vel|vfield|flow)[^/]*\.fits?(\.gz)?$/.test(p)) return 'flow';
  return 'cube';
}

/** The catalogue format of a file, by its name (/v1/catalogue/open). */
export function catalogueFormat(path) {
  const p = String(path || '').toLowerCase();
  if (/\.(xml|vot|votable)$/.test(p)) return 'votable';
  if (/\.(fits|fit|fits\.gz)$/.test(p)) return 'fits';
  if (/\.(tbl|ipac)$/.test(p)) return 'ipac';
  if (p.endsWith('.speck')) return 'speck';
  if (p.endsWith('.bin') || p.endsWith('.head')) return 'vbt';
  return 'csv';
}

/** The pairing's kind as the page's mode: the desktop writes cube, points or velocity. */
export function pairedMode(kind) {
  const k = String(kind || '').toLowerCase();
  if (k === 'points' || k === 'vbt' || k === 'catalogue' || k === 'table') return 'points';
  if (k === 'velocity' || k === 'flow') return 'flow';
  return 'cube';
}

const STRIP_PX = [1024, 120];
const STRIP_SIZE = [P.PANEL_SIZE[0], P.PANEL_SIZE[0] * STRIP_PX[1] / STRIP_PX[0]];

/** The strip's buttons for a mode: [{id, label}], at most five. Pure. */
export function stripButtons(mode, s) {
  if (mode === 'points') {
    return [
      { id: 'pt-style', label: s.style === 'splats' ? 'splats' : 'points' },
      { id: 'pt-colour', label: `colour: ${s.colourField || 'none'}` },
      { id: 'pt-budget', label: `${(s.budgetRows / 1e6).toFixed(1)} M rows` },
      { id: 'pt-smaller', label: 'size −' }, { id: 'pt-larger', label: 'size +' },
    ];
  }
  if (mode === 'flow') {
    return [
      { id: 'fl-stream', label: `streamlines ${s.stream ? 'on' : 'off'}`, on: s.stream },
      { id: 'fl-arrows', label: `arrows ${s.arrows ? 'on' : 'off'}`, on: s.arrows },
      { id: 'fl-shorter', label: 'arrows −' }, { id: 'fl-longer', label: 'arrows +' },
      { id: 'fl-retrace', label: 'new seeds' },
    ];
  }
  return [
    { id: 'ct-show', label: `catalogue ${s.catShow ? 'on' : 'off'}`, on: s.catShow },
    { id: 'ct-labels', label: `labels ${s.catLabels ? 'on' : 'off'}`, on: s.catLabels },
    { id: 'ct-pins', label: `through velocity ${s.catPins ? 'on' : 'off'}`, on: s.catPins },
  ];
}

export function stripLayout(buttons) {
  const n = Math.max(1, buttons.length), gap = 8, x0 = 12;
  const w = (STRIP_PX[0] - 2 * x0 - gap * (n - 1)) / n;
  return buttons.map((b, i) => ({ ...b, kind: 'button', rect: [x0 + i * (w + gap), 40, w, 68] }));
}

export function createLayers(ctx) {
  const { gl, R, state, $ } = ctx;
  const lut = gl.createTexture();
  const L = {
    mode: 'cube',
    points: null, flow: null, overlay: null, overlaySegs: null,
    s: {
      style: 'points', pointPx: 2, splatSize: 1, intensity: 35, c0: 0, c1: 1,
      colourField: null, colourLog: false, budget: 'quest', budgetRows: PT.BUDGETS.quest,
      stream: true, arrows: true, arrowScale: 1, seeds: 200, maxArrows: 50000, seed: 1,
      catShow: true, catLabels: true, catPins: true, govern: true,
    },
    box: null, info: null, load: {}, error: '',
  };
  let cloud = null, lines = null;
  // The catalogue's lines: a line set of the renderer's own kind (its line
  // program and vertex layout), so neither the box's nor the selection's.
  const catLines = (() => {
    const vao = gl.createVertexArray(), buf = gl.createBuffer();
    gl.bindVertexArray(vao);
    gl.bindBuffer(gl.ARRAY_BUFFER, buf);
    const lp = gl.getAttribLocation(R.line.p, 'aPos'), lc = gl.getAttribLocation(R.line.p, 'aColor');
    gl.enableVertexAttribArray(lp);
    gl.vertexAttribPointer(lp, 3, gl.FLOAT, false, 28, 0);
    gl.enableVertexAttribArray(lc);
    gl.vertexAttribPointer(lc, 4, gl.FLOAT, false, 28, 12);
    gl.bindVertexArray(null);
    return { vao, buf, count: 0 };
  })();
  const strip = {
    canvas: Object.assign(document.createElement('canvas'), { width: STRIP_PX[0], height: STRIP_PX[1] }),
    tex: null, key: '', layout: [], pointer: null, hit: null,
  };

  function setLut(name) {
    gl.bindTexture(gl.TEXTURE_2D, lut);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, 256, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE, colormapTable(name));
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
  }
  setLut(ctx.colormap());

  // ── The page's controls ───────────────────────────────────────────────
  const el = (tag, props = {}, ...kids) => { const e = Object.assign(document.createElement(tag), props); e.append(...kids); return e; };
  const lab = (text, input, cls) => el('label', cls ? { className: cls } : {}, text, input);
  const sel = (id, opts, value) => {
    const s = el('select', { id });
    for (const [v, t] of opts) s.appendChild(el('option', { value: v, textContent: t }));
    if (value !== undefined) s.value = value;
    return s;
  };
  const kindSel = sel('ly-kind', [['auto', 'auto (by file name)'], ['cube', 'spectral cube'],
    ['points', 'point table (VBT, catalogue)'], ['flow', 'velocity field']], 'auto');
  const boxIn = el('input', { id: 'ly-box', type: 'number', min: 0, step: 'any', placeholder: 'grid units',
    title: 'A velocity field\'s box size in Mpc (as on the desktop); empty: grid units' });
  const dataRow = el('div', { className: 'row' }, lab('Data kind', kindSel), lab('Box (Mpc)', boxIn));
  $('load').before(dataRow);

  const sec = $('layers');
  const ptSec = el('div', { id: 'ly-points', hidden: true });
  const fieldSel = sel('pt-field', []);
  const budgetSel = sel('pt-budget', Object.entries(PT.BUDGETS).map(([k, v]) => [k, `${k}: ${(v / 1e6).toFixed(1)} M rows`]), 'quest');
  const styleSel = sel('pt-style', [['points', 'points'], ['splats', 'splats (additive)']], 'points');
  const logBox = el('input', { id: 'pt-log', type: 'checkbox' });
  const sizeIn = el('input', { id: 'pt-size', type: 'range', min: 1, max: 12, step: 0.5, value: 2 });
  const c0In = el('input', { id: 'pt-c0', type: 'range', min: 0, max: 1000, value: 0 });
  const c1In = el('input', { id: 'pt-c1', type: 'range', min: 0, max: 1000, value: 1000 });
  const intIn = el('input', { id: 'pt-intensity', type: 'range', min: 1, max: 100, value: 35 });
  const ptReload = el('button', { id: 'pt-reload', textContent: 'Sample again' });
  const govBox = el('input', { id: 'pt-govern', type: 'checkbox', checked: true,
    title: 'Fewer of the sampled rows while a frame would be late (a prefix of a low-discrepancy order: spread over the whole sample)' });
  ptSec.append(
    el('strong', { textContent: 'Point table' }),
    el('div', { className: 'row' }, lab('Colour by', fieldSel), lab(' log', logBox, 'inline')),
    el('div', { className: 'row' }, lab('Rows drawn', budgetSel), lab('Style', styleSel)),
    lab('Size', sizeIn), lab('Colour from', c0In), lab('Colour to', c1In), lab('Splat intensity', intIn),
    el('div', { className: 'row' }, lab(' hold the frame rate', govBox, 'inline'), ptReload));
  const flSec = el('div', { id: 'ly-flow', hidden: true });
  const streamBox = el('input', { id: 'fl-stream', type: 'checkbox', checked: true });
  const arrowBox = el('input', { id: 'fl-arrows', type: 'checkbox', checked: true });
  const seedsIn = el('input', { id: 'fl-seeds', type: 'number', min: 10, max: 5000, value: 200 });
  const arrowsIn = el('input', { id: 'fl-maxarrows', type: 'number', min: 1000, max: 500000, step: 1000, value: 50000 });
  const scaleIn = el('input', { id: 'fl-scale', type: 'range', min: 0, max: 400, value: 100 });
  const flRetrace = el('button', { id: 'fl-retrace', textContent: 'Trace again' });
  flSec.append(
    el('strong', { textContent: 'Velocity field' }),
    el('div', { className: 'row' }, lab(' streamlines', streamBox, 'inline'), lab(' arrows', arrowBox, 'inline')),
    el('div', { className: 'row' }, lab('Seeds', seedsIn), lab('Arrows (max)', arrowsIn)),
    lab('Arrow scale', scaleIn), el('div', { className: 'row' }, flRetrace));
  const ctSec = el('div', { id: 'ly-catalogue' });
  const srcSel = sel('ct-source', [['catalogue', 'catalogue file on the backend'], ['vlkb_sources', 'VLKB compact sources'],
    ['vlkb_filaments', 'VLKB filaments'], ['crossmatch:simbad', 'cross-match: SIMBAD'], ['crossmatch:NVSS', 'cross-match: NVSS'],
    ['crossmatch:FIRST', 'cross-match: FIRST'], ['crossmatch:2MASS', 'cross-match: 2MASS']], 'catalogue');
  const catPath = el('input', { id: 'ct-path', type: 'text', spellcheck: false, placeholder: '/path/to/catalogue.csv (RA/Dec or GLON/GLAT)' });
  const bandSel = sel('ct-band', [['ft', 'band-merged'], ['70', '70 µm'], ['160', '160 µm'], ['250', '250 µm'],
    ['350', '350 µm'], ['500', '500 µm'], ['870', '870 µm'], ['1100', '1.1 mm'], ['24', '24 µm'], ['22', '22 µm'], ['21', '21 µm']], 'ft');
  const ctShow = el('input', { id: 'ct-show', type: 'checkbox', checked: true });
  const ctLabels = el('input', { id: 'ct-labels', type: 'checkbox', checked: true });
  const ctPins = el('input', { id: 'ct-pins', type: 'checkbox', checked: true });
  const ctLoad = el('button', { id: 'ct-load', textContent: 'Place on the cube' });
  const ctClear = el('button', { id: 'ct-clear', textContent: 'Clear' });
  const ctStatus = el('div', { id: 'ct-status', className: 'muted' });
  ctStatus.style.whiteSpace = 'pre-line';      // the summary, then a warning per line
  ctSec.append(
    el('strong', { textContent: 'Catalogue over the cube' }),
    lab('Sources', srcSel), lab('File', catPath), lab('VLKB band', bandSel),
    el('div', { className: 'row' }, lab(' show', ctShow, 'inline'), lab(' labels', ctLabels, 'inline'),
      lab(' through every channel (no velocity)', ctPins, 'inline')),
    el('div', { className: 'row' }, ctLoad, ctClear), ctStatus);
  const lyStatus = el('div', { id: 'ly-status', className: 'muted' });
  sec.append(ptSec, flSec, ctSec, lyStatus);

  function showSections() {
    ptSec.hidden = L.mode !== 'points';
    flSec.hidden = L.mode !== 'flow';
    ctSec.hidden = L.mode !== 'cube';
    for (const id of ['view', 'selection']) { const s = $(id); if (s) s.hidden = L.mode !== 'cube'; }
  }
  showSections();

  // ── Points ───────────────────────────────────────────────────────────
  async function fetchSample(b) {
    const p = L.points;
    const t0 = performance.now();
    // A new colour column leaves the scale to the backend (color_log null: a
    // positive column over more than three decades goes on a log scale), in
    // the same request; the answer says which.
    const body = { dataset_id: p.datasetId, max_points: L.s.budgetRows,
      color_field: L.s.colourField, color_log: L.s.autoLog ? null : L.s.colourLog,
      x: p.axes[0], y: p.axes[1], z: p.axes[2] };
    // Through the page's Backend, as every request (api.js), so a prefix or
    // a change of address it learns applies here too.
    // A newer request (another budget, another column) cancels this one: the
    // backend's sample slots and the transfer go to the one that will be shown.
    if (L.sampleAbort) L.sampleAbort.abort();
    const ac = new AbortController();
    L.sampleAbort = ac;
    const t1 = performance.now();
    let resp, buf;
    try {
      ({ resp, buf } = await b._binary('/v1/points/sample_bin', JSON.stringify(body), 'application/json', ac.signal));
    } catch (err) {
      if (ac.signal.aborted) return;          // replaced: not an error
      throw err;
    }
    if (L.sampleAbort !== ac) return;
    L.sampleAbort = null;
    const tBody = performance.now();
    const info = PT.sampleInfo((k) => resp.headers.get(k));
    const arr = PT.sampleArrays(buf, info);
    if (!cloud) cloud = new PT.PointCloud(gl, lut);
    const up = cloud.upload(arr.positions, arr.colour);
    L.s.autoLog = false;
    L.s.colourLog = info.colourLog;
    logBox.checked = info.colourLog;
    p.sample = info;
    p.drawRows = info.count;
    p.governedAt = 0;
    L.box = PT.dataToModel(info.bounds);
    L.load = { requestMs: tBody - t1, uploadMs: up, mb: buf.byteLength / 1e6, totalMs: performance.now() - t0 };
    state.extent = L.box.extent;
    decorate(p.axes);
    ctx.refiner.setDataset(p.datasetId);
  }

  async function openPoints(b, { datasetId, path }) {
    if (!datasetId) {
      const fmt = catalogueFormat(path);
      if (fmt === 'vbt') {
        const head = path.endsWith('.head') ? path : `${path}.head`;
        const r = await b._json('POST', '/v1/vbt/open', { path: head });
        if (!r.valid) throw new Error(r.error || 'The table could not be opened.');
        datasetId = r.dataset_id;
      } else {
        const r = await b._json('POST', '/v1/catalogue/open', { path, format: fmt });
        if (!r.valid) throw new Error(r.error || 'The catalogue could not be opened.');
        datasetId = r.dataset_id;
      }
    }
    const info = await b._json('POST', '/v1/points/info', { dataset_id: datasetId });
    L.points = { datasetId, path, axes: info.axes, fields: info.fields, rows: info.num_rows };
    fieldSel.innerHTML = '';
    fieldSel.appendChild(el('option', { value: '', textContent: '(none: one colour)' }));
    for (const f of info.fields) fieldSel.appendChild(el('option', { value: f, textContent: f }));
    const others = info.fields.filter((f) => !info.axes.includes(f) && !f.startsWith('@'));
    L.s.colourField = others.length ? others[0] : null;
    L.s.autoLog = true;
    L.s.colourLog = false;
    logBox.checked = false;
    fieldSel.value = L.s.colourField || '';
    enter('points');
    await fetchSample(b);
    return { kind: 'points', datasetId };
  }

  // ── Flow ─────────────────────────────────────────────────────────────
  async function fetchLines(b) {
    const f = L.flow;
    const t0 = performance.now();
    // The desktop's box when the link gave one (0: grid units); else the
    // box the field was opened with (box_mpc omitted).
    const body = { dataset_id: f.datasetId, n_seeds: L.s.seeds, max_arrows: L.s.maxArrows, seed: L.s.seed };
    if (f.boxMpc !== null && f.boxMpc !== undefined) body.box_mpc = f.boxMpc;
    const t1 = performance.now();
    const { resp, buf } = await b._binary('/v1/velocity/lines_bin', JSON.stringify(body));
    const tBody = performance.now();
    const info = FL.linesInfo((k) => resp.headers.get(k), { boxMpc: f.boxMpc });
    const arr = FL.linesArrays(buf, info);
    if (!lines) lines = new FL.FlowLines(gl, lut);
    const t2 = performance.now();
    lines.setStreamlines(arr.points, arr.speed, arr.offsets);
    f.arr = arr;
    f.info = info;
    setArrowScale();
    L.box = PT.dataToModel(info.bounds);
    L.load = { requestMs: tBody - t1, uploadMs: performance.now() - t2, mb: buf.byteLength / 1e6, totalMs: performance.now() - t0 };
    state.extent = L.box.extent;
    const unitAxis = info.frame && /supergal/i.test(info.frame) ? ['SGX', 'SGY', 'SGZ'] : ['X', 'Y', 'Z'];
    // The box the backend traced with (X-Visivo-Box-Mpc), not the one asked.
    decorate(unitAxis.map((a) => `${a}${info.boxMpc > 0 ? ' (Mpc)' : ' (grid units)'}`));
  }

  function setArrowScale() {
    const f = L.flow;
    if (!f || !f.arr || !lines) return;
    // The desktop's glyph scale: user x 3 x cell / full-resolution speed max.
    const scale = (L.s.arrowScale * 3 * f.info.cell) / Math.max(1e-12, f.info.speedMax);
    lines.setArrows(f.arr.arrowPos, f.arr.arrowVec, scale);
  }

  async function openFlow(b, { datasetId, path, boxMpc }) {
    // The box: a number (0: grid units) from the link or the page; '' or
    // missing: the one the dataset was opened with.
    const given = boxMpc === '' || boxMpc === null || boxMpc === undefined || !Number.isFinite(Number(boxMpc))
      ? null : Math.max(0, Number(boxMpc));
    if (!datasetId) {
      const r = await b._json('POST', '/v1/velocity/open', { path, box_mpc: given > 0 ? given : null });
      if (!r.valid) throw new Error(r.error || 'The velocity field could not be opened.');
      datasetId = r.dataset_id;
    }
    L.flow = { datasetId, path, boxMpc: given };
    enter('flow');
    await fetchLines(b);
    return { kind: 'flow', datasetId };
  }

  // ── Modes ────────────────────────────────────────────────────────────
  function enter(mode) {
    L.mode = mode;
    if (mode !== 'cube') {
      state.cube = null;
      state.info = null;
      state.open = null;
      state.clip = { min: [0, 0, 0], max: [1, 1, 1] };
      L.overlay = null;
      L.overlaySegs = null;
      R.setLines([], catLines);
    }
    if (mode !== 'points') L.points = null;
    if (mode !== 'flow') L.flow = null;
    if (mode === 'flow') {
      ctx.refiner.setDataset(null).then(() => {
        if (L.mode === 'flow') ctx.refiner.unavailable = 'a velocity field is drawn by the headset alone (the backend traces its lines)';
      });
    }
    showSections();
    strip.key = '';
  }

  function decorate(axes) {
    const { segs, labels } = PT.boxDecorations(L.box, axes, niceTicks);
    R.setLines(segs);
    R.setLabels(labels);
  }

  // ── Catalogue over a cube ────────────────────────────────────────────
  async function loadOverlay(b) {
    if (!state.open) throw new Error('Open a cube from the backend first.');
    const [src, cat] = srcSel.value.split(':');
    const choice = { source: src, bands: [bandSel.value], catalogue: cat };
    if (src === 'catalogue') {
      const path = catPath.value.trim();
      if (!path) throw new Error('Give the path of a catalogue on the backend.');
      const r = await b._json('POST', '/v1/catalogue/open', { path, format: catalogueFormat(path) });
      if (!r.valid) throw new Error(r.error || 'The catalogue could not be opened.');
      choice.catalogueId = r.dataset_id;
    }
    ctStatus.textContent = 'Placing the sources…';
    const t0 = performance.now();
    const ov = await b._json('POST', '/v1/cube/catalogue_pixels', CT.overlayRequest(state.open.dataset_id, choice));
    ov.ms = performance.now() - t0;
    L.overlay = ov;
    rebuildOverlay();
    // Every warning the backend gave (a reading changed, sources outside the
    // channels, a frame not converted) is shown, here and on the strip in VR.
    const warn = (ov.warnings || []).map((w) => `⚠ ${w}`).join('\n');
    ctStatus.textContent = `${CT.overlaySummary(ov)} (${ov.ms.toFixed(0)} ms)${warn ? `\n${warn}` : ''}`;
    strip.key = '';
    return ov;
  }

  /** The overlay in the cube's current box (clipping box, depth): its lines
   *  uploaded; its labels go with the cube's (main.js rebuildDecorations). */
  function placeOverlay() {
    L.overlaySegs = L.overlay && state.info
      ? CT.overlaySegments(L.overlay, state.info.full_shape, state.extent, state.clip,
        { labels: L.s.catLabels, pins: L.s.catPins })
      : null;
    R.setLines(L.overlaySegs ? L.overlaySegs.segs : [], catLines);
  }
  function rebuildOverlay() { ctx.rebuild(); }

  // ── Wiring ───────────────────────────────────────────────────────────
  const fail = (err) => { L.error = String(err.message || err); lyStatus.textContent = L.error; };
  const withBackend = (fn) => () => { try { Promise.resolve(fn(ctx.ensureBackend())).catch(fail); } catch (e) { fail(e); } };
  fieldSel.addEventListener('change', withBackend((b) => { L.s.colourField = fieldSel.value || null; L.s.autoLog = true; L.s.colourLog = false; logBox.checked = false; return L.points && fetchSample(b); }));
  logBox.addEventListener('change', withBackend((b) => { L.s.colourLog = logBox.checked; return L.points && fetchSample(b); }));
  budgetSel.addEventListener('change', withBackend((b) => { L.s.budget = budgetSel.value; L.s.budgetRows = PT.BUDGETS[budgetSel.value]; return L.points && fetchSample(b); }));
  ptReload.addEventListener('click', withBackend((b) => L.points && fetchSample(b)));
  styleSel.addEventListener('change', () => { L.s.style = styleSel.value; sizeIn.value = L.s.style === 'splats' ? L.s.splatSize : L.s.pointPx; });
  sizeIn.addEventListener('input', () => { if (L.s.style === 'splats') L.s.splatSize = Number(sizeIn.value); else L.s.pointPx = Number(sizeIn.value); });
  c0In.addEventListener('input', () => { L.s.c0 = Math.min(Number(c0In.value) / 1000, L.s.c1 - 0.001); });
  c1In.addEventListener('input', () => { L.s.c1 = Math.max(Number(c1In.value) / 1000, L.s.c0 + 0.001); });
  intIn.addEventListener('input', () => { L.s.intensity = Number(intIn.value); });
  govBox.addEventListener('change', () => { L.s.govern = govBox.checked; });
  streamBox.addEventListener('change', () => { L.s.stream = streamBox.checked; });
  arrowBox.addEventListener('change', () => { L.s.arrows = arrowBox.checked; });
  scaleIn.addEventListener('input', () => { L.s.arrowScale = Number(scaleIn.value) / 100; setArrowScale(); });
  flRetrace.addEventListener('click', withBackend((b) => {
    L.s.seeds = Math.max(10, Math.min(5000, Number(seedsIn.value) || 200));
    L.s.maxArrows = Math.max(1000, Math.min(500000, Number(arrowsIn.value) || 50000));
    return L.flow && fetchLines(b);
  }));
  ctLoad.addEventListener('click', withBackend((b) => loadOverlay(b).catch((e) => { ctStatus.textContent = String(e.message || e); })));
  ctClear.addEventListener('click', () => { L.overlay = null; ctStatus.textContent = ''; rebuildOverlay(); });
  ctShow.addEventListener('change', () => { L.s.catShow = ctShow.checked; });
  ctLabels.addEventListener('change', () => { L.s.catLabels = ctLabels.checked; rebuildOverlay(); });
  ctPins.addEventListener('change', () => { L.s.catPins = ctPins.checked; rebuildOverlay(); });

  // ── The in-VR strip ──────────────────────────────────────────────────
  function stripState() { return { ...L.s }; }
  function stripModel(panelModel) {
    return M.multiply(panelModel, M.translation(0, -(P.PANEL_SIZE[1] / 2 + STRIP_SIZE[1] / 2 + 0.008), 0));
  }
  function applyStrip(id) {
    const b = (() => { try { return ctx.ensureBackend(); } catch { return null; } })();
    const s = L.s;
    if (id === 'pt-style') { s.style = s.style === 'splats' ? 'points' : 'splats'; styleSel.value = s.style; }
    else if (id === 'pt-colour' && L.points && b) {
      const opts = [null, ...L.points.fields.filter((f) => !L.points.axes.includes(f))];
      s.colourField = opts[(opts.indexOf(s.colourField) + 1) % opts.length];
      fieldSel.value = s.colourField || '';
      fetchSample(b).catch(fail);
    } else if (id === 'pt-budget' && L.points && b) {
      const keys = ['quest-low', 'quest', 'quest-high', 'desktop', 'desktop-high'];
      s.budget = keys[(keys.indexOf(s.budget) + 1) % keys.length];
      s.budgetRows = PT.BUDGETS[s.budget];
      budgetSel.value = s.budget;
      fetchSample(b).catch(fail);
    } else if (id === 'pt-smaller' || id === 'pt-larger') {
      const f = id === 'pt-larger' ? 1.25 : 0.8;
      if (s.style === 'splats') s.splatSize = Math.max(0.25, Math.min(12, s.splatSize * f));
      else s.pointPx = Math.max(1, Math.min(12, s.pointPx * f));
    } else if (id === 'fl-stream') { s.stream = !s.stream; streamBox.checked = s.stream; }
    else if (id === 'fl-arrows') { s.arrows = !s.arrows; arrowBox.checked = s.arrows; }
    else if (id === 'fl-shorter' || id === 'fl-longer') {
      s.arrowScale = Math.max(0.05, Math.min(10, s.arrowScale * (id === 'fl-longer' ? 1.25 : 0.8)));
      scaleIn.value = Math.round(100 * s.arrowScale);
      setArrowScale();
    } else if (id === 'fl-retrace' && L.flow && b) { s.seed += 1; fetchLines(b).catch(fail); }
    else if (id === 'ct-show') { s.catShow = !s.catShow; ctShow.checked = s.catShow; }
    else if (id === 'ct-labels') { s.catLabels = !s.catLabels; ctLabels.checked = s.catLabels; rebuildOverlay(); }
    else if (id === 'ct-pins') { s.catPins = !s.catPins; ctPins.checked = s.catPins; rebuildOverlay(); }
  }
  /** The line the strip shows over its buttons: a catalogue's summary, or
   *  its first warning (and how many there are). */
  function stripNote() {
    if (L.mode !== 'cube' || !L.overlay) return null;
    const w = L.overlay.warnings || [];
    if (w.length) return { warn: true, text: `⚠ ${w[0]}${w.length > 1 ? ` (+${w.length - 1} more on the page)` : ''}` };
    return { warn: false, text: CT.overlaySummary(L.overlay) };
  }
  function stripVisible() { return L.mode !== 'cube' || !!L.overlay; }
  function stripTexture() {
    const buttons = stripButtons(L.mode, stripState());
    const note = stripNote();
    const key = JSON.stringify([buttons, strip.pointer && strip.pointer.hover, note]);
    if (!strip.tex) strip.tex = R.createPanel();
    if (key !== strip.key) {
      if (JSON.stringify(buttons.map((b) => b.id)) !== JSON.stringify(strip.layout.map((b) => b.id))) {
        strip.layout = stripLayout(buttons);
        strip.pointer = new P.PanelPointer(strip.layout);
      } else {
        strip.layout = stripLayout(buttons);
        strip.pointer.layout = strip.layout;
      }
      const c = strip.canvas.getContext('2d');
      c.clearRect(0, 0, STRIP_PX[0], STRIP_PX[1]);
      c.fillStyle = 'rgba(12, 16, 24, 0.92)';
      c.fillRect(0, 0, STRIP_PX[0], STRIP_PX[1]);
      c.fillStyle = '#9aa3b2';
      c.font = '22px -apple-system, system-ui, sans-serif';
      c.textBaseline = 'middle';
      const title = L.mode === 'points' ? 'Point table' : L.mode === 'flow' ? 'Velocity field' : 'Catalogue';
      c.fillText(title, 14, 20);
      if (note) {
        // The overlay's summary, and its first warning in amber.
        const x0 = 14 + c.measureText(title).width + 16;
        c.font = '20px -apple-system, system-ui, sans-serif';
        c.fillStyle = note.warn ? '#ffd27a' : '#9aa3b2';
        c.fillText(note.text, x0, 20, STRIP_PX[0] - x0 - 14);
      }
      for (const w of strip.layout) {
        const [x, y, ww, hh] = w.rect;
        c.fillStyle = w.on ? '#2f7fd8' : strip.pointer.hover === w.id ? '#2b3650' : '#1d2433';
        c.fillRect(x, y, ww, hh);
        c.fillStyle = '#e6e9ef';
        c.font = '24px -apple-system, system-ui, sans-serif';
        c.textAlign = 'center';
        c.fillText(w.label, x + ww / 2, y + hh / 2, ww - 12);
        c.textAlign = 'left';
      }
      R.updatePanel(strip.tex, strip.canvas);
      strip.key = key;
    }
    return strip.tex;
  }

  // ── Hooks for main.js ────────────────────────────────────────────────
  const api = {
    get mode() { return L.mode; },
    get state() { return L; },
    kindOf: (path) => kindOfPath(path, kindSel.value),
    /** Opens `path` as a point table or a velocity field; false for a cube. */
    async openPath(b, path) {
      const kind = kindOfPath(path, kindSel.value);
      if (kind === 'points') { await openPoints(b, { path }); return true; }
      if (kind === 'flow') { await openFlow(b, { path, boxMpc: boxIn.value }); return true; }
      return false;
    },
    /** The desktop's pairing of a point table or a velocity field: its dataset
     *  in the session, or its path again when the session no longer holds it. */
    async openPaired(b, pairing, onFallback) {
      const mode = pairedMode(pairing.kind);
      if (mode === 'cube') return false;
      const open = mode === 'points' ? openPoints : openFlow;
      try {
        if (!pairing.dataset) throw new Error('404: no dataset in the link');
        await open(b, { datasetId: pairing.dataset, boxMpc: pairing.box });
      } catch (err) {
        const next = await onFallback(err);
        if (next === 'fail') throw err;
        await open(b, { path: pairing.path, boxMpc: pairing.box });
      }
      return true;
    },
    /** A cube was installed: back to the cube mode, its overlay forgotten. */
    onCube() {
      if (L.mode !== 'cube') enter('cube');
      L.overlay = null;
      placeOverlay();
      ctStatus.textContent = '';
    },
    setColormap(name) { setLut(name); },
    /** The cube's lines changed (its clipping box, its depth): the overlay follows. */
    /** The cube's box changed (rebuildDecorations): the overlay is placed again
     *  and its labels returned, to be drawn with the cube's. */
    cubeLabels() {
      placeOverlay();
      return L.overlay && L.s.catShow && L.overlaySegs ? L.overlaySegs.labels : [];
    },
    /** Over the cube, after its box: the catalogue. */
    drawOverCube(view, model) {
      if (L.overlay && L.s.catShow && catLines.count) R.drawLines(view, model, catLines);
    },
    /** The scene of a point table or a velocity field, for one eye. */
    drawScene(view, eye, viewportH) {
      const model = state.model;
      if (L.mode === 'points' && cloud && L.points && L.points.sample) {
        const pt = L.points, now = performance.now();
        if (eye === 0 && L.s.govern && now - (pt.governedAt || 0) > 250) {
          // The rows drawn follow the frame time (four times a second).
          pt.governedAt = now;
          pt.drawRows = PT.governRows(pt.drawRows, pt.sample.count, R.gpuMs, ctx.targetHz());
        }
        // Sized for the rows drawn now (the governor's), so fewer cover as much.
        const drawn = L.s.govern ? pt.drawRows : pt.sample.count;
        const st = PT.splatStyle(pt.sample.bounds, pt.sample.totalRows, drawn, L.s.splatSize, L.s.intensity);
        cloud.draw(view, model, { box: L.box, style: L.s.style, pointPx: L.s.pointPx, radius: st.sampleRadius,
          opacity: st.opacity, c0: L.s.c0, c1: L.s.c1, viewportH, count: L.s.govern ? pt.drawRows : pt.sample.count });
      } else if (L.mode === 'flow' && lines && L.flow && L.flow.info) {
        lines.draw(view, model, { box: L.box, speedMin: L.flow.info.speedMin, speedMax: L.flow.info.speedMax,
          stream: L.s.stream, arrows: L.s.arrows });
      }
      if (L.mode === 'points' && ctx.refineOn() && ctx.refiner.showing) ctx.refiner.composite(view, model, eye);
      R.drawLines(view, model);
      R.drawLabels(view, model, M.uniformScale(model));
    },
    hasScene() { return L.mode !== 'cube' && !!L.box; },
    /** Refinement of a point table (none for a velocity field). */
    refineFrame(t, eyes, centreView, background) {
      if (L.mode !== 'points' || !L.points || !L.points.sample) return;
      const sm = L.points.sample;
      const commands = PT.pointLook({ axes: L.points.axes, colourField: sm.colourField, colourRange: sm.colourRange,
        colourLog: sm.colourLog, lut: colormapTable(ctx.colormap()), c0: L.s.c0, c1: L.s.c1, style: L.s.style,
        splatSize: L.s.splatSize, pointPx: L.s.pointPx, intensity: L.s.intensity });
      const key = JSON.stringify([commands, sm.bounds]);
      ctx.refiner.frame({ t, eyes, aCentre: M.multiply(centreView, state.model), model: state.model, zStretch: 1,
        look: { commands, cropError: '' }, lookKey: key, background,
        poseExtra: { data_to_model: Array.from(L.box.matrix) } });
    },
    hudLines() {
      const out = [];
      if (L.mode === 'points' && L.points && L.points.sample) {
        const s = L.points.sample;
        const d = L.s.govern ? L.points.drawRows : s.count;
        out.push(`points ${(d / 1e6).toFixed(2)} M drawn of ${(s.count / 1e6).toFixed(2)} M sampled, ${(s.totalRows / 1e6).toFixed(2)} M rows, ${L.s.style}`);
        out.push(`  sample ${L.load.mb.toFixed(1)} MB: request ${L.load.requestMs.toFixed(0)} ms, upload ${L.load.uploadMs.toFixed(1)} ms`);
      } else if (L.mode === 'flow' && L.flow && L.flow.info) {
        const i = L.flow.info;
        out.push(`flow ${i.lines} streamlines (${i.points} points), ${i.arrows} arrows, grid ${i.shape.join('×')}`);
        out.push(`  lines ${L.load.mb.toFixed(1)} MB: request ${L.load.requestMs.toFixed(0)} ms, upload ${L.load.uploadMs.toFixed(1)} ms`);
      } else if (L.overlay) {
        out.push(CT.overlaySummary(L.overlay).slice(0, 70));
      }
      if (L.error) out.push(`  ${L.error}`.slice(0, 70));
      return out;
    },
    /** The strip under the in-VR panel: pointer (ray or null), returns whether it takes the ray. */
    stripPointer(ray, pressed, panelModel) {
      if (!stripVisible()) { strip.hit = null; return false; }
      stripTexture();
      const hit = ray ? P.rayPanel(stripModel(panelModel), STRIP_SIZE, ray.o, ray.d, STRIP_PX) : null;
      strip.hit = hit;
      for (const a of strip.pointer.update(hit, pressed)) if (a.type === 'press') applyStrip(a.id);
      return strip.pointer.takesRay() || strip.pointer.owns;
    },
    stripHit() { return strip.hit; },
    drawStrip(view, panelModel) {
      if (!stripVisible()) return;
      R.drawPanel(view, stripModel(panelModel), stripTexture(), STRIP_SIZE);
    },
    pressStrip: applyStrip,
    loadOverlay: () => loadOverlay(ctx.ensureBackend()),
    listable: (name) => /\.(head|csv|tsv|xml|vot|tbl|ipac|speck)$/i.test(name),
    gpuBytes() { return (cloud ? cloud.bytes : 0) + (lines ? lines.bytesOnGpu : 0); },
  };
  return api;
}

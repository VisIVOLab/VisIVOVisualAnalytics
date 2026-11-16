// The page's side of the overlay (issue #357, overlay.js): which fields are
// overlaid and their looks, the storage beside the capacity (planned outside
// VR), loading the fields and writing them into it (in VR too: texSubImage3D
// only), the Overlay page's presses and sliders, the draw's uniforms, the
// colour bars and the hub's word. main.js owns the cube on show; this module
// owns only the overlay, and is told when the cube changes (onCube).

import * as OV from './overlay.js';
import { planOverlay, sameOverlay } from './quality.js';
import { variablesOf, fieldSource } from './sim-controls.js';
import * as LV from './levels.js';

/** The next poll of a field's brick pyramid build (ms). */
const POLL_MS = 1000;

/**
 * `deps`: {R (renderer), state (main.js state), backend() (the Backend or
 * null), cubePrep, nextTask, writeStepMs, limitBytes() (this device's share
 * for cubes), isSimulation(), log(level, event, detail), event(name, data)
 * (measurements), toast(text, key), changed() (labels and decorations to
 * redraw), dropDetail(why) (the crop's detail back to the coarse cube),
 * sliderToNorm, normToSlider}.
 */
export function createOverlay(deps) {
  const { R, state } = deps;
  const ov = {
    names: [],            // the source's variables (reduced_info fields)
    source: '',           // their data file (overlay.js fieldSource)
    fields: [],           // the names overlaid, in channel order
    looks: new Map(),     // field path -> its kept look (data values), across loads
    composite: 'sum',
    edit: 0,
    fieldPage: 0,
    on: false,            // the user wants the overlay drawn
    loading: false,
    gen: 0,
    room: '',             // why there is no storage ('' when there is one)
    loaded: null,         // {key, slot, input, fields (parsed), looks: [live look], dims}
    error: '',
    last: null,           // the last load's numbers
  };

  const cubeOk = () => !!(state.cube && state.open && state.info && deps.isSimulation() && variablesOf(state.info) && state.cube.mode !== 'synthetic');

  /** Is the overlay drawn now? */
  function drawn() {
    return !!(ov.on && ov.loaded && R.ov && R.ov.shown && ov.loaded.slot && R.ov.shown === ov.loaded.slot);
  }

  /** Is anything of the overlay drawn (a field shown)? With every field
   *  hidden the single cube is drawn instead, with its own colour bar. */
  function visible() {
    return drawn() && OV.anyOn(ov.loaded.looks, ov.loaded.fields.length);
  }

  /** The fields the storage holds at most (its channels; three without one yet). */
  function maxFields() { return R.ov ? Math.min(OV.OVERLAY_MAX, R.ov.channels) : OV.OVERLAY_MAX; }

  /** The scale last asked for field `name` (its look's, else the kept one's, else auto). */
  function askedScale(name) {
    const L = ov.loaded;
    const i = L ? L.fields.findIndex((f) => f.name === name) : -1;
    if (i >= 0 && L.looks[i] && L.looks[i].scale && L.looks[i].scale !== 'auto') return L.looks[i].scale;
    if (i >= 0) return L.fields[i].scale;
    return scaleOf(name);
  }

  /** The look of overlaid field i (live, on its codes), or null. */
  const lookOf = (i) => (ov.loaded && ov.loaded.looks[i]) || null;

  /** Outside VR: the overlay storage planned for the cube on show (beside the
   *  capacity, within the device's share), allocated or given back. */
  function ensureStorage() {
    if (R.xrActive) {
      // Nothing is allocated in VR: what the storage held before decides.
      if (!R.ov) ov.room = ov.room || 'No overlay storage: it is planned outside VR (leave VR and come back).';
      else if (!cubeOk()) ov.room = 'Overlays are for a simulation volume with two variables or more.';
      else ov.room = '';
      return;
    }
    const v = cubeOk() ? variablesOf(state.info) : null;
    if (!v || !R.cap) {
      if (R.ov) { off(''); R.releaseOverlay(); }
      ov.room = !v ? 'Overlays are for a simulation volume with two variables or more.' : 'No room for cubes on this GPU: no overlay.';
      return;
    }
    const channels = OV.storageChannels(v.names.length);
    const g = R.gpuStats();
    const limitBytes = deps.limitBytes() - (g.cubeBytes - g.overlayBytes);
    const plan = planOverlay({ capDims: R.cap.dims, channels, encoding: 'uint8', limitBytes, maxTex3D: R.maxTex3D });
    if (!plan.ok) {
      if (R.ov) { off(''); R.releaseOverlay(); }
      ov.room = plan.reason;
      deps.log('info', 'overlay-storage', `none: ${plan.reason}`);
      return;
    }
    if (!sameOverlay(plan, R.ov)) {
      off('');
      ov.loaded = null;
      try {
        const o = R.allocOverlay(plan.dims, plan.channels, plan.encoding, plan.cell);
        deps.log('info', 'overlay-storage', `${o.dims.join('×')} ${o.channels} channels, ${(o.totalBytes / 1e6).toFixed(1)} MB, ${o.allocMs.toFixed(0)} ms`);
      } catch (err) {
        try { R.releaseOverlay(); } catch { /* the context is lost */ }
        ov.room = `The overlay storage could not be allocated: ${err.message || err}`;
        deps.log('warning', 'overlay-storage-failed', String(err.message || err));
        return;
      }
    }
    ov.room = '';
  }

  /** Outside VR, before the capacity is planned again: the storage given back. */
  function releaseStorage() {
    if (R.xrActive || !R.ov) return;
    off('');
    ov.loaded = null;
    R.releaseOverlay();
  }

  /** The overlay no longer drawn (kept in its storage for a quick return). */
  function off(why) {
    const was = drawn();
    ov.on = false;
    ov.gen++;
    ov.loading = false;
    R.hideOverlay();
    if (was && why) deps.log('info', 'overlay-off', why);
    if (was) deps.changed();
  }

  /** A cube went on show (main.js showInstalled): the overlay is off; the
   *  fields picked are kept for the same source, else the source's default. */
  function onCube(info, path) {
    off('another cube is on show');
    ov.loaded = null;
    if (R.ov) R.hideOverlay();
    const v = deps.isSimulation() ? variablesOf(info) : null;
    const src = fieldSource(path);
    if (!v) { ov.names = []; ov.fields = []; ov.source = ''; return; }
    // The same source (its paths may be spelled two ways: /var and /private/var on macOS).
    const base = (p) => String(p).split(/[\\/]/).pop();
    const same = base(src) === base(ov.source) && v.names.join('\n') === ov.names.join('\n');
    ov.names = v.names.slice();
    ov.source = src;
    if (!same || ov.fields.some((n) => !v.names.includes(n)) || ov.fields.length < 2) {
      ov.fields = OV.defaultFields(v.names, v.current).slice(0, maxFields());
      ov.edit = 0;
      ov.fieldPage = 0;
    }
  }

  /** The look a field takes when loaded: its kept look (data values on its new
   *  codes) or its default (colour map i, the backend's threshold, the 99 %
   *  of its values above it). */
  function lookFor(field, i, quantile) {
    const kept = ov.looks.get(field.path);
    const base = OV.defaultLook(field, i, { cmaxNorm: Number.isFinite(quantile) && quantile > 0 ? quantile : 1, stretch: LV.DEFAULT_STRETCH });
    if (!kept) return base;
    const n = OV.lookOnField(kept, field);
    return { ...base, colormap: kept.colormap || base.colormap, stretch: kept.stretch || base.stretch, opacity: Number.isFinite(kept.opacity) ? kept.opacity : base.opacity,
      on: kept.on !== false, scale: kept.scale || 'auto', ...(n || {}) };
  }

  /** Every overlaid field's look kept as data values (a reload or a later return). */
  function keepLooks() {
    const L = ov.loaded;
    if (!L) return;
    L.fields.forEach((f, i) => { if (L.looks[i]) ov.looks.set(f.path, OV.keptLook(L.looks[i], f)); });
  }

  /** The scale asked for field `name`: the one it was last loaded with, else auto. */
  function scaleOf(name) {
    const L = ov.loaded;
    const i = L ? L.fields.findIndex((f) => f.name === name) : -1;
    if (i >= 0 && L.looks[i] && L.looks[i].scale) return L.looks[i].scale;
    const v = variablesOf(state.info);
    const path = v && v.paths[name];
    const kept = path ? ov.looks.get(path) : null;
    return (kept && kept.scale) || 'auto';
  }

  const keyOf = (names, scales) => JSON.stringify([state.open && state.open.dataset_id, state.cube && state.cube.factors, names, scales]);

  /**
   * The fields overlaid now (ov.fields) loaded, prepared, written into the
   * storage and drawn. In VR too: the storage exists, the write is
   * texSubImage3D in steps between frames, the single cube drawn meanwhile.
   * `scales`: {name: scale} to ask for (Log pressed).
   */
  async function load(scales = {}) {
    if (ov.fields.length < 2) throw new Error('two fields at least');
    if (!cubeOk()) throw new Error('no simulation volume with several variables on show');
    if (!R.ov) throw new Error(ov.room || 'no overlay storage on this device');
    const big = OV.tooLarge(state.cube.dims, R.ov.channels);
    if (big) throw new Error(big);
    const b = deps.backend();
    if (!b) throw new Error('no backend');
    const names = ov.fields.slice();
    const asked = names.map((n) => scales[n] || scaleOf(n));
    const key = keyOf(names, asked);
    keepLooks();
    // The same fields on the same scales still in the storage: drawn again at once.
    if (ov.loaded && ov.loaded.key === key && R.ov.gen === ov.loaded.slot.gen) {
      ov.gen++;                      // a load still on its way is superseded
      ov.loading = false;
      ov.error = '';
      R.showOverlay(ov.loaded.slot, ov.loaded.input);
      R.setOverlayColormaps(ov.loaded.looks.map((l) => l.colormap));
      ov.on = true;
      deps.dropDetail('the overlay is on');
      deps.changed();
      return 'shown';
    }
    const gen = ++ov.gen;
    const stale = () => { if (gen !== ov.gen) { const e = new Error('superseded'); e.superseded = true; throw e; } };
    const cube = state.cube;
    const red = cube.reduction || {};
    const req = { dataset_id: state.open.dataset_id, fields: names, scales: asked, channels: R.ov.channels,
      factor_xy: red.factor_xy || 0, factor_z: red.factor_z || 0, max_dim: red.max_dim || 256, mode: cube.mode === 'max' ? 'max' : 'mean',
      encoding: 'uint8', source: red.source || 'auto' };
    ov.loading = true;
    ov.error = '';
    deps.changed();
    const t0 = performance.now();
    try {
      let r;
      for (;;) {
        r = await b.reducedFields(req);
        stale();
        if (state.cube !== cube) { const e = new Error('superseded'); e.superseded = true; throw e; }
        if (!r.preparing) break;
        ov.status = `Building the fields' brick pyramids… ${(100 * r.progress).toFixed(0)} %`;
        deps.changed();
        await new Promise((res) => setTimeout(res, POLL_MS));
        stale();
      }
      const tNet = performance.now();
      if (r.dims.join() !== cube.dims.join() || r.factors.join() !== cube.factors.join()) {
        throw new Error(`the fields came ${r.dims.join('×')} binned ${r.factors.join('×')}, not on the cube's grid ${cube.dims.join('×')} binned ${cube.factors.join('×')}`);
      }
      const fields = r.fields;
      const pre = fields.map((f, i) => lookFor(f, i, NaN));
      const input = { data: r.data, dims: r.dims, channels: r.channels, encoding: r.encoding, fields };
      // Off the main thread: each field's skip grid, colour range and the apron.
      const prep = await deps.cubePrep.prepare(input, { cell: 8, channels: r.channels, fields: fields.length,
        aboves: pre.map((l) => l.thrNorm), store: R.ov.dims.slice() });
      stale();
      input.prep = prep;
      keepLooks();                   // edits made to the overlay on show while this one loaded
      const looks = fields.map((f, i) => {
        const l = lookFor(f, i, prep.quantiles ? prep.quantiles[i] : NaN);
        l.scale = asked[i];
        return l;
      });
      const tPrep = performance.now();
      if (!R.ov || !R.fitsOverlay(input)) throw new Error('the overlay storage changed meanwhile: try again');
      const job = R.beginOverlayWrite(input, 8, prep);
      while (!job.step(deps.writeStepMs)) {
        await deps.nextTask();
        if (gen !== ov.gen || state.cube !== cube) { job.cancelled = true; const e = new Error('superseded'); e.superseded = true; throw e; }
      }
      R.setOverlayColormaps(looks.map((l) => l.colormap));
      R.showOverlay(job.slot, input);
      ov.loaded = { key, slot: job.slot, input, fields, looks, dims: r.dims.slice() };
      ov.on = true;
      ov.loading = false;
      ov.status = '';
      if (ov.edit >= fields.length) ov.edit = 0;
      deps.dropDetail('the overlay is on');
      const t1 = performance.now();
      ov.last = { fields: names, mb: r.bytes / 1e6, netMs: Math.round(tNet - t0), prepMs: Math.round(tPrep - tNet), writeMs: Math.round(t1 - tPrep),
        totalMs: Math.round(t1 - t0), steps: job.steps, maxStepMs: Math.round(job.maxStepMs), inXR: !!R.xrActive, offMain: !!prep.offMain };
      deps.log('info', 'overlay', `${names.join(' + ')} (${asked.join(', ')}): ${r.dims.join('×')}×${r.channels}, ${(r.bytes / 1e6).toFixed(1)} MB in ${ov.last.netMs} ms, `
        + `prepared ${ov.last.prepMs} ms${prep.offMain ? ' (worker)' : ''}, written ${ov.last.writeMs} ms in ${job.steps} steps${R.xrActive ? ' in VR' : ''}`);
      deps.event('overlay-shown', { fields: names, ms: ov.last.totalMs, mb: Math.round(r.bytes / 1e5) / 10, inXR: ov.last.inXR, steps: job.steps });
      deps.changed();
      return 'shown';
    } catch (err) {
      if (gen === ov.gen) { ov.loading = false; ov.status = ''; }
      if (err.superseded) return 'superseded';
      // A write cancelled under it (a lost context, the storage planned again): not the user's failure.
      if (err.code === 'write-cancelled') { ov.on = false; deps.log('info', 'overlay-cancelled', String(err.message || err)); deps.changed(); return 'cancelled'; }
      ov.error = String(err.message || err);
      ov.on = false;
      deps.log('warning', 'overlay-failed', ov.error.slice(0, 200));
      deps.toast('The overlay could not be shown', 'overlay');
      deps.changed();
      throw err;
    }
  }

  /** Load and report a failure on the page (never throws). */
  function show(scales) {
    load(scales).catch(() => { /* said in ov.error, logged */ });
  }

  // ── The Overlay page ─────────────────────────────────────────────────────

  /** The hand menu's context (hand-menu.js ctx.ov), or null when it has none. */
  function menuContext() {
    if (!cubeOk() || ov.names.length < 2) return null;
    const L = ov.loaded;
    const fields = ov.fields.map((name, i) => {
      const k = L ? L.fields.findIndex((f) => f.name === name) : -1;
      const l = k >= 0 ? L.looks[k] : null;
      return { name, colormap: l ? l.colormap : OV.OVERLAY_DEFAULT_MAPS[i % 3], log: askedScale(name) === 'log', on: l ? l.on !== false : true };
    });
    // A cube whose fields would be a larger answer than the backend gives (overlay.js OVERLAY_MAX_BYTES).
    const big = R.ov ? OV.tooLarge(state.cube.dims, R.ov.channels) : '';
    return { fields, edit: ov.edit, composite: ov.composite, shown: drawn(), drawing: visible(), loading: ov.loading, room: ov.room || big,
      fieldPage: ov.fieldPage, max: maxFields() };
  }

  /** The edited field's index among the loaded ones (-1 when it is not loaded). */
  function editedLoaded() {
    const L = ov.loaded;
    const name = ov.fields[ov.edit];
    return L && name ? L.fields.findIndex((f) => f.name === name) : -1;
  }

  /** The Overlay page's slider positions, notes and status lines. */
  function view(values, notes, status) {
    const L = ov.loaded, k = editedLoaded();
    const l = k >= 0 ? L.looks[k] : null, f = k >= 0 ? L.fields[k] : null;
    if (l) {
      values['ov-thr'] = deps.normToSlider(l.thrNorm) / 1000;
      values['ov-cmax'] = deps.normToSlider(l.cmaxNorm) / 1000;
      values['ov-opacity'] = l.opacity / 2;
      const kept = OV.keptLook(l, f);
      const unit = f.bunit ? ` ${f.bunit}` : '';
      notes.thr = `${f.name}: ${fmtV(kept.threshold)}${unit}${f.scale === 'log' ? ' (log scale)' : ''}`;
      notes.cmax = `${fmtV(kept.colourMax)}${unit}`;
      notes.opacity = l.opacity.toFixed(2);
      notes.cmap = l.colormap;
    }
    notes.fields = ov.fields.length < 2 ? 'Pick two fields or three.' : ov.fields.join(' + ');
    if (ov.room) status.push([ov.room, '#ffd27a']);
    if (ov.loading) status.push([ov.status || `Loading ${ov.fields.join(' + ')}…`, '#ffd27a']);
    if (ov.error) status.push([`The overlay could not be shown: ${ov.error}`, '#ff8a8a']);
    if (drawn()) {
      status.push([ov.composite === 'mip' ? 'MIP: the brightest field value along each ray, in its own colours.'
        : 'Sum: the fields emit and absorb together; where they overlap their colours mix by opacity.', '#b9c2d0']);
      if (ov.last) status.push([`${ov.last.fields.join(' + ')}: ${ov.last.mb.toFixed(1)} MB, shown in ${(ov.last.totalMs / 1000).toFixed(1)} s`, '#b9c2d0']);
    }
  }

  const fmtV = (v) => {
    if (!Number.isFinite(v)) return 'n/a';
    const a = Math.abs(v);
    return a !== 0 && (a < 1e-3 || a >= 1e5) ? v.toExponential(2) : String(Number(v.toPrecision(3)));
  };

  /** A press on the Overlay page (true when it was one of its widgets). */
  function press(id, w) {
    if (!id.startsWith('ov-') && id !== 'overlay-page') return false;
    if (id === 'overlay-page') return false;     // main.js turns the page
    if (w.group === 'ov-field') {
      const before = ov.fields.join();
      ov.fields = OV.toggleField(ov.fields, w.value, maxFields());
      if (ov.fields.join() === before) return true;
      if (ov.edit >= ov.fields.length) ov.edit = Math.max(0, ov.fields.length - 1);
      if (ov.on || ov.loading) { if (ov.fields.length >= 2) show(); else off('fewer than two fields'); }
      deps.changed();
      return true;
    }
    if (id === 'ov-fields-more') { ov.fieldPage++; return true; }
    if (w.group === 'ov-edit') { ov.edit = w.value; return true; }
    if (id === 'ov-on') {
      if (drawn() || ov.loading) off('switched off');
      else show();
      deps.changed();
      return true;
    }
    if (w.group === 'ov-comp') { ov.composite = w.value; deps.changed(); return true; }
    const k = editedLoaded();
    const L = ov.loaded;
    if (id === 'ov-log') {
      const name = ov.fields[ov.edit];
      if (!name) return true;
      // From the scale asked for last (a field loaded on log and asked linear while off reads linear).
      const sc = askedScale(name) === 'log' ? 'linear' : 'log';
      if (L && k >= 0) L.looks[k].scale = sc;
      const v = variablesOf(state.info);
      const path = v && v.paths[name];
      if (path) ov.looks.set(path, { ...(ov.looks.get(path) || {}), ...(L && k >= 0 ? OV.keptLook(L.looks[k], L.fields[k]) : {}), scale: sc });
      if (ov.on || ov.loading) show({ [name]: sc });
      deps.changed();
      return true;
    }
    if (k < 0) return true;
    const l = L.looks[k];
    if (w.group === 'ov-cmap') {
      l.colormap = w.value;
      R.setOverlayColormaps(L.looks.map((x) => x.colormap));
    } else if (id === 'ov-show-field') {
      l.on = l.on === false;
    } else if (id === 'ov-thr-reset') {
      const d = OV.defaultLook(L.fields[k], k, { cmaxNorm: L.input.prep && L.input.prep.quantiles ? L.input.prep.quantiles[k] : 1 });
      l.thrNorm = d.thrNorm; l.cmaxNorm = d.cmaxNorm;
    }
    deps.changed();
    return true;
  }

  /** A slider of the Overlay page held (true when it was one). */
  function slide(id, value) {
    if (!id.startsWith('ov-')) return false;
    const k = editedLoaded();
    if (k < 0) return true;
    const l = ov.loaded.looks[k];
    if (id === 'ov-thr') l.thrNorm = Math.min(l.cmaxNorm - 1e-3, deps.sliderToNorm(value * 1000));
    else if (id === 'ov-cmax') l.cmaxNorm = Math.max(l.thrNorm + 1e-3, deps.sliderToNorm(value * 1000));
    else if (id === 'ov-opacity') l.opacity = Math.max(0.01, value * 2);
    deps.changed();
    return true;
  }

  // ── Drawing ──────────────────────────────────────────────────────────────

  /** The stretch parameter of a field's look (levels.js stretchParam, as stretchA in main.js). */
  function stretchA(l, f) {
    if (f.scale === 'log' && l.stretch === 'log') return LV.LOG_A_DEFAULT;
    return LV.stretchParam(l.stretch, { gamma: LV.DEFAULT_GAMMA, thrData: OV.keptLook(l, f).threshold, cmaxData: OV.keptLook(l, f).colourMax });
  }

  /** The draw's parameters from the single cube's (`base`, main.js volParams). */
  function params(base, override = {}) {
    const L = ov.loaded;
    const u = OV.overlayUniforms(L.looks, L.fields, { stretchCode: (s) => LV.STRETCH_CODE[s] || 0, stretchA });
    return { ...base, ov: u, mode: override.mode !== undefined ? override.mode : OV.COMPOSITE_CODE[ov.composite] || 0 };
  }

  /** The colour bars, one a field shown (levels.js volumeBar objects with their colour map). */
  let barsCache = { key: '', bars: [] };
  function bars() {
    const L = ov.loaded;
    if (!drawn()) return [];
    const key = JSON.stringify(L.looks.map((l) => [l.thrNorm, l.cmaxNorm, l.colormap, l.stretch, l.on]) + L.key);
    if (key !== barsCache.key) {
      barsCache = { key, bars: L.fields.map((f, i) => {
        const l = L.looks[i];
        if (l.on === false) return null;
        const bar = LV.volumeBar({ thrNorm: l.thrNorm, cmaxNorm: l.cmaxNorm, lo: f.lo, hi: f.hi, scale: f.scale, stretch: l.stretch,
          stretchA: stretchA(l, f), title: `${i + 1} ${f.name}`, unit: f.bunit || '' });
        return { ...bar, colormap: l.colormap, key: JSON.stringify([i, key]) };
      }).filter(Boolean) };
    }
    return barsCache.bars;
  }

  /** The `overlay` key of the hub's look event, and its summary, or null. */
  function hub() {
    if (!visible()) return null;
    const L = ov.loaded;
    return OV.hubOverlay(L.fields, L.looks, ov.composite);
  }

  /** The readout's overlay ({data, dims, channels, encoding, fields}) while it is drawn. */
  function readout() { return visible() ? ov.loaded.input : null; }

  /** The undo step's part: the looks and the compositing. */
  function snapshot() {
    const L = ov.loaded;
    return L ? { key: L.key, looks: L.looks.map((l) => ({ ...l })), composite: ov.composite } : null;
  }
  function restore(s) {
    const L = ov.loaded;
    if (!s || !L || s.key !== L.key) return;
    L.looks = s.looks.map((l) => ({ ...l }));
    ov.composite = s.composite;
    R.setOverlayColormaps(L.looks.map((l) => l.colormap));
  }

  /** After a lost context (R.restore wrote the overlay on show again into a
   *  new storage): its new slot, or the overlay off when it could not be. */
  function afterRestore() {
    const L = ov.loaded;
    if (!L) return;
    if (R.ov && R.ov.shown && R._ovInput === L.input) { L.slot = R.ov.shown; R.setOverlayColormaps(L.looks.map((l) => l.colormap)); return; }
    ov.loaded = null;
    ov.on = false;
  }

  return { ov, drawn, visible, afterRestore, ensureStorage, releaseStorage, off, onCube, load, show, menuContext, view, press, slide, params, bars, hub, readout,
    snapshot, restore, lookOf };
}

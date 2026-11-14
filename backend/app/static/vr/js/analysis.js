// The headset's analysis tools beyond the selections (issue #191), put
// together: lasso seeds and Isolate/Remove (tools-lasso.js), the moment of
// the selection (tools-moment.js), the PV path (tools-pv.js), their page of
// the hand menu (tools-panel.js's layout and drawing; issue #201) and their
// section on the page, and the product textures shown in result windows.
// main.js calls it through a few hooks: segments, act, press, view,
// productTexture, reset.

import * as PC from './product-canvas.js';
import * as TP from './tools-panel.js';
import { createLassoTools, SEED_MODE_NAMES } from './tools-lasso.js';
import { createMomentTool } from './tools-moment.js';
import { createPvTool } from './tools-pv.js';

/**
 * ctx: {state, selection, renderer, backend() (null without a cube from the
 * backend), onChange(), showDataset(id, path), mount (a DOM element or null)}.
 */
export function createAnalysis(ctx) {
  const st = () => ctx.state;
  const datasetId = () => (st().open ? st().open.dataset_id : '');
  const info = () => st().info;
  let dirty = true;
  const changed = () => { dirty = true; ctx.onChange && ctx.onChange(); };
  const base = { selection: ctx.selection, backend: ctx.backend, datasetId, info, onChange: changed };
  const lasso = createLassoTools({ ...base, showDataset: ctx.showDataset, path: () => st().openPath || '', cube: () => st().cube });
  const moment = createMomentTool({ ...base, lassoTools: lasso, clipZ: () => [st().clip.min[2], st().clip.max[2]] });
  const pv = createPvTool({ ...base, state: st });
  const show = { moment: true, pv: true };

  // ── The menu's Analysis page (tools-panel.js layout) ──────────────────────
  const layout = TP.toolsLayout();
  const tex = { moment: null, pv: null };

  function view() {
    const sel = ctx.selection.sel;
    const status = [...lasso.lines(), ...moment.lines(), ...pv.lines()];
    const err = lasso.state.error || moment.state.error || pv.state.error;
    return {
      seedMode: sel.seedMode || 'new', moment: moment.state.result && show.moment ? moment.state.result.order : null,
      pvArmed: pv.state.armed, canApply: !!(sel.lasso && (sel.lasso.bytes || sel.lasso.request)), canShow: !!lasso.state.applied,
      pvWidth: pv.state.width, status, error: err,
    };
  }

  function press(id, w) {
    if (w && w.group === 'seed') lasso.setSeedMode(w.value);
    else if (id === 'isolate') lasso.run('isolate');
    else if (id === 'remove') lasso.run('remove');
    else if (id === 'show-applied') lasso.showApplied();
    else if (w && w.group === 'moment') { show.moment = true; moment.compute(w.value); }
    else if (id === 'moment-hide') { show.moment = false; changed(); }
    else if (id === 'pv-draw') { show.pv = true; pv.arm(); }
    else if (id === 'pv-undo') pv.undo();
    else if (id === 'pv-new') pv.newPath();
    else if (id === 'pv-narrow') pv.setWidth(pv.state.width - 1);
    else if (id === 'pv-wide') pv.setWidth(pv.state.width + 1);
    else if (id === 'pv-hide') { show.pv = false; changed(); }
  }

  /** A product's window shown or closed (closing is the page's Hide). */
  function setShown(which, on) { if (show[which] !== on) { show[which] = on; changed(); } }

  function productTexture(which) {
    const tool = which === 'moment' ? moment : pv;
    if (!tool.state.result || !show[which] || !tool.state.canvas) return null;
    const R = ctx.renderer;
    if (!tex[which]) tex[which] = R.createPanel();
    if (tool.state.uploaded !== tool.state.result) {
      R.updatePanel(tex[which], tool.state.canvas);
      tool.state.uploaded = tool.state.result;
    }
    return tex[which];
  }

  /** The trigger or a click: true when the PV path took it. */
  /** Follow the selection: a new grow, Clear or a shared selection drops the
   *  moment and the cubes of the old one (the implicit ones are deleted). */
  function sync() {
    pv.poll();
    lasso.sync();
    moment.sync();
  }

  function act(ray) {
    sync();
    if (!pv.state.armed || !st().cube) return false;
    show.pv = true;           // a point added opens the PV window again
    pv.act(ray);
    return true;
  }

  function segments() {
    const h = ctx.selection.sel.hover;
    return pv.segments(h ? h.p : null);
  }

  function reset() {
    lasso.reset();
    moment.clear();
    pv.newPath();
    pv.state.armed = false;
    changed();
  }

  // ── The page (desktop browser) ────────────────────────────────────────────
  const dom = {};
  if (ctx.mount && typeof document !== 'undefined') {
    const sec = document.createElement('section');
    sec.id = 'analysis';
    sec.innerHTML = `
      <div class="row">
        <label>Lasso seed <select id="an-seed">
          <option value="new">${SEED_MODE_NAMES.new}</option><option value="add">${SEED_MODE_NAMES.add}</option>
          <option value="carve">${SEED_MODE_NAMES.carve}</option></select></label>
        <button id="an-isolate" title="A new cube with everything outside the selection blanked (the desktop's Isolate)">Isolate</button>
        <button id="an-remove" title="A new cube with the selection blanked (the desktop's Remove)">Remove</button>
        <button id="an-show" disabled title="Load the cube just made in this view">Show result cube</button>
      </div>
      <div class="row">
        <span title="Of the Kinematic Lasso selection (its mask); with no selection, of the whole field over the velocity range">Moment</span>
        <button id="an-m0">Intensity (M0)</button><button id="an-m1">Velocity (M1)</button><button id="an-m2">Dispersion (M2)</button>
      </div>
      <div class="row">
        <button id="an-pv" title="Clicks (or the trigger) add the path's points">Draw PV slice</button>
        <button id="an-pv-undo">Undo</button><button id="an-pv-new">New path</button>
        <label>PV width (px) <input id="an-pv-width" type="number" min="1" max="25" step="1" value="1"></label>
      </div>
      <div id="an-status" class="muted"></div>
      <div id="an-products"></div>`;
    ctx.mount.after(sec);
    const $ = (id) => sec.querySelector(`#${id}`);
    Object.assign(dom, { sec, status: $('an-status'), seed: $('an-seed'), show: $('an-show'), pvBtn: $('an-pv'), width: $('an-pv-width'), products: $('an-products') });
    $('an-seed').addEventListener('change', () => lasso.setSeedMode($('an-seed').value));
    $('an-isolate').addEventListener('click', () => lasso.run('isolate'));
    $('an-remove').addEventListener('click', () => lasso.run('remove'));
    $('an-show').addEventListener('click', () => lasso.showApplied());
    for (const o of [0, 1, 2]) $(`an-m${o}`).addEventListener('click', () => { show.moment = true; moment.compute(o); });
    $('an-pv').addEventListener('click', () => { show.pv = true; pv.arm(); });
    $('an-pv-undo').addEventListener('click', () => pv.undo());
    $('an-pv-new').addEventListener('click', () => pv.newPath());
    $('an-pv-width').addEventListener('change', () => pv.setWidth(Number($('an-pv-width').value)));
    for (const which of ['moment', 'pv']) {
      const tool = which === 'moment' ? moment : pv;
      if (tool.state.canvas && tool.state.canvas.tagName) {
        tool.state.canvas.className = 'sel-canvas';
        tool.state.canvas.hidden = true;
        dom.products.appendChild(tool.state.canvas);
      } else {
        const c = document.createElement('canvas');
        c.width = PC.PLANE_PX[0]; c.height = PC.PLANE_PX[1]; c.className = 'sel-canvas'; c.hidden = true;
        dom.products.appendChild(c);
        dom[`${which}Canvas`] = c;
      }
    }
  }

  /** Bring the page's copy up to date (cheap; called when something changed). */
  function refreshDom() {
    sync();
    if (!dirty || !dom.sec) return;
    dirty = false;
    const v = view();
    dom.status.textContent = v.status.join('\n');
    dom.seed.value = v.seedMode;
    dom.show.disabled = !v.canShow;
    dom.pvBtn.textContent = v.pvArmed ? 'Drawing: click to add points' : 'Draw PV slice';
    dom.pvBtn.classList.toggle('primary', v.pvArmed);
    dom.width.value = String(pv.state.width);
    for (const which of ['moment', 'pv']) {
      const tool = which === 'moment' ? moment : pv;
      const c = dom[`${which}Canvas`] || tool.state.canvas;
      const has = !!tool.state.result && show[which];
      if (c) c.hidden = !has;
      if (has && dom[`${which}Canvas`] && tool.state.canvas && dom[`${which}Canvas`].drawn !== tool.state.result) {
        const d = dom[`${which}Canvas`].getContext('2d');
        d.clearRect(0, 0, c.width, c.height);
        d.drawImage(tool.state.canvas, 0, 0);
        dom[`${which}Canvas`].drawn = tool.state.result;
      }
    }
  }

  function hudLines() {
    const out = [];
    if (moment.state.result) out.push(`moment ${moment.state.result.order} ${moment.state.result.ms.toFixed(0)} ms`);
    if (pv.state.result) out.push(`PV ${pv.state.result.ms.toFixed(0)} ms (${pv.state.result.samples}×${pv.state.result.depth})`);
    if (lasso.state.applied) out.push(`${lasso.state.applied.mode === 'blank_inside' ? 'remove' : 'isolate'} ${lasso.state.applied.ms.toFixed(0)} ms`);
    return out.length ? [out.join('  ')] : [];
  }

  return { lasso, moment, pv, sync, act, segments, press, view, productTexture, setShown,
    shown: (which) => !!show[which], reset, refreshDom, hudLines, layout, isDirty: () => dirty };
}

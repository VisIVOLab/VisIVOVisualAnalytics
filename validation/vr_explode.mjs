// A simulation model taken apart (issue #371), run by vr_lifecycle.mjs
// --explode (alone) and --regressions (after the overlay's checks, on the
// page they leave: no new load, one short VR session), through the WebXR
// stand-in (XR_FAKE) with the texture probe (TEX3D_PROBE). On the synthetic
// three-field VBT of vr_overlay.mjs:
//   - value ranges of the variable on show: three pieces of the cube's own
//     texture, apart, drawn in VR frames (the animation runs to the end);
//   - the overlaid fields as pieces: three, each its own channel;
//   - no 3-D texture allocated in VR for any of it;
//   - a hidden piece is not drawn; assembled, the volume is drawn as before;
//   - the frame cost: the draw time and steps per ray of the single cube, the
//     overlay and the exploded pieces on the same view.

import { writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { makeOverlayVolume } from './vr_overlay.mjs';

const allocsIn = (p) => (p.inXR.texImage3D || 0) + (p.inXR.texStorage3D || 0);
const median = (a) => { const s = [...a].sort((x, y) => x - y); return s[s.length >> 1]; };

/** The draw time (ms) of the volume as drawn now and of the single cube, on the same view. */
async function drawCost(c, { rounds, frames }) {
  const one = [], now = [];
  for (let r = 0; r < rounds; r++) {
    one.push((await c.evaluate(`window.__vrVolumePixels({ single: true, frames: ${frames} })`)).ms);
    now.push((await c.evaluate(`window.__vrVolumePixels({ frames: ${frames} })`)).ms);
  }
  return { singleMs: +median(one).toFixed(3), ms: +median(now).toFixed(3) };
}

/**
 * `reuse`: the page already shows the overlay's synthetic volume (after
 * runOverlayChecks): no load. `cheap`: fewer timed draws (CI).
 */
export async function runExplodeChecks({ c, check, waitFor, base, token, python, shape = undefined, reuse = false, cheap = false, out = '', results = {} }) {
  const L = 'explode';
  const cost = cheap ? { rounds: 3, frames: 4 } : { rounds: 7, frames: 30 };
  const onSim = reuse && await c.evaluate(`!!(window.__vrState.cube && window.__vrState.loading === 0 && window.__vrState.openPath.includes('ov.bin')
    && typeof window.__tex3dProbe === 'function' && window.__vrExplode)`);
  if (!onSim) {
    const data = makeOverlayVolume(python, shape);
    await c.send('Page.navigate', { url: `${base}/vr/?debug&refine=0&xrfake&n=explode#token=${encodeURIComponent(token)}` });
    const up = await waitFor(c, `!!window.__vrShell && window.__vrShell.life.state === 'ok' && location.search.includes('n=explode')
      && typeof window.__tex3dProbe === 'function'`, 30000);
    if (!up.value) { check(`${L}: page up with the texture probe`, false); return results; }
    await c.evaluate(`(() => { const $ = (id) => document.getElementById(id); $('encoding').value = 'uint8'; $('fxy').value = '2'; $('fz').value = '2';
      $('scale').value = 'auto'; $('path').value = ${JSON.stringify(`${data}::vbt-field=0`)}; $('load').click(); })()`);
    const loaded = await waitFor(c, `!!(window.__vrState.cube && window.__vrState.loading === 0 && window.__vrState.openPath.endsWith('ov.bin::vbt-field=0'))`, 60000);
    check(`${L}: the density loaded`, !!loaded.value, `${loaded.ms} ms`);
    if (!loaded.value) return results;
  }
  const disp = await c.evaluate(`(() => { const m = window.__vrMenu; m.ui.page = 'display'; return m.layout().map((w) => w.id); })()`);
  check(`${L}: Display's Rendering row leads to the Explode page`, disp.includes('explode-page'), disp.filter((i) => /render|overlay|explode/.test(i)).join(','));

  await c.evaluate(`(async () => { const b = document.getElementById('enter-vr');
    for (let i = 0; i < 50 && b.disabled; i++) await new Promise((r) => setTimeout(r, 50)); b.click(); })()`);
  const running = await waitFor(c, 'window.__xrfake.frames > 10 && !!window.__vrXR() && window.__vrTex3D().xrActive', 15000);
  if (!running.value) { check(`${L}: VR session draws frames`, false); return results; }
  const p0 = await c.evaluate('window.__tex3dProbe()');
  // The single variable alone first (the overlay off), every value above a quarter of its range drawn.
  await c.evaluate(`(() => { window.__vrOverlay.off('explode checks'); const s = window.__vrState; s.thrNorm = 0.25; s.cmaxNorm = 1; })()`);
  const single = await c.evaluate('window.__vrVolumePixels({ single: true, frames: 1 })');
  const singleSteps = await c.evaluate('window.__vrVolumePixels({ single: true, override: { mode: 2 }, frames: 1 })');

  // Value ranges of the variable on show, through the page's press (animated over real VR frames).
  const press = (id, extra = {}) => c.evaluate(`(() => { const m = window.__vrMenu; m.ui.page = 'explode'; m.layout();
    m.act({ type: 'press', id: ${JSON.stringify(id)}, ...${JSON.stringify(extra)} }); })()`);
  await press('ex-src:bands');
  await press('ex-bands:3');
  const f0 = await c.evaluate('window.__xrfake.frames');
  await press('ex-go');
  const apart = await waitFor(c, `window.__vrExplode.amount() >= 1 && window.__xrfake.frames > ${f0} + 5 && window.__vrExplode.stats.pieces === 3`, 10000);
  const st = await c.evaluate(`(() => { const e = window.__vrExplode; return { amount: e.amount(), pieces: e.stats.pieces, draws: e.stats.draws,
    labels: e.menuContext().pieces.map((p) => p.label), hud: e.hudLine() }; })()`);
  check(`${L}: value ranges: three pieces of the cube's texture, apart, drawn in VR frames`, !!apart.value && st.pieces === 3,
    `${apart.ms} ms; ${JSON.stringify(st)}`);
  const bands = await c.evaluate('window.__vrVolumePixels({ frames: 1 })');
  const bandSteps = await c.evaluate('window.__vrVolumePixels({ override: { mode: 2 }, frames: 1 })');
  check(`${L}: the ranges draw (the exploded view in place of the volume)`, bands.explode && bands.fraction > 0.005,
    `${(100 * bands.fraction).toFixed(2)} % of the view, single cube ${(100 * single.fraction).toFixed(2)} %`);
  results.bands = { ...(await drawCost(c, cost)), fraction: +bands.fraction.toFixed(4), singleFraction: +single.fraction.toFixed(4),
    steps: bandSteps.steps && +bandSteps.steps.toFixed(1), singleSteps: singleSteps.steps && +singleSteps.steps.toFixed(1) };

  // Mid-animation: the volume shrinking (drawn once), then the pieces parting at their scale; neither costs more than the cube.
  const mid = {};
  for (const amount of [0.2, 0.6]) {
    await c.evaluate(`window.__vrExplode.set(true, { amount: ${amount} })`);
    const px = await c.evaluate('window.__vrVolumePixels({ frames: 1 })');
    const st2 = await c.evaluate('({ whole: window.__vrExplode.stats.whole, pieces: window.__vrExplode.stats.pieces })');
    mid[amount] = { ...(await drawCost(c, cost)), fraction: +px.fraction.toFixed(4), ...st2 };
  }
  await c.evaluate('window.__vrExplode.set(true, { now: true })');
  results.midAnimation = mid;
  check(`${L}: mid-animation the volume shrinks alone (0.2), then the pieces part at their scale (0.6)`,
    mid[0.2].whole && mid[0.2].pieces === 0 && !mid[0.6].whole && mid[0.6].pieces === 3, JSON.stringify(mid));

  // The clipping plane (#369) cuts every piece alike, in its own box: on, the pieces draw less.
  const hasPlane = await c.evaluate('!!(window.__vrPlane && window.__vrPlane.get().available)');
  if (hasPlane) {
    const open = await c.evaluate('window.__vrVolumePixels({ frames: 1 })');
    await c.evaluate('window.__vrPlane.set({ on: true, slab: false })');
    const cut = await c.evaluate('window.__vrVolumePixels({ frames: 1 })');
    const cutPieces = await c.evaluate('window.__vrExplode.stats.pieces');
    await c.evaluate('window.__vrPlane.set({ on: false })');
    results.plane = { open: +open.fraction.toFixed(4), cut: +cut.fraction.toFixed(4) };
    check(`${L}: the clipping plane cuts the pieces (each in its own box)`, cut.explode && cutPieces === 3 && cut.fraction < 0.9 * open.fraction && cut.fraction > 0,
      `${(100 * open.fraction).toFixed(2)} % -> ${(100 * cut.fraction).toFixed(2)} % with the plane`);
  }

  // A hidden piece is not drawn.
  await press('ex-piece:1');
  await c.evaluate('window.__vrVolumePixels({ frames: 1 })');
  const hid = await c.evaluate('window.__vrExplode.stats.pieces');
  await press('ex-piece:1');
  check(`${L}: a hidden piece is not drawn`, hid === 2, `${hid} drawn with one hidden`);

  // The overlaid fields as pieces (the overlay switched on in VR: a write, no allocation).
  await c.evaluate(`(() => { const m = window.__vrMenu; m.ui.page = 'overlay'; m.layout(); m.act({ type: 'press', id: 'ov-on' }); })()`);
  const ovOn = await waitFor(c, 'window.__vrOverlay.drawn() && !window.__vrOverlay.ov.loading', 30000);
  await c.evaluate(`(() => { const L = window.__vrOverlay.ov.loaded; if (L) L.looks.forEach((l) => { l.thrNorm = 0.25; l.cmaxNorm = 1; l.on = true; }); })()`);
  const ovPx = await c.evaluate('window.__vrVolumePixels({ single: false, frames: 1 })');
  await press('ex-src:fields');
  await c.evaluate('window.__vrVolumePixels({ frames: 1 })');
  const fs = await c.evaluate(`(() => { const e = window.__vrExplode; return { pieces: e.stats.pieces, src: e.ex.pieces.map((p) => p.src + ':' + p.channel),
    labels: e.menuContext().pieces.map((p) => p.label) }; })()`);
  const fields = await c.evaluate('window.__vrVolumePixels({ frames: 1 })');
  const fieldSteps = await c.evaluate('window.__vrVolumePixels({ override: { mode: 2 }, frames: 1 })');
  check(`${L}: the overlaid fields as pieces: three, each its own channel of the overlay storage`,
    !!ovOn.value && fs.pieces === 3 && fs.src.join() === 'overlay:0,overlay:1,overlay:2' && fields.explode && fields.fraction > 0.005,
    `${JSON.stringify(fs)}; ${(100 * fields.fraction).toFixed(2)} % of the view`);
  results.fields = { ...(await drawCost(c, cost)), fraction: +fields.fraction.toFixed(4), steps: fieldSteps.steps && +fieldSteps.steps.toFixed(1) };
  // The overlay on the same view, assembled, for comparison.
  await c.evaluate('window.__vrExplode.set(false, { now: true })');
  const ovCost = await drawCost(c, cost);
  const ovSteps = await c.evaluate('window.__vrVolumePixels({ override: { mode: 2 }, frames: 1 })');
  results.overlay = { ms: ovCost.ms, fraction: +ovPx.fraction.toFixed(4), steps: ovSteps.steps && +ovSteps.steps.toFixed(1) };
  const assembled = await c.evaluate('window.__vrVolumePixels({ frames: 1 })');
  check(`${L}: assembled, the overlay is drawn as before`, !assembled.explode && assembled.overlay, JSON.stringify({ explode: assembled.explode, overlay: assembled.overlay }));
  check(`${L}: the frame cost measured (single cube, overlay, ranges, fields)`, [results.bands.ms, results.fields.ms, results.overlay.ms].every(Number.isFinite),
    JSON.stringify({ bands: results.bands, fields: results.fields, overlay: results.overlay }));

  const p1 = await c.evaluate('window.__tex3dProbe()');
  check(`${L}: no 3-D texture allocated in VR (ranges, fields, the overlay written)`, allocsIn(p1) === 0 && allocsIn(p0) === 0,
    `in-VR texImage3D/texStorage3D ${allocsIn(p1)}`);
  const errs = await c.evaluate(`window.__vrShell.diag.items.filter((e) => e.level === 'error').map((e) => e.event + ': ' + e.message)`);
  check(`${L}: no page error`, !errs.length, errs.join(' / '));
  await c.evaluate('window.__vrShell.act("exit-vr")');
  await waitFor(c, '!window.__vrXR()', 10000);
  if (!cheap && out) {
    // The desktop view, for a look: the fields on a ring, then the density's ranges in a row.
    const shot = async (name) => {
      await new Promise((r) => setTimeout(r, 800));
      const r = await c.send('Page.captureScreenshot', { format: 'png' });
      writeFileSync(join(out, `explode_${name}.png`), Buffer.from(r.data, 'base64'));
    };
    await c.evaluate(`window.__vrExplode.set(true, { source: 'fields', layout: 'ring', now: true })`);
    await shot('fields_ring');
    await c.evaluate(`(() => { window.__vrOverlay.off('screenshot'); window.__vrExplode.set(true, { source: 'bands', bands: 4, layout: 'row', now: true }); })()`);
    await shot('ranges_row');
    await c.evaluate(`window.__vrExplode.set(false, { now: true })`);
  }
  return results;
}

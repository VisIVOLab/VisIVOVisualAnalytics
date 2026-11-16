// Two or three variables of a simulation volume overlaid (issue #357), run by
// vr_lifecycle.mjs --overlay (and --xrfake, --regressions) through the WebXR
// stand-in (XR_FAKE) with the texture probe (TEX3D_PROBE). A synthetic VBT of
// three fields (a density shell over seven decades, a hot spot of temperature
// beside it, a signed velocity) is loaded by its density; the overlay storage
// must exist before the VR session; in the session the Overlay page shows the
// three fields together, each on its own scale (log, log, linear), and:
//   - no 3-D texture is allocated (the fields are written into the storage);
//   - each field draws with its own colour map (red, green, blue: with only
//     one field shown its channel dominates), threshold and opacity; one field
//     alone draws exactly as the single cube does with the same look;
//   - one colour bar a field shown; MIP draws; the readout gives each value;
//   - the variable picker turns the overlay off, and it comes back on the new
//     variable's cube, still without a 3-D allocation; the crop's detail is
//     not asked for while the overlay is on;
//   - the headset hub's look event names the fields;
//   - the frame cost: the draw time and steps per ray of the single cube and
//     of the overlay, measured on the same view.

import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { spawnSync } from 'node:child_process';

const allocsIn = (p) => (p.inXR.texImage3D || 0) + (p.inXR.texStorage3D || 0);
const median = (a) => { const s = [...a].sort((x, y) => x - y); return s[s.length >> 1]; };

/** The draw time (ms) of the single cube and of the overlay on the same view:
 *  `rounds` alternations of `frames` draws each, the median of each. */
async function drawCost(c, { rounds = 7, frames = 30 } = {}) {
  const one = [], ov = [];
  for (let r = 0; r < rounds; r++) {
    one.push((await c.evaluate(`window.__vrVolumePixels({ single: true, frames: ${frames} })`)).ms);
    ov.push((await c.evaluate(`window.__vrVolumePixels({ frames: ${frames} })`)).ms);
  }
  return { singleMs: +median(one).toFixed(3), overlayMs: +median(ov).toFixed(3) };
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/** A VBT volume of three fields (rho, T, vx) in a new temporary directory; its data file's path. */
export function makeOverlayVolume(python, shape = [24, 48, 64]) {
  const dir = mkdtempSync(join(tmpdir(), 'vr-overlay-'));
  const code = `
import sys, numpy as np
nz, ny, nx = ${shape.join(', ')}
z, y, x = np.mgrid[0:nz, 0:ny, 0:nx].astype(np.float32)
r = np.sqrt((x - nx / 2) ** 2 + (y - ny / 2) ** 2 + (2 * (z - nz / 2)) ** 2)
rho = (1e-12 * 10 ** (7 * np.exp(-(r - 14 * nx / 64) ** 2 / (10 * (nx / 64) ** 2)))).astype('<f4')
rt = np.sqrt((x - nx * 0.25) ** 2 + (y - ny / 2) ** 2 + (z - nz / 2) ** 2)
T = (1e3 * 10 ** (4 * np.exp(-rt ** 2 / (40 * (nx / 64) ** 2)))).astype('<f4')
vx = (100 * np.sin(x / 6) * np.cos(y / 9)).astype('<f4')
data = sys.argv[1] + '/ov.bin'
with open(data, 'wb') as fh:
    rho.tofile(fh); T.tofile(fh); vx.tofile(fh)
open(data + '.head', 'w').write('\\n'.join(['float', '3', f'{nx * ny * nz} {nx} {ny} {nz} 1.0 1.0 2.0', 'little', 'rho', 'T', 'vx']) + '\\n')
print(data)
`;
  const r = spawnSync(python, ['-c', code, dir], { encoding: 'utf8' });
  if (r.status !== 0) throw new Error(`could not write the overlay volume: ${r.stderr}`);
  return r.stdout.trim();
}

export async function runOverlayChecks({ c, check, waitFor, api, base, token, python, shape = undefined, results = {} }) {
  const L = 'overlay';
  const data = makeOverlayVolume(python, shape);
  await c.send('Page.navigate', { url: `${base}/vr/?debug&refine=0&xrfake&n=overlay#token=${encodeURIComponent(token)}` });
  const up = await waitFor(c, `!!window.__vrShell && window.__vrShell.life.state === 'ok' && location.search.includes('n=overlay')
    && typeof window.__tex3dProbe === 'function'`, 30000);
  if (!up.value) { check(`${L}: page up with the texture probe`, false); return results; }
  await c.evaluate(`(() => { const $ = (id) => document.getElementById(id); $('encoding').value = 'uint8'; $('fxy').value = '2'; $('fz').value = '2';
    $('scale').value = 'auto'; $('path').value = ${JSON.stringify(`${data}::vbt-field=0`)}; $('load').click(); })()`);
  const loaded = await waitFor(c, `!!(window.__vrState.cube && window.__vrState.loading === 0 && window.__vrState.openPath.endsWith('ov.bin::vbt-field=0'))`, 60000);
  check(`${L}: the density loaded`, !!loaded.value, `${loaded.ms} ms`);
  if (!loaded.value) return results;

  // Before any session: the storage beside the capacity, three channels (RGB8), not on show.
  const g0 = await c.evaluate('window.__vrGpu()');
  check(`${L}: the overlay storage is allocated before VR (3 channels, the capacity's dims)`,
    !!(g0.overlay && g0.overlay.channels === 3 && g0.capacity && g0.overlay.dims.join() === g0.capacity.dims.join() && !g0.overlay.onShow),
    JSON.stringify({ overlay: g0.overlay, capacity: g0.capacity && g0.capacity.dims }));
  const disp = await c.evaluate(`(() => { const m = window.__vrMenu; m.ui.page = 'display'; return m.layout().map((w) => w.id); })()`);
  check(`${L}: Display's Rendering row leads to the Overlay page`, disp.includes('overlay-page'), disp.filter((i) => /render|overlay/.test(i)).join(','));

  // One VR session.
  await c.evaluate(`(async () => { const b = document.getElementById('enter-vr');
    for (let i = 0; i < 50 && b.disabled; i++) await new Promise((r) => setTimeout(r, 50)); b.click(); })()`);
  const running = await waitFor(c, 'window.__xrfake.frames > 20 && !!window.__vrXR() && window.__vrTex3D().xrActive', 15000);
  if (!running.value) { check(`${L}: VR session draws frames`, false); return results; }
  const p0 = await c.evaluate('window.__tex3dProbe()');
  const single = await c.evaluate('window.__vrVolumePixels({ single: true, frames: 1 })');
  const singleSteps = await c.evaluate('window.__vrVolumePixels({ single: true, override: { mode: 2 }, frames: 1 })');

  const press = (page, id) => c.evaluate(`(() => { const m = window.__vrMenu; m.ui.page = '${page}'; m.layout(); m.act({ type: 'press', id: ${JSON.stringify(id)} }); })()`);
  const t0 = Date.now();
  await press('overlay', 'ov-on');
  const on = await waitFor(c, 'window.__vrOverlay.drawn() && !window.__vrOverlay.ov.loading', 30000);
  const onMs = Date.now() - t0;
  const ovState = await c.evaluate(`(() => { const o = window.__vrOverlay.ov; return { error: o.error, room: o.room, last: o.last,
    fields: o.loaded ? o.loaded.fields.map((f) => [f.name, f.scale]) : null }; })()`);
  check(`${L}: in VR the Overlay page shows rho, T and vx together`, !!on.value && JSON.stringify(ovState.fields && ovState.fields.map((f) => f[0])) === '["rho","T","vx"]',
    `${onMs} ms; ${JSON.stringify(ovState)}`);
  if (!on.value) return results;
  check(`${L}: each field on its own scale (rho log, T log, vx linear)`, JSON.stringify(ovState.fields.map((f) => f[1])) === '["log","log","linear"]', JSON.stringify(ovState.fields));
  const p1 = await c.evaluate('window.__tex3dProbe()');
  check(`${L}: the overlay written in VR with no 3-D texture allocated`, allocsIn(p1) === 0 && allocsIn(p0) === 0
    && (p1.inXR.texSubImage3D || 0) > (p0.inXR.texSubImage3D || 0),
    `in-VR texImage3D/texStorage3D ${allocsIn(p1)}; texSubImage3D ${(p1.inXR.texSubImage3D || 0) - (p0.inXR.texSubImage3D || 0)}; ${JSON.stringify(ovState.last)}`);
  results.loadMs = onMs;
  results.last = ovState.last;

  // The frame cost on the same view: the single cube against the three fields summed.
  const all = await c.evaluate('window.__vrVolumePixels({ frames: 1 })');
  const allSteps = await c.evaluate('window.__vrVolumePixels({ override: { mode: 2 }, frames: 1 })');
  results.cost = { ...(await drawCost(c)), singleSteps: singleSteps.steps && +singleSteps.steps.toFixed(1),
    overlaySteps: allSteps.steps && +allSteps.steps.toFixed(1), canvas: `${all.w}×${all.h}`, singleFraction: +single.fraction.toFixed(3), overlayFraction: +all.fraction.toFixed(3) };
  // At the default thresholds the synthetic fields can sit entirely below
  // them in this view on a software GL (CI SwiftShader drew nothing for the
  // single cube either), so coverage is asserted at the lower thresholds below.
  check(`${L}: the overlay path draws (frame cost measured)`, all.overlay && Number.isFinite(all.ms), JSON.stringify(results.cost));

  // The cost again with every field over a quarter of its range (more of the box drawn).
  await c.evaluate(`(() => { const L = window.__vrOverlay.ov.loaded; L.looks.forEach((l) => { l.thrNorm = 0.25; l.cmaxNorm = 1; }); window.__vrState.thrNorm = 0.25; window.__vrState.cmaxNorm = 1; })()`);
  const singleLow = await c.evaluate('window.__vrVolumePixels({ single: true, frames: 1 })');
  const allLow = await c.evaluate('window.__vrVolumePixels({ frames: 1 })');
  const singleLowSteps = await c.evaluate('window.__vrVolumePixels({ single: true, override: { mode: 2 }, frames: 1 })');
  const allLowSteps = await c.evaluate('window.__vrVolumePixels({ override: { mode: 2 }, frames: 1 })');
  results.costLow = { ...(await drawCost(c)), singleSteps: +singleLowSteps.steps.toFixed(1), overlaySteps: +allLowSteps.steps.toFixed(1),
    singleFraction: +singleLow.fraction.toFixed(3), overlayFraction: +allLow.fraction.toFixed(3) };
  check(`${L}: the overlay covers the view at a quarter of each range (cost measured)`, allLow.overlay && allLow.fraction > 0.01, JSON.stringify(results.costLow));

  // Per-field looks: one field shown at a time, its own colour map's channel dominating.
  const look = (i, patch) => c.evaluate(`(() => { const L = window.__vrOverlay.ov.loaded; Object.assign(L.looks[${i}], ${JSON.stringify(patch)}); window.__vrMenu.ui.page = 'overlay'; })()`);
  // Thresholds low enough for each field to cover part of the view.
  for (const i of [0, 1, 2]) await look(i, { thrNorm: 0.25, cmaxNorm: 1 });
  const only = async (k) => {
    for (const i of [0, 1, 2]) await look(i, { on: i === k });
    return c.evaluate('window.__vrVolumePixels({ frames: 1 })');
  };
  const px = [await only(0), await only(1), await only(2)];
  const dom = (m, ch) => m[ch] > 1.3 * Math.max(...m.filter((_, j) => j !== ch));
  check(`${L}: each field in its own colour map (red, green, blue: its channel dominates when shown alone)`,
    dom(px[0].mean, 0) && dom(px[1].mean, 1) && dom(px[2].mean, 2) && px.every((p) => p.fraction > 0.002),
    px.map((p) => `${p.mean.map((v) => v.toFixed(0)).join('/')} (${(100 * p.fraction).toFixed(1)} %)`).join('; '));
  // The temperature's hot spot sits beside the density's shell: shown alone, the two cover different pixels.
  // A field's threshold: raised, it draws less.
  await look(0, { on: true });
  await look(1, { on: false });
  await look(2, { on: false });
  await c.evaluate(`(() => { const l = window.__vrOverlay.ov.loaded.looks[0]; l.thrNorm = 0.3; l.cmaxNorm = 1; })()`);
  const before = await c.evaluate('window.__vrVolumePixels({ frames: 1 })');
  await c.evaluate(`(() => { const l = window.__vrOverlay.ov.loaded.looks[0]; l.thrNorm = 0.6; })()`);
  const after = await c.evaluate('window.__vrVolumePixels({ frames: 1 })');
  check(`${L}: a field's own threshold: raised, the field covers less`, after.fraction < before.fraction * 0.9,
    `${(100 * before.fraction).toFixed(2)} % -> ${(100 * after.fraction).toFixed(2)} %`);
  // One field alone with the single cube's look draws as the single cube does (the same codes, the same rule).
  await c.evaluate(`(() => { const s = window.__vrState, L = window.__vrOverlay.ov.loaded;
    Object.assign(L.looks[0], { on: true, thrNorm: s.thrNorm, cmaxNorm: s.cmaxNorm, stretch: s.stretch, opacity: s.opacity, colormap: s.colormap });
    L.looks[1].on = false; L.looks[2].on = false;
    window.__vrOverlaySetMaps(L.looks.map((l) => l.colormap)); })()`);
  const alone = await c.evaluate('window.__vrVolumePixels({ frames: 1 })');
  const singleNow = await c.evaluate('window.__vrVolumePixels({ single: true, frames: 1 })');
  const close = (a, b) => Math.abs(a - b) <= Math.max(0.5, 0.01 * Math.max(a, b));
  check(`${L}: one field alone, with the single cube's look, draws as the single cube`,
    Math.abs(alone.fraction - singleNow.fraction) <= 0.002 && alone.mean.every((v, i) => close(v, singleNow.mean[i])),
    `overlay ${(100 * alone.fraction).toFixed(2)} % ${alone.mean.map((v) => v.toFixed(1)).join('/')}; single ${(100 * singleNow.fraction).toFixed(2)} % ${singleNow.mean.map((v) => v.toFixed(1)).join('/')}`);
  // Back to the three-colour look, all shown.
  await c.evaluate(`(() => { const L = window.__vrOverlay.ov.loaded; ['red', 'green', 'blue'].forEach((m, i) => { L.looks[i].colormap = m; L.looks[i].on = true; });
    window.__vrOverlaySetMaps(['red', 'green', 'blue']); })()`);

  // The legend: one bar a field shown; MIP draws.
  const bars = await c.evaluate('window.__vrOverlay.bars().map((b) => b.title)');
  check(`${L}: one colour bar a field, titled by its name`, bars.length === 3 && /rho/.test(bars[0]) && /T/.test(bars[1]) && /vx/.test(bars[2]), bars.join(' | '));
  await press('overlay', 'ov-comp:mip');
  const mip = await c.evaluate('window.__vrVolumePixels({ frames: 20 })');
  results.cost.mipMs = +mip.ms.toFixed(3);
  check(`${L}: MIP draws the brightest field value`, mip.overlay && mip.fraction > 0.01 && (await c.evaluate('window.__vrOverlay.ov.composite')) === 'mip',
    `${(100 * mip.fraction).toFixed(1)} %, ${mip.ms.toFixed(2)} ms`);
  await press('overlay', 'ov-comp:sum');
  const ro = await c.evaluate(`(() => { const o = window.__vrOverlay.readout(); return o ? o.fields.map((f) => f.name) : null; })()`);
  const lines = await c.evaluate(`window.__vrReadoutAt ? window.__vrReadoutAt([0.5, 0.5, 0.5]) : null`);
  check(`${L}: the readout gives each field's value`, JSON.stringify(ro) === '["rho","T","vx"]' && !!lines && /rho .* T .* vx /.test(lines[lines.length - 1]),
    lines ? lines[lines.length - 1] : 'no readout hook');

  // The headset hub: a look event naming the overlaid fields.
  await sleep(1500);
  const session = await c.evaluate('window.__vrBackend().session');
  let ev = null;
  for (let i = 0; i < 20 && !ev; i++) {
    const act = await api('GET', '/v1/vr/activity?after=0&wait=0', token, { session });
    let events = [];
    try { events = JSON.parse(act.body).events || []; } catch { events = []; }
    ev = events.filter((e) => e.kind === 'look' && e.payload && e.payload.overlay).pop() || null;
    if (!ev) await sleep(300);
  }
  check(`${L}: the desktop is told through the hub (a look event with the overlay)`,
    !!ev && ev.payload.overlay.fields.map((f) => f.name).join() === 'rho,T,vx' && /^Overlay/.test(ev.summary), ev ? ev.summary : 'no look event with the overlay');

  // The crop's detail is not asked for while the overlay is on.
  await c.evaluate(`(() => { window.__vrState.clip = { min: [0.25, 0.25, 0.25], max: [0.6, 0.6, 0.6] }; })()`);
  // As long as the detail takes to come with the overlay off (below), and more.
  await sleep(8000);
  const det = await c.evaluate('({ shown: !!window.__vrDetail.shown, busy: window.__vrDetail.busy })');
  // The overlay off: the same crop gets its detail (the cube is binned 2×, level 0 is finer); on again: dropped.
  await press('overlay', 'ov-on');
  const got = await waitFor(c, '!!window.__vrDetail.shown', 15000);
  await press('overlay', 'ov-on');
  const again = await waitFor(c, 'window.__vrOverlay.drawn() && !window.__vrDetail.shown', 15000);
  check(`${L}: no crop detail while the overlay is on (off: the detail comes; on again: dropped)`, !det.shown && !det.busy && !!got.value && !!again.value,
    JSON.stringify({ whileOn: det, detailWhenOff: !!got.value, droppedWhenOn: !!again.value }));
  await c.evaluate(`(() => { window.__vrState.clip = { min: [0, 0, 0], max: [1, 1, 1] }; })()`);

  // The variable picker turns the overlay off; it comes back on the velocity's cube.
  await press('display', 'var:vx');
  const vx = await waitFor(c, `window.__vrState.info.field_name === 'vx' && window.__vrState.loading === 0 && !window.__vrVariables.pending()`, 30000);
  const offNow = await c.evaluate('window.__vrOverlay.drawn()');
  check(`${L}: picking a variable shows it alone (the overlay off)`, !!vx.value && !offNow, `vx ${!!vx.value}, overlay ${offNow}`);
  await press('overlay', 'ov-on');
  const back = await waitFor(c, 'window.__vrOverlay.drawn() && !window.__vrOverlay.ov.loading', 30000);
  const p2 = await c.evaluate('window.__tex3dProbe()');
  const f2 = await c.evaluate('window.__vrOverlay.ov.loaded ? window.__vrOverlay.ov.loaded.fields.map((f) => f.name) : null');
  check(`${L}: the overlay again on the velocity's cube, still no 3-D allocation in VR`, !!back.value && allocsIn(p2) === 0 && JSON.stringify(f2) === '["rho","T","vx"]',
    `fields ${JSON.stringify(f2)}; in-VR texImage3D/texStorage3D ${allocsIn(p2)}`);

  const errs = await c.evaluate(`window.__vrShell.diag.items.filter((e) => e.level === 'error').map((e) => e.event + ': ' + e.message)`);
  check(`${L}: no page error`, !errs.length, errs.join(' / '));
  await c.evaluate('window.__vrShell.act("exit-vr")');
  await waitFor(c, '!window.__vrXR()', 10000);
  return results;
}

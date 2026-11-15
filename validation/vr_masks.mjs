// Mask layers in the headset (issue #282), run by vr_lifecycle.mjs
// --regressions (and --xrfake, or --masks alone) through the WebXR stand-in.
//
// A synthetic label mask on the synthetic cube's grid (source 1: the
// Gaussian's bright core, source 2: a small box elsewhere): loaded from the
// page; the display modes change what is drawn (only the masked emission is
// less than the cube, the residuals less again, the outline more); in VR the
// desktop's `mask` command loads it; Pick picks source 1 with the
// controller's ray; a New source from the lasso takes only free voxels and
// Undo takes it back; Add grows source 1; Save writes a FITS on the same grid
// with the same IDs, read back here with astropy, and the hub's `mask`
// product names it; no 3-D texture is allocated in VR.
//
// With DemoData's HCG 44 cube and its SoFiA-2 mask (hgc44-run5_mask.fits)
// present (not on CI), the same page also loads them: the 17 sources and
// 42 171 voxels arrive, and the load and encoding times are reported.

import { mkdtempSync, existsSync } from 'node:fs';
import { spawnSync } from 'node:child_process';
import { tmpdir, homedir } from 'node:os';
import { join } from 'node:path';

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const allocsIn = (p) => (p.inXR.texImage3D || 0) + (p.inXR.texStorage3D || 0);
export const HCG44 = { cube: join(homedir(), 'Downloads/DemoData/hgc44.fits'), mask: join(homedir(), 'Downloads/DemoData/hgc44-run5_mask.fits') };

/** A label mask on `cube`'s grid: 1 the bright core (> 0.3), 2 a 3 x 3 x 3 box at a corner. */
function makeMask(python, cube) {
  const dir = mkdtempSync(join(tmpdir(), 'vr-masks-'));
  const out = join(dir, 'synthetic_mask.fits');
  const code = `
import sys, numpy as np
from astropy.io import fits
d = fits.getdata(sys.argv[1]); h = fits.getheader(sys.argv[1])
m = np.zeros(d.shape, np.int32)
m[d > 0.3] = 1
m[2:5, 2:5, 2:5] = 2
for k in ('BUNIT', 'BSCALE', 'BZERO'):
    h.remove(k, ignore_missing=True)
fits.writeto(sys.argv[2], m, h, overwrite=True)
print(int((m == 1).sum()), int((m == 2).sum()))
`;
  const r = spawnSync(python, ['-c', code, cube, out], { encoding: 'utf8' });
  if (r.status !== 0) throw new Error(`could not write the synthetic mask: ${r.stderr}`);
  const [n1, n2] = r.stdout.trim().split(/\s+/).map(Number);
  return { path: out, n1, n2 };
}

/** Voxel counts per ID of a FITS mask, and its shape and CDELT3. */
function readMask(python, path) {
  const code = `
import sys, json, numpy as np
from astropy.io import fits
d = fits.getdata(sys.argv[1]); h = fits.getheader(sys.argv[1])
ids, n = np.unique(d[d > 0], return_counts=True)
print(json.dumps({"shape": list(d.shape), "cdelt3": h.get("CDELT3"), "counts": {str(int(i)): int(c) for i, c in zip(ids, n)},
  "history": " ".join(str(x) for x in h.get("HISTORY", []))}))
`;
  const r = spawnSync(python, ['-c', code, path], { encoding: 'utf8' });
  if (r.status !== 0) throw new Error(r.stderr);
  return JSON.parse(r.stdout);
}

export async function runMaskChecks({ c, check, waitFor, api, base, token, cube, python }) {
  const L = 'xrfake masks';
  const out = {};
  const json = async (method, path, session, body) => {
    const r = await api(method, path, token, { session, body });
    let parsed = null;
    try { parsed = JSON.parse(r.body); } catch { /* text */ }
    return { status: r.status, body: parsed };
  };
  const mask = makeMask(python, cube);
  // An address of its own: the previous check's (the same) would be a same-document
  // navigation, no reload, the page still in its VR session.
  await c.send('Page.navigate', { url: `${base}/vr/?debug&refine=0&xrfake&n=masks#token=${encodeURIComponent(token)}` });
  const up = await waitFor(c, `!!window.__vrShell && window.__vrShell.life.state === 'ok' && typeof window.__tex3dProbe === 'function'
    && location.search.includes('n=masks')`, 30000);
  if (!up.value) { check(`${L}: page up`, false); return out; }
  await c.evaluate(`(() => { const $ = (id) => document.getElementById(id); $('encoding').value = 'uint8'; $('fxy').value = '1'; $('path').value = ${JSON.stringify(cube)}; $('load').click(); })()`);
  const loaded = await waitFor(c, `!!(window.__vrState.open && window.__vrState.cube && window.__vrState.loading === 0)`, 60000);
  if (!loaded.value) { check(`${L}: cube loaded`, false); return out; }
  const session = await c.evaluate('window.__vrRefine.refiner.backend().session');

  // ── From the page: the layer, then the display modes (outside VR, the page's own view) ──
  await c.evaluate(`(() => { document.getElementById('mask-path').value = ${JSON.stringify(mask.path)}; document.getElementById('mask-load').click(); })()`);
  const got = await waitFor(c, '!!(window.__vrMasks.state.layer)', 20000);
  const st = await c.evaluate(`(() => { const L = window.__vrMasks.state.layer; return L ? { ids: L.ids(), n1: L.get(1).count, n2: L.get(2).count,
    mask: !!window.__vrGL && !!window.__vrMasks.state.layer, loadMs: window.__vrMasks.state.loadMs } : null; })()`);
  check(`${L}: the page loads a mask on the cube's grid: its sources and voxels as the file has them`, got.value && st
    && st.ids.join() === '1,2' && st.n1 === mask.n1 && st.n2 === mask.n2, JSON.stringify(st));
  const frac = {};
  for (const m of ['off', 'outline', 'only', 'residual']) {
    await c.evaluate(`window.__vrMasks.press('mode:${m}')`);
    await sleep(100);
    frac[m] = (await c.evaluate('window.__vrDrawnFraction()')).ink;
  }
  // The light drawn (the summed difference from the background): the residuals
  // keep the faint margin around the core, so the area alone barely changes.
  check(`${L}: Mask only draws less light than the cube, Residuals less too, the outline more`,
    frac.only < frac.off && frac.residual < frac.off && frac.outline > frac.off && frac.only > 0,
    Object.entries(frac).map(([k, v]) => `${k} ${(v / frac.off).toFixed(3)}`).join(', '));
  await c.evaluate(`window.__vrMasks.press('mode:outline')`);
  const labels = await c.evaluate('window.__vrMasks.labels().map((l) => l.text)');
  check(`${L}: a label at each source's centroid`, labels.join() === '1,2', JSON.stringify(labels));

  // ── In VR: the desktop's mask command, Pick, New, Undo, Add, Save ──
  await c.evaluate('window.__vrMasks.forget()');
  await c.evaluate(`(async () => { const b = document.getElementById('enter-vr');
    for (let i = 0; i < 50 && b.disabled; i++) await new Promise((r) => setTimeout(r, 50)); b.click(); })()`);
  const running = await waitFor(c, 'window.__xrfake.frames > 20 && !!window.__vrXR() && window.__vrTex3D().xrActive', 15000);
  if (!running.value) { check(`${L}: VR session draws frames`, false); return out; }
  const p0 = await c.evaluate('window.__tex3dProbe()');
  const opened = await json('POST', '/v1/datasets/open', session, { path: mask.path });
  const maskId = opened.body && opened.body.dataset_id;
  const sent = await json('POST', '/v1/vr/headset/command', session, { type: 'mask', dataset_id: maskId, path: mask.path });
  const viaHub = await waitFor(c, `!!(window.__vrMasks.state.layer && window.__vrMasks.state.mask.datasetId === ${JSON.stringify(maskId || '')})`, 20000);
  const shown = await waitFor(c, '!!window.__vrMenu.wins.mask.model', 5000);
  check(`${L}: the desktop's mask command loads it over the cube in VR and opens the Mask window`, sent.status === 200 && viaHub.value && shown.value,
    `command ${sent.status}`);

  const aim0 = await c.evaluate('window.__xrfake.aimRight.slice()');
  const pull = async () => {
    await sleep(300);     // the presses before it applied (the window, the seed mode) and a frame drawn
    await c.evaluate('window.__xrfake.buttons.right[0].pressed = true');
    await sleep(150);
    await c.evaluate('window.__xrfake.buttons.right[0].pressed = false');
  };
  await c.evaluate(`(() => { window.__vrMenu.act({ type: 'press', id: 'reset-view' }); window.__vrMenu.act({ type: 'press', id: 'tab:select' });
    window.__vrSelection.setTool('lasso'); window.__xrfake.aimRight = ${JSON.stringify(aim0)}; })()`);
  await sleep(300);
  // From Explore (#305): Pick takes Select, so the trigger picks, never pins the readout.
  await c.evaluate(`(() => { const s = document.getElementById('ui-mode'); s.value = 'explore'; s.dispatchEvent(new Event('change')); })()`);
  const modeBefore = await c.evaluate('window.__vrMenu.ui.mode');
  await c.evaluate(`window.__vrMenu.act({ type: 'press', id: 'mask-pick' })`);
  const modeArmed = await c.evaluate('window.__vrMenu.ui.mode');
  await pull();
  const pickedExplore = await waitFor(c, 'window.__vrMasks.state.selected === 1', 5000);
  const pinned = await c.evaluate('!!window.__vrMenu.ui.pin');
  check(`${L}: Pick pressed in Explore: Select is taken and the trigger picks the source, no readout pinned`,
    modeBefore === 'explore' && modeArmed === 'select' && pickedExplore.value && !pinned,
    `mode ${modeBefore} -> ${modeArmed}, selected ${await c.evaluate('window.__vrMasks.state.selected')}, pinned ${pinned}`);
  await c.evaluate('window.__vrMasks.select(0)');
  await c.evaluate(`window.__vrMenu.act({ type: 'press', id: 'mask-pick' })`);
  await pull();
  const picked = await waitFor(c, 'window.__vrMasks.state.selected === 1', 5000);
  check(`${L}: Pick, then the trigger on the cube, picks the source under the ray`, picked.value,
    `selected ${await c.evaluate('window.__vrMasks.state.selected')}`);

  // New: a source from the lasso, of free voxels only; Undo takes it back.
  await c.evaluate(`window.__vrMasks.press('new-source'); window.__vrMasks.press('seed:new')`);
  await pull();
  const made = await waitFor(c, '!!window.__vrMasks.state.layer.get(3)', 30000);
  const m3 = await c.evaluate(`(() => { const L = window.__vrMasks.state.layer, s3 = L.get(3), s1 = L.get(1);
    let shared = 0; if (s3) for (const v of s3.idx) if (L.idAt(v) !== 3) shared++;
    return s3 ? { n3: s3.count, n1: s1.count, shared, selected: window.__vrMasks.state.selected }
      : { status: window.__vrMasks.view().status, error: window.__vrSelection.sel.error, busy: window.__vrSelection.sel.busy,
        newSource: window.__vrMasks.state.newSource, seed: window.__vrSelection.sel.seedMode, tool: window.__vrSelection.sel.tool }; })()`);
  check(`${L}: a New source from the lasso gets the next ID and only free voxels`, made.value && m3 && m3.n3 > 0 && m3.n1 === mask.n1 && m3.shared === 0
    && m3.selected === 3, JSON.stringify(m3));
  await c.evaluate(`window.__vrMenu.act({ type: 'press', id: 'mask-undo' })`);
  const undone = await c.evaluate('({ gone: !window.__vrMasks.state.layer.get(3), selected: window.__vrMasks.state.selected })');
  check(`${L}: Undo takes the new source back`, undone.gone && undone.selected === 0, JSON.stringify(undone));

  // Add to source 1: the lasso's grow beyond its core.
  await c.evaluate(`window.__vrMasks.select(1); window.__vrMasks.press('seed:add')`);
  await pull();
  const grown = await waitFor(c, `window.__vrMasks.state.layer.get(1).count > ${mask.n1}`, 30000);
  const n1 = await c.evaluate('window.__vrMasks.state.layer.get(1).count');
  check(`${L}: Add grows the picked source with the lasso`, grown.value, `${mask.n1} -> ${n1} voxels`);

  // Save: a FITS on the same grid, the same IDs; the hub's product names it.
  const cursor = (await json('GET', '/v1/vr/activity?after=0&wait=0', session)).body?.next || 0;
  const t0 = Date.now();
  await c.evaluate(`window.__vrMenu.act({ type: 'press', id: 'mask-save' })`);
  const saved = await waitFor(c, '!!window.__vrMasks.state.saved', 30000);
  let ev = null;
  for (let i = 0; i < 60 && !ev; i++) {
    const page = (await json('GET', `/v1/vr/activity?after=${cursor}&wait=1`, session)).body || { events: [] };
    ev = page.events.find((e) => e.kind === 'product' && e.payload && e.payload.product === 'mask') || null;
  }
  // Said sent only once the hub posted it (#305).
  const told = await waitFor(c, `/sent to the desktop$/.test((window.__vrMasks.state.status[0] || [''])[0])`, 10000);
  check(`${L}: the Mask window says sent to the desktop once the product is posted`, told.value,
    await c.evaluate(`(window.__vrMasks.state.status[0] || [''])[0]`));
  const r = saved.value ? await c.evaluate('window.__vrMasks.state.saved') : null;
  const back = r ? readMask(python, r.output_path) : null;
  const orig = readMask(python, mask.path);
  check(`${L}: Save writes the refined mask on the same grid with the same IDs, and the desktop is told`,
    !!r && !!back && back.shape.join() === orig.shape.join() && back.cdelt3 === orig.cdelt3 && back.counts['1'] === n1
      && back.counts['2'] === mask.n2 && !back.counts['3'] && /source 1 edited/.test(back.history) && !!ev && ev.payload.output_path === r.output_path
      && ev.payload.recipe && ev.payload.recipe.product.kind === 'mask',
    r ? `${Date.now() - t0} ms; ${r.workspace_filename}: ${JSON.stringify(back && back.counts)}` : 'not saved');
  // The desktop sends a cube, then its mask at once: the mask goes over that cube, never over
  // the one shown before (and the command poll is not held while the cube loads).
  await c.evaluate('window.__vrMasks.forget()');
  const again = await json('POST', '/v1/datasets/open', session, { path: cube });
  const newId = again.body && again.body.dataset_id;
  const s1 = await json('POST', '/v1/vr/headset/command', session, { type: 'show', dataset_id: newId, path: cube });
  const s2 = await json('POST', '/v1/vr/headset/command', session, { type: 'mask', dataset_id: maskId, path: mask.path });
  const queued = await waitFor(c, `!!(window.__vrMasks.state.layer && window.__vrMasks.state.cube === ${JSON.stringify(newId || '')}
    && window.__vrState.open.dataset_id === ${JSON.stringify(newId || '')})`, 60000);
  const waited = await c.evaluate(`window.__vrShell.diag.items.some((e) => e.event === 'hub-mask-waits')`);
  check(`${L}: a mask sent right after its cube goes over that cube`, s1.status === 200 && s2.status === 200 && queued.value,
    `${queued.ms} ms; ${waited ? 'waited for the cube being loaded' : 'the cube was shown before the mask arrived'}`);
  const p1 = await c.evaluate('window.__tex3dProbe()');
  check(`${L}: no 3-D texture allocated in VR by the mask layer`, allocsIn(p1) === allocsIn(p0), `${allocsIn(p0)} -> ${allocsIn(p1)}`);
  const bench = await c.evaluate(`(() => { const m = window.__vrMasks, t = [];
    for (let i = 0; i < 20; i++) { const t0 = performance.now(); m.redraw(); t.push(performance.now() - t0); }
    t.sort((a, b) => a - b); return { p50: t[10], max: t[19] }; })()`);
  check(`${L}: the layer is encoded and written in under 30 ms here`, bench.p50 < 30, `p50 ${bench.p50.toFixed(2)} ms, max ${bench.max.toFixed(2)} ms`);
  await c.evaluate(`window.__xrfake.aimRight = ${JSON.stringify(aim0)}`);
  out.bench = bench;

  // A mask whose cube never comes (#305): the desktop sends a cube that cannot
  // be opened (held back 2.5 s, so the mask arrives while it loads), then its
  // mask; the load fails and the mask is dropped, not loaded over the next
  // cube shown (here the page's own load of the cube).
  await c.evaluate('window.__vrMasks.forget()');
  await c.evaluate(`(() => { const f = window.fetch; window.__fetchOrig = f;
    window.fetch = (u, init) => ((String(u).includes('vrmissing') || (init && typeof init.body === 'string' && /vrmissing|vr_missing_cube/.test(init.body)))
      ? new Promise((r) => setTimeout(r, 2500)).then(() => f(u, init)) : f(u, init)); })()`);
  const missing = join(tmpdir(), 'vr_missing_cube.fits');
  const f1 = await json('POST', '/v1/vr/headset/command', session, { type: 'show', dataset_id: 'vrmissing0001', path: missing });
  const f2 = await json('POST', '/v1/vr/headset/command', session, { type: 'mask', dataset_id: maskId, path: mask.path });
  const waitedFor = await waitFor(c, `window.__vrShell.diag.items.some((e) => e.event === 'hub-mask-waits' && /command ${f2.body && f2.body.seq}/.test(e.message || ''))`, 20000);
  const dropped = await waitFor(c, `window.__vrShell.diag.items.some((e) => e.event === 'hub-mask-dropped')`, 30000);
  await c.evaluate('window.fetch = window.__fetchOrig');
  const before = await c.evaluate('window.__vrState.open && window.__vrState.open.dataset_id');
  await c.evaluate(`(() => { const $ = (id) => document.getElementById(id); $('path').value = ${JSON.stringify(cube)}; $('load').click(); })()`);
  const reshown = await waitFor(c, `!!(window.__vrState.open && window.__vrState.open.dataset_id !== ${JSON.stringify(before || '')} && window.__vrState.loading === 0)`, 60000);
  await sleep(1500);
  const after = await c.evaluate('({ layer: !!window.__vrMasks.state.layer, status: (window.__vrMasks.state.status[0] || [""])[0] })');
  check(`${L}: a mask whose cube fails to load is dropped, never loaded over the next cube`,
    f1.status === 200 && f2.status === 200 && waitedFor.value && dropped.value && reshown.value && !after.layer,
    `waited ${waitedFor.value}, dropped ${dropped.value}, next cube shown ${reshown.value}, layer ${after.layer}; ${after.status}`);

  // ── HCG 44 with its SoFiA-2 mask (DemoData, when present) ──
  if (existsSync(HCG44.cube) && existsSync(HCG44.mask)) {
    // Another address (the same one would be a same-document navigation: no reload, still in VR).
    await c.send('Page.navigate', { url: `${base}/vr/?debug&refine=0&xrfake&n=hcg44#token=${encodeURIComponent(token)}` });
    await waitFor(c, `!!window.__vrShell && window.__vrShell.life.state === 'ok' && location.search.includes('n=hcg44')`, 30000);
    // Binned 3 x 3 x 1, as the headset shows it (234 x 234 x 158 reduced voxels).
    await c.evaluate(`(() => { const $ = (id) => document.getElementById(id); $('encoding').value = 'uint8'; $('fxy').value = '3'; $('fz').value = '1';
      $('path').value = ${JSON.stringify(HCG44.cube)}; $('load').click(); })()`);
    const ok = await waitFor(c, `!!(window.__vrState.open && window.__vrState.cube && window.__vrState.loading === 0)`, 120000);
    await c.evaluate(`(() => { document.getElementById('mask-path').value = ${JSON.stringify(HCG44.mask)}; document.getElementById('mask-load').click(); })()`);
    const lay = await waitFor(c, '!!(window.__vrMasks.state.layer)', 60000);
    const h = await c.evaluate(`(() => { const m = window.__vrMasks.state, L = m.layer; if (!L) return null;
      const f = {}, ms = {};
      for (const k of ['off', 'outline', 'only', 'residual']) {
        window.__vrMasks.press('mode:' + k);
        f[k] = window.__vrDrawnFraction().ink;
        // A draw and a full read-back, five times (the read-back is the same for every mode).
        const t = [];
        for (let i = 0; i < 5; i++) { const t0 = performance.now(); window.__vrDrawnFraction(); t.push(performance.now() - t0); }
        ms[k] = t.sort((a, b) => a - b)[2];
      }
      window.__vrMasks.press('mode:outline');
      return { n: L.ids().length, voxels: [...L.byId.values()].reduce((s, x) => s + x.count, 0), loadMs: m.loadMs, encodeMs: m.encodeMs,
        binning: window.__vrState.info.factors, shape: window.__vrState.info.full_shape, f, ms,
        status: document.getElementById('load-status').textContent.slice(0, 200) }; })()`);
    out.hcg44 = h;
    // SoFiA's mask holds the emission above the threshold, and its faint
    // margin below it: Mask only (every masked voxel) draws more light than
    // the cube, the residuals almost none, the outline more.
    check(`${L}: HCG 44 with its SoFiA-2 mask: 17 sources, 42 171 voxels; residuals almost nothing, the outline more`,
      ok.value && lay.value && h && h.n === 17 && h.voxels === 42171 && h.f.only > 0 && h.f.residual < 0.2 * h.f.off && h.f.outline > h.f.off,
      h ? `${h.n} sources, ${h.voxels} voxels${ok.value ? '' : ' (cube NOT loaded)'}; load ${h.loadMs.toFixed(0)} ms, encode ${h.encodeMs.toFixed(1)} ms at ${h.binning.join('×')}; light drawn ${Object.entries(h.f).map(([k, v]) => `${k} ${(v / (h.f.off || 1)).toFixed(3)}`).join(', ')}; draw+read ms ${Object.entries(h.ms).map(([k, v]) => `${k} ${v.toFixed(1)}`).join(', ')}` : 'not loaded');
  }
  return out;
}

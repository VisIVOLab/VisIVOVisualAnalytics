// The variable picker of a simulation volume on the VR page (issue #317), run
// by vr_lifecycle.mjs --variables (and --xrfake, --regressions) through the
// WebXR stand-in (XR_FAKE) with the texture probe (TEX3D_PROBE): a VBT volume
// of two fields (a density over seven decades, a signed velocity) loaded by
// its first field; in one VR session the hand menu's Variable row switches to
// the velocity, back to the density and to the velocity again. Each variable
// must come with its own scale (log, linear) and keep its own look (colour
// map, threshold, colour max, stretch) while the crop stays; no 3-D texture
// may be allocated during the session (each variable is written into the
// capacity's spare bank); the headset hub must say which variable is shown.

import { makeSimulationVolume } from './vr_simulation.mjs';

const allocsIn = (p) => (p.inXR.texImage3D || 0) + (p.inXR.texStorage3D || 0);

export async function runVariableChecks({ c, check, waitFor, api, base, token, python, results = {} }) {
  const L = 'variables';
  const data = makeSimulationVolume(python);
  const rho = `${data}::vbt-field=0`;
  await c.send('Page.navigate', { url: `${base}/vr/?debug&refine=0&xrfake&n=variables#token=${encodeURIComponent(token)}` });
  const up = await waitFor(c, `!!window.__vrShell && window.__vrShell.life.state === 'ok' && location.search.includes('n=variables')
    && typeof window.__tex3dProbe === 'function'`, 30000);
  if (!up.value) { check(`${L}: page up with the texture probe`, false); return results; }
  await c.evaluate(`(() => { const $ = (id) => document.getElementById(id); $('encoding').value = 'uint8'; $('fxy').value = '1'; $('fz').value = '1';
    $('scale').value = 'auto'; $('path').value = ${JSON.stringify(rho)}; $('load').click(); })()`);
  // The backend names files by their resolved path (/private/var/… for /var/…): compared by the field's reference end.
  const loaded = await waitFor(c, `!!(window.__vrState.cube && window.__vrState.loading === 0 && window.__vrState.openPath.endsWith('sim.bin::vbt-field=0'))`, 60000);
  check(`${L}: the density loaded`, !!loaded.value, `${loaded.ms} ms`);
  if (!loaded.value) return results;

  const row = async () => c.evaluate(`(() => { const m = window.__vrMenu; m.ui.page = 'display';
    return m.layout().filter((w) => w.id.startsWith('var') || w.id.startsWith('ct-')).map((w) => w.id); })()`);
  const r0 = await row();
  check(`${L}: the Display page's last row picks a variable (rho, vx), not the catalogue`,
    JSON.stringify(r0) === JSON.stringify(['var:rho', 'var:vx']), r0.join(','));

  // The density's look, set before the session: its own colour map and threshold, a crop.
  const cmaps = await c.evaluate(`[...document.getElementById('cmap').options].map((o) => o.value)`);
  const [cmapRho, cmapVx] = [cmaps[1] || cmaps[0], cmaps[2] || cmaps[0]];
  await c.evaluate(`(() => { const m = window.__vrMenu; m.ui.page = 'display'; m.act({ type: 'press', id: 'cmap:${cmapRho}' });
    const s = window.__vrState; s.thrNorm = 0.42; s.cmaxNorm = 0.9; s.clip = { min: [0.1, 0.2, 0], max: [0.8, 0.9, 1] }; })()`);
  const look = `(() => { const s = window.__vrState, c = s.cube; const v = (n) => c.scale === 'log' ? 10 ** (c.lo + n * (c.hi - c.lo)) : c.lo + n * (c.hi - c.lo);
    return { path: s.openPath, field: s.info.field_name, scale: c.scale, colormap: s.colormap, thr: v(s.thrNorm), cmax: v(s.cmaxNorm), thrNorm: s.thrNorm,
      stretch: s.stretch, clip: s.clip, dims: c.dims }; })()`;
  const rhoLook = await c.evaluate(look);

  // One VR session.
  await c.evaluate(`(async () => { const b = document.getElementById('enter-vr');
    for (let i = 0; i < 50 && b.disabled; i++) await new Promise((r) => setTimeout(r, 50)); b.click(); })()`);
  const running = await waitFor(c, 'window.__xrfake.frames > 20 && !!window.__vrXR() && window.__vrTex3D().xrActive', 15000);
  if (!running.value) { check(`${L}: VR session draws frames`, false); return results; }
  const p0 = await c.evaluate('window.__tex3dProbe()');
  const pick = async (name) => {
    const t0 = Date.now();
    await c.evaluate(`(() => { const m = window.__vrMenu; m.ui.page = 'display'; m.act({ type: 'press', id: 'var:${name}' }); })()`);
    const ok = await waitFor(c, `window.__vrState.info.field_name === '${name}' && window.__vrState.loading === 0 && !window.__vrVariables.pending()`, 30000);
    if (!ok.value) {
      console.log(`     variables: ${name} not shown: ${await c.evaluate(`JSON.stringify({ path: window.__vrState.openPath, loading: window.__vrState.loading,
        pending: !!window.__vrVariables.pending(), status: document.getElementById('load-status').textContent.slice(0, 200),
        diag: window.__vrShell.diag.items.slice(-6).map((e) => e.event + ': ' + String(e.message).slice(0, 80)) })`)}`);
    }
    const ms = Date.now() - t0;
    // A user does not switch twice within the hub's tick (250 ms): each switch is told.
    await new Promise((r) => setTimeout(r, 600));
    return { ok: !!ok.value, ms };
  };
  const s1 = await pick('vx');
  const vxFirst = await c.evaluate(look);
  check(`${L}: in VR the velocity is shown, on a linear scale, the crop kept`,
    s1.ok && vxFirst.field === 'vx' && vxFirst.scale === 'linear' && JSON.stringify(vxFirst.clip) === JSON.stringify(rhoLook.clip),
    `${s1.ms} ms; ${JSON.stringify({ field: vxFirst.field, scale: vxFirst.scale, clip: vxFirst.clip })}`);
  // The velocity's own look.
  await c.evaluate(`(() => { const m = window.__vrMenu; m.ui.page = 'display'; m.act({ type: 'press', id: 'cmap:${cmapVx}' });
    window.__vrState.thrNorm = 0.6; window.__vrState.cmaxNorm = 0.95; })()`);
  const vxLook = await c.evaluate(look);
  const s2 = await pick('rho');
  const rhoBack = await c.evaluate(look);
  const near = (a, b) => Math.abs(a / b - 1) < 0.02;
  check(`${L}: back to the density: its log scale, colour map, threshold and colour max as it was left`,
    s2.ok && rhoBack.scale === 'log' && rhoBack.colormap === cmapRho && near(rhoBack.thr, rhoLook.thr) && near(rhoBack.cmax, rhoLook.cmax)
      && JSON.stringify(rhoBack.clip) === JSON.stringify(rhoLook.clip),
    `${s2.ms} ms; ${JSON.stringify({ cmap: rhoBack.colormap, thr: [rhoBack.thr, rhoLook.thr], cmax: [rhoBack.cmax, rhoLook.cmax] })}`);
  const s3 = await pick('vx');
  const vxBack = await c.evaluate(look);
  check(`${L}: the velocity again: its own colour map and threshold`,
    s3.ok && vxBack.colormap === cmapVx && Math.abs(vxBack.thr - vxLook.thr) <= 1e-6 * Math.max(1, Math.abs(vxLook.thr)) + (vxLook.cmax - vxLook.thr) * 0.01,
    `${s3.ms} ms; ${JSON.stringify({ cmap: vxBack.colormap, thr: [vxBack.thr, vxLook.thr] })}`);
  const p1 = await c.evaluate('window.__tex3dProbe()');
  const t = await c.evaluate('window.__vrTex3D()');
  check(`${L}: three variable switches in one VR session, no 3-D texture allocated (the spare bank each time)`,
    allocsIn(p1) === allocsIn(p0) && allocsIn(p1) === 0 && !!(t.capacity && t.capacity.onShow),
    `in-VR texImage3D/texStorage3D ${allocsIn(p1) - allocsIn(p0)}; texSubImage3D in VR ${(p1.inXR.texSubImage3D || 0) - (p0.inXR.texSubImage3D || 0)}; capacity ${JSON.stringify(t.capacity)}`);
  results.switchMs = [s1.ms, s2.ms, s3.ms];

  // The headset hub: each switch said as a variable of the same file.
  const session = await c.evaluate('window.__vrBackend().session');
  let events = [], vars = [];
  for (let i = 0; i < 40 && vars.length < 3; i++) {
    const act = await api('GET', '/v1/vr/activity?after=0&wait=0', token, { session });
    try { events = JSON.parse(act.body).events || []; } catch { events = []; }
    vars = events.filter((e) => e.kind === 'cube' && e.payload && e.payload.field);
    if (vars.length < 3) await new Promise((r) => setTimeout(r, 250));
  }
  // In order: vx (from rho), rho (from vx), vx (from rho), each naming the cube it shows and the one it replaces.
  const want = [['vx', 1, 0], ['rho', 0, 1], ['vx', 1, 0]];
  const seq = [...vars].sort((a, b) => a.seq - b.seq);
  check(`${L}: the headset hub says each switch ("Variable vx of sim.bin"), with the variable it replaces`,
    seq.length === 3 && seq.every((e, i) => e.summary === `Variable ${want[i][0]} of sim.bin` && e.payload.field === want[i][0]
      && e.cube_path.endsWith(`sim.bin::vbt-field=${want[i][1]}`) && e.payload.from.endsWith(`sim.bin::vbt-field=${want[i][2]}`)),
    events.map((e) => `${e.kind}:${e.summary}`).join(' / '));
  const errs = await c.evaluate(`window.__vrShell.diag.items.filter((e) => e.level === 'error').map((e) => e.event + ': ' + e.message)`);
  check(`${L}: no page error`, !errs.length, errs.join(' / '));
  await c.evaluate('window.__vrShell.act("exit-vr")');
  await waitFor(c, '!window.__vrXR()', 10000);
  return results;
}

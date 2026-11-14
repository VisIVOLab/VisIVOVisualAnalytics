// One headset page driven by the desktop (issue #237), run by
// vr_lifecycle.mjs --regressions (and --xrfake alone) through the WebXR
// stand-in (XR_FAKE) and the 3-D texture probe (TEX3D_PROBE).
//
// - The page says hello to the session's headset hub and keeps its state.
// - A `show` command sent as the desktop sends it switches the cube in a
//   running VR session: no new tab, no 3-D allocation, a notice naming it.
// - The hub receives one event per action, each with the payload the desktop
//   reads (the same rules as src/app/VrHeadsetHub.h, mirrored in
//   backend/tests/vr/desktop_hub_parser.mjs): look, clip, a spectrum, a
//   moment, a PV slice, an Isolate, each product's file in the Workspace
//   Exports.
// - Fewer Enter VR presses: launched as an app the page enters by itself,
//   and falls back to the button when the browser wants a press; with
//   offerSession (?xroffer) the browser's own Enter VR is offered, used only
//   where it exists.

import { existsSync, readFileSync } from 'node:fs';
import { parseProduct, openIn, parseLook, parseClip, aboutCube, parseSpectrumCsv } from '../backend/tests/vr/desktop_hub_parser.mjs';

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const allocsIn = (p) => (p.inXR.texImage3D || 0) + (p.inXR.texStorage3D || 0);

/** The page's session (the hub's routes are per session). */
const sessionOf = (c) => c.evaluate('window.__vrRefine.refiner.backend().session');

export async function runHeadsetHubChecks({ c, check, waitFor, api, base, token, cube, other, cdpPort }) {
  const say = (m) => console.log(`     hub: ${m}`);
  const results = {};
  const json = async (method, path, session, body) => {
    const r = await api(method, path, token, { session, body });
    let parsed = null;
    try { parsed = JSON.parse(r.body); } catch { /* text */ }
    return { status: r.status, body: parsed, text: r.body };
  };
  const pages = async (session) => (await json('GET', '/v1/vr/headset', session)).body?.headsets || [];
  const events = async (session, after) => (await json('GET', `/v1/vr/activity?after=${after}&wait=0`, session)).body || { events: [], next: after };
  const tabs = async () => (await (await fetch(`http://127.0.0.1:${cdpPort}/json/list`)).json()).filter((t) => t.type === 'page').length;

  // ── The page in a VR session, its cube from the backend ───────────────────
  await c.send('Page.navigate', { url: `${base}/vr/?debug&refine=0&xrfake#token=${encodeURIComponent(token)}` });
  const up = await waitFor(c, `!!window.__vrShell && window.__vrShell.life.state === 'ok' && typeof window.__tex3dProbe === 'function'`, 30000);
  if (!up.value) { check('hub: page up', false); return results; }
  await c.evaluate(`(() => { const $ = (id) => document.getElementById(id); $('encoding').value = 'uint8'; $('fxy').value = '1'; $('path').value = ${JSON.stringify(cube)}; $('load').click(); })()`);
  const loaded = await waitFor(c, `!!(window.__vrState.open && window.__vrState.cube && window.__vrState.loading === 0)`, 60000);
  if (!loaded.value) { check('hub: cube loaded', false); return results; }
  const session = await sessionOf(c);
  const client = await c.evaluate('window.__vrHub.st.client');
  let listed = null;
  for (let i = 0; i < 40; i++) {
    listed = (await pages(session)).find((p) => p.client === client);
    if (listed && listed.cube && listed.cube.path === cube) break;
    await sleep(100);
  }
  check('hub: the page says hello (transport adb on localhost) and its cube is listed for the desktop',
    !!listed && listed.transport === 'adb' && listed.cube && listed.cube.path === cube && !listed.in_vr,
    JSON.stringify(listed && { transport: listed.transport, label: listed.label, cube: listed.cube && listed.cube.path.split('/').pop(), in_vr: listed.in_vr }));
  await c.evaluate(`(async () => { const b = document.getElementById('enter-vr');
    for (let i = 0; i < 50 && b.disabled; i++) await new Promise((r) => setTimeout(r, 50)); b.click(); })()`);
  const running = await waitFor(c, 'window.__xrfake.frames > 20 && !!window.__vrXR() && window.__vrTex3D().xrActive', 15000);
  if (!running.value) { check('hub: VR session draws frames', false); return results; }
  for (let i = 0; i < 40; i++) { listed = (await pages(session)).find((p) => p.client === client); if (listed && listed.in_vr) break; await sleep(100); }
  check('hub: in VR is reported to the hub', !!listed && listed.in_vr === true);

  // ── A `show` from the desktop, in the running session ─────────────────────
  const opened = await json('POST', '/v1/datasets/open', session, { path: other });
  const otherId = opened.body && opened.body.dataset_id;
  const cursor0 = (await events(session, 0)).next || 0;
  const tabs0 = await tabs();
  const p0 = await c.evaluate('window.__tex3dProbe()');
  const f0 = await c.evaluate('window.__xrfake.frames');
  const t0 = Date.now();
  const sent = await json('POST', '/v1/vr/headset/command', session, { type: 'show', dataset_id: otherId, path: other });
  // The notice naming the cube, caught while it is up: in VR a toast low in
  // the view (issue #256), not the card.
  const notice = await waitFor(c, `(() => { const n = window.__vrShell.notices.current(); if (n && n.id === 'cube-from-desktop') return n.title;
    const t = window.__vrWrist && window.__vrWrist().toast; return t && /from the computer/.test(t.text) ? t.text : ''; })()`, 15000, 50);
  const shown = await waitFor(c, `window.__vrState.loading === 0 && window.__vrState.openPath === ${JSON.stringify(other)} && !window.__vrTex3D().staged`, 60000, 50);
  const showMs = Date.now() - t0;
  await sleep(400);
  const p1 = await c.evaluate('window.__tex3dProbe()');
  const after = await c.evaluate(`({ xr: !!window.__vrXR(), active: window.__vrTex3D().xrActive, frames: window.__xrfake.frames, ended: window.__xrfake.ended,
    dims: window.__vrState.cube.dims.join('x'), title: (window.__vrShell.notices.current() || {}).title || '' })`);
  const tabs1 = await tabs();
  results.showMs = showMs;
  check('hub: a show command switches the cube inside the running VR session: no new tab, no texImage3D/texStorage3D, frames go on',
    sent.status === 200 && shown.value && after.xr && after.active && after.frames > f0 && tabs1 === tabs0 && allocsIn(p1) === allocsIn(p0),
    JSON.stringify({ status: sent.status, ms: showMs, tabs: [tabs0, tabs1], allocsInVR: [allocsIn(p0), allocsIn(p1)], dims: after.dims }));
  const otherName = other.split('/').pop();
  check('hub: a short notice (in VR a toast) names the cube sent from the desktop', String(notice.value || '').includes(otherName) && /from the computer/.test(notice.value || ''),
    `${notice.value || '(none)'} / then ${after.title}`);
  for (let i = 0; i < 40; i++) { listed = (await pages(session)).find((p) => p.client === client); if (listed && listed.cube && listed.cube.path === other) break; await sleep(100); }
  check('hub: the state follows (the new cube on show, still in VR)', !!listed && listed.in_vr && listed.cube && listed.cube.path === other && listed.cube.dataset_id === otherId);

  // ── One event per action, as the desktop reads it ─────────────────────────
  await sleep(600);
  // The look: three changes in 200 ms (a slider dragged) -> one event.
  await c.evaluate(`(async () => {
    const $ = (id) => document.getElementById(id);
    $('cmap').value = 'inferno'; $('cmap').dispatchEvent(new Event('change'));
    for (const v of [180, 200, 220]) { $('thr').value = String(v); $('thr').dispatchEvent(new Event('input')); await new Promise((r) => setTimeout(r, 70)); }
  })()`);
  const lookWant = await c.evaluate('({ thr: window.__vrHub && window.__vrState.thrNorm, colormap: window.__vrState.colormap })');
  await sleep(1500);
  // The clip: the middle half in x.
  await c.evaluate(`(() => { window.__vrState.clip = { min: [0.25, 0, 0], max: [0.75, 1, 1] }; })()`);
  await sleep(1500);
  // A spectrum, a lasso, its moment, a PV path, an Isolate.
  await c.evaluate(`(() => { window.__vrMenu.ui.mode = 'select'; window.__vrSelection.setTool('probe'); window.__xrfake.aimRight = [0, 1.48, -0.75]; })()`);
  const trigger = async () => {
    const f = await c.evaluate('window.__xrfake.frames');
    await c.evaluate('window.__xrfake.buttons.right[0].pressed = true');
    await waitFor(c, `window.__xrfake.frames > ${f + 2}`, 5000);
    await c.evaluate('window.__xrfake.buttons.right[0].pressed = false');
  };
  await sleep(300);
  await trigger();
  const spec = await waitFor(c, '!window.__vrSelection.sel.busy && (!!window.__vrSelection.sel.spectrum || !!window.__vrSelection.sel.error)', 30000);
  await c.evaluate(`window.__vrSelection.setTool('lasso')`);
  await sleep(200);
  await trigger();
  const lasso = await waitFor(c, '!window.__vrSelection.sel.busy && (!!window.__vrSelection.sel.lasso || !!window.__vrSelection.sel.error)', 30000);
  await c.evaluate(`window.__vrAnalysis.press('m0', { group: 'moment', value: 0 })`);
  const mom = await waitFor(c, '!!window.__vrAnalysis.moment.state.result || !!window.__vrAnalysis.moment.state.error', 60000);
  await c.evaluate(`(() => { const pv = window.__vrAnalysis.pv; pv.newPath(); pv.addVertex([5, 5]); pv.addVertex([30, 20]); pv.addVertex([60, 40]); })()`);
  const pv = await waitFor(c, '!!window.__vrAnalysis.pv.state.result && !window.__vrAnalysis.pv.state.inFlight', 60000);
  await c.evaluate(`window.__vrAnalysis.press('isolate')`);
  const iso = await waitFor(c, '!!window.__vrAnalysis.lasso.state.applied && !window.__vrAnalysis.lasso.state.busy', 60000);
  const sel = await c.evaluate(`({ spectrum: !!window.__vrSelection.sel.spectrum, lasso: !!window.__vrSelection.sel.lasso, error: window.__vrSelection.sel.error,
    moment: window.__vrAnalysis.moment.state.error, pv: window.__vrAnalysis.pv.state.error })`);
  say(`tools: ${JSON.stringify({ spec: spec.ms, lasso: lasso.ms, moment: mom.ms, pv: pv.ms, isolate: iso.ms, ...sel })}`);
  // Saves and posts go after the tools (and a PV path after 1.5 s): wait for the six products.
  let got = [];
  const productsWanted = ['spectrum', 'moment', 'pv', 'isolate'];
  for (let i = 0; i < 120; i++) {
    got = (await events(session, cursor0)).events || [];
    const kinds = got.filter((e) => e.kind === 'product').map((e) => e.payload.product);
    if (productsWanted.every((k) => kinds.includes(k))) break;
    await sleep(250);
  }
  await sleep(2500);       // anything else that would come (a second event of one action) comes now
  got = (await events(session, cursor0)).events || [];
  const count = (kind, product) => got.filter((e) => e.kind === kind && (!product || e.payload.product === product)).length;
  const byKind = Object.fromEntries(['look', 'clip', 'cube'].map((k) => [k, count(k)]));
  for (const k of productsWanted) byKind[k] = count('product', k);
  results.activity = byKind;
  check('hub: one event per action (cube switch, look, clip, spectrum, moment, PV, Isolate)',
    Object.values(byKind).every((n) => n === 1), JSON.stringify(byKind));
  const cubeEv = got.find((e) => e.kind === 'cube');
  check('hub: the cube event names the cube switched to', !!cubeEv && cubeEv.cube_path === other && cubeEv.payload.path === other && cubeEv.payload.dataset_id === otherId,
    JSON.stringify(cubeEv && cubeEv.payload));
  const lookEv = got.find((e) => e.kind === 'look');
  const look = lookEv ? parseLook(lookEv.payload) : null;
  check('hub: the look event, as the desktop parses it: colour map and the threshold as a data value',
    !!look && look.colormap === 'inferno' && look.threshold !== null && look.colourMax !== null && aboutCube(lookEv, other, otherId),
    JSON.stringify(look) + ` (page thrNorm ${lookWant.thr})`);
  const clipEv = got.find((e) => e.kind === 'clip');
  const clip = clipEv ? parseClip(clipEv.payload) : null;
  const full = await c.evaluate('window.__vrState.info.full_shape');
  check('hub: the clip event, voxels of the full cube (x the middle half, y and z whole)',
    !!clip && clip.set && clip.min[0] === Math.round(0.25 * full[0]) && clip.max[0] === Math.round(0.75 * full[0]) - 1
      && clip.min[1] === 0 && clip.max[1] === full[1] - 1 && clip.min[2] === 0 && clip.max[2] === full[2] - 1,
    JSON.stringify({ clip, full }));
  const prods = got.filter((e) => e.kind === 'product');
  const bad = [];
  for (const e of prods) {
    const p = parseProduct(e);
    const where = openIn(p);
    const file = e.payload.output_path || '';
    const okFile = !!file && existsSync(file);
    let extra = '';
    if (p.kind === 'spectrum' && okFile) {
      const s = parseSpectrumCsv(readFileSync(file, 'utf8'));
      if (!s.ok) bad.push(`spectrum CSV: ${s.values.length} rows`);
      extra = ` ${s.values.length} channels`;
    }
    if (where === 'Nothing' || !okFile || !aboutCube(e, other, otherId) || !p.recipeId) bad.push(`${p.kind}: ${where}, file ${okFile ? 'ok' : 'missing'}, recipe ${p.recipeId || 'none'}`);
    say(`${p.kind} → ${where}, ${p.workspaceFilename}${extra}; "${e.summary}"`);
  }
  check('hub: every product opens on the desktop (VrHeadsetHub.h rules): its file in the Workspace Exports, its recipe, about this cube',
    prods.length >= 4 && !bad.length, bad.join(' / ') || `${prods.length} products`);
  const p2 = await c.evaluate('window.__tex3dProbe()');
  check('hub: no 3-D allocation in VR through all of it', allocsIn(p2) === allocsIn(p0), JSON.stringify(p2.inXR));
  const hubErr = await c.evaluate(`window.__vrShell.diag.items.filter((e) => e.event === 'hub-failed').map((e) => e.message)`);
  check('hub: nothing failed on the way to the hub', !hubErr.length, hubErr.join(' / '));
  await c.evaluate('window.__vrShell.act("exit-vr")');
  await waitFor(c, '!window.__vrXR() && !window.__vrTex3D().xrActive', 10000);
  return results;
}

/** Fewer Enter VR presses: auto-enter when launched as an app, its fallback, offerSession. */
export async function runEntryChecks({ c, check, waitFor, base, token, cube }) {
  const load = async (query) => {
    await c.send('Page.navigate', { url: `${base}/vr/?debug&refine=0&xrfake&${query}#token=${encodeURIComponent(token)}` });
    const up = await waitFor(c, `!!window.__vrShell && window.__vrShell.life.state === 'ok'`, 30000);
    if (!up.value) return false;
    await c.evaluate(`(() => { const $ = (id) => document.getElementById(id); $('encoding').value = 'uint8'; $('path').value = ${JSON.stringify(cube)}; $('load').click(); })()`);
    return (await waitFor(c, `!!(window.__vrState.open && window.__vrState.cube && window.__vrState.loading === 0)`, 60000)).value;
  };
  // Launched as the installed app (the loader's ?launch=app): in VR with no press.
  if (await load('launch=app')) {
    const inVR = await waitFor(c, '!!window.__vrXR() && window.__xrfake.frames > 10', 10000);
    const e = await c.evaluate('window.__vrEntry()');
    check('entry: launched as an app, the page enters VR by itself once its cube is on show', inVR.value && e.tried && e.outcome === 'entered',
      `${inVR.ms} ms; ${e.line}`);
    await c.evaluate('window.__vrShell.act("exit-vr")');
    await waitFor(c, '!window.__vrXR()', 10000);
    await new Promise((r) => setTimeout(r, 1000));
    check('entry: tried once only (leaving VR does not send it back in)', !(await c.evaluate('!!window.__vrXR()')) && (await c.evaluate('window.__xrfake.requests')) === 1);
  } else check('entry: cube loaded (launch=app)', false);
  // The browser wants a press: refused quietly, the button stays.
  if (await load('launch=app&xrgesture')) {
    const tried = await waitFor(c, 'window.__vrEntry().tried && !!window.__vrEntry().outcome', 10000);
    const r = await c.evaluate(`({ e: window.__vrEntry(), xr: !!window.__vrXR(), btn: document.getElementById('enter-vr').disabled,
      refused: window.__xrfake.refused, notice: (window.__vrShell.notices.current() || {}).level || '',
      errors: window.__vrShell.diag.items.filter((x) => x.level === 'error').length })`);
    check('entry: refused without a press, it falls back to the Enter VR button (no error, no notice)',
      tried.value && !r.xr && !r.btn && r.refused === 1 && /refused/.test(r.e.outcome) && r.notice !== 'error' && !r.errors, JSON.stringify({ ...r, e: r.e.line }));
  } else check('entry: cube loaded (launch=app&xrgesture)', false);
  // offerSession where it exists: the browser's own Enter VR.
  if (await load('xroffer')) {
    const offered = await waitFor(c, 'window.__xrfake.offers >= 1 && window.__vrEntry().offered', 10000);
    await c.evaluate('window.__xrfake.acceptOffer()');
    const inVR = await waitFor(c, '!!window.__vrXR() && window.__xrfake.frames > 10', 10000);
    await c.evaluate('window.__vrShell.act("exit-vr")');
    await waitFor(c, '!window.__vrXR()', 10000);
    const again = await waitFor(c, 'window.__xrfake.offers >= 2 && window.__vrEntry().offered', 10000);
    check('entry: with offerSession, the session is offered, entered through the browser\'s button, and offered again after',
      offered.value && inVR.value && again.value, (await c.evaluate('window.__vrEntry().line')));
  } else check('entry: cube loaded (xroffer)', false);
  // Without it (most browsers): never called, never assumed.
  if (await load('')) {
    await new Promise((r) => setTimeout(r, 800));
    const e = await c.evaluate('window.__vrEntry()');
    check('entry: offerSession is used only where it exists', e.offers === 0 && /not in this browser/.test(e.line) && !e.tried, e.line);
  } else check('entry: cube loaded', false);
}

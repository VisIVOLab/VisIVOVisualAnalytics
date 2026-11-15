// The source list in the headset (issue #279), run by vr_lifecycle.mjs
// --regressions (and --xrfake) through the WebXR stand-in: a catalogue of
// three sources over the synthetic cube (placed by the backend with the
// cube's WCS), the Sources window opened from the Select tab, a row chosen
// with the controller's ray and trigger (the visit brings the source to
// 0.55 m ahead of the eyes, the velocity range moved to hold it), Next,
// flags and a note, a source found with the Kinematic Lasso, Send: the
// VOTable in the Workspace Exports with the flag column, the hub's
// `classification` product read as the desktop reads it, and no 3-D texture
// allocated in VR.

import { mkdtempSync, writeFileSync, readFileSync, existsSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { parseProduct, openIn, parseClassification } from '../backend/tests/vr/desktop_hub_parser.mjs';

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const allocsIn = (p) => (p.inXR.texImage3D || 0) + (p.inXR.texStorage3D || 0);

export async function runSourceListChecks({ c, check, waitFor, api, base, token, cube }) {
  const L = 'xrfake sources';
  const json = async (method, path, session, body) => {
    const r = await api(method, path, token, { session, body });
    let parsed = null;
    try { parsed = JSON.parse(r.body); } catch { /* text */ }
    return { status: r.status, body: parsed };
  };
  // An address of its own: the previous check's (the same) would be a same-document
  // navigation, no reload, the page still in its VR session.
  await c.send('Page.navigate', { url: `${base}/vr/?debug&refine=0&xrfake&n=sources#token=${encodeURIComponent(token)}` });
  const up = await waitFor(c, `!!window.__vrShell && window.__vrShell.life.state === 'ok' && typeof window.__tex3dProbe === 'function'
    && location.search.includes('n=sources')`, 30000);
  if (!up.value) { check(`${L}: page up`, false); return {}; }
  await c.evaluate(`(() => { const $ = (id) => document.getElementById(id); $('encoding').value = 'uint8'; $('fxy').value = '1'; $('path').value = ${JSON.stringify(cube)}; $('load').click(); })()`);
  const loaded = await waitFor(c, `!!(window.__vrState.open && window.__vrState.cube && window.__vrState.loading === 0)`, 60000);
  if (!loaded.value) { check(`${L}: cube loaded`, false); return {}; }
  const session = await c.evaluate('window.__vrRefine.refiner.backend().session');
  const dataset = await c.evaluate('window.__vrState.open.dataset_id');
  const shape = await c.evaluate('window.__vrState.info.full_shape');

  // ── A catalogue: three sources at known voxels, their sky and velocity from the cube's WCS ──
  // The synthetic cube: VRAD 1000 km/s at channel 0, 5 km/s a channel.
  const truth = [['GAL-A', [shape[0] / 2, Math.round(shape[1] * 0.47), shape[2] / 2]], ['GAL-B', [10, 12, 5]], ['GAL-C', [50, 52, 30]]];
  const rows = [];
  for (const [name, [x, y, z]] of truth) {
    const s = (await json('POST', '/v1/datasets/pixel_to_sky', session, { dataset_id: dataset, x, y, z })).body;
    rows.push(`${name},${s.ra_deg.toFixed(9)},${s.dec_deg.toFixed(9)},${(1000 + 5 * z).toFixed(3)}`);
  }
  const dir = mkdtempSync(join(tmpdir(), 'vr-sources-'));
  const csv = join(dir, 'sources.csv');
  writeFileSync(csv, `name,ra,dec,v_rad\n${rows.join('\n')}\n`);
  await c.evaluate(`(() => { const $ = (id) => document.getElementById(id); $('ct-source').value = 'catalogue'; $('ct-path').value = ${JSON.stringify(csv)}; $('ct-load').click(); })()`);
  const placed = await waitFor(c, `(() => { const o = window.__vrLayers.state.overlay; return !!o && o.n_sources === 3; })()`, 30000);
  const pos = await c.evaluate(`window.__vrLayers.state.overlay.sources.map((s) => [s.label, s.x, s.y, s.z])`);
  const off = Math.max(...pos.map(([n, x, y, z]) => { const t = truth.find((r) => r[0] === n)[1]; return Math.max(Math.abs(x - t[0]), Math.abs(y - t[1]), Math.abs(z - t[2])); }));
  check(`${L}: the catalogue is placed at its voxels (sky and velocity through the cube's WCS)`, placed.value && off < 0.01, `max offset ${off.toExponential(2)} px`);

  // ── In VR: the Sources window from the Select tab ──
  await c.evaluate(`(async () => { const b = document.getElementById('enter-vr');
    for (let i = 0; i < 50 && b.disabled; i++) await new Promise((r) => setTimeout(r, 50)); b.click(); })()`);
  const running = await waitFor(c, 'window.__xrfake.frames > 20 && !!window.__vrXR() && window.__vrTex3D().xrActive', 15000);
  if (!running.value) { check(`${L}: VR session draws frames`, false); return {}; }
  const p0 = await c.evaluate('window.__tex3dProbe()');
  await c.evaluate(`window.__vrMenu.act({ type: 'press', id: 'tab:select' }); window.__vrMenu.act({ type: 'press', id: 'src-list' })`);
  const shown = await waitFor(c, '!!window.__vrMenu.wins.sources.model', 5000);
  const view0 = await c.evaluate('(() => { const v = window.__vrSources.view(); return { total: v.total, rows: v.rows.map((r) => r.label), sort: v.sort.key }; })()');
  check(`${L}: List opens the Sources window with the three sources sorted by name`, shown.value && view0.total === 3
    && view0.rows.join() === 'GAL-A,GAL-B,GAL-C', JSON.stringify(view0));

  // A row chosen with the ray and the trigger: the visit.
  const lay = await c.evaluate(`window.__vrSources.layout().map((w) => ({ id: w.id, rect: w.rect }))`);
  const rowB = lay.find((w) => w.id === 'row:c:1');
  const at = await c.evaluate(`window.__vrMenu.contentWorld('sources', ${rowB.rect[0] + 60}, ${rowB.rect[1] + rowB.rect[3] / 2})`);
  const aim0 = await c.evaluate('window.__xrfake.aimRight.slice()');
  await c.evaluate(`window.__xrfake.aimRight = ${JSON.stringify(at)}`);
  await sleep(200);
  await c.evaluate('window.__xrfake.buttons.right[0].pressed = true');
  await sleep(150);
  await c.evaluate('window.__xrfake.buttons.right[0].pressed = false');
  await sleep(200);
  const v1 = await c.evaluate(`({ cur: window.__vrSources.state.current, visit: window.__vrVisit(), clip: window.__vrState.clip.max[2] - window.__vrState.clip.min[2] })`);
  const vd = v1.visit && v1.visit.target ? Math.hypot(...v1.visit.world.map((w, i) => w - v1.visit.target[i])) : null;
  check(`${L}: a row pressed with the ray visits the source: it lands 0.55 m ahead of the eyes`, v1.cur === 'c:1' && vd !== null && vd < 1e-3,
    `current ${v1.cur}, ${vd === null ? 'no visit' : `${(1000 * vd).toFixed(2)} mm from the target`}, scale ${v1.visit ? v1.visit.scale.toFixed(2) : '?'}`);
  // Next (sorted by name): GAL-C, after the velocity range was narrowed away from it.
  await c.evaluate(`(() => { const s = window.__vrState; s.clip.min[2] = 0; s.clip.max[2] = 0.3; })()`);
  await c.evaluate(`window.__vrMenu.act({ type: 'press', id: 'src-next' })`);
  await sleep(100);
  const v2 = await c.evaluate(`({ cur: window.__vrSources.state.current, clip: [window.__vrState.clip.min[2], window.__vrState.clip.max[2]], visit: window.__vrVisit() })`);
  const zC = (30 + 0.5) / shape[2];
  check(`${L}: Next steps to the next source and moves the velocity range to hold it (width kept)`, v2.cur === 'c:2'
    && v2.clip[0] <= zC && v2.clip[1] >= zC && Math.abs(v2.clip[1] - v2.clip[0] - 0.3) < 1e-6, JSON.stringify(v2.clip));

  // Flags and a note, through the window's own buttons.
  await c.evaluate(`window.__vrSources.press('flag:spurious'); window.__vrSources.press('note:RFI'); window.__vrSources.choose('c:0', false); window.__vrSources.press('flag:real')`);
  // A source found with the Kinematic Lasso, added from the Select tab.
  await c.evaluate(`(() => { window.__vrMenu.ui.mode = 'select'; window.__vrSelection.setTool('lasso'); window.__xrfake.aimRight = ${JSON.stringify(aim0)}; })()`);
  await c.evaluate('window.__vrMenu.act({ type: "press", id: "reset-view" })');
  await sleep(300);
  await c.evaluate('window.__xrfake.buttons.right[0].pressed = true');
  await sleep(150);
  await c.evaluate('window.__xrfake.buttons.right[0].pressed = false');
  const lasso = await waitFor(c, '!!(window.__vrSelection.sel.lasso && window.__vrSelection.sel.lasso.stats)', 30000);
  await c.evaluate(`window.__vrMenu.act({ type: 'press', id: 'src-add' })`);
  const found = await waitFor(c, 'window.__vrSources.state.user.length === 1 && window.__vrSources.state.user[0].lon !== null', 10000);
  const u = await c.evaluate('window.__vrSources.state.user[0]');
  check(`${L}: + Found adds the lasso's object (its brightest voxel, its ICRS position from the backend)`, lasso.value && found.value
    && Math.abs(u.x - truth[0][1][0]) <= 2 && Math.abs(u.z - truth[0][1][2]) <= 2, JSON.stringify(u && { label: u.label, x: u.x, y: u.y, z: u.z, v: u.velocity }));
  await c.evaluate(`window.__vrSources.press('flag:doubtful')`);   // the found one is current

  // ── Send: the VOTable, the hub's product ──
  const cursor = (await json('GET', '/v1/vr/activity?after=0&wait=0', session)).body?.next || 0;
  const t0 = Date.now();
  await c.evaluate(`window.__vrSources.press('send')`);
  let ev = null;
  for (let i = 0; i < 100 && !ev; i++) {
    const page = (await json('GET', `/v1/vr/activity?after=${cursor}&wait=1`, session)).body || { events: [] };
    ev = page.events.find((e) => e.kind === 'product' && e.payload && e.payload.product === 'classification') || null;
  }
  const sendMs = Date.now() - t0;
  const prod = ev ? parseProduct(ev) : null;
  const cls = ev ? parseClassification(ev.payload) : null;
  check(`${L}: Send reports a classification the desktop opens in its catalogue table`, !!ev && openIn(prod) === 'CatalogueTable'
    && cls.flagged === 2 && cls.found === 1 && cls.counts.join() === '1,0,1' && cls.sources.length === 3,
    ev ? `${sendMs} ms; ${ev.summary}` : 'no event');
  const file = prod && prod.path;
  const xml = file && existsSync(file) ? readFileSync(file, 'utf8') : '';
  const cells = (xml.match(/<TD>[^<]*<\/TD>/g) || []).map((t) => t.slice(4, -5));
  check(`${L}: the VOTable keeps the catalogue's columns and rows, adds vr_flag and the found source`,
    /name="vr_flag"/.test(xml) && /name="v_rad"/.test(xml) && (xml.match(/<TR>/g) || []).length === 4
      && cells.includes('spurious') && cells.includes('real') && cells.includes('doubtful') && cells.includes('RFI') && cells.includes('VR-1'),
    file ? `${file.split('/').pop()}, ${(xml.match(/<TR>/g) || []).length} rows` : 'no file');
  const p1 = await c.evaluate('window.__tex3dProbe()');
  check(`${L}: no 3-D texture allocated in VR by the source list`, allocsIn(p1) === allocsIn(p0), `${allocsIn(p0)} -> ${allocsIn(p1)}`);
  // The window's redraw and upload (a hover change each time), on this machine (not the Quest).
  const bench = await c.evaluate(`(() => { const s = window.__vrSources, ids = s.layout().map((w) => w.id), t = [];
    for (let i = 0; i < 40; i++) { s.pointer.hover = ids[i % ids.length]; const t0 = performance.now(); s.texture(); t.push(performance.now() - t0); }
    s.pointer.hover = null; t.sort((a, b) => a - b); return { p50: t[20], p95: t[38] }; })()`);
  check(`${L}: the Sources window redraws and uploads in under 8 ms (this machine)`, bench.p95 < 8,
    `p50 ${bench.p50.toFixed(2)} ms, p95 ${bench.p95.toFixed(2)} ms`);
  await c.evaluate(`window.__xrfake.aimRight = ${JSON.stringify(aim0)}`);
  return { sendMs, bench };
}

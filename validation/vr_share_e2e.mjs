// Selections shared between the VR page and the desktop (issue #188), end to
// end in a headless Chrome against a local backend. This script plays the
// desktop: it opens the cube in a session of its own, opens the page with the
// desktop's pairing link (token, session, dataset, path), waits on the hub as
// the cube viewer does (SharedSelectionLink), and
//
//   1. makes a Kinematic Lasso in the page (a pick through NGC 3187 along the
//      spectral axis, as vr_selection_identity.py does) and reads it from the
//      hub as the desktop would: voxels, flux, the mask itself, the record;
//      checks them against the page's own numbers, against mask_stats_bin on
//      the fetched mask and against kinematic_select_bin with the page's
//      request (Dice 1.0), and times page -> desktop;
//   2. publishes a desktop lasso (a smaller reach) and waits until the page
//      draws it, without a recipe step of its own, timing desktop -> page;
//   3. makes a box in the page and reads it from the hub.
//
//   node validation/vr_share_e2e.mjs --url http://localhost:8795 --token-file FILE \
//        --cube /path/hgc44_3axes.fits [--chrome PATH] [--out DIR]
//
// Needs the page's ?debug hooks (main.js). The token is read from a file and
// is put only in the page's #fragment; it is never printed.
import { spawn } from 'node:child_process';
import { mkdirSync, readFileSync, writeFileSync, mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const args = Object.fromEntries(process.argv.slice(2).reduce((a, v, i, all) => {
  if (v.startsWith('--')) a.push([v.slice(2), all[i + 1] && !all[i + 1].startsWith('--') ? all[i + 1] : true]);
  return a;
}, []));
const URL0 = args.url || 'http://localhost:8795';
const TOKEN = readFileSync(args['token-file'], 'utf8').trim();
const CUBE = args.cube || '/Users/fvitello/Downloads/DemoData/hgc44_3axes.fits';
const CHROME = args.chrome || '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
const OUT = args.out || mkdtempSync(join(tmpdir(), 'vr188-out-'));
mkdirSync(OUT, { recursive: true });
// NGC 3187 in HCG 44: the pixel of its NED position (astropy all_world2pix, 0-based).
const NGC3187_PX = [465.45, 297.57];
const W = 1600, H = 1000;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function launch() {
  const port = 9800 + Math.floor(Math.random() * 150);
  const profile = mkdtempSync(join(tmpdir(), 'vr188-'));
  const proc = spawn(CHROME, ['--headless=new', '--use-angle=metal', `--remote-debugging-port=${port}`,
    `--user-data-dir=${profile}`, `--window-size=${W},${H}`, '--force-device-scale-factor=1',
    '--no-first-run', '--no-default-browser-check', 'about:blank'], { stdio: 'ignore' });
  for (let i = 0; i < 100; i++) {
    try {
      const list = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
      const page = list.find((t) => t.type === 'page');
      if (page) return { proc, ws: page.webSocketDebuggerUrl };
    } catch { /* not up yet */ }
    await sleep(100);
  }
  throw new Error('Chrome did not start');
}

function cdp(url) {
  const ws = new WebSocket(url);
  let id = 0;
  const waiting = new Map();
  ws.addEventListener('message', (e) => {
    const m = JSON.parse(e.data);
    if (m.id && waiting.has(m.id)) { waiting.get(m.id)(m); waiting.delete(m.id); }
  });
  const ready = new Promise((r) => { if (ws.readyState === 1) r(); else ws.addEventListener('open', () => r()); });
  const send = async (method, params = {}) => {
    await ready;
    const i = ++id;
    ws.send(JSON.stringify({ id: i, method, params }));
    const m = await new Promise((r) => waiting.set(i, r));
    if (m.error) throw new Error(`${method}: ${m.error.message}`);
    return m.result;
  };
  const evaluate = async (expr) => {
    const r = await send('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true });
    if (r.exceptionDetails) throw new Error(`page: ${r.exceptionDetails.exception?.description || r.exceptionDetails.text}`);
    return r.result.value;
  };
  return { send, evaluate, close: () => ws.close() };
}

async function waitFor(c, expr, ms = 120000, every = 20) {
  const t0 = Date.now();
  for (;;) {
    if (await c.evaluate(expr)) return Date.now() - t0;
    if (Date.now() - t0 > ms) throw new Error(`timeout waiting for ${expr.slice(0, 120)}`);
    await sleep(every);
  }
}

// ── The desktop's side: plain HTTP, as BackendClient / SharedSelectionLink ──
let SESSION = '';
const hdr = (extra = {}) => ({ 'X-Visivo-Token': TOKEN, ...(SESSION ? { 'X-Visivo-Session': SESSION } : {}), ...extra });
async function json(method, path, body) {
  const r = await fetch(`${URL0}${path}`, { method, headers: hdr({ 'Content-Type': 'application/json' }), body: body === undefined ? undefined : JSON.stringify(body) });
  if (!r.ok) throw new Error(`${method} ${path}: ${r.status} ${(await r.text()).slice(0, 200)}`);
  return r.json();
}
async function binary(path, body, contentType) {
  const r = await fetch(`${URL0}${path}`, { method: 'POST', headers: hdr({ 'Content-Type': contentType }), body });
  if (!r.ok) throw new Error(`POST ${path}: ${r.status} ${(await r.text()).slice(0, 200)}`);
  return { headers: r.headers, buf: await r.arrayBuffer() };
}
/** The desktop's long poll: GET /v1/share/selection?after=…&wait=20, then the mask. */
async function desktopWait(ds, after) {
  const q = new URLSearchParams({ dataset_id: ds, after: String(after), wait: '20' });
  const r = await fetch(`${URL0}/v1/share/selection?${q}`, { headers: hdr() });
  const tInfo = Date.now();
  if (r.status === 204) return { status: 204, tInfo };
  const info = await r.json();
  const m = await fetch(`${URL0}/v1/share/selection/mask?${new URLSearchParams({ dataset_id: ds, seq: String(info.seq) })}`, { headers: hdr() });
  const buf = await m.arrayBuffer();
  return { status: r.status, info, mask: new Float32Array(buf), maskHeaders: m.headers, buf, tInfo, tMask: Date.now() };
}
const countOf = (m) => { let n = 0; for (const v of m) if (v !== 0 && Number.isFinite(v)) n++; return n; };
function dice(a, b) {
  let inter = 0, na = 0, nb = 0;
  for (let i = 0; i < a.length; i++) { const x = a[i] > 0.5, y = b[i] > 0.5; na += x; nb += y; inter += x && y; }
  return (2 * inter) / (na + nb);
}

async function main() {
  const report = { cube: CUBE, url: URL0 };
  const s = await json('POST', '/v1/sessions/new', {});
  SESSION = s.session_id;
  const open = await json('POST', '/v1/datasets/open', { path: CUBE });
  const DS = open.dataset_id;
  report.full_shape = [open.width, open.height, open.depth];

  const chrome = await launch();
  const c = cdp(chrome.ws);
  try {
    await c.send('Page.enable');
    await c.send('Runtime.enable');
    const frag = new URLSearchParams({ token: TOKEN, session: SESSION, dataset: DS, path: CUBE }).toString();
    const tNav = Date.now();
    await c.send('Page.navigate', { url: `${URL0}/vr/?debug#${frag}` });
    await waitFor(c, `!!(window.__vrShare && window.__vrShare.st.running && window.__vrSelection && window.__vrSelection.sel)`, 180000, 200);
    report.paired_to_linked_ms = Date.now() - tNav;
    report.page_dataset = await c.evaluate('window.__vrShare.st.dataset');

    // 1. Page -> desktop: the desktop waits first, then the page makes a lasso.
    const wait1 = desktopWait(DS, 0);
    const n = report.full_shape;
    const ray = { o: [(NGC3187_PX[0] + 0.5) / n[0], (NGC3187_PX[1] + 0.5) / n[1], 1.5], d: [0, 0, -1] };
    await c.evaluate(`window.__vrSelection.setTool('lasso')`);
    const tAct = Date.now();
    await c.evaluate(`window.__vrSelection.act(${JSON.stringify(ray)})`);
    const tActDone = Date.now();
    const page = await c.evaluate(`(() => { const L = window.__vrSelection.sel.lasso; return L && { count: L.count, origin: L.origin, dims: L.dims, request: L.request, ms: L.ms, statsMs: L.statsMs,
      flux: L.stats && L.stats.flux_jy_kms, sum: L.stats && L.stats.sum, recipes: window.__vrSelection.sel.recipes.length,
      error: window.__vrSelection.sel.error }; })()`);
    if (!page) throw new Error(`the page made no lasso: ${await c.evaluate('window.__vrSelection.sel.error')}`);
    const got = await wait1;
    const shared = got.info;
    const viaStats = JSON.parse(new TextDecoder().decode((await binary(`/v1/cube/mask_stats_bin?${new URLSearchParams({
      dataset_id: DS, origin_x: shared.origin[0], origin_y: shared.origin[1], origin_z: shared.origin[2],
      width: shared.dims[0], height: shared.dims[1], depth: shared.dims[2] })}`, got.buf, 'application/octet-stream')).buf));
    const regrown = await binary('/v1/cube/kinematic_select_bin', JSON.stringify({ dataset_id: DS, ...shared.meta.grow }), 'application/json');
    const regrownMask = new Float32Array(regrown.buf);
    report.page_to_desktop = {
      page_count: page.count, page_flux_jy_kms: page.flux, page_recipes: page.recipes,
      page_grow_ms: page.ms, page_stats_ms: page.statsMs, act_total_ms: tActDone - tAct,
      desktop_count: shared.count, desktop_mask_count: countOf(got.mask), desktop_flux_jy_kms: shared.stats.flux_jy_kms,
      mask_stats_on_fetched_mask_flux: viaStats.flux_jy_kms, mask_stats_voxels: viaStats.selected_voxels,
      same_box: JSON.stringify(shared.origin) === JSON.stringify(page.origin) && JSON.stringify(shared.dims) === JSON.stringify(page.dims),
      record_is_page_request: JSON.stringify(shared.meta.grow) === JSON.stringify(page.request),
      dice_vs_regrow: regrownMask.length === got.mask.length ? dice(regrownMask, got.mask) : null,
      source: shared.source, kind: shared.kind,
      // From the end of the page's lasso (the act's promise, CDP round trip
      // included) to the desktop's long poll answering, and to its mask.
      act_done_to_desktop_info_ms: got.tInfo - tActDone,
      act_start_to_desktop_mask_ms: got.tMask - tAct,
      act_done_to_desktop_mask_ms: got.tMask - tActDone,
      page_publish_ms: await c.evaluate('window.__vrShare.st.lastLatencyMs'),
    };

    // 2. Desktop -> page: a smaller lasso grown by the "desktop", published.
    const lastSeq = shared.seq;
    const deskReq = { ...shared.meta.grow, reach: 4, reach_max: 4 };
    const desk = await binary('/v1/cube/kinematic_select_bin', JSON.stringify({ dataset_id: DS, ...deskReq }), 'application/json');
    const dh = (k) => desk.headers.get(k);
    const dOrigin = dh('X-Visivo-Origin').split(',').map(Number);
    const dDims = [dh('X-Visivo-Width'), dh('X-Visivo-Height'), dh('X-Visivo-Depth')].map(Number);
    const dCount = Number(dh('X-Visivo-Selected-Count'));
    const tPub = Date.now();
    const pub = await binary(`/v1/share/selection?${new URLSearchParams({
      dataset_id: DS, kind: 'lasso', source: 'desktop', client: 'desktop-e2e',
      origin_x: dOrigin[0], origin_y: dOrigin[1], origin_z: dOrigin[2], width: dDims[0], height: dDims[1], depth: dDims[2],
      meta: JSON.stringify({ grow: deskReq, made_in: 'desktop' }) })}`, desk.buf, 'application/octet-stream');
    const tPubDone = Date.now();
    const pubJ = JSON.parse(new TextDecoder().decode(pub.buf));
    await waitFor(c, `(() => { const L = window.__vrSelection.sel.lasso; return !!(L && L.shared && L.shared.seq === ${pubJ.seq}); })()`, 30000, 5);
    const tDrawn = Date.now();
    const pageGot = await c.evaluate(`(() => { const L = window.__vrSelection.sel.lasso; return { count: L.count, drawn: L.reduced.count, flux: L.stats && L.stats.flux_jy_kms,
      source: L.shared.source, recipes: window.__vrSelection.sel.recipes.length, receiveMs: window.__vrShare.st.receiveMs, note: window.__vrShare.st.note }; })()`);
    await sleep(300);
    const shot = await c.send('Page.captureScreenshot', { format: 'png' });
    writeFileSync(join(OUT, 'page_with_desktop_lasso.png'), Buffer.from(shot.data, 'base64'));
    report.desktop_to_page = {
      desktop_count: dCount, published_count: pubJ.count, desktop_flux_jy_kms: pubJ.stats.flux_jy_kms,
      page_count: pageGot.count, page_drawn_reduced: pageGot.drawn, page_flux_jy_kms: pageGot.flux,
      page_source: pageGot.source, page_recipes_after: pageGot.recipes, page_note: pageGot.note,
      publish_ms: tPubDone - tPub, publish_done_to_drawn_ms: tDrawn - tPubDone, publish_start_to_drawn_ms: tDrawn - tPub,
      page_receive_ms: pageGot.receiveMs,
    };

    // 3. A box made in the page, read from the hub.
    const wait3 = desktopWait(DS, pubJ.seq);
    await c.evaluate(`window.__vrSelection.setTool('box')`);
    const b0 = { o: [0.62, 0.40, 1.5], d: [0, 0, -1] }, b1 = { o: [0.70, 0.46, 1.5], d: [0, 0, -1] };
    await c.evaluate(`window.__vrSelection.act(${JSON.stringify(b0)})`);
    await c.evaluate(`window.__vrSelection.act(${JSON.stringify(b1)})`);
    const pageBox = await c.evaluate(`(() => { const B = window.__vrSelection.sel.box; return B && { range: B.range, voxels: B.stats.selected_voxels, flux: B.stats.flux_jy_kms, sum: B.stats.sum }; })()`);
    const got3 = await wait3;
    report.box = pageBox && got3.info ? {
      page_range: pageBox.range, page_voxels: pageBox.voxels, page_flux_jy_kms: pageBox.flux, page_sum: pageBox.sum,
      desktop_kind: got3.info.kind, desktop_count: got3.info.count, desktop_mask_count: countOf(got3.mask),
      desktop_flux_jy_kms: got3.info.stats.flux_jy_kms, desktop_sum: got3.info.stats.sum, desktop_box: got3.info.meta.box,
    } : { page: pageBox, hub_status: got3.status };
    report.seq_after = lastSeq;
    report.hud = await c.evaluate('window.__vrShare.hudLines()');
  } finally {
    c.close();
    chrome.proc.kill();
  }
  writeFileSync(join(OUT, 'vr_share_e2e.json'), JSON.stringify(report, null, 2));
  console.log(JSON.stringify(report, null, 2));
}

main().catch((e) => { console.error(e.stack || String(e)); process.exit(1); });

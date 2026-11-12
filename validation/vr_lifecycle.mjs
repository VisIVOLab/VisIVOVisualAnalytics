// The VR page's lifecycle in a headless Chrome against a backend this script
// starts and stops itself (epic #166): the page loads a small synthetic cube
// and draws it; then, unless --smoke, each failure state is driven and the
// page's answer checked: a stale build and API, the controls guide on the
// message card, the render session's socket dropped by the backend, a WebGL
// context lost and restored, the cube's dataset gone (404), the backend
// killed and restarted, the token changed under the page, and the
// diagnostics text and the backend log scrubbed of the token.
//
//   node validation/vr_lifecycle.mjs [--smoke] [--python PATH] [--chrome PATH]
//        [--gl metal|swiftshader] [--port 8791] [--out DIR] [--cube FILE]
//
// --smoke is what CI runs (tests.yml): start, load, first frame drawn, no page
// error, versions agree. Exit code 0 when every check passed. The tokens are
// random, made here, and never printed.
import { spawn, spawnSync } from 'node:child_process';
import { mkdirSync, mkdtempSync, writeFileSync, readFileSync, openSync, existsSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { randomBytes } from 'node:crypto';

const args = Object.fromEntries(process.argv.slice(2).reduce((a, v, i, all) => {
  if (v.startsWith('--')) a.push([v.slice(2), all[i + 1] && !all[i + 1].startsWith('--') ? all[i + 1] : true]);
  return a;
}, []));
const HERE = dirname(fileURLToPath(import.meta.url));
const REPO = resolve(HERE, '..');
const SMOKE = !!args.smoke;
const PY = args.python || process.env.VISIVO_PYTHON || 'python3';
// `--chrome` given without a value (an empty shell variable) parses as true:
// that is "not found", said at once rather than as a failed spawn later.
const chromeArg = typeof args.chrome === 'string' ? args.chrome : '';
const CHROME = chromeArg || process.env.CHROME || (args.chrome === true ? '' : (process.platform === 'darwin'
  ? '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome' : 'google-chrome'));
if (!CHROME) {
  console.log('FAIL run — no Chrome: pass --chrome PATH (or set CHROME); none was found');
  process.exit(2);
}
if (CHROME.includes('/') && !existsSync(CHROME)) {
  console.log(`FAIL run — Chrome not found at ${CHROME}`);
  process.exit(2);
}
const GL = args.gl || (process.platform === 'darwin' ? 'metal' : 'swiftshader');
const PORT = Number(args.port || 8791);
const OUT = args.out || join(HERE, 'out', 'vr_lifecycle');
mkdirSync(OUT, { recursive: true });
const BASE = `http://localhost:${PORT}`;
const QUEST_UA = 'Mozilla/5.0 (X11; Linux x86_64; Quest 3) AppleWebKit/537.36 (KHTML, like Gecko) OculusBrowser/40.0 Chrome/132.0 VR Safari/537.36';
const W = 1280, H = 800;

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const tokenA = randomBytes(24).toString('base64url');
const tokenB = randomBytes(24).toString('base64url');
const results = { smoke: SMOKE, gl: GL, checks: [], timings: {} };
let failed = 0;
function check(name, ok, detail = '') {
  results.checks.push({ name, ok: !!ok, detail: String(detail).slice(0, 300) });
  if (!ok) failed++;
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${name}${detail !== '' ? ` — ${String(detail).slice(0, 200)}` : ''}`);
}

// ── A synthetic cube (a Gaussian source in noise, RA/Dec/velocity WCS) ─────
function makeCube(name = 'synthetic_lifecycle.fits', { shape = [40, 64, 64], ra = 150.0 } = {}) {
  if (args.cube && name === 'synthetic_lifecycle.fits') return args.cube;
  const dir = mkdtempSync(join(tmpdir(), 'vr-lifecycle-'));
  const path = join(dir, name);
  const [nz, ny, nx] = shape;
  const code = `
import sys, numpy as np
from astropy.io import fits
nz, ny, nx = ${nz}, ${ny}, ${nx}
z, y, x = np.mgrid[0:nz, 0:ny, 0:nx].astype(np.float32)
rng = np.random.default_rng(1)
d = 0.01 * rng.standard_normal((nz, ny, nx)) + np.exp(-(((x - nx / 2) / 8) ** 2 + ((y - ny * 0.47) / 6) ** 2 + ((z - nz / 2) / 5) ** 2))
h = fits.Header()
for k, v in dict(CTYPE1='RA---SIN', CRPIX1=nx / 2, CRVAL1=${ra}, CDELT1=-0.002, CUNIT1='deg',
                 CTYPE2='DEC--SIN', CRPIX2=ny / 2, CRVAL2=2.0, CDELT2=0.002, CUNIT2='deg',
                 CTYPE3='VRAD', CRPIX3=1.0, CRVAL3=1.0e6, CDELT3=5000.0, CUNIT3='m/s',
                 BUNIT='Jy/beam', SPECSYS='BARYCENT').items():
    h[k] = v
fits.writeto(sys.argv[1], d.astype(np.float32), h, overwrite=True)
`;
  const r = spawnSync(PY, ['-c', code, path]);
  if (r.status !== 0) throw new Error(`could not write the synthetic cube: ${r.stderr}`);
  return path;
}


// ── The backend ─────────────────────────────────────────────────────────────
let backend = null;
let backendLog = join(OUT, 'backend.log');
async function startBackend(token) {
  const fd = openSync(backendLog, 'a');
  const env = { ...process.env, VISIVO_TOKEN: token, PYTHONUNBUFFERED: '1' };
  const proc = spawn(PY, ['-m', 'uvicorn', 'app.main:app', '--host', 'localhost', '--port', String(PORT), '--log-level', 'info'],
    { cwd: join(REPO, 'backend'), env, stdio: ['ignore', fd, fd] });
  for (let i = 0; i < 600; i++) {
    try { if ((await fetch(`${BASE}/v1/ping`, { signal: AbortSignal.timeout(2000) })).ok) { backend = proc; return proc; } } catch { /* not yet */ }
    if (proc.exitCode !== null) throw new Error(`the backend exited (${proc.exitCode}): see ${backendLog}`);
    await sleep(100);
  }
  throw new Error('the backend did not start');
}
async function stopBackend() {
  if (!backend) return;
  const p = backend;
  backend = null;
  p.kill('SIGTERM');
  for (let i = 0; i < 100 && p.exitCode === null; i++) await sleep(100);
  if (p.exitCode === null) p.kill('SIGKILL');
  for (let i = 0; i < 50; i++) {
    // A backend half way down can accept and never answer: bounded.
    try { await fetch(`${BASE}/v1/ping`, { signal: AbortSignal.timeout(1000) }); } catch { return; }
    await sleep(100);
  }
}

// ── Chrome over CDP ─────────────────────────────────────────────────────────
let cdpPort = 0;
async function launch() {
  const port = 9400 + Math.floor(Math.random() * 400);
  cdpPort = port;
  const profile = mkdtempSync(join(tmpdir(), 'vr-lifecycle-chrome-'));
  const gl = GL === 'metal' ? ['--use-angle=metal'] : ['--use-angle=swiftshader', '--enable-unsafe-swiftshader', '--use-gl=angle'];
  const proc = spawn(CHROME, ['--headless=new', ...gl, `--remote-debugging-port=${port}`, `--user-data-dir=${profile}`,
    `--window-size=${W},${H}`, '--force-device-scale-factor=1', '--no-first-run', '--no-default-browser-check',
    // A headless window counts as hidden after a while: Chrome then throttles
    // and freezes the page (nothing runs for minutes, CDP included), which
    // looks like a hung page. Keep it a foreground page.
    '--disable-background-timer-throttling', '--disable-renderer-backgrounding',
    '--disable-backgrounding-occluded-windows', '--disable-features=IntensiveWakeUpThrottling,CalculateNativeWinOcclusion',
    ...(process.platform === 'linux' ? ['--no-sandbox'] : []), '--enable-logging=stderr', '--v=0', 'about:blank'],
  { stdio: ['ignore', 'ignore', openSync(join(OUT, 'chrome.log'), 'w')] });
  for (let i = 0; i < 150; i++) {
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
  // The DevTools connection can be replaced (reconnect()): on this Mac a
  // connection was seen to stop answering while the page itself was idle and
  // fine. A fresh connection then answers at once; the run counts how often
  // that was needed (results.cdpReconnects), so a frozen page (where a fresh
  // connection does not help either) still fails.
  let ws = null;
  let ready = null;
  let id = 0;
  const waiting = new Map();
  const pageErrors = [];
  const paused = [];
  const onMessage = (e) => {
    const m = JSON.parse(e.data);
    if (m.id && waiting.has(m.id)) { waiting.get(m.id)(m); waiting.delete(m.id); }
    if (m.method === 'Runtime.exceptionThrown') pageErrors.push(m.params.exceptionDetails.exception?.description || m.params.exceptionDetails.text);
    if (m.method === 'Fetch.requestPaused') paused.push(m.params);
    if (process.env.VR_DEBUG && m.method === 'Runtime.consoleAPICalled') {
      console.log('console:', JSON.stringify(m.params.args.map((a) => a.value ?? a.description)).slice(0, 400));
    }
  };
  const connect = () => {
    const sock = new WebSocket(url);
    sock.addEventListener('message', onMessage);
    ready = new Promise((r) => { if (sock.readyState === 1) r(); else sock.addEventListener('open', () => r()); });
    ws = sock;
  };
  connect();
  const send = async (method, params = {}) => {
    await ready;
    const i = ++id;
    ws.send(JSON.stringify({ id: i, method, params }));
    const m = await new Promise((r) => waiting.set(i, r));
    if (m.error) throw new Error(`${method}: ${m.error.message}`);
    return m.result;
  };
  const timed = (promise, ms, what) => {
    let timer;
    return Promise.race([promise, new Promise((_, reject) => {
      timer = setTimeout(() => reject(new Error(`${what} timed out`)), ms);
    })]).finally(() => clearTimeout(timer));
  };
  const reconnect = async () => {
    try { ws.close(); } catch { /* gone */ }
    connect();
    await timed(ready, 5000, 'reconnect');
    await send('Runtime.enable');
    results.cdpReconnects = (results.cdpReconnects || 0) + 1;
  };
  const evaluate = async (expr, ms = 30000) => {
    const ask = () => send('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true });
    let r;
    try {
      r = await timed(ask(), ms, `evaluate ${expr.slice(0, 60)}`);
    } catch (err) {
      if (!/timed out/.test(err.message)) throw err;
      if (process.env.VR_DEBUG) console.log(`no answer in ${ms} ms: a new DevTools connection`);
      await reconnect();
      // A page that is really stuck does not answer this one either.
      r = await timed(ask(), 10000, `evaluate (after a reconnection) ${expr.slice(0, 60)}`);
    }
    if (r.exceptionDetails) throw new Error(`page: ${r.exceptionDetails.exception?.description || r.exceptionDetails.text}`);
    return r.result.value;
  };
  return { send, evaluate, pageErrors, paused, close: () => ws.close() };
}

async function waitFor(c, expr, ms = 30000, every = 100) {
  const t0 = Date.now();
  for (;;) {
    let v;
    try { v = await c.evaluate(expr); } catch (err) { v = null; if (Date.now() - t0 > ms) throw err; }
    if (v) return { value: v, ms: Date.now() - t0 };
    if (Date.now() - t0 > ms) return { value: null, ms: Date.now() - t0, timeout: true };
    await sleep(every);
  }
}

const noticeId = `(() => { const n = window.__vrShell && window.__vrShell.notices.current(); return n ? n.id : ''; })()`;
const waitNotice = (c, id, ms) => waitFor(c, `${noticeId} === ${JSON.stringify(id)}`, ms);

async function api(method, path, token, { session, body } = {}) {
  const headers = { 'X-Visivo-Token': token };
  if (session) headers['X-Visivo-Session'] = session;
  if (body !== undefined) headers['Content-Type'] = 'application/json';
  const r = await fetch(`${BASE}${path}`, { method, headers, body: body === undefined ? undefined : JSON.stringify(body) });
  return { status: r.status, body: await r.text() };
}

// ── The run ─────────────────────────────────────────────────────────────────
// The whole run is bounded: a hang is a failure, not a stuck CI job.
setTimeout(() => {
  console.log('FAIL run — timed out after 10 minutes');
  try { if (backend) backend.kill('SIGKILL'); } catch { /* gone */ }
  process.exit(2);
}, 600000).unref();
const cube = makeCube();
// A second cube: another shape, and a right ascension crossing 0°.
const cubeB = SMOKE ? null : makeCube('synthetic_ra0.fits', { shape: [24, 40, 48], ra: 0.0 });
writeFileSync(backendLog, '');
await startBackend(tokenA);
const { proc: chrome, ws } = await launch();
const c = cdp(ws);
let exitCode = 1;
try {
  await c.send('Page.enable');
  // A focused, active page (see the flags in launch()).
  await c.send('Emulation.setFocusEmulationEnabled', { enabled: true }).catch(() => {});
  await c.send('Page.setWebLifecycleState', { state: 'active' }).catch(() => {});
  await c.send('Runtime.enable');
  // A Quest's browser, so the page words its messages for the headset over USB.
  if (!SMOKE) await c.send('Emulation.setUserAgentOverride', { userAgent: QUEST_UA });
  await c.send('Page.navigate', { url: `${BASE}/vr/?debug&panel3d${SMOKE ? '&refine=0' : ''}#token=${encodeURIComponent(tokenA)}` });
  const up = await waitFor(c, `!!window.__vrShell && window.__vrShell.life.state === 'ok'`, 30000);
  check('page up, backend reachable, token accepted', up.value, `${up.ms} ms`);
  const ver = await c.evaluate(`({ page: window.__vrShell.life.page, info: window.__vrShell.life.info })`);
  results.versions = ver;
  check('page build is the backend\'s build (versioned address)', ver.page.build && ver.page.build === ver.info.build, `${ver.page.build} / ${ver.info.build}`);
  check('no version notice', !(await c.evaluate(noticeId)));

  // Load the cube and check the first frame.
  await c.evaluate(`(() => { const $ = (id) => document.getElementById(id); $('path').value = ${JSON.stringify(cube)}; $('load').click(); })()`);
  const loaded = await waitFor(c, `!!(window.__vrState.open && window.__vrState.cube && /upload/.test(document.getElementById('load-status').textContent))`, 60000);
  check('cube loaded', loaded.value, `${loaded.ms} ms; ${await c.evaluate(`document.getElementById('load-status').textContent.replace(/\\n/g, ' | ')`)}`);
  results.timings.loadMs = loaded.ms;
  await sleep(500);
  const drawn = await c.evaluate('window.__vrDrawnFraction()');
  check('first frame draws the cube', drawn.fraction > 0.003, `${(100 * drawn.fraction).toFixed(2)} % of ${drawn.w}×${drawn.h}`);
  // The backend's render sessions are not the page's errors (a runner may have
  // no offscreen GL for them): failed requests to /v1/render/ are left out.
  const errs = await c.evaluate(`window.__vrShell.diag.items.filter((e) => e.level === 'error'
    && !(e.event === 'request-failed' && e.message.includes('/v1/render/'))).map((e) => e.event + ': ' + e.message)`);
  check('no page error', !errs.length && !c.pageErrors.length, [...errs, ...c.pageErrors].join(' / '));

  if (!SMOKE) {
    // ── Loads: the path a load started with, the newest load wins ─────────
    const idle = `window.__vrState.loading === 0`;
    const shapeOf = `JSON.stringify(window.__vrState.info.full_shape)`;
    // 1. The field changed while A loads: A is shown, and A is what the page names.
    await c.evaluate(`(() => { const $ = (id) => document.getElementById(id);
      $('path').value = ${JSON.stringify(cube)}; $('load').click(); $('path').value = ${JSON.stringify(cubeB)}; })()`);
    await sleep(200);
    await waitFor(c, idle, 60000);
    const f1 = await c.evaluate(`({ path: window.__vrState.openPath, shape: ${shapeOf} })`);
    check('load: the path field changed during a load does not rename the cube', f1.path === cube && f1.shape === '[64,64,40]', JSON.stringify(f1));
    // 2. Two loads finishing in reverse order: B (started last) wins.
    await c.send('Fetch.enable', { patterns: [{ urlPattern: '*reduced_bin*', requestStage: 'Request' }] });
    c.paused.length = 0;
    await c.evaluate(`(() => { const $ = (id) => document.getElementById(id); $('path').value = ${JSON.stringify(cube)}; $('load').click(); })()`);
    const pA = await waitFor(c, '1', 20000).then(async () => { for (let i = 0; i < 200 && c.paused.length < 1; i++) await sleep(50); return c.paused[0]; });
    await c.evaluate(`(() => { const $ = (id) => document.getElementById(id); $('path').value = ${JSON.stringify(cubeB)}; $('load').click(); })()`);
    for (let i = 0; i < 400 && c.paused.length < 2; i++) await sleep(50);
    const pB = c.paused[1];
    if (pB) await c.send('Fetch.continueRequest', { requestId: pB.requestId });
    await waitFor(c, `JSON.stringify(window.__vrState.info.full_shape) === '[48,40,24]'`, 30000);
    if (pA) await c.send('Fetch.continueRequest', { requestId: pA.requestId });   // A's transfer, now, last
    await c.send('Fetch.disable');
    await sleep(300);
    await waitFor(c, idle, 60000);
    await sleep(300);
    const f2 = await c.evaluate(`({ path: window.__vrState.openPath, shape: ${shapeOf}, status: document.getElementById('load-status').textContent })`);
    check('load: an older load finishing last does not replace the newer cube', !!pA && !!pB && f2.path === cubeB && f2.shape === '[48,40,24]'
      && !/Failed/.test(f2.status), JSON.stringify({ ...f2, status: f2.status.slice(0, 60) }));
    // The page's session holds the cube on show (the superseded load changed nothing).
    const sessionOk = await c.evaluate(`(async () => {
      const b = window.__vrRefine.refiner.backend();
      try { await b.reducedInfo({ dataset_id: window.__vrState.open.dataset_id, factor_xy: 0, factor_z: 0, max_dim: 256 }); return true; }
      catch (e) { return e.message; }
    })()`);
    check('load: the page\'s session holds the cube on show', sessionOk === true, String(sessionOk));
    // A load that fails moves the generation once (at its start), not again at its end.
    const g0 = await c.evaluate('window.__vrState.loadGen');
    await c.evaluate(`(() => { const $ = (id) => document.getElementById(id); $('path').value = '/nonexistent/none.fits'; $('load').click(); })()`);
    await sleep(300);
    await waitFor(c, idle, 30000);
    const g1 = await c.evaluate(`({ gen: window.__vrState.loadGen, status: document.getElementById('load-status').textContent, shape: ${shapeOf} })`);
    check('load: a failed load bumps the generation once and leaves the cube', g1.gen === g0 + 1 && /Failed/.test(g1.status)
      && g1.shape === '[48,40,24]', JSON.stringify({ g0, ...g1, status: g1.status.slice(0, 50) }));
    // Restore the path field to the cube on show (later steps read the page).
    await c.evaluate(`document.getElementById('path').value = ${JSON.stringify(cubeB)}`);
    // 3. B's right ascension crosses 0°: its ticks are around 0, in [0, 360).
    const ra = await c.evaluate(`(async () => {
      const V = await import('/vr/js/volume.js');
      const ax = window.__vrState.info.axes.x;
      const w = ax.world.filter((v) => v !== null);
      const t = V.axisTicks(ax, 'x', 5).ticks.map((k) => k.value);
      return { crosses: w.some((v) => v > 359) && w.some((v) => v < 1), ticks: t, edge: V.axisTexToWorld(ax, ax.world, 0.5) };
    })()`);
    check('RA across 0°: ticks near 0, none through 180', ra.crosses && ra.ticks.length >= 2
      && ra.ticks.every((v) => v >= 0 && v < 360 && (v < 0.2 || v > 359.8)) && (ra.edge < 0.05 || ra.edge > 359.95), JSON.stringify(ra));
  }

  if (!SMOKE) {
    // ── The guide on the card ───────────────────────────────────────────
    await c.evaluate('window.__vrShell.showGuide(0)');
    await waitFor(c, '!!window.__vrShell.card.model', 5000);
    check('guide: Next through the card', await c.evaluate(`window.__vrCard.press('guide-next')`)
      && await c.evaluate('window.__vrShell.notices.current().page') === 1);
    await c.evaluate(`window.__vrCard.press('guide-close')`);
    check('guide: Close dismisses and is remembered', !(await c.evaluate(noticeId))
      && (await c.evaluate('window.__vrPanel.settings().guideSeen')) === true);

    // ── Versions ────────────────────────────────────────────────────────
    await c.evaluate(`(async () => { const l = window.__vrShell.life; l.page.build = 'stale0000000'; await l.probe(); })()`);
    const soft = await c.evaluate(`({ id: ${noticeId}, dom: document.getElementById('notice').hidden ? '' : document.getElementById('notice-title').textContent })`);
    check('another build: "reload" notice on the card and the page', soft.id === 'reload' && /updated/.test(soft.dom), JSON.stringify(soft));
    await c.evaluate(`(async () => { const l = window.__vrShell.life; l.page.build = ${JSON.stringify(ver.page.build)}; l.page.api = 99; await l.probe(); })()`);
    const newer = await c.evaluate(`({ id: ${noticeId}, state: window.__vrShell.life.state })`);
    check('page API newer than the backend: "update the app"', newer.id === 'update-app' && newer.state === 'mismatch', JSON.stringify(newer));
    await c.evaluate(`(async () => { const l = window.__vrShell.life; l.page.api = 1; await l.probe(); })()`);
    check('versions agree again: notice gone', !(await c.evaluate(noticeId)));

    // ── The render session's socket dropped ─────────────────────────────
    const streaming = await waitFor(c, `!!(window.__vrRefine.refiner.ws && window.__vrRefine.refiner.streaming)`, 60000);
    if (!streaming.value) {
      const why = await c.evaluate(`window.__vrRefine.refiner.unavailable || window.__vrRefine.refiner.error || window.__vrRefine.refiner.status`);
      check('refinement streaming (needed for the socket test)', false, why);
    } else {
      const sid = await c.evaluate('window.__vrRefine.refiner.sessionId');
      const t0 = Date.now();
      const del = await api('DELETE', `/v1/render/${sid}`, tokenA);
      const degraded = await waitFor(c, `(() => { const r = window.__vrRefine.refiner; return r.status === 'reconnecting' && !r.showing; })()`, 10000, 20);
      check('socket dropped: the page shows its own rendering', degraded.value, `DELETE ${del.status}; ${Date.now() - t0} ms`);
      const panelText = await c.evaluate(`window.__vrPanel.view().refine.text`);
      check('the panel says refinement is lost and when it retries', /retrying in \d+ s/.test(panelText), panelText);
      const back = await waitFor(c, `(() => { const r = window.__vrRefine.refiner; return r.streaming && r.sessionId && r.sessionId !== ${JSON.stringify(sid)} && r.failures === 0; })()`, 30000);
      check('socket dropped: reconnected to a new render session', back.value, `${Date.now() - t0} ms after the drop`);
      results.timings.refineRecoverMs = Date.now() - t0;
    }

    // ── WebGL context loss ──────────────────────────────────────────────
    const t1 = Date.now();
    await c.evaluate(`(() => { window.__lose = window.__vrGL.getExtension('WEBGL_lose_context'); window.__lose.loseContext(); })()`);
    const lost = await waitNotice(c, 'context-lost', 5000);
    check('context lost: notice', lost.value, `${lost.ms} ms`);
    await sleep(300);
    await c.evaluate('window.__lose.restoreContext()');
    const restored = await waitNotice(c, 'context-restored', 5000);
    await sleep(300);
    const drawn2 = await c.evaluate('window.__vrDrawnFraction()');
    check('context restored: notice, cube drawn again', restored.value && drawn2.fraction > 0.003 && !drawn2.lost,
      `${(100 * drawn2.fraction).toFixed(2)} % drawn; ${Date.now() - t1} ms lost→drawn`);
    results.timings.contextRestoreMs = Date.now() - t1;
    await c.evaluate(`window.__vrCard.press('dismiss')`);

    // ── The dataset gone (the desktop's session ended) ──────────────────
    const before = await c.evaluate(`({ id: window.__vrState.open.dataset_id, session: window.__vrRefine.refiner.backend().session })`);
    const delDs = await api('DELETE', `/v1/datasets/${before.id}`, tokenA, { session: before.session });
    const t2 = Date.now();
    // A spectrum at the box centre: the request finds no dataset.
    await c.evaluate(`(() => { const [x, y] = window.__vrProject([0.5, 0.47, 0.5]); window.__vrClick(x, y); })()`);
    const reopened = await waitNotice(c, 'reopened', 15000);
    const after = await c.evaluate('window.__vrState.open.dataset_id');
    check('dataset 404: the cube is opened again by its path', reopened.value && after !== before.id,
      `DELETE ${delDs.status}; ${before.id} → ${after}; ${Date.now() - t2} ms`);
    await c.evaluate(`window.__vrCard.press('dismiss')`);

    // ── The backend killed, then back ───────────────────────────────────
    await stopBackend();
    const t3 = Date.now();             // from the moment nothing answers any more
    const down = await waitNotice(c, 'unreachable', 15000);
    const downText = await c.evaluate(`document.getElementById('notice-body').textContent`);
    check('backend killed: "not reachable" on the card and the page', down.value, `${down.ms} ms after the backend stopped answering`);
    results.timings.unreachableDetectMs = down.ms;
    check('as a headset over USB: the message names adb reverse and Open on Headset', /adb reverse/.test(downText) && /Open on Headset/.test(downText));
    await sleep(7000);
    const attempts = await c.evaluate('window.__vrShell.life.backoff.attempts');
    check('backend down: retries with a backoff', attempts >= 3, `${attempts} attempts in about 7 s`);
    // A sticky card never blocks the view for good: aside after 6 s, and Hide
    // folds it into a line on the panel until the state changes.
    const parked = await c.evaluate('window.__vrShell.card.parked');
    check('backend down: the card moved aside after 6 s', parked === true);
    await c.evaluate(`window.__vrCard.press('hide')`);
    const hidden = await c.evaluate(`({ id: ${noticeId}, status: window.__vrPanel.view().status })`);
    check('Hide: card gone, the panel says it', !hidden.id && /Backend not reachable/.test(hidden.status), JSON.stringify(hidden));
    await sleep(1500);
    check('Hide: still hidden after the next failed checks', !(await c.evaluate(noticeId)));
    const t4 = Date.now();
    await startBackend(tokenA);
    const restarted = await waitNotice(c, 'restarted', 30000);
    const after2 = await c.evaluate(`({ id: window.__vrState.open.dataset_id, state: window.__vrShell.life.state })`);
    check('backend back: notice gone, restart seen, cube opened again', restarted.value && after2.state === 'ok' && after2.id !== after,
      `${Date.now() - t4} ms after the backend was up; ${after} → ${after2.id}`);
    results.timings.restartRecoverMs = Date.now() - t4;
    const refineBack = await waitFor(c, `!!(window.__vrRefine.refiner.streaming)`, 45000);
    check('backend back: refinement streams again', refineBack.value, `${refineBack.ms} ms`);
    await c.evaluate(`window.__vrCard.press('dismiss')`);

    // ── The token changed under the page ────────────────────────────────
    await stopBackend();
    await startBackend(tokenB);
    const auth = await waitNotice(c, 'auth', 30000);
    check('token refused: notice says to re-open from the desktop', auth.value
      && /Open on Headset/.test(await c.evaluate(`window.__vrShell.notices.current().lines.join(' ')`)));
    // The way out on the page: the new token, then "Check again".
    await c.evaluate(`(() => { const t = document.getElementById('token'); t.value = ${JSON.stringify(tokenB)}; t.dispatchEvent(new Event('change')); })()`);
    await c.evaluate(`window.__vrCard.press('retry')`);
    const fixed = await waitFor(c, `window.__vrShell.life.state === 'ok' && ${noticeId} !== 'auth'`, 15000);
    check('new token: back to normal', fixed.value);

    // ── A load during a recovery wins ───────────────────────────────────
    // A recovery is started and, before it can finish, a synthetic cube is
    // loaded: the recovery must leave the synthetic cube and its state alone.
    await sleep(3000);               // the reopen after the token change has finished
    const race = await c.evaluate(`(async () => {
      const gen0 = window.__vrState.loadGen;
      const p = window.__vrShell.recover('user');
      document.getElementById('synthetic-size').value = '128';
      document.getElementById('synthetic').click();
      const r = p ? await p : null;
      return { started: !!p, superseded: !!(r && r.superseded), genMoved: window.__vrState.loadGen > gen0,
        mode: window.__vrState.cube.mode, open: window.__vrState.open, refine: window.__vrRefine.refiner.datasetId };
    })()`);
    check('a synthetic load during a recovery supersedes it', race.started && race.superseded && race.genMoved
      && race.mode === 'synthetic' && race.open === null && race.refine === null, JSON.stringify(race));

    // ── Diagnostics and the log ─────────────────────────────────────────
    await c.evaluate(`document.getElementById('copy-diag').click()`);
    await sleep(300);
    const diag = await c.evaluate(`document.getElementById('diag-out').textContent`);
    writeFileSync(join(OUT, 'diagnostics.txt'), diag);
    check('diagnostics: versions, GPU, settings, events, no token',
      diag.split("\n").length > 20 && /page build: [0-9a-f]{12}/.test(diag) && /3-D texture \d+/.test(diag) && /backend-restarted/.test(diag)
      && !diag.includes(tokenA) && !diag.includes(tokenB), `${diag.split('\n').length} lines`);
    await sleep(4000);            // the sender's interval
    const log = readFileSync(backendLog, 'utf8');
    // The page's render WebSocket carries its token as a subprotocol: the
    // access log must show none of its addresses with ?token=.
    const wsWithToken = log.split('\n').filter((l) => /WebSocket \/v1\/render\/\S+\?token=/.test(l)).length;
    check('render WebSocket: no token in the access log', wsWithToken === 0, `${wsWithToken} lines`);
    const vrLines = log.split('\n').filter((l) => l.includes('[vr '));
    writeFileSync(join(OUT, 'backend_vr_lines.txt'), vrLines.join('\n'));
    check('backend log: the page\'s events arrived, scrubbed', vrLines.length >= 3
      && vrLines.some((l) => /webgl-context-lost|backend-unreachable|dataset-gone/.test(l))
      && !vrLines.some((l) => l.includes(tokenA) || l.includes(tokenB) || l.includes(tmpdir())
        || /\/v1\/render\/[0-9a-f]{8,}/.test(l)), `${vrLines.length} lines`);
    const errs2 = c.pageErrors.filter((e) => !/WEBGL_lose_context|context/i.test(e));
    check('no uncaught page exception during the run', !errs2.length, errs2.join(' / '));
  }
  exitCode = failed ? 1 : 0;
} catch (err) {
  check('run', false, err.stack || err.message);
} finally {
  results.failed = failed;
  writeFileSync(join(OUT, SMOKE ? 'smoke.json' : 'results.json'), JSON.stringify(results, null, 2));
  try { c.close(); } catch { /* closed */ }
  chrome.kill();
  console.log(`${results.checks.length - failed}/${results.checks.length} checks passed${results.cdpReconnects ? ` (DevTools reconnected ${results.cdpReconnects}×)` : ""}`);
  await stopBackend();
  process.exit(failed ? 1 : exitCode);
}

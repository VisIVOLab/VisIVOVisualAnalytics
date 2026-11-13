// The VR page's lifecycle in a headless Chrome against a backend this script
// starts and stops itself (epic #166): the page loads a small synthetic cube
// and draws it; then, unless --smoke, each failure state is driven and the
// page's answer checked: a stale build and API, the controls guide on the
// message card, the render session's socket dropped by the backend, a WebGL
// context lost and restored, the cube's dataset gone (404), the backend
// killed and restarted, the token changed under the page, and the
// diagnostics text and the backend log scrubbed of the token.
//
//   node validation/vr_lifecycle.mjs [--smoke] [--regressions] [--menu] [--measure] [--python PATH] [--chrome PATH]
//        [--alloc] [--xrfake] [--gl metal|swiftshader] [--port 8791] [--cdp-port N] [--out DIR] [--cube FILE]
//
// --smoke is what CI runs (tests.yml): start, load, first frame drawn, no page
// error, versions agree. Exit code 0 when every check passed. The tokens are
// random, made here, and never printed.
// --regressions adds recovery, stale layer replies and large-binning checks
// to that smoke run, with the hand menu driven by the mouse (vr_menu.mjs,
// issue #201; --menu runs that alone on the smoke run). CI runs both; the full
// lifecycle run remains separate.
// --measure (issue #224) adds the measurement record's checks to the smoke
// run (validation/vr_measure.mjs): recording on vs off, a guided run through
// every condition, the record read back and the report script run on it.
// --paired (issue #213) runs the smoke (and --regressions) checks with the
// page in the public loader (web/vr-loader/, served here on --loader-port),
// paired over WebRTC data channels through validation/vr_pair_fake.py (a
// minimal fake of the pairing service and the backend's bridge, on
// --pair-port); then a dropped link resumed without a new code, an unknown
// resume code asking for a new one, and the transport's overhead against
// direct HTTP. The checks run in the page's iframe. --pair-fake PATH runs
// another script with the fake's command line instead (the real service and
// bridge); --paired-backend URL names the backend that script's bridge
// serves, for the direct comparisons. --pair-stress N adds N transfers of an
// 8.4 MB cube, one DevTools call each (--pair-stress-in-page: one loop in the
// page; --pair-stress-detached: the same with DevTools detached while it
// runs); VR_CHROME_ARGS passes more flags to Chrome (e.g. --vmodule logging).
import { spawn, spawnSync } from 'node:child_process';
import { createServer } from 'node:http';
import { mkdirSync, mkdtempSync, writeFileSync, readFileSync, openSync, existsSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { randomBytes } from 'node:crypto';
import { runRegressions } from './vr_regressions.mjs';
import { runMenuChecks } from './vr_menu.mjs';
import { runMeasureChecks } from './vr_measure.mjs';
import { runAllocChecks, GL_PROBE } from './vr_alloc.mjs';
import { XR_FAKE, runXRFakeChecks, runSinglePageChecks, runDelayedTakeoverCheck, runReloadCheck } from './vr_xr_fake.mjs';
import { runCutChecks } from './vr_cuts.mjs';
import { runTex3DChecks, TEX3D_PROBE } from './vr_tex3d.mjs';

const args = Object.fromEntries(process.argv.slice(2).reduce((a, v, i, all) => {
  if (v.startsWith('--')) a.push([v.slice(2), all[i + 1] && !all[i + 1].startsWith('--') ? all[i + 1] : true]);
  return a;
}, []));
const HERE = dirname(fileURLToPath(import.meta.url));
const REPO = resolve(HERE, '..');
const PAIRED = !!args.paired;
const MEASURE = !!args.measure;
// --alloc (issue #226): the repeated-selection allocation run alone (also part of --regressions).
const ALLOC = !!args.alloc || !!args.regressions;
// --xrfake (issue #226): the headset path through a WebXR stand-in, and one active page (also part of --regressions),
// with the debug cuts of issue #229 (vr_cuts.mjs) and no 3-D texture allocated in VR (vr_tex3d.mjs).
const XRFAKE = (!!args.xrfake || !!args.regressions) && !args.paired;
const SMOKE = PAIRED || MEASURE || !!args.smoke || !!args.regressions || !!args.menu || !!args.alloc || !!args.xrfake;
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
const LOADER_PORT = Number(args['loader-port'] || PORT + 1);
const PAIR_PORT = Number(args['pair-port'] || PORT + 2);
const PAIR_CODE = 'K7Q2ZP';
const OUT = args.out || join(HERE, 'out', 'vr_lifecycle');
mkdirSync(OUT, { recursive: true });
const BASE = `http://localhost:${PORT}`;
// Paired: the backend the bridge serves (another process when --pair-fake
// runs a real bridge with its own backend); the direct checks go there.
const API_BASE = typeof args['paired-backend'] === 'string' ? args['paired-backend'].replace(/\/+$/, '') : BASE;
const PAIR_FAKE = typeof args['pair-fake'] === 'string' ? resolve(args['pair-fake']) : null;
const QUEST_UA = 'Mozilla/5.0 (X11; Linux x86_64; Quest 3) AppleWebKit/537.36 (KHTML, like Gecko) OculusBrowser/40.0 Chrome/132.0 VR Safari/537.36';
const W = 1280, H = 800;

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const tokenA = randomBytes(24).toString('base64url');
const tokenB = randomBytes(24).toString('base64url');
const results = { smoke: SMOKE, paired: PAIRED, gl: GL, checks: [], timings: {} };
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
  // Measurement records (issue #224) go to this run's own directory.
  const env = { ...process.env, VISIVO_TOKEN: token, PYTHONUNBUFFERED: '1', VISIVO_VR_MEASURE_DIR: join(OUT, 'measurements') };
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

// ── Paired mode: the public loader and the fake pairing (issue #213) ───────
const MIME = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript', '.css': 'text/css', '.json': 'application/json',
  '.webmanifest': 'application/manifest+json', '.png': 'image/png' };
let loaderServer = null;
/** The loader directory as any static web server would serve it. */
function startLoader() {
  const root = join(REPO, 'web', 'vr-loader');
  loaderServer = createServer((req, res) => {
    let rel;
    try { rel = decodeURIComponent(new URL(req.url, 'http://x').pathname).replace(/^\/+/, ''); } catch { rel = ''; }
    if (!rel || rel.endsWith('/')) rel += 'index.html';
    const file = resolve(root, rel);
    if (!file.startsWith(`${root}/`) || !existsSync(file)) { res.writeHead(404); res.end('not found'); return; }
    const ext = file.slice(file.lastIndexOf('.'));
    res.writeHead(200, { 'Content-Type': MIME[ext] || 'application/octet-stream', 'Cache-Control': 'no-cache' });
    res.end(readFileSync(file));
  });
  return new Promise((r) => loaderServer.listen(LOADER_PORT, 'localhost', r));
}
let pairFake = null;
async function startPairFake(token) {
  const fd = openSync(join(OUT, 'pair_fake.log'), 'a');
  pairFake = spawn(PY, [PAIR_FAKE || join(HERE, 'vr_pair_fake.py'), '--port', String(PAIR_PORT), '--backend', BASE, '--code', PAIR_CODE],
    { env: { ...process.env, VISIVO_TOKEN: token }, stdio: ['ignore', fd, fd] });
  for (let i = 0; i < 300; i++) {
    if (readFileSync(join(OUT, 'pair_fake.log'), 'utf8').includes(`ws://localhost:${PAIR_PORT}/v1/join`)) return;
    if (pairFake.exitCode !== null) throw new Error(`the pairing fake exited: see ${join(OUT, 'pair_fake.log')}`);
    await sleep(100);
  }
  throw new Error('the pairing fake did not start');
}

// ── Chrome over CDP ─────────────────────────────────────────────────────────
let cdpPort = 0;
let chrome = null;
async function launch() {
  const port = Number(args['cdp-port']) || 9400 + Math.floor(Math.random() * 400);
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
    ...(process.platform === 'linux' ? ['--no-sandbox'] : []), '--enable-logging=stderr', '--v=0',
    // Debugging only: more Chrome flags (e.g. WebRTC logging with --vmodule).
    ...(process.env.VR_CHROME_ARGS ? process.env.VR_CHROME_ARGS.split(' ').filter(Boolean) : []), 'about:blank'],
  { stdio: ['ignore', 'ignore', openSync(join(OUT, 'chrome.log'), 'w')] });
  chrome = proc; // Also clean up when startup fails before CDP is available.
  let spawnError = null;
  proc.once('error', (err) => { spawnError = err; });
  // A cold CI runner can take more than 15 seconds to start Chrome. Bound
  // both the overall wait and each HTTP probe, without hiding an early exit.
  const deadline = Date.now() + 60000;
  while (Date.now() < deadline) {
    if (spawnError) throw new Error(`could not start Chrome: ${spawnError.message}`);
    if (proc.exitCode !== null || proc.signalCode !== null) {
      throw new Error(`Chrome exited (${proc.exitCode ?? proc.signalCode}): see ${join(OUT, 'chrome.log')}`);
    }
    try {
      const list = await (await fetch(`http://127.0.0.1:${port}/json/list`,
        { signal: AbortSignal.timeout(1000) })).json();
      const page = list.find((t) => t.type === 'page');
      if (page) return { proc, ws: page.webSocketDebuggerUrl };
    } catch { /* not up yet */ }
    await sleep(100);
  }
  throw new Error(`Chrome did not expose CDP within 60 seconds: see ${join(OUT, 'chrome.log')}`);
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
  const contexts = new Map();     // frameId -> default execution context id
  const onMessage = (e) => {
    const m = JSON.parse(e.data);
    if (m.method === 'Runtime.executionContextCreated') {
      const ctx = m.params.context;
      if (ctx.auxData && ctx.auxData.isDefault) contexts.set(ctx.auxData.frameId, ctx.id);
    }
    if (m.method === 'Runtime.executionContextDestroyed') {
      for (const [f, id] of contexts) if (id === m.params.executionContextId) contexts.delete(f);
    }
    if (m.method === 'Runtime.executionContextsCleared') contexts.clear();
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
  // Paired (#213): the page runs in the loader's iframe; expressions go to it.
  const target = { frameId: null };
  const evaluate = async (expr, ms = 30000, { top = false } = {}) => {
    const ask = () => {
      const params = { expression: expr, awaitPromise: true, returnByValue: true };
      if (target.frameId && !top) {
        const id = contexts.get(target.frameId);
        if (!id) return Promise.reject(new Error('page: the app frame has no context yet'));
        params.contextId = id;
      }
      return send('Runtime.evaluate', params);
    };
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
  return { send, evaluate, pageErrors, paused, target, close: () => ws.close() };
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
  const r = await fetch(`${API_BASE}${path}`, { method, headers, body: body === undefined ? undefined : JSON.stringify(body) });
  return { status: r.status, body: await r.text() };
}

// ── Paired: transport, resume, a new code, overhead (issue #213) ───────────
async function pairedChecks() {
  // Garbage collections must not end the link: the loader's objects that
  // hold it must stay reachable (a link dropped by a collection ends every
  // request in flight).
  // Only what happens from here on counts (an earlier drop is the early-drop
  // check's to judge, below).
  const gcFrom = await c.evaluate('({ state: window.__vrLink().state, t: Date.now() })');
  const linkBefore = gcFrom.state;
  for (let i = 0; i < 3; i++) { await c.send('HeapProfiler.collectGarbage'); await sleep(300); }
  await sleep(500);
  const afterGc = await c.evaluate(`({ state: window.__vrLink().state,
    events: window.__vrShell.diag.items.filter((e) => /^pair-link-(down|lost)/.test(e.event) && e.t >= ${gcFrom.t}).map((e) => e.message) })`);
  check('paired: garbage collections leave the link up', linkBefore === 'up' && afterGc.state === 'up' && !afterGc.events.length,
    JSON.stringify(afterGc));
  await c.evaluate(`document.getElementById('copy-diag').click()`);
  await sleep(300);
  const diag = await c.evaluate(`document.getElementById('diag-out').textContent`);
  const tline = diag.split('\n').find((l) => l.startsWith('transport:')) || '';
  results.transport = tline;
  check('paired: the diagnostics name the transport', /WebRTC data channel/.test(tline) && !diag.includes(tokenA), tline);
  check('paired: no token field, no token in the page',
    await c.evaluate(`document.getElementById('connect').hidden && !document.documentElement.outerHTML.includes(${JSON.stringify(tokenA)})`));
  // Overhead: the same reduced cube and the same check, over the pairing (the
  // page's own Backend) and over direct HTTP (from this script, same machine).
  // A cube of 256×256×128 (8.4 MB at 8 bits, unbinned), loaded by the page.
  const big = makeCube('synthetic_pair_big.fits', { shape: [128, 256, 256] });
  // Started and awaited in the page (one DevTools call, no polling during
  // the transfer: see the overhead measurement below).
  const bigLoaded = await c.evaluate(`(async () => { const $ = (id) => document.getElementById(id);
    $('path').value = ${JSON.stringify(big)}; $('load').click();
    const t0 = Date.now();
    await new Promise((r) => setTimeout(r, 300));
    while (Date.now() - t0 < 120000) {
      if (window.__vrState.loading === 0 && window.__vrState.openPath === ${JSON.stringify(big)}) return true;
      await new Promise((r) => setTimeout(r, 100));
    }
    return false; })()`, 130000);
  if (!bigLoaded) throw new Error('the 256×256×128 cube did not load over the pairing');
  const req = await c.evaluate(`JSON.stringify({ dataset_id: window.__vrState.open.dataset_id, factor_xy: 1, factor_z: 1, max_dim: 256, encoding: 'uint8', mode: 'mean' })`);
  const session = await c.evaluate('window.__vrRefine.refiner.backend().session');
  const pairedT = [], infoP = [], direct = [], infoD = [];
  // The paired side in one DevTools call, looped in the page: a DevTools call
  // sent into the page's iframe while it receives at full speed was seen, on
  // this Mac's headless Chrome, to stall for 30-40 s (the run then waits on
  // the debugger, not on the page).
  const pairedRuns = await c.evaluate(`(async () => { const b = window.__vrRefine.refiner.backend(); const out = [];
    for (let i = 0; i < 7; i++) {
      let t0 = performance.now();
      const r = await b.reducedCube(JSON.parse(${JSON.stringify(req)}));
      const cube = { ms: performance.now() - t0, bytes: r.bytes };
      t0 = performance.now();
      await b.info();
      out.push({ cube, info: { ms: performance.now() - t0 } });
    }
    return out; })()`, 120000);
  for (const p of pairedRuns) { pairedT.push(p.cube); infoP.push(p.info); }
  for (let i = 0; i < 7; i++) {
    let t0 = performance.now();
    const r = await fetch(`${API_BASE}/v1/cube/reduced_bin`, { method: 'POST',
      headers: { 'X-Visivo-Token': tokenA, 'X-Visivo-Session': session, 'Content-Type': 'application/json' }, body: req });
    direct.push({ ms: performance.now() - t0, bytes: (await r.arrayBuffer()).byteLength });
    t0 = performance.now();
    await (await fetch(`${API_BASE}/v1/vr/info`, { headers: { 'X-Visivo-Token': tokenA } })).json();
    infoD.push({ ms: performance.now() - t0 });
  }
  const med = (a) => a.map((x) => x.ms).sort((x, y) => x - y)[Math.floor(a.length / 2)];
  const o = { reducedBytes: pairedT[0].bytes, pairedMs: med(pairedT), directMs: med(direct), infoPairedMs: med(infoP), infoDirectMs: med(infoD) };
  o.pairedMBps = o.reducedBytes / 1e6 / (o.pairedMs / 1000);
  o.directMBps = o.reducedBytes / 1e6 / (o.directMs / 1000);
  results.timings.overhead = o;
  check('paired: the reduced cube comes over the pairing, as many bytes as direct', o.reducedBytes > 0 && o.reducedBytes === direct[0].bytes,
    JSON.stringify(o, (k, v) => (typeof v === 'number' ? Number(v.toFixed(2)) : v)));
  // --pair-stress N: N more transfers of the 8.4 MB cube, each bounded, to
  // see whether heavy traffic drops the link (each one's time is kept).
  const stress = Number(args['pair-stress']) || 0;
  if (stress > 0) {
    // Through the page's own boundedRequest (the same module instance as the
    // page's): its 20 s deadline aborts the request and its body (a cancel on
    // the channel), so a stalled transfer never runs on behind the next ones.
    const one = `(async () => { const b = window.__vrRefine.refiner.backend(); const t0 = performance.now();
        const at = new Date().toISOString();
        const api = await import(new URL('api.js', document.querySelector('script[type=module]').src));
        try {
          const bytes = await api.boundedRequest('/v1/cube/reduced_bin',
            { method: 'POST', headers: b.headers(true), body: ${JSON.stringify(req)} }, 20000, null,
            async (resp) => { if (!resp.ok) throw new Error('HTTP ' + resp.status); return (await resp.arrayBuffer()).byteLength; });
          return { ms: performance.now() - t0, bytes, at };
        } catch (e) { return { ms: performance.now() - t0, error: e.message, at }; } })()`;
    let times = [];
    if (args['pair-stress-detached']) {
      // The loop started in the page, then DevTools detached while it runs:
      // no debugger session at all during the transfers. Attached again
      // afterwards to read the results.
      await c.evaluate(`(window.__stress = { done: false, out: [] }, (async () => {
        for (let i = 0; i < ${stress}; i++) window.__stress.out.push(await ${one});
        window.__stress.done = true; })(), 1)`);
      // Exceptions while detached are kept by the page itself (both frames),
      // and those seen before go over to the new connection.
      const keepErrors = `(() => { window.__detachedErrors = [];
        addEventListener('error', (e) => window.__detachedErrors.push(String(e.message || e.error)));
        addEventListener('unhandledrejection', (e) => window.__detachedErrors.push(String(e.reason && (e.reason.stack || e.reason.message) || e.reason)));
        return 1; })()`;
      await c.evaluate(keepErrors);
      await c.evaluate(keepErrors, 5000, { top: true });
      const frameId = c.target.frameId;
      const earlierErrors = c.pageErrors.slice();
      c.close();
      await sleep(stress * 1200 + 5000);
      c = cdp(cdpUrl);
      await c.send('Runtime.enable');
      c.target.frameId = frameId;
      c.pageErrors.push(...earlierErrors);
      for (const top of [false, true]) {
        c.pageErrors.push(...await c.evaluate('window.__detachedErrors || []', 10000, { top }));
      }
      const done = await waitFor(c, 'window.__stress && window.__stress.done', stress * 20000);
      times = done.value ? await c.evaluate('window.__stress.out') : [{ error: 'the detached loop did not finish', ms: 0, at: '' }];
    } else if (args['pair-stress-in-page']) {
      // All in the page, one DevTools call: the debugger stays quiet while
      // the transfers run (see the overhead measurement above for why).
      times = await c.evaluate(`(async () => { const out = []; for (let i = 0; i < ${stress}; i++) out.push(await ${one}); return out; })()`,
        stress * 25000);
    } else {
      for (let i = 0; i < stress; i++) times.push(await c.evaluate(one, 40000));
    }
    let failures = 0;
    times.forEach((r, i) => { if (r.error) { failures++; console.log(`stress ${i}: ${r.at} failed after ${r.ms.toFixed(0)} ms: ${r.error}`); } });
    results.stress = times;
    const ok = times.filter((t) => !t.error).map((t) => t.ms).sort((a, b) => a - b);
    check(`paired: ${stress} more transfers of 8.4 MB`, failures === 0,
      `${failures} failed; median ${ok.length ? ok[Math.floor(ok.length / 2)].toFixed(0) : 'n/a'} ms`);
  }
  // Nothing dropped the link so far: no pair-link event before the drop made below.
  const early = await c.evaluate(`window.__vrShell.diag.items.filter((e) => /^pair-link-(down|lost)/.test(e.event)).map((e) => e.event + ': ' + e.message)`);
  results.earlyLinkEvents = early;
  // A connection the browser closed by itself ("connection closed, not
  // closed here"): Chrome's renderer closing the peer connection with no
  // close() of the page's (checked with a probe on close()), no transport
  // error and no garbage collection involved, as a large transfer begins; seen
  // with DevTools attached or detached, against the real bridge (#213). The
  // page resumes it, so it is recorded and said, not failed. Anything else (a
  // channel the bridge closed, a close of the page's own) fails the check.
  const browserClosed = early.filter((e) => /connection closed, not closed here/.test(e));
  const otherDrops = early.filter((e) => !browserClosed.includes(e));
  results.browserClosedDrops = browserClosed.length;
  if (browserClosed.length) console.log(`note paired: ${browserClosed.length} drop(s) with the connection closed by the browser itself (resumed): ${browserClosed.join(' / ')}`);
  check('paired: the link stayed up until the deliberate drop (browser-side closes aside)', !otherDrops.length, otherDrops.join(' / '));
  // A dropped link: resumed with the resume code, the page not reloaded.
  await c.evaluate('window.__pairMarker = 42');
  const t0 = Date.now();
  pairFake.kill('SIGUSR1');
  const down = await waitFor(c, `window.__vrLink().state === 'down'`, 15000, 20);
  const up = await waitFor(c, `window.__vrLink().state === 'up' && window.__pairMarker === 42`, 30000, 50);
  results.timings.resumeMs = Date.now() - t0;
  check('paired: a dropped link is resumed without a new code, the page kept', !!(down.value && up.value),
    `down after ${down.ms} ms, up again ${Date.now() - t0} ms after the drop`);
  const back = await waitFor(c, `window.__vrShell.life.state === 'ok'`, 15000);
  const after = await c.evaluate(`(async () => { try { await window.__vrRefine.refiner.backend().reducedInfo({ dataset_id: window.__vrState.open.dataset_id, factor_xy: 0, factor_z: 0, max_dim: 256 }); return true; } catch (e) { return e.message; } })()`);
  check('paired: requests work again after the resume', !!back.value && after === true, String(after));
  // The page reloads itself (its "Reload page"): the link stays, the new
  // document works, and the build's files come from Cache Storage.
  const before = await c.evaluate(`(async () => ({ joins: window.__vrLoader.state.resumes,
    caches: (await caches.keys()).filter((k) => k.startsWith('visivo-vr-')) }))()`, 5000, { top: true });
  await c.evaluate('setTimeout(() => location.reload(), 50), 1');
  const t1 = Date.now();
  await sleep(300);
  const reloaded = await waitFor(c, `window.__pairMarker === undefined && !!window.__vrShell && window.__vrShell.life.state === 'ok' && window.__vrLink().state === 'up'`, 30000, 50);
  const afterReload = await c.evaluate(`(async () => ({ joins: window.__vrLoader.state.resumes,
    caches: (await caches.keys()).filter((k) => k.startsWith('visivo-vr-')) }))()`, 5000, { top: true });
  check('paired: the page reloads inside the loader, the link kept, the build cached',
    !!reloaded.value && afterReload.joins === before.joins && afterReload.caches.length === 1 && afterReload.caches[0] === `visivo-vr-${results.versions.page.build}`,
    `${Date.now() - t1} ms; ${JSON.stringify(afterReload)}`);
  // A pairing the backend forgot: the resume fails, the page asks for a new code.
  pairFake.kill('SIGUSR2');
  await sleep(200);
  pairFake.kill('SIGUSR1');
  const lost = await waitFor(c, `window.__vrLink().state === 'lost' && !!window.__vrShell.notices.current() && window.__vrShell.notices.current().id === 'pair-lost'`, 30000, 50);
  check('paired: an unknown resume code asks for a new code (notice)', !!lost.value, `${lost.ms} ms`);
  await c.evaluate(`window.__vrShell.act('new-code')`);
  await sleep(300);
  const form = await c.evaluate(`!document.getElementById('pair-form').hidden && !document.getElementById('home').hidden`, 5000, { top: true });
  check('paired: the notice\'s action opens the loader\'s code form', form);
  const errs = c.pageErrors.filter((e) => !/abort|link to the computer|pairing/i.test(e));
  check('paired: no uncaught page exception', !errs.length, errs.join(' / '));
}

// ── The run ─────────────────────────────────────────────────────────────────
// The whole run is bounded: a hang is a failure, not a stuck CI job.
setTimeout(() => {
  console.log('FAIL run — timed out after 10 minutes');
  // Everything this run started goes with it, or the next run finds its ports taken.
  for (const p of [backend, pairFake, chrome]) { try { if (p) p.kill('SIGKILL'); } catch { /* gone */ } }
  try { if (loaderServer) loaderServer.close(); } catch { /* closed */ }
  process.exit(2);
}, 600000).unref();
const cube = makeCube();
// A second cube: another shape, and a right ascension crossing 0°.
const cubeB = SMOKE ? null : makeCube('synthetic_ra0.fits', { shape: [24, 40, 48], ra: 0.0 });
writeFileSync(backendLog, '');
let c = null;
let cdpUrl = '';   // the page's DevTools address (to attach again after a detached stress)
let exitCode = 1;
try {
  await startBackend(tokenA);
  if (PAIRED) { writeFileSync(join(OUT, 'pair_fake.log'), ''); await startLoader(); await startPairFake(tokenA); }
  const { ws } = await launch();
  cdpUrl = ws;
  c = cdp(ws);
  await c.send('Page.enable');
  // A focused, active page (see the flags in launch()).
  await c.send('Emulation.setFocusEmulationEnabled', { enabled: true }).catch(() => {});
  await c.send('Page.setWebLifecycleState', { state: 'active' }).catch(() => {});
  await c.send('Runtime.enable');
  // WebGL objects and their bytes, counted from before the page's scripts run.
  if (ALLOC) await c.send('Page.addScriptToEvaluateOnNewDocument', { source: GL_PROBE });
  // A WebXR stand-in, active only on an address with ?xrfake.
  if (XRFAKE) await c.send('Page.addScriptToEvaluateOnNewDocument', { source: XR_FAKE });
  // Texture calls split by the VR session (issue #229), before the page's scripts.
  if (XRFAKE) await c.send('Page.addScriptToEvaluateOnNewDocument', { source: TEX3D_PROBE });
  // A Quest's browser, so the page words its messages for the headset over USB.
  if (!SMOKE) await c.send('Emulation.setUserAgentOverride', { userAgent: QUEST_UA });
  if (PAIRED) {
    // The loader on its own origin (localhost:<loader port>), the service
    // from ?pair_service= (honoured on a loopback loader), the code from #pair=.
    const t0 = Date.now();
    await c.send('Page.navigate', { url: `http://localhost:${LOADER_PORT}/?debug&panel3d&refine=0&pair_service=${encodeURIComponent(`ws://localhost:${PAIR_PORT}`)}#pair=${PAIR_CODE}` });
    let frameId = null;
    for (let i = 0; i < 300 && !frameId; i++) {
      const tree = await c.send('Page.getFrameTree');
      const child = (tree.frameTree.childFrames || [])[0];
      if (child && /\/app\/[a-z0-9]+\/vr\//.test(child.frame.url)) frameId = child.frame.id;
      else await sleep(100);
    }
    const addr = await c.evaluate('location.href', 5000, { top: true });
    check('paired: the loader paired and opened the page in its iframe', !!frameId, `${Date.now() - t0} ms`);
    check('paired: the code left the loader\'s address bar', !addr.includes(PAIR_CODE));
    c.target.frameId = frameId;
    results.timings.pairMs = Date.now() - t0;
  } else {
    await c.send('Page.navigate', { url: `${BASE}/vr/?debug&panel3d${SMOKE ? '&refine=0' : ''}#token=${encodeURIComponent(tokenA)}` });
  }
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

  if (args.regressions || args.menu) await runMenuChecks({ c, check, waitFor, results });
  if (ALLOC) await runAllocChecks({ c, check, waitFor, results, chromePid: chrome ? chrome.pid : 0, assert: !args['alloc-no-assert'] });
  if (args.regressions) await runRegressions({ c, check, waitFor, api, python: PY, makeCube, token: tokenA, cube });
  // Before the paired checks, which end the pairing.
  if (MEASURE) await runMeasureChecks({ c, check, waitFor, results, out: OUT, python: PY, secrets: [tokenA, tokenB], paired: PAIRED });
  if (PAIRED) await pairedChecks();
  if (XRFAKE) {
    // The headset path with composition layers, then without (the fallback),
    // each on a fresh page with the cube loaded again; then two tabs.
    for (const [variant, expectLayers] of [['xrfake', true], ['xrfake=nolayers', false]]) {
      const url = `${BASE}/vr/?debug&refine=0&${variant}#token=${encodeURIComponent(tokenA)}`;
      await c.send('Page.navigate', { url });
      await waitFor(c, `!!window.__vrShell && window.__vrShell.life.state === 'ok'`, 30000);
      await c.evaluate(`(() => { const $ = (id) => document.getElementById(id); $('path').value = ${JSON.stringify(cube)}; $('load').click(); })()`);
      const ok = await waitFor(c, `!!(window.__vrState.open && window.__vrState.cube && /upload/.test(document.getElementById('load-status').textContent))`, 60000);
      if (!ok.value) { check(`xrfake ${variant}: cube loaded`, false); continue; }
      await runXRFakeChecks({ c, check, waitFor, label: expectLayers ? 'layers' : 'no layers', expectLayers });
      if (expectLayers) await runDelayedTakeoverCheck({ c, check, waitFor });
    }
    await runReloadCheck({ c, check, waitFor, path: cube });
    // The debug cuts of issue #229, each loaded and a lasso made in VR with it.
    await runCutChecks({ c, check, waitFor, base: BASE, token: tokenA, cube });
    // Issue #229: lassos, an Isolate and a binning change in one VR session, then the benchmark: no 3-D allocation in VR.
    await runTex3DChecks({ c, check, waitFor, base: BASE, token: tokenA, cube });
    await runSinglePageChecks({ c, check, waitFor, cdpPort, connect: cdp,
      url: `${BASE}/vr/?debug&refine=0#token=${encodeURIComponent(tokenA)}` });
  }

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
  if (chrome) chrome.kill();
  console.log(`${results.checks.length - failed}/${results.checks.length} checks passed${results.cdpReconnects ? ` (DevTools reconnected ${results.cdpReconnects}×)` : ""}`);
  if (pairFake) pairFake.kill('SIGTERM');
  if (loaderServer) loaderServer.close();
  await stopBackend();
  process.exit(failed ? 1 : exitCode);
}

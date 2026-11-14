// How long a cube switch inside a VR session blocks the page's main thread
// (issue #246), in a headless Chrome through the WebXR stand-in (XR_FAKE).
// The page loads a synthetic cube (filling the fixed capacity, #235), enters
// VR, then puts on show N more cubes of the same size, generated beforehand
// (their generation is not timed). For each switch it reports the longest
// gap between two animation frames (the frame loop's hitch), the longest
// task Chrome saw (PerformanceObserver 'longtask', 50 ms and up) and the
// time from the request to the cube on show.
//
//   node validation/vr_switch_block.mjs [--gl metal|swiftshader] [--size 256] [--switches 8]
//        [--encoding uint8|float16] [--noworker] [--python PATH] [--chrome PATH] [--port 8893] [--cdp-port 9353]
//
// Prints one line per switch and a summary (median and max); exit code 0
// when the page ran. The token is random, made here, and never printed.
import { spawn } from 'node:child_process';
import { mkdtempSync, openSync, mkdirSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { randomBytes } from 'node:crypto';
import { XR_FAKE } from './vr_xr_fake.mjs';

const args = Object.fromEntries(process.argv.slice(2).reduce((a, v, i, all) => {
  if (v.startsWith('--')) a.push([v.slice(2), all[i + 1] && !all[i + 1].startsWith('--') ? all[i + 1] : true]);
  return a;
}, []));
const HERE = dirname(fileURLToPath(import.meta.url));
const REPO = resolve(HERE, '..');
const PY = args.python || process.env.VISIVO_PYTHON || 'python3';
const CHROME = args.chrome || process.env.CHROME || '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
const GL = args.gl || 'metal';
const PORT = Number(args.port || 8893);
const CDP_PORT = Number(args['cdp-port'] || 9353);
const SIZE = Number(args.size || 256);
const SWITCHES = Number(args.switches || 8);
const ENCODING = args.encoding === 'float16' ? 'float16' : 'uint8';
const OUT = join(HERE, 'out', 'vr_switch_block');
mkdirSync(OUT, { recursive: true });
const BASE = `http://localhost:${PORT}`;
const token = randomBytes(24).toString('base64url');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const procs = [];
const done = (code) => { for (const p of procs) { try { p.kill('SIGKILL'); } catch { /* gone */ } } process.exit(code); };
setTimeout(() => { console.log('FAIL timed out'); done(2); }, 15 * 60000).unref();

const backend = spawn(PY, ['-m', 'uvicorn', 'app.main:app', '--host', 'localhost', '--port', String(PORT), '--log-level', 'warning'],
  { cwd: join(REPO, 'backend'), env: { ...process.env, VISIVO_TOKEN: token }, stdio: ['ignore', openSync(join(OUT, 'backend.log'), 'w'), 'inherit'] });
procs.push(backend);
for (let i = 0; ; i++) {
  try { if ((await fetch(`${BASE}/v1/ping`)).ok) break; } catch { /* not yet */ }
  if (i > 600) { console.log('FAIL backend did not start'); done(2); }
  await sleep(100);
}
const gl = GL === 'metal' ? ['--use-angle=metal'] : ['--use-angle=swiftshader', '--enable-unsafe-swiftshader', '--use-gl=angle'];
const chrome = spawn(CHROME, ['--headless=new', ...gl, `--remote-debugging-port=${CDP_PORT}`, `--user-data-dir=${mkdtempSync(join(tmpdir(), 'vr-switch-'))}`,
  '--window-size=1280,800', '--no-first-run', '--disable-background-timer-throttling', '--disable-renderer-backgrounding',
  '--disable-backgrounding-occluded-windows', 'about:blank'], { stdio: 'ignore' });
procs.push(chrome);
let wsUrl = null;
for (let i = 0; !wsUrl; i++) {
  try { wsUrl = (await (await fetch(`http://127.0.0.1:${CDP_PORT}/json/list`)).json()).find((t) => t.type === 'page')?.webSocketDebuggerUrl; } catch { /* not yet */ }
  if (i > 600) { console.log('FAIL chrome did not start'); done(2); }
  if (!wsUrl) await sleep(100);
}
const ws = new WebSocket(wsUrl);
await new Promise((r) => ws.addEventListener('open', r));
let id = 0;
const waiting = new Map();
const errors = [];
ws.addEventListener('message', (e) => {
  const m = JSON.parse(e.data);
  if (m.id && waiting.has(m.id)) { waiting.get(m.id)(m); waiting.delete(m.id); }
  if (m.method === 'Runtime.exceptionThrown') errors.push(m.params.exceptionDetails.exception?.description || m.params.exceptionDetails.text);
});
const send = (method, params = {}) => new Promise((r) => { const i = ++id; waiting.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); })
  .then((m) => { if (m.error) throw new Error(m.error.message); return m.result; });
const evaluate = async (expression) => {
  const r = await send('Runtime.evaluate', { expression, awaitPromise: true, returnByValue: true });
  if (r.exceptionDetails) throw new Error(r.exceptionDetails.exception?.description || r.exceptionDetails.text);
  return r.result.value;
};
const waitFor = async (expr, ms = 60000) => {
  for (const t0 = Date.now(); Date.now() - t0 < ms;) { if (await evaluate(expr).catch(() => false)) return true; await sleep(100); }
  return false;
};

await send('Page.enable');
await send('Runtime.enable');
await send('Emulation.setFocusEmulationEnabled', { enabled: true }).catch(() => {});
await send('Page.addScriptToEvaluateOnNewDocument', { source: XR_FAKE });
await send('Page.navigate', { url: `${BASE}/vr/?debug&refine=0&xrfake${args.noworker ? '&noworker' : ''}#token=${encodeURIComponent(token)}` });
if (!await waitFor(`!!window.__vrShell && window.__vrShell.life.state === 'ok'`)) { console.log('FAIL page up'); done(1); }
await evaluate(`(() => { const $ = (id) => document.getElementById(id); $('maxdim').value = '${SIZE}'; $('encoding').value = '${ENCODING}';
  $('synthetic-size').value = '${SIZE}'; $('synthetic').click(); })()`);
if (!await waitFor(`!!(window.__vrState.cube && window.__vrState.cube.dims[0] === ${SIZE} && window.__vrTex3D().capacity && window.__vrTex3D().capacity.onShow)`)) {
  console.log('FAIL first cube in the capacity', JSON.stringify(await evaluate('window.__vrTex3D()'))); done(1);
}
await evaluate(`(async () => { const b = document.getElementById('enter-vr');
  for (let i = 0; i < 50 && b.disabled; i++) await new Promise((r) => setTimeout(r, 50)); b.click(); })()`);
if (!await waitFor('window.__xrfake.frames > 30 && window.__vrTex3D().xrActive', 20000)) { console.log('FAIL VR session'); done(1); }

// The cubes, generated in the page before any switch (not timed), each its own voxels.
await evaluate(`(async () => {
  const V = await import('./js/volume.js');
  const base = V.syntheticCube(${SIZE});
  const f16 = ${ENCODING === 'float16'};
  window.__swCubes = Array.from({ length: ${SWITCHES} }, (_, k) => {
    let data;
    if (f16) { data = new Uint16Array(base.length); for (let i = 0; i < base.length; i++) data[i] = base[i] ? 0x3000 + base[i] * 8 + k : 0xbc00; }
    else { data = base.slice(); data[k] = 200; }
    return { data, dims: [${SIZE}, ${SIZE}, ${SIZE}], encoding: '${ENCODING}', lo: 0, hi: 1, factors: [1, 1, 1], mode: 'synthetic',
      reducedThreshold: 0.25, bunit: '', fullShape: [${SIZE}, ${SIZE}, ${SIZE}] };
  });
  window.__swInfo = V.syntheticInfo(${SIZE});
  window.__swLong = [];
  try { new PerformanceObserver((l) => { for (const e of l.getEntries()) window.__swLong.push(e.duration); }).observe({ type: 'longtask' }); } catch { /* none */ }
  return true;
})()`);

// The frame gap with nothing happening, for comparison (headless Chrome paces about 60 Hz).
const idle = await evaluate(`new Promise((resolve) => {
  let last = 0, maxGap = 0, gaps = [];
  const t0 = performance.now();
  const tick = (now) => { if (last) { maxGap = Math.max(maxGap, now - last); gaps.push(now - last); } last = now;
    if (now - t0 < 2000) requestAnimationFrame(tick); else { gaps.sort((a, b) => a - b); resolve({ maxGap, median: gaps[gaps.length >> 1] }); } };
  requestAnimationFrame(tick);
})`);
console.log(`idle: frame gap median ${idle.median.toFixed(1)} ms, max ${idle.maxGap.toFixed(1)} ms over 2 s`);
const rows = [];
for (let k = 0; k < SWITCHES; k++) {
  await sleep(500);
  const r = await evaluate(`new Promise((resolve) => {
    const cube = window.__swCubes[${k}];
    window.__swLong.length = 0;
    let last = performance.now(), maxGap = 0, frames = 0, shownAt = 0, f0 = window.__xrfake.frames;
    const t0 = performance.now();
    const tick = () => {
      const now = performance.now();
      maxGap = Math.max(maxGap, now - last); last = now; frames++;
      if (!shownAt && window.__vrState.cube === cube) shownAt = now;
      if (shownAt && now - shownAt > 300) {
        resolve({ maxGap, toShown: shownAt - t0, longest: Math.max(0, ...window.__swLong), xrFrames: window.__xrfake.frames - f0,
          load: { uploadMs: window.__vrState.load.uploadMs, macroMs: window.__vrState.load.macroMs, blockMs: window.__vrState.load.blockMs, offMain: window.__vrState.load.offMain, steps: window.__vrState.load.writeSteps, stepMs: window.__vrState.load.writeStepMs } });
      } else if (now - t0 > 30000) resolve({ timeout: true });
      else requestAnimationFrame(tick);
    };
    requestAnimationFrame(() => { last = performance.now(); window.__vrInstall(window.__swInfo, cube); requestAnimationFrame(tick); });
  })`);
  if (r.timeout) { console.log(`switch ${k + 1}: not shown within 30 s`); continue; }
  rows.push(r);
  console.log(`switch ${k + 1}: frame gap max ${r.maxGap.toFixed(1)} ms, longest task ${r.longest.toFixed(0)} ms, shown after ${r.toShown.toFixed(0)} ms`
    + ` (upload ${r.load.uploadMs?.toFixed(1)} ms, skip grid ${r.load.macroMs?.toFixed(1)} ms${r.load.offMain ? " in the worker" : ""}${r.load.blockMs !== undefined ? `, longest main-thread slice ${r.load.blockMs.toFixed(1)} ms` : ''}${r.load.steps ? `, write in ${r.load.steps} steps, longest ${r.load.stepMs.toFixed(1)} ms` : ''}), ${r.xrFrames} XR frames`);
}
const med = (a) => { const s = [...a].sort((x, y) => x - y); return s[Math.floor(s.length / 2)]; };
const gaps = rows.slice(1).map((r) => r.maxGap);     // the first switch warms the code up
console.log(`SUMMARY gl=${GL} ${SIZE}^3 ${ENCODING}: frame gap median ${med(gaps).toFixed(1)} ms, max ${Math.max(...gaps).toFixed(1)} ms;`
  + ` shown after median ${med(rows.slice(1).map((r) => r.toShown)).toFixed(0)} ms; page errors ${errors.length}`);
if (errors.length) console.log(errors.join('\n'));
done(errors.length ? 1 : 0);

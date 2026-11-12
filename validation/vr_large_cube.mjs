// A large cube on the VR page (part of epic #166), in a headless Chrome on this
// machine's GPU against a local backend: the time from "Open and load" to the
// first reduced frame (the brick pyramid built first, then with it ready), the
// level the reduced cube is read from, and the refinement out of core:
// pose->frame and stop->refined, against the same numbers for an in-core cube.
//
//   node validation/vr_large_cube.mjs --url http://localhost:8766 --token-file FILE \
//        --cube /scratch/large_cube.fits [--baseline /path/hgc44_3axes.fits] [--sweeps 10]
//        [--thr cube|reduced] [--chrome PATH] [--out FILE.json]
//
// Needs the page's ?debug hooks (main.js). The token is read from a file and
// put only in the page's #fragment.
import { spawn } from 'node:child_process';
import { mkdtempSync, readFileSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const args = Object.fromEntries(process.argv.slice(2).reduce((a, v, i, all) => {
  if (v.startsWith('--')) a.push([v.slice(2), all[i + 1] && !all[i + 1].startsWith('--') ? all[i + 1] : true]);
  return a;
}, []));
const URL0 = args.url || 'http://localhost:8766';
const TOKEN = readFileSync(args['token-file'], 'utf8').trim();
const CHROME = args.chrome || '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
const SWEEPS = Number(args.sweeps || 10);
// The threshold the page starts from: 'cube' (the full cube's, as the desktop)
// or 'reduced' (for the binning in use; with coarse binning it lets the full
// cube's noise into the refined frames, which then cost more to draw and encode).
const THR = args.thr || 'cube';
const W = 1600, H = 1000;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function launch() {
  const port = 9300 + Math.floor(Math.random() * 500);
  const profile = mkdtempSync(join(tmpdir(), 'vrbig-'));
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

async function waitFor(c, expr, ms = 600000, every = 100) {
  const t0 = Date.now();
  for (;;) {
    const v = await c.evaluate(expr);
    if (v) return v;
    if (Date.now() - t0 > ms) throw new Error(`timeout waiting for ${expr}`);
    await sleep(every);
  }
}

const status = (c) => c.evaluate(`document.getElementById('load-status').textContent`);

/** Open and load `path`; the times (ms) to the first reduced frame and to the first refined one. */
async function load(c, path) {
  const before = await status(c);
  const t0 = Date.now();
  await c.evaluate(`(() => {
    const $ = (id) => document.getElementById(id);
    $('path').value = ${JSON.stringify(path)};
    $('governor').checked = false; $('governor').dispatchEvent(new Event('change'));
    $('thr-source').value = ${JSON.stringify(THR)}; $('thr-source').dispatchEvent(new Event('change'));
    $('load').click();
  })()`);
  let sawBuild = false;
  const progress = [];
  await waitFor(c, `(() => { const t = document.getElementById('load-status').textContent;
    return t !== ${JSON.stringify(before)} && /upload/.test(t) && /binned/.test(t) || /Failed/.test(t) ? t : ''; })()`,
  600000, 100);
  const firstFrame = Date.now() - t0;
  const text = await status(c);
  if (/Failed/.test(text)) throw new Error(text);
  sawBuild = /brick pyramid built/.test(text);
  await waitFor(c, `(() => { const r = window.__vrRefine.refiner; return r.status === 'refined' && r.weight >= 1; })()`, 600000);
  const firstRefined = Date.now() - t0;
  return { firstFrameMs: firstFrame, firstRefinedMs: firstRefined, status: text, builtPyramid: sawBuild, progress };
}

async function stillRefined(c) {
  await sleep(300);
  await waitFor(c, `(() => { const r = window.__vrRefine.refiner; return r.motion.state === 'still' && r.weight >= 1 && !!r.draw.shown; })()`, 60000);
  await sleep(300);
}

async function refineRun(c) {
  const view = { yaw: -0.5, pitch: 0.35, dist: 1.0, turn: 0 };
  await c.evaluate(`window.__vrRefine.refiner.stats.latency.length = 0`).catch(() => {});
  const sweeps = [];
  for (let i = 0; i < SWEEPS; i++) {
    await c.evaluate(`window.__vrRefine.setCam(${JSON.stringify(view)})`);
    await stillRefined(c);
    sweeps.push(await c.evaluate(`window.__vrRefine.sweep(${i % 2 ? -90 : 90}, 600)`));
    await sleep(400);
  }
  // Slow motion: poses streamed while turning at 8 deg/s, for pose->frame.
  const slow = await c.evaluate(`window.__vrRefine.sweep(8, 3000)`);
  return { sweeps, slow, summary: await c.evaluate('window.__vrRefine.refiner.summary()') };
}

const out = { url: URL0, canvas: [W, H], runs: {} };
const { proc, ws } = await launch();
const c = cdp(ws);
try {
  await c.send('Page.enable');
  await c.send('Runtime.enable');
  await c.send('Page.navigate', { url: `${URL0}/vr/?debug&refine=1#token=${encodeURIComponent(TOKEN)}` });
  await waitFor(c, `!!window.__vrRefine`, 20000);
  out.gpu = await c.evaluate(`(() => { const g = document.getElementById('gl').getContext('webgl2'); const d = g.getExtension('WEBGL_debug_renderer_info'); return d ? g.getParameter(d.UNMASKED_RENDERER_WEBGL) : g.getParameter(g.RENDERER); })()`);
  const cubes = [['large', args.cube], ...(args.baseline ? [['baseline', args.baseline]] : [])];
  for (const [label, path] of cubes) {
    const run = { path };
    run.first = await load(c, path);
    run.again = await load(c, path);             // the pyramid ready, the page's own session anew
    Object.assign(run, await refineRun(c));
    out.runs[label] = run;
    console.error(label, JSON.stringify({ first: run.first.firstFrameMs, again: run.again.firstFrameMs,
      refinedAgain: run.again.firstRefinedMs, latency: run.summary.latency, stop: run.summary.stopFirst,
      level: run.summary.outOfCoreLevel }));
  }
} finally {
  if (args.out) writeFileSync(args.out, JSON.stringify(out, null, 2));
  c.close();
  proc.kill();
}
console.log(JSON.stringify(out.runs, (k, v) => (k === 'sweeps' ? undefined : v), 1));

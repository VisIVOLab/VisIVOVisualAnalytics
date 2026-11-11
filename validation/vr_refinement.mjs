// Refinement measurements of the VR page (issue #170), in a headless Chrome
// on this machine's GPU against a local backend: what the page shows with and
// without the backend's frames, the backend's full-quality reference, the time
// from "view stopped" to "refined shown", the local frame-time cost of
// compositing, and how well a frame reprojects after the eye moved.
// Images and raw numbers go to validation/out/vr_refinement/; the metrics are
// computed by vr_refinement.py from them.
//
//   node validation/vr_refinement.mjs --url http://localhost:8766 --token-file FILE \
//        --cube /path/hgc44_3axes.fits [--chrome PATH] [--sweeps 15] [--binnings auto,bin2] [--out DIR]
//
// Needs the page's ?debug hooks (main.js). The token is read from a file and
// put only in the page's #fragment.
import { spawn } from 'node:child_process';
import { mkdirSync, readFileSync, writeFileSync, mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const args = Object.fromEntries(process.argv.slice(2).reduce((a, v, i, all) => {
  if (v.startsWith('--')) a.push([v.slice(2), all[i + 1] && !all[i + 1].startsWith('--') ? all[i + 1] : true]);
  return a;
}, []));
const URL0 = args.url || 'http://localhost:8766';
const TOKEN = readFileSync(args['token-file'], 'utf8').trim();
const CUBE = args.cube || '/Users/fvitello/Downloads/DemoData/hgc44_3axes.fits';
const CHROME = args.chrome || '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
const SWEEPS = Number(args.sweeps || 15);
const OUT = args.out || join(dirname(fileURLToPath(import.meta.url)), 'out', 'vr_refinement');
mkdirSync(OUT, { recursive: true });
const W = 1600, H = 1000;

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function launch() {
  const port = 9300 + Math.floor(Math.random() * 500);
  const profile = mkdtempSync(join(tmpdir(), 'vr170-'));
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
    if (process.env.VR_DEBUG && (m.method === 'Runtime.exceptionThrown' || m.method === 'Runtime.consoleAPICalled')) {
      console.error(JSON.stringify(m.params.exceptionDetails ? m.params.exceptionDetails.exception.description : m.params.args).slice(0, 3000));
    }
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
    if (process.env.VR_DEBUG) console.error(new Date().toISOString().slice(11, 23), 'eval', expr.replace(/\s+/g, ' ').slice(0, 90));
    const r = await send('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true });
    if (r.exceptionDetails) throw new Error(`page: ${r.exceptionDetails.exception?.description || r.exceptionDetails.text}`);
    return r.result.value;
  };
  return { send, evaluate, close: () => ws.close() };
}

function savePng(name, dataUrl) {
  writeFileSync(join(OUT, name), Buffer.from(dataUrl.split(',')[1], 'base64'));
}

async function waitFor(c, expr, ms = 120000, every = 200) {
  const t0 = Date.now();
  for (;;) {
    const v = await c.evaluate(expr);
    if (v) return v;
    if (process.env.VR_DEBUG && (Date.now() - t0) % 5000 < every) {
      console.error(await c.evaluate(`JSON.stringify({ load: document.getElementById('load-status').textContent, r: window.__vrRefine && window.__vrRefine.refiner.summary(), fs: window.__vrRefine && window.__vrRefine.stats(), det: window.__vrRefine && [window.__vrRefine.refiner.motion.degS, window.__vrRefine.refiner.motion.mS] })`));
    }
    if (Date.now() - t0 > ms) throw new Error(`timeout waiting for ${expr}`);
    await sleep(every);
  }
}

async function loadCube(c, fxy, fz, maxdim) {
  const before = await c.evaluate(`document.getElementById('load-status').textContent`);
  await c.evaluate(`(() => {
    const $ = (id) => document.getElementById(id);
    $('path').value = ${JSON.stringify(CUBE)};
    for (const [id, v] of [['fxy', '${fxy}'], ['fz', '${fz}'], ['maxdim', '${maxdim}']]) {
      const s = $(id);
      if (![...s.options].some((o) => o.value === v)) s.add(new Option(v, v));
      s.value = v;
    }
    $('governor').checked = false; $('governor').dispatchEvent(new Event('change'));
    $('load').click();
  })()`);
  // The new cube in place (a new load line), then refined.
  await waitFor(c, `(() => { const r = window.__vrRefine.refiner; const t = document.getElementById('load-status').textContent;
    return r.datasetId && t !== ${JSON.stringify(before)} && /upload/.test(t) && r.status === 'refined' && r.weight >= 1; })()`);
  return c.evaluate(`document.getElementById('load-status').textContent`);
}

async function stillRefined(c) {
  // A new still pose, and its refined frame fully faded in.
  await sleep(300);
  await waitFor(c, `(() => { const r = window.__vrRefine.refiner; return r.motion.state === 'still' && r.weight >= 1 && !!r.draw.shown; })()`, 30000);
  await sleep(300);
}

const out = { cube: CUBE, canvas: [W, H], runs: {} };
const { proc, ws } = await launch();
const c = cdp(ws);
try {
  await c.send('Page.enable');
  await c.send('Runtime.enable');
  await c.send('Page.navigate', { url: `${URL0}/vr/?debug&refine=1#token=${encodeURIComponent(TOKEN)}` });
  await waitFor(c, `!!window.__vrRefine`, 20000);
  out.gpu = await c.evaluate(`(() => { const g = document.getElementById('gl').getContext('webgl2'); const d = g.getExtension('WEBGL_debug_renderer_info'); return d ? g.getParameter(d.UNMASKED_RENDERER_WEBGL) : g.getParameter(g.RENDERER); })()`);
  out.ua = await c.evaluate('navigator.userAgent');

  const view = { yaw: -0.5, pitch: 0.35, dist: 1.0, turn: 0 };
  const binnings = [['auto', 0, 0, 256], ['bin2', 2, 1, 512]].filter(([l]) => !args.binnings || args.binnings.split(',').includes(l));   // 3x3x1 (auto) and 2x2x1; 1x1x1 is over the page's 64 Mi-voxel cap
  for (const [label, fxy, fz, maxdim] of binnings) {
    let status;
    try { status = await loadCube(c, fxy, fz, maxdim); }
    catch (err) { out.runs[label] = { error: String(err.message || err) }; continue; }
    const run = { status };
    await c.evaluate(`window.__vrRefine.setCam(${JSON.stringify(view)})`);
    await stillRefined(c);
    // ── Quality: the page alone, refined, and the backend's reference. ──
    const local = await c.evaluate(`window.__vrRefine.snapshot('local')`);
    savePng(`${label}_local.png`, local.png);
    // Refined as shown, at two settings: 0.6 x the canvas, JPEG 85, and the
    // default, full size, JPEG 95 (left in force for what follows).
    run.refinedDrew = {};
    for (const [scale, quality] of [[0.6, 85], [1, 95]]) {
      await c.evaluate(`(() => { const r = window.__vrRefine.refiner; r.o.scale = ${scale}; r.o.quality = ${quality}; r.lastSentA = null; })()`);
      await waitFor(c, `(() => { const r = window.__vrRefine.refiner; const s = r.draw.shown; return r.weight >= 1 && s && s.header.eyes[0].width === Math.round(document.getElementById('gl').width * ${scale}); })()`, 30000);
      const refined = await c.evaluate(`window.__vrRefine.snapshot('refined')`);
      savePng(`${label}_refined_s${scale}_q${quality}.png`, refined.png);
      run.refinedDrew[`${scale}`] = refined.drew;
    }
    await c.evaluate('window.__vrRefine.refiner.paused = true');
    const ref = await c.evaluate(`window.__vrRefine.reference({ quality: 100, sample_distance: 0.25 }, 1)`);
    savePng(`${label}_reference.png`, ref.png);
    run.referenceTiming = ref.header.timing_ms;
    const frame06 = await c.evaluate(`window.__vrRefine.reference({}, ${0.6})`);
    savePng(`${label}_frame_at_pose.png`, frame06.png);
    await c.evaluate('window.__vrRefine.refiner.paused = false');
    await stillRefined(c);

    // ── Local frame-time cost of compositing (refined shown, view still). ──
    const cost = {};
    for (const mode of ['off', 'auto', 'off', 'auto']) {
      await c.evaluate(`window.__vrRefine.setMode('${mode}')`);
      await sleep(400);
      await c.evaluate('window.__vrRefine.resetStats()');
      await sleep(3000);
      const s = await c.evaluate('window.__vrRefine.stats()');
      (cost[mode] ||= []).push({ gpu: s.gpu, cpu: s.cpu, fps: s.fps, n: s.n });
    }
    await c.evaluate(`window.__vrRefine.setMode('auto')`);
    run.cost = cost;

    // ── Stop to refined: turn at 90 deg/s for 0.6 s, then hold. ──
    const sweeps = [];
    for (let i = 0; i < SWEEPS; i++) {
      await c.evaluate(`window.__vrRefine.setCam(${JSON.stringify(view)})`);
      await stillRefined(c);
      sweeps.push(await c.evaluate(`window.__vrRefine.sweep(${i % 2 ? -90 : 90}, 600)`));
      await sleep(400);
    }
    run.sweeps = sweeps;
    run.summary = await c.evaluate('window.__vrRefine.refiner.summary()');

    // ── Reprojection: a frame for pose A drawn at pose B, against B's own. ──
    const rep = [];
    await c.evaluate(`window.__vrRefine.setCam(${JSON.stringify(view)})`);
    await stillRefined(c);
    await c.evaluate('window.__vrRefine.refiner.paused = true');
    const atA = await c.evaluate(`window.__vrRefine.reference({}, 1)`);
    // A's frame stays on show while the camera jumps to each B (a jump reads as
    // motion, which would fade it out).
    await c.evaluate('window.__vrRefine.refiner.frozen = true');
    savePng(`${label}_rep_A.png`, atA.png);
    const moves = [['orbit', 0.5], ['orbit', 1], ['orbit', 2], ['orbit', 4], ['turn', 2], ['turn', 5]];
    for (const [kind, deg] of moves) {
      const cam = { ...view };
      if (kind === 'orbit') cam.yaw += deg * Math.PI / 180;
      else cam.turn = deg * Math.PI / 180;
      const shots = await c.evaluate(`(() => {
        const R = window.__vrRefine; R.setCam(${JSON.stringify(cam)});
        const d = R.snapshot('refined', true), p = R.snapshot('refined', false), l = R.snapshot('local');
        R.setCam(${JSON.stringify(view)});
        return { d, p, l };
      })()`);
      const tag = `${label}_rep_${kind}${deg}`;
      savePng(`${tag}_depth.png`, shots.d.png);
      savePng(`${tag}_plane.png`, shots.p.png);
      savePng(`${tag}_local.png`, shots.l.png);
      await c.evaluate(`window.__vrRefine.setCam(${JSON.stringify(cam)})`);
      const fresh = await c.evaluate(`window.__vrRefine.reference({}, 1)`);
      savePng(`${tag}_fresh.png`, fresh.png);
      await c.evaluate(`window.__vrRefine.setCam(${JSON.stringify(view)})`);
      rep.push({ kind, deg, tag, drew: [shots.d.drew, shots.p.drew] });
    }
    run.reprojection = rep;
    await c.evaluate('window.__vrRefine.refiner.frozen = false; window.__vrRefine.refiner.paused = false');
    out.runs[label] = run;
  }
} finally {
  writeFileSync(join(OUT, 'raw.json'), JSON.stringify(out, null, 2));
  c.close();
  proc.kill();
}
console.log(JSON.stringify(Object.fromEntries(Object.entries(out.runs).map(([k, r]) => [k, r.error || r.status])), null, 1));

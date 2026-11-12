// Refined frames as JPEG over the WebSocket against H.264 over WebRTC (issue
// #189), in a headless Chrome on this machine's GPU against a local backend:
// stop -> refined, pose -> frame, the refined frame rate while the view moves
// slowly, the bandwidth of each, and the image against the backend's
// full-quality reference. Optionally with the page's network throttled
// through the DevTools protocol (Network.emulateNetworkConditions applies to
// the WebSocket only; the WebRTC video, over UDP, is not throttled).
//
//   node validation/vr_refine_webrtc.mjs --url http://localhost:8811 --token-file FILE \
//        --cube /path/hgc44_3axes.fits [--transports jpeg,webrtc,auto] \
//        [--throttle none,wifi40] [--window 1600x1000] [--sweeps 10] [--out FILE.json]
//        [--chrome-flags "flag1=a;flag2"]   (extra Chrome switches, without their dashes)
//        [--stripe-fallback]   (read the stripe through an image, not VideoFrame.copyTo)
//
// Throttle profiles: none; wifi200 (200 Mbit/s down, 3 ms); wifi15 (15 Mbit/s,
// 10 ms); wifi40 (40 Mbit/s, 8 ms). Needs the page's ?debug hooks (main.js).
// The token is read from a file and put only in the page's #fragment.
import { spawn } from 'node:child_process';
import { mkdtempSync, readFileSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const args = Object.fromEntries(process.argv.slice(2).reduce((a, v, i, all) => {
  if (v.startsWith('--')) a.push([v.slice(2), all[i + 1] && !all[i + 1].startsWith('--') ? all[i + 1] : true]);
  return a;
}, []));
const URL0 = args.url || 'http://localhost:8811';
const TOKEN = readFileSync(args['token-file'], 'utf8').trim();
const CUBE = args.cube || '/Users/fvitello/Downloads/DemoData/hgc44_3axes.fits';
const CHROME = args.chrome || '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
const SWEEPS = Number(args.sweeps || 10);
const [W, H] = (args.window || '1600x1000').split('x').map(Number);
const TRANSPORTS = (args.transports || 'jpeg,webrtc').split(',');
const PROFILES = {
  none: null,
  wifi200: { latency: 3, downloadThroughput: 200e6 / 8, uploadThroughput: 100e6 / 8 },
  wifi40: { latency: 8, downloadThroughput: 40e6 / 8, uploadThroughput: 20e6 / 8 },
  wifi15: { latency: 10, downloadThroughput: 15e6 / 8, uploadThroughput: 10e6 / 8 },
};
const THROTTLES = (args.throttle || 'none').split(',');

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function launch() {
  // A port no other Chrome answers on (a Chrome left over from an earlier run
  // would otherwise take the commands meant for this one).
  let port = null;
  for (const p of [8812, 8813, 8814, 8815, 8816, 8817, 8818]) {
    try { await fetch(`http://127.0.0.1:${p}/json/version`); } catch { port = p; break; }
  }
  if (port === null) throw new Error('no free debugging port in 8812-8818');
  const profile = mkdtempSync(join(tmpdir(), 'vr189-'));
  const proc = spawn(CHROME, ['--headless=new', '--use-angle=metal', `--remote-debugging-port=${port}`,
    `--user-data-dir=${profile}`, `--window-size=${W},${H}`, '--force-device-scale-factor=1',
    '--no-first-run', '--no-default-browser-check',
    ...(args['chrome-flags'] ? String(args['chrome-flags']).split(';').map((f) => `--${f}`) : []), 'about:blank'], { stdio: 'ignore' });
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
  const listeners = [];
  ws.addEventListener('message', (e) => {
    const m = JSON.parse(e.data);
    if (m.id && waiting.has(m.id)) { waiting.get(m.id)(m); waiting.delete(m.id); }
    if (m.method) for (const f of listeners) f(m);
    if (process.env.VR_DEBUG && (m.method === 'Runtime.exceptionThrown' || m.method === 'Runtime.consoleAPICalled')) {
      console.error(JSON.stringify(m.params.exceptionDetails ? m.params.exceptionDetails.exception.description : m.params.args).slice(0, 2000));
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
    const r = await send('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true });
    if (r.exceptionDetails) throw new Error(`page: ${r.exceptionDetails.exception?.description || r.exceptionDetails.text}`);
    return r.result.value;
  };
  return { send, evaluate, on: (f) => listeners.push(f), close: () => ws.close() };
}

async function waitFor(c, expr, ms = 120000, every = 200) {
  const t0 = Date.now();
  for (;;) {
    const v = await c.evaluate(expr);
    if (v) return v;
    if (Date.now() - t0 > ms) {
      const s = await c.evaluate('JSON.stringify(window.__vrRefine && window.__vrRefine.refiner.summary())');
      throw new Error(`timeout waiting for ${expr}: ${s}`);
    }
    await sleep(every);
  }
}

async function loadCube(c) {
  await c.evaluate(`(() => {
    const $ = (id) => document.getElementById(id);
    $('path').value = ${JSON.stringify(CUBE)};
    for (const [id, v] of [['fxy', '0'], ['fz', '0'], ['maxdim', '256']]) {
      const s = $(id);
      if (![...s.options].some((o) => o.value === v)) s.add(new Option(v, v));
      s.value = v;
    }
    $('governor').checked = false; $('governor').dispatchEvent(new Event('change'));
    $('load').click();
  })()`);
  await waitFor(c, `(() => { const r = window.__vrRefine.refiner; const t = document.getElementById('load-status').textContent;
    return r.datasetId && /upload/.test(t) && r.status === 'refined' && r.weight >= 1; })()`);
}

async function stillRefined(c) {
  await sleep(300);
  await waitFor(c, `(() => { const r = window.__vrRefine.refiner; return r.motion.state === 'still' && r.weight >= 1 && !!r.draw.shown; })()`, 30000);
  await sleep(300);
}

// PSNR of two PNG data URLs over the reference's emission (pixels more than
// 8 levels from the background in some channel), computed in the page.
const PSNR_FN = `async (a, b) => {
  const load = async (u) => { const im = new Image(); im.src = u; await im.decode();
    const c2 = Object.assign(document.createElement('canvas'), { width: im.width, height: im.height });
    const x = c2.getContext('2d'); x.drawImage(im, 0, 0); return x.getImageData(0, 0, im.width, im.height).data; };
  const [pa, pb] = await Promise.all([load(a), load(b)]);
  const bg = [pb[0], pb[1], pb[2]];
  let se = 0, n = 0; const bias = [0, 0, 0];
  for (let i = 0; i < pb.length; i += 4) {
    if (Math.max(Math.abs(pb[i] - bg[0]), Math.abs(pb[i + 1] - bg[1]), Math.abs(pb[i + 2] - bg[2])) <= 8) continue;
    for (let k = 0; k < 3; k++) { const d = pa[i + k] - pb[i + k]; se += d * d; bias[k] += d; }
    n += 3;
  }
  return { psnr: 10 * Math.log10(255 * 255 / (se / n)), pixels: n / 3, bias: bias.map((b) => +(3 * b / n).toFixed(2)) };
}`;

async function run(c, transport, throttle) {
  const wsBytes = { n: 0, on: false };
  c.on((m) => {
    if (wsBytes.on && m.method === 'Network.webSocketFrameReceived') {
      const r = m.params.response;
      wsBytes.n += r.opcode === 2 ? Math.floor(r.payloadData.length * 3 / 4) : r.payloadData.length;
    }
  });
  await c.send('Network.enable');
  await c.send('Network.emulateNetworkConditions', { offline: false, latency: 0, downloadThroughput: -1, uploadThroughput: -1 });
  await c.send('Page.navigate', { url: `${URL0}/vr/?debug&refine=1&refine_transport=${transport}#token=${encodeURIComponent(TOKEN)}` });
  await waitFor(c, `!!window.__vrRefine`, 20000);
  // --stripe-fallback: read the stripe through an image, as on a decoder whose
  // frames refuse copyTo (a headset's hardware decoder may).
  if (args['stripe-fallback']) await c.evaluate('window.__vrRefine.refiner.rtc._copyFails = true');
  const view = { yaw: -0.5, pitch: 0.35, dist: 1.0, turn: 0 };
  await c.evaluate(`window.__vrRefine.setCam(${JSON.stringify(view)})`);
  await loadCube(c);
  if (PROFILES[throttle]) await c.send('Network.emulateNetworkConditions', { offline: false, ...PROFILES[throttle] });
  const out = { transport, throttle, canvas: await c.evaluate(`[document.getElementById('gl').width, document.getElementById('gl').height]`) };
  if (transport === 'webrtc') {
    await waitFor(c, `(() => { const r = window.__vrRefine.refiner; return r.rtc.state === 'video'; })()`, 20000);
  }
  // The look and a first still frame on the transport chosen (auto: a few
  // slow turns first, so it can measure the JPEG frames).
  for (let i = 0; i < (transport === 'auto' ? 4 : 1); i++) {
    await c.evaluate(`window.__vrRefine.sweep(${i % 2 ? -90 : 90}, 600)`);
  }
  await stillRefined(c);
  out.chosen = await c.evaluate('window.__vrRefine.refiner.rtc.summary()');

  // ── stop -> refined ──
  await c.evaluate(`(() => { const s = window.__vrRefine.refiner.stats; for (const k of ['latency', 'network', 'decode', 'stopFirst', 'stopFull', 'upload']) s[k].v.length = 0; })()`);
  const sweeps = [];
  for (let i = 0; i < SWEEPS; i++) {
    await c.evaluate(`window.__vrRefine.setCam(${JSON.stringify(view)})`);
    await sleep(500);
    sweeps.push(await c.evaluate(`window.__vrRefine.sweep(${i % 2 ? -90 : 90}, 600)`));
  }
  const q = (v, p) => { const s = [...v].filter(Number.isFinite).sort((a, b) => a - b); return s.length ? s[Math.min(s.length - 1, Math.floor(p * s.length))] : null; };
  out.stopToRefined = { first: { p50: q(sweeps.map((s) => s.firstSinceStop), 0.5), p95: q(sweeps.map((s) => s.firstSinceStop), 0.95) },
    full: { p50: q(sweeps.map((s) => s.fullSinceStop), 0.5), p95: q(sweeps.map((s) => s.fullSinceStop), 0.95) },
    timeouts: sweeps.filter((s) => s.timeout).length };

  // ── slow orbit: refined frames per second, pose -> frame, bandwidth ──
  await c.evaluate(`window.__vrRefine.setCam(${JSON.stringify(view)})`);
  await stillRefined(c);
  await c.evaluate(`(() => { const s = window.__vrRefine.refiner.stats; for (const k of ['latency', 'network', 'decode', 'upload']) s[k].v.length = 0; s.arrivals.n = 100000; s.arrivals.v.length = 0; })()`);
  const rtcBytes = `(async () => { const pc = window.__vrRefine.refiner.rtc.pc; if (!pc) return 0; let b = 0;
    (await pc.getStats()).forEach((s) => { if (s.type === 'inbound-rtp' && s.kind === 'video') b = s.bytesReceived; }); return b; })()`;
  const rtc0 = await c.evaluate(rtcBytes);
  wsBytes.n = 0; wsBytes.on = true;
  const ORBIT_S = 6, DEG_S = 2.5;   // at 1 m: 4.4 cm/s, "slow" (poses asked continuously)
  const t0 = await c.evaluate('performance.now()');
  await c.evaluate(`new Promise((resolve) => { const R = window.__vrRefine; const t0 = performance.now(); let last = t0;
    const step = (t) => { const c = R.cam(); R.setCam({ yaw: c.yaw + ${DEG_S} * Math.PI / 180 * (t - last) / 1000 }); last = t;
      if (t - t0 < ${ORBIT_S * 1000}) requestAnimationFrame(step); else resolve(); };
    requestAnimationFrame(step); })`);
  const t1 = await c.evaluate('performance.now()');
  wsBytes.on = false;
  const rtc1 = await c.evaluate(rtcBytes);
  const slow = await c.evaluate(`(() => { const r = window.__vrRefine.refiner; const s = r.stats;
    return { arrivals: s.arrivals.v.filter((t) => t >= ${t0} && t <= ${t1}).length, latency: [...s.latency.v], decode: [...s.decode.v], upload: [...s.upload.v],
      motion: r.motion.state, server: s.lastServer, transport: r.rtc.summary(), page: window.__vrRefine.stats() }; })()`);
  const secs = (t1 - t0) / 1000;
  out.slowOrbit = { degPerS: DEG_S, seconds: +secs.toFixed(2), refinedFps: +(slow.arrivals / secs).toFixed(1),
    poseToFrame: { p50: q(slow.latency, 0.5), p95: q(slow.latency, 0.95), n: slow.latency.length },
    decodeMs: q(slow.decode, 0.5), uploadMs: q(slow.upload, 0.5),
    mbitPerS: { websocket: +(8 * wsBytes.n / secs / 1e6).toFixed(2), webrtc: +(8 * (rtc1 - rtc0) / secs / 1e6).toFixed(2) },
    kBPerFrame: +(((wsBytes.n + rtc1 - rtc0) / Math.max(1, slow.arrivals)) / 1024).toFixed(1),
    server: slow.server, transport: slow.transport, pageFps: slow.page && slow.page.fps };

  // ── image: shown refined frame against the full-quality reference, and
  //    against a JPEG of the very same pose (a pairing with the wrong pose
  //    would show here as a low PSNR) ──
  const views = [view, { ...view, yaw: view.yaw + 0.05 }, { ...view, yaw: view.yaw + 0.1, pitch: 0.3 }];
  out.image = [];
  let prevSame = null;          // the JPEG of the pose before: a wrong pairing would look like this
  for (const v of views) {
    await c.evaluate(`window.__vrRefine.setCam(${JSON.stringify(v)})`);
    await stillRefined(c);
    const shown = await c.evaluate(`window.__vrRefine.snapshot('refined')`);
    const header = await c.evaluate('window.__vrRefine.refiner.draw.shown.header');
    await c.evaluate('window.__vrRefine.refiner.paused = true');
    const ref = await c.evaluate(`window.__vrRefine.reference({ quality: 100, sample_distance: 0.25 }, 1)`);
    const same = await c.evaluate(`window.__vrRefine.reference({}, 1)`);
    await c.evaluate('window.__vrRefine.refiner.paused = false');
    const local = await c.evaluate(`window.__vrRefine.snapshot('local')`);
    // The reference images are the left eye as a PNG; snapshot is the canvas.
    const vsRef = await c.evaluate(`(${PSNR_FN})(${JSON.stringify(shown.png)}, ${JSON.stringify(ref.png)})`);
    const vsSame = await c.evaluate(`(${PSNR_FN})(${JSON.stringify(shown.png)}, ${JSON.stringify(same.png)})`);
    const vsPrev = prevSame && await c.evaluate(`(${PSNR_FN})(${JSON.stringify(shown.png)}, ${JSON.stringify(prevSame)})`);
    prevSame = same.png;
    const localVsRef = await c.evaluate(`(${PSNR_FN})(${JSON.stringify(local.png)}, ${JSON.stringify(ref.png)})`);
    if (args.save) {
      const k = `${transport}_${throttle}_${out.image.length}`;
      for (const [name, png] of [['shown', shown.png], ['reference', ref.png], ['same_pose_jpeg', same.png], ['local', local.png]]) {
        writeFileSync(join(args.save, `${k}_${name}.png`), Buffer.from(png.split(',')[1], 'base64'));
      }
    }
    out.image.push({ colorFormat: header.color.format, shownVsReference: vsRef.psnr, shownVsJpegSamePose: vsSame.psnr, shownVsJpegPoseBefore: vsPrev ? vsPrev.psnr : null, biasVsJpegSamePose: vsSame.bias,
      localVsReference: localVsRef.psnr, pixels: vsRef.pixels });
  }
  out.final = await c.evaluate('window.__vrRefine.refiner.summary()');
  return out;
}

const results = { cube: CUBE, window: [W, H], runs: [] };
const { proc, ws } = await launch();
const c = cdp(ws);
try {
  await c.send('Page.enable');
  await c.send('Runtime.enable');
  for (const throttle of THROTTLES) {
    for (const transport of TRANSPORTS) {
      try {
        // A run that hangs (seen once: a page that never loaded) is given up after 8 min.
        const r = await Promise.race([run(c, transport, throttle),
          sleep(8 * 60000).then(() => { throw new Error('run took over 8 min'); })]);
        results.runs.push(r);
        console.log(JSON.stringify({ transport, throttle, chosen: r.chosen.state, stop: r.stopToRefined, slow: { fps: r.slowOrbit.refinedFps, p2f: r.slowOrbit.poseToFrame, mbit: r.slowOrbit.mbitPerS, kB: r.slowOrbit.kBPerFrame }, image: r.image.map((i) => [i.colorFormat, +i.shownVsReference.toFixed(1), +i.shownVsJpegSamePose.toFixed(1)]) }));
      } catch (err) {
        results.runs.push({ transport, throttle, error: String(err.message || err) });
        console.log(JSON.stringify({ transport, throttle, error: String(err.message || err).slice(0, 600) }));
      }
      if (args.out) writeFileSync(args.out, JSON.stringify(results, null, 1));   // kept as it goes
    }
  }
  results.ua = await c.evaluate('navigator.userAgent');
} finally {
  c.close();
  proc.kill();
}
if (args.out) writeFileSync(args.out, JSON.stringify(results, null, 1));

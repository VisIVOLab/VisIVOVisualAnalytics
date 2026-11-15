// The headset's simulation controls in a real page (issue #259), headless:
// a series of three VBT snapshots on a local backend; the page opens the
// middle one and the checks press the menu as the headset would
// (window.__vrMenu.act): the next snapshot (columns and colour kept), another
// X column, splats sized by the smoothing length, the box selection (inside,
// outside, all: what is drawn changes), the colour bar's title.
//
//   VISIVO_TOKEN=... node validation/vr_sim_controls.mjs --python backend/.venv/bin/python
//     [--port 8885] [--cdp-port 9485] [--gl swiftshader|metal] [--chrome PATH]

import { spawn, spawnSync } from 'node:child_process';
import { mkdtempSync, openSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { randomBytes } from 'node:crypto';

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO = resolve(HERE, '..');
const args = Object.fromEntries(process.argv.slice(2).reduce((a, v, i, all) => {
  if (v.startsWith('--')) a.push([v.slice(2), all[i + 1] && !all[i + 1].startsWith('--') ? all[i + 1] : true]);
  return a;
}, []));
const PY = typeof args.python === 'string' ? resolve(args.python) : 'python3';
const PORT = Number(args.port || 8885);
const CDP = Number(args['cdp-port'] || 9485);
const GL = args.gl === 'metal' ? 'metal' : 'swiftshader';
const CHROME = (typeof args.chrome === 'string' && args.chrome) || process.env.CHROME || (process.platform === 'darwin'
  ? '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome' : 'google-chrome');
const BASE = `http://localhost:${PORT}`;
const TOKEN = randomBytes(18).toString('hex');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const OUT = mkdtempSync(join(tmpdir(), 'vr-sim-controls-'));

// Three snapshots: x, y, z, vx, hsml (a smoothing length), rho, zero (no
// positive value: no usable size); 20 000 rows each, each a little larger.
const made = spawnSync(PY, ['-c', `
import numpy as np, sys
out = sys.argv[1]
for k in (1, 2, 3):
    rng = np.random.default_rng(k)
    n = 20000
    x, y, z = rng.normal(0, 1, (3, n)) * (1 + 0.1 * k)
    vx = rng.normal(0, 100, n)
    h = np.exp(rng.normal(-2, 0.5, n))
    rho = np.exp(rng.normal(0, 1.5, n))
    np.stack([x, y, z, vx, h, rho, np.zeros(n)]).astype('<f4').tofile(f"{out}/run_{k:03d}.bin")
    open(f"{out}/run_{k:03d}.bin.head", "w").write(f"float\\n7\\n{n}\\nlittle\\nx\\ny\\nz\\nvx\\nhsml\\nrho\\nzero\\n")
from astropy.io import fits
fits.PrimaryHDU(np.random.default_rng(9).normal(0, 1, (16, 24, 24)).astype('<f4')).writeto(f"{out}/cube.fits")
`, OUT], { encoding: 'utf8' });
if (made.status !== 0) { console.log(`FAIL could not write the snapshots: ${made.stderr}`); process.exit(1); }

let failed = 0;
const check = (name, ok, detail = '') => { if (!ok) failed++; console.log(`${ok ? 'ok  ' : 'FAIL'} ${name}${detail ? ` — ${detail}` : ''}`); };

const backend = spawn(PY, ['-m', 'uvicorn', 'app.main:app', '--host', 'localhost', '--port', String(PORT), '--log-level', 'warning'],
  { cwd: join(REPO, 'backend'), env: { ...process.env, VISIVO_TOKEN: TOKEN, PYTHONUNBUFFERED: '1' },
    stdio: ['ignore', openSync(join(OUT, 'backend.log'), 'w'), openSync(join(OUT, 'backend.log'), 'a')] });
const gl = GL === 'metal' ? ['--use-angle=metal'] : ['--use-angle=swiftshader', '--enable-unsafe-swiftshader', '--use-gl=angle'];
let chrome = null;
try {
  for (let i = 0; i < 600; i++) {
    try { if ((await fetch(`${BASE}/v1/ping`, { signal: AbortSignal.timeout(2000) })).ok) break; } catch { /* not yet */ }
    await sleep(100);
  }
  chrome = spawn(CHROME, ['--headless=new', ...gl, `--remote-debugging-port=${CDP}`, `--user-data-dir=${mkdtempSync(join(tmpdir(), 'vr-sim-chrome-'))}`,
    '--window-size=1400,900', '--force-device-scale-factor=1', '--no-first-run', '--disable-background-timer-throttling',
    '--disable-renderer-backgrounding', ...(process.platform === 'linux' ? ['--no-sandbox'] : []), 'about:blank'], { stdio: 'ignore' });
  let ws = null;
  for (let i = 0; i < 300 && !ws; i++) {
    try { ws = ((await (await fetch(`http://127.0.0.1:${CDP}/json/list`)).json()).find((t) => t.type === 'page') || {}).webSocketDebuggerUrl; } catch { /* */ }
    if (!ws) await sleep(100);
  }
  const sock = new WebSocket(ws);
  await new Promise((r) => (sock.readyState === 1 ? r() : sock.addEventListener('open', r)));
  let id = 0;
  const waiting = new Map();
  const pageErrors = [];
  sock.addEventListener('message', (e) => {
    const m = JSON.parse(e.data);
    if (m.id && waiting.has(m.id)) { waiting.get(m.id)(m); waiting.delete(m.id); }
    if (m.method === 'Runtime.exceptionThrown') pageErrors.push(String(m.params.exceptionDetails.exception?.description || m.params.exceptionDetails.text).slice(0, 200));
  });
  const send = (method, params = {}) => new Promise((r) => { const i = ++id; waiting.set(i, r); sock.send(JSON.stringify({ id: i, method, params })); });
  const ev = async (expr) => {
    const m = await send('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true });
    if (m.result && m.result.exceptionDetails) throw new Error(m.result.exceptionDetails.exception?.description || 'evaluate failed');
    return m.result ? m.result.result.value : undefined;
  };
  const waitFor = async (expr, ms = 30000) => {
    const t0 = Date.now();
    while (Date.now() - t0 < ms) { try { if (await ev(expr)) return true; } catch { /* not yet */ } await sleep(100); }
    return false;
  };
  await send('Page.enable');
  await send('Runtime.enable');
  await send('Page.navigate', { url: `${BASE}/vr/?debug&refine=0#token=${TOKEN}` });
  check('page up', await waitFor(`!!window.__vrShell && window.__vrShell.life.state === 'ok'`));
  const L = 'window.__vrLayers.state';
  await ev(`(() => { const $ = (id) => document.getElementById(id); $('path').value = ${JSON.stringify(join(OUT, 'run_002.bin.head'))}; $('load').click(); })()`);
  check('snapshot 2 of 3 open', await waitFor(`!!(${L}.points && ${L}.points.sample)`, 60000));
  check('the series is known: 3 snapshots, this one second', await waitFor(`!!(${L}.points.series && ${L}.points.series.count === 3 && ${L}.points.series.index === 1)`));
  const act = (a) => ev(`window.__vrMenu.act(${JSON.stringify(a)})`);
  // The Data page from Display.
  await act({ type: 'press', id: 'tab:display' });
  await act({ type: 'press', id: 'pt-data' });
  check('Data ▸ opens the Data page', (await ev('window.__vrMenu.ui.page')) === 'data');
  const colourBefore = await ev(`${L}.s.colourField`);
  // Another X column (x → y), the same table.
  await act({ type: 'press', id: 'axis:0:next' });
  check('X: the next column', await waitFor(`${L}.points.axes[0] === 'y' && ${L}.points.sample.axes[0] === 'y'`), await ev(`${L}.points.sample.axes.join(',')`));
  // Splats sized by hsml.
  await act({ type: 'press', id: 'tab:display' });
  await act({ type: 'press', id: 'pt-style:splats' });
  if (await ev(`${L}.s.box`)) await act({ type: 'press', id: 'pt-box' });   // the particles alone in the picture
  await act({ type: 'press', id: 'pt-data' });
  check('the smoothing length is found', (await ev(`${L}.s.sizeField`)) === 'hsml');
  await act({ type: 'press', id: 'size:field' });
  check('splats by hsml: the sample carries the sizes', await waitFor(`!!(${L}.s.sizeOn && ${L}.points.size && ${L}.points.size.field === 'hsml' && ${L}.points.arr.size)`),
    JSON.stringify(await ev(`${L}.points.size`)));
  await sleep(400);
  const sized = await ev('window.__vrDrawnFraction()');
  check('sized splats are drawn', sized.fraction > 0.001, `${(100 * sized.fraction).toFixed(2)} %`);
  await act({ type: 'press', id: 'size:uniform' });
  await waitFor(`!${L}.points.size`);
  await sleep(400);
  const uniform = await ev('window.__vrDrawnFraction()');
  check('the sizes change the picture (uniform against by hsml)', Math.abs(uniform.fraction - sized.fraction) > 0.002,
    `uniform ${(100 * uniform.fraction).toFixed(2)} %, by hsml ${(100 * sized.fraction).toFixed(2)} %`);
  await act({ type: 'press', id: 'size:field' });
  await waitFor(`!!${L}.points.size`);
  // A size column with no positive value: uniform splats and a note, the table still drawn.
  await ev(`${L}.s.sizeField = 'zero'`);
  await act({ type: 'press', id: 'size:uniform' });
  await act({ type: 'press', id: 'size:field' });
  check('a size column with no positive value: uniform splats, a note, the column passed over next time',
    await waitFor(`!!(${L}.points.sample && ${L}.points.sizeNote && !${L}.points.size && ${L}.badSize.has('zero'))`), await ev(`${L}.points.sizeNote`));
  await ev(`${L}.s.sizeField = 'hsml'`);
  await act({ type: 'press', id: 'size:uniform' });
  await act({ type: 'press', id: 'size:field' });
  await waitFor(`!!${L}.points.size`);
  // Two quick presses (Previous then Next): the second goes on from the first's
  // target; the step overtaken installs nothing and closes what it opened.
  const firstId = await ev(`${L}.points.datasetId`);
  await act({ type: 'press', id: 'snap-prev' });
  await act({ type: 'press', id: 'snap-next' });
  check('Previous then Next at once: back on snapshot 2, the newest step wins', await waitFor(`${L}.stepTarget === null && ${L}.points.series.index === 1
    && /run_002/.test(${L}.points.path || '') && ${L}.points.datasetId !== ${JSON.stringify(firstId)} && !!${L}.points.sample`, 60000),
    await ev(`${L}.points.path + ' #' + ${L}.points.series.index`));
  const closed = await waitFor(`window.__vrBackend()._json('POST', '/v1/points/info', { dataset_id: ${JSON.stringify(firstId)} }).then(() => false, () => true)`, 10000);
  check('the snapshot left behind is closed on the backend (DELETE)', closed);
  // The next snapshot (larger: other bounds) keeps the columns, the colour, the sizes, and the crop in table units.
  const cropData = { lo: 0.25, hi: 0.75 };
  await ev(`window.__vrState.clip = { min: [${cropData.lo}, 0, 0], max: [${cropData.hi}, 1, 1] }`);
  const b0 = await ev(`${L}.points.sample.bounds`);
  const wantLo = b0[0] + cropData.lo * (b0[1] - b0[0]), wantHi = b0[0] + cropData.hi * (b0[1] - b0[0]);
  await act({ type: 'press', id: 'snap-next' });
  check('Next ▸: snapshot 3', await waitFor(`${L}.points.series && ${L}.points.series.index === 2 && /run_003/.test(window.__vrState.openPath || '')`, 60000));
  check('the next snapshot keeps X, the colour column and the sizes', await waitFor(`${L}.points.sample && ${L}.points.axes[0] === 'y'
    && ${L}.s.colourField === ${JSON.stringify(colourBefore)} && !!${L}.points.size`));
  const b1 = await ev(`${L}.points.sample.bounds`);
  const clip1 = await ev('window.__vrState.clip');
  const gotLo = b1[0] + clip1.min[0] * (b1[1] - b1[0]), gotHi = b1[0] + clip1.max[0] * (b1[1] - b1[0]);
  check('the crop keeps its place in table units on a snapshot with other bounds', (b1[0] !== b0[0] || b1[1] !== b0[1]) && Math.abs(gotLo - wantLo) < 1e-3 * (b0[1] - b0[0]) && Math.abs(gotHi - wantHi) < 1e-3 * (b0[1] - b0[0]),
    `x ${wantLo.toFixed(4)}..${wantHi.toFixed(4)} -> ${gotLo.toFixed(4)}..${gotHi.toFixed(4)}; bounds x ${b0[0].toFixed(3)}..${b0[1].toFixed(3)} -> ${b1[0].toFixed(3)}..${b1[1].toFixed(3)}, crop ${clip1.min[0].toFixed(3)}..${clip1.max[0].toFixed(3)}`);
  await ev('window.__vrState.clip = { min: [0, 0, 0], max: [1, 1, 1] }');
  // The box selection.
  await act({ type: 'press', id: 'tab:select' });
  await ev(`window.__vrLayers.setSelBox({ min: [0, 0, 0], max: [0.5, 1, 1] })`);
  await act({ type: 'press', id: 'psel-inside' });
  await sleep(400);
  const inside = await ev('window.__vrDrawnFraction()');
  await act({ type: 'press', id: 'psel-outside' });
  await sleep(400);
  const outside = await ev('window.__vrDrawnFraction()');
  await act({ type: 'press', id: 'psel-all' });
  await sleep(400);
  const all = await ev('window.__vrDrawnFraction()');
  check('Show inside, Hide inside, Show all: each draws part of it', inside.fraction > 0 && outside.fraction > 0 && all.fraction >= Math.max(inside.fraction, outside.fraction) * 0.95,
    `inside ${(100 * inside.fraction).toFixed(2)} %, outside ${(100 * outside.fraction).toFixed(2)} %, all ${(100 * all.fraction).toFixed(2)} %`);
  await act({ type: 'press', id: 'undo' });
  check('Undo takes back the last selection press', (await ev(`${L}.s.selMode`)) === 'outside');
  await act({ type: 'press', id: 'sel-clear' });
  check('Clear drops the box', (await ev(`${L}.s.selBox`)) === null);
  const bar = await ev('window.__vrLayers.colourBar()');
  check('the colour bar names its column and says when the file has no unit', !!bar && /no unit in the file/.test(bar.title), bar && bar.title);

  // A crop and an inside-only box wholly outside the next snapshot: nothing
  // drawn and said, never the whole table; back on a snapshot that holds them,
  // they are there again (they are kept in table units).
  const xRange = (bb, c) => [bb[0] + c.min[0] * (bb[1] - bb[0]), bb[0] + c.max[0] * (bb[1] - bb[0])];
  await ev(`window.__vrState.clip = { min: [0.995, 0, 0], max: [1, 1, 1] }`);
  await ev(`window.__vrLayers.setSelBox({ min: [0.995, 0, 0], max: [1, 1, 1] })`);
  await act({ type: 'press', id: 'psel-inside' });
  const edge = xRange(await ev(`${L}.points.sample.bounds`), await ev('window.__vrState.clip'));
  await act({ type: 'press', id: 'snap-prev' });
  check('a crop outside the next snapshot: nothing drawn, said, not the whole table', await waitFor(`${L}.points.series.index === 1 && ${L}.stepTarget === null
    && !!${L}.cropEmpty && !!${L}.selEmpty && ${L}.s.selMode === 'inside'`, 60000),
    JSON.stringify(await ev(`({ crop: ${L}.cropEmpty, sel: ${L}.selEmpty, mode: ${L}.s.selMode })`)));
  await sleep(400);
  const empty = await ev('window.__vrDrawnFraction()');
  const notes = await ev(`window.__vrLayers.menuView().status.map((x) => x[0]).join(' | ')`);
  check('… the picture is empty and the menu says why', empty.fraction === 0 && /outside this snapshot/.test(notes), `${(100 * empty.fraction).toFixed(2)} %; ${notes}`);
  await act({ type: 'press', id: 'snap-next' });
  await waitFor(`${L}.points.series.index === 2 && ${L}.stepTarget === null && !!${L}.points.sample`, 60000);
  const back = xRange(await ev(`${L}.points.sample.bounds`), await ev('window.__vrState.clip'));
  check('… and back on a snapshot that holds them, the same crop again', !(await ev(`${L}.cropEmpty`)) && Math.abs(back[0] - edge[0]) < 1e-3 && Math.abs(back[1] - edge[1]) < 1e-3,
    `${edge.map((v) => v.toFixed(4))} -> ${back.map((v) => v.toFixed(4))}`);
  await act({ type: 'press', id: 'crop-reset' });
  await act({ type: 'press', id: 'sel-clear' });
  // Two quick presses (Previous, Previous): the crop comes from the snapshot on
  // show when they began, not from the one half loaded between them.
  await ev(`window.__vrState.clip = { min: [0.3, 0, 0], max: [0.6, 1, 1] }`);
  const want = xRange(await ev(`${L}.points.sample.bounds`), await ev('window.__vrState.clip'));
  await act({ type: 'press', id: 'snap-prev' });
  await act({ type: 'press', id: 'snap-prev' });
  await waitFor(`${L}.points.series.index === 0 && ${L}.stepTarget === null && !!${L}.points.sample && /run_001/.test(${L}.points.path)`, 60000);
  const got = xRange(await ev(`${L}.points.sample.bounds`), await ev('window.__vrState.clip'));
  check('quick steps keep the crop of the snapshot on show when they began', Math.abs(got[0] - want[0]) < 1e-3 && Math.abs(got[1] - want[1]) < 1e-3,
    `${want.map((v) => v.toFixed(4))} -> ${got.map((v) => v.toFixed(4))}`);
  // One colour (none) stays one colour on the next snapshot.
  await act({ type: 'press', id: 'tab:display' });
  await act({ type: 'press', id: 'pt-field:' });
  await waitFor(`${L}.s.colourField === null && ${L}.points.sample && !${L}.points.sample.colourField`);
  await act({ type: 'press', id: 'snap-next' });
  await waitFor(`${L}.points.series.index === 1 && ${L}.stepTarget === null && !!${L}.points.sample`, 60000);
  check('one colour (none) is kept on the next snapshot', (await ev(`${L}.s.colourField`)) === null);
  // A cube opened while a step is under way: the cube stays, the step installs
  // nothing. The step's table info is held back 4 s, so it ends after the cube.
  await ev(`(() => { const b = window.__vrBackend(); const orig = b._json.bind(b);
    b._json = (m, p, body) => (p === '/v1/points/info' ? new Promise((r) => setTimeout(r, 4000)).then(() => orig(m, p, body)) : orig(m, p, body)); })()`);
  await act({ type: 'press', id: 'snap-next' });
  await ev(`(() => { const $ = (id) => document.getElementById(id); $('path').value = ${JSON.stringify(join(OUT, 'cube.fits'))}; $('load').click(); })()`);
  await waitFor('!!(window.__vrState.cube && window.__vrState.open)', 60000);
  await sleep(6000);
  check('a cube opened during a step stays on show', await ev(`!!window.__vrState.cube && window.__vrLayers.mode === 'cube' && !${L}.points`),
    await ev(`window.__vrLayers.mode + ' ' + !!window.__vrState.cube`));
  check('no page error', !pageErrors.length, pageErrors.join(' / '));
  sock.close();
} catch (err) {
  check('run', false, err.message);
} finally {
  if (chrome) chrome.kill('SIGKILL');
  backend.kill('SIGTERM');
}
console.log(failed ? `${failed} checks failed (logs in ${OUT})` : 'all checks passed');
process.exit(failed ? 1 : 0);

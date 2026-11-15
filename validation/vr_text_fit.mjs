// Every text the VR page draws on its canvases fits its box (issue #276): the
// hand menu (every tab, data kind and state, every widget hovered for its
// tooltip), the tool palette and its tooltips, the wrist cards, the toasts,
// the notices (lifecycle and room cards, the guide), the result windows'
// bars, products and selection panel, the readout. The drawing runs in a
// headless Chrome with the browser's own fonts (validation/vr_text_fit_probe.js);
// a text wider than its box, a text over a widget, two texts or widgets
// overlapping, a cut button label or a text past the canvas edge fails.
//
//   node validation/vr_text_fit.mjs [--chrome PATH] [--port 8893] [--cdp-port N] [--out DIR] [--font FAMILY] [--all] [--ellipses]
//
// --font draws the page's sans-serif text in FAMILY instead (another
// machine's font: Roboto as on the Quest, DejaVu Sans as on a Linux runner);
// --all lists every problem; --ellipses lists the texts cut with an ellipsis;
// --snap REGEX writes the canvases whose case name matches as PNG files.
//
// Standalone it serves backend/app/static/vr itself (no backend, no cube):
// a few seconds. vr_lifecycle.mjs --regressions (CI) runs the same checks in
// its page with the cube loaded, the menu then also with the page's own values
// (runTextFitChecks). Exit code 0 when everything fits.
import { spawn } from 'node:child_process';
import { createServer } from 'node:http';
import { readFileSync, existsSync, mkdtempSync, mkdirSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, dirname, resolve, extname, normalize } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO = resolve(HERE, '..');
const STATIC = join(REPO, 'backend', 'app', 'static', 'vr');
export const PROBE = readFileSync(join(HERE, 'vr_text_fit_probe.js'), 'utf8');

/** The problems as lines (at most `n`), grouped by kind. */
export function summarise(report, n = 40) {
  // The same problem on the same canvas under every hover state: once, with its count.
  const groups = new Map();
  for (const p of report.problems) {
    const key = `${p.kind}|${p.text}|${p.name.replace(/ hover=\S+/, '')}`;
    const g = groups.get(key);
    if (g) g.n++; else groups.set(key, { p, n: 1 });
  }
  const lines = [];
  for (const { p, n: k } of [...groups.values()].slice(0, n)) {
    lines.push(`  ${p.kind}: ${p.name} — "${String(p.text).slice(0, 80)}" (${p.detail})${k > 1 ? ` ×${k}` : ''}`);
  }
  if (groups.size > n) lines.push(`  … and ${groups.size - n} more`);
  return lines;
}

/**
 * The checks in a page of the VR page's origin (the lifecycle run's): `c`
 * its DevTools client ({evaluate}), `check(name, ok, detail)`. `live`: the
 * menu also drawn with the page's own values (window.__vrMenu).
 */
export async function runTextFitChecks({ c, check, live = true, results = {} }) {
  await c.evaluate(PROBE);
  const base = await c.evaluate(`(() => { const s = document.querySelector('script[type=module][src*="main.js"]');
    return s ? new URL('./', s.src).href : new URL('js/', location.href).href; })()`);
  const t0 = Date.now();
  const r = await c.evaluate(`window.__vrTextFit({ base: ${JSON.stringify(base)}, live: ${!!live} })`, 120000);
  results.textFit = { cases: r.cases, texts: r.texts, problems: r.problems.length, ms: Date.now() - t0, perCanvas: r.perCanvas };
  check('text fit: the check finds a label too wide, overlapping texts and widgets, a text past the edge', r.selfTest.ok, r.selfTest.kinds.join(', '));
  check(`text fit: ${r.texts} texts on ${r.cases} canvases (${Object.keys(r.perCanvas).join(', ')}) inside their boxes, no overlap`,
    r.problems.length === 0, r.problems.length ? `${r.problems.length} problems; first: ${summarise(r, 3).join(' | ')}` : `${Date.now() - t0} ms`);
  return r;
}

// ── Standalone ──────────────────────────────────────────────────────────────
const isMain = process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url);
if (isMain) {
  const args = Object.fromEntries(process.argv.slice(2).reduce((a, v, i, all) => {
    if (v.startsWith('--')) a.push([v.slice(2), all[i + 1] && !all[i + 1].startsWith('--') ? all[i + 1] : true]);
    return a;
  }, []));
  const CHROME = (typeof args.chrome === 'string' && args.chrome) || process.env.CHROME || (process.platform === 'darwin'
    ? '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome' : 'google-chrome');
  const PORT = Number(args.port || 8893);
  const CDP = Number(args['cdp-port']) || 9500 + Math.floor(Math.random() * 400);
  const OUT = typeof args.out === 'string' ? args.out : join(HERE, 'out', 'vr_text_fit');
  mkdirSync(OUT, { recursive: true });
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  const TYPES = { '.js': 'text/javascript', '.html': 'text/html', '.css': 'text/css', '.json': 'application/json', '.glb': 'model/gltf-binary' };
  const server = createServer((req, res) => {
    const url = new URL(req.url, 'http://x');
    if (url.pathname === '/probe.html') { res.writeHead(200, { 'Content-Type': 'text/html' }); res.end('<!doctype html><meta charset="utf-8"><title>text fit</title>'); return; }
    const rel = normalize(decodeURIComponent(url.pathname)).replace(/^\/vr\//, '');
    const file = join(STATIC, rel);
    if (!file.startsWith(STATIC) || !existsSync(file)) { res.writeHead(404); res.end(); return; }
    res.writeHead(200, { 'Content-Type': TYPES[extname(file)] || 'application/octet-stream' });
    res.end(readFileSync(file));
  });
  await new Promise((r) => server.listen(PORT, '127.0.0.1', r));
  const profile = mkdtempSync(join(tmpdir(), 'vr-text-fit-'));
  const chrome = spawn(CHROME, ['--headless=new', `--remote-debugging-port=${CDP}`, `--user-data-dir=${profile}`, '--force-device-scale-factor=1',
    '--no-first-run', '--no-default-browser-check', ...(process.platform === 'linux' ? ['--no-sandbox'] : []), 'about:blank'], { stdio: 'ignore' });
  let code = 1;
  try {
    let ws = null;
    for (let i = 0; i < 300 && !ws; i++) {
      try {
        const list = await (await fetch(`http://127.0.0.1:${CDP}/json/list`, { signal: AbortSignal.timeout(1000) })).json();
        ws = (list.find((t) => t.type === 'page') || {}).webSocketDebuggerUrl;
      } catch { /* not up yet */ }
      if (!ws) await sleep(100);
    }
    if (!ws) throw new Error('Chrome did not expose DevTools');
    const sock = new WebSocket(ws);
    await new Promise((r) => (sock.readyState === 1 ? r() : sock.addEventListener('open', r)));
    let id = 0;
    const waiting = new Map();
    sock.addEventListener('message', (e) => { const m = JSON.parse(e.data); if (m.id && waiting.has(m.id)) { waiting.get(m.id)(m); waiting.delete(m.id); } });
    const send = (method, params = {}) => new Promise((r) => { const i = ++id; waiting.set(i, r); sock.send(JSON.stringify({ id: i, method, params })); });
    const c = {
      async evaluate(expr) {
        const m = await send('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true });
        if (m.error) throw new Error(m.error.message);
        if (m.result.exceptionDetails) throw new Error(m.result.exceptionDetails.exception?.description || m.result.exceptionDetails.text);
        return m.result.result.value;
      },
    };
    await send('Page.enable');
    await send('Page.navigate', { url: `http://127.0.0.1:${PORT}/probe.html` });
    for (let i = 0; i < 100; i++) { try { if (await c.evaluate('document.readyState') === 'complete') break; } catch { /* navigating */ } await sleep(50); }
    let failed = 0;
    const check = (name, ok, detail = '') => { if (!ok) failed++; console.log(`${ok ? 'ok  ' : 'FAIL'} ${name}${detail ? ` — ${detail}` : ''}`); };
    const t0 = Date.now();
    await c.evaluate(PROBE);
    const r = await c.evaluate(`window.__vrTextFit({ base: 'http://127.0.0.1:${PORT}/vr/js/', font: ${JSON.stringify(typeof args.font === 'string' ? args.font : '')},
      snap: ${JSON.stringify(typeof args.snap === 'string' ? args.snap : '')} })`);
    // --snap REGEX: the matching canvases as PNG files, to look at.
    r.snaps.forEach((s, i) => {
      const file = join(OUT, `snap_${String(i).padStart(2, '0')}_${s.name.replace(/[^a-z0-9]+/gi, '_').slice(0, 60)}.png`);
      writeFileSync(file, Buffer.from(s.url.split(',')[1], 'base64'));
      console.log(`  snap: ${file}`);
    });
    delete r.snaps;
    check('text fit: the check finds a label too wide, overlapping texts and widgets, a text past the edge', r.selfTest.ok, r.selfTest.kinds.join(', '));
    check(`text fit: ${r.texts} texts on ${r.cases} canvases inside their boxes, no overlap`, r.problems.length === 0,
      `${r.problems.length} problems, ${Date.now() - t0} ms; ${JSON.stringify(r.perCanvas)}`);
    for (const l of summarise(r, args.all ? 100000 : 60)) console.log(l);
    if (args.ellipses) for (const e of r.ellipses) console.log(`  ellipsis: ${e}`);
    writeFileSync(join(OUT, 'text_fit.json'), JSON.stringify(r, null, 1));
    sock.close();
    code = failed ? 1 : 0;
  } catch (err) {
    console.log(`FAIL run — ${err.message}`);
  } finally {
    chrome.kill('SIGKILL');
    server.close();
  }
  process.exit(code);
}

// A cube whose brick pyramid build outlasts the page's request deadline
// (issue #329), on the VR page in a headless Chrome. The backend's answers to
// reduced_info are replaced over DevTools (Fetch domain) while the "build"
// runs: 202 with a growing fraction, one poll whose connection is reset, one
// poll that never answers (it ends at the page's poll deadline, shortened with
// ?polltimeout), then the real answer. The page must show "Preparing … N %"
// the whole time and load the cube after, both as the headset app launched by
// the desktop (its room in VR) and as the page loading a path. A first request
// that cannot reach the backend still ends the load with a clear message.
//
// Run by vr_lifecycle.mjs (--pyramid-wait alone, part of --xrfake and
// --regressions): runPyramidWaitChecks({ c, check, waitFor, api, base, token, cube }).

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
/** The page's poll deadline in these checks (ms). */
const POLL_MS = 1500;

const b64 = (s) => Buffer.from(s, 'utf8').toString('base64');

/**
 * Answers the page's reduced_info requests while `plan` says the build runs:
 * plan(n, elapsedMs) is 'building' (a 202 with the fraction), 'reset' (the
 * connection reset), 'hang' (no answer: let go only at the end), or 'real'
 * (sent to the backend). Returns {stop(), seen} where seen lists what each
 * request got.
 */
function fakeBuild(c, plan) {
  const seen = [];
  const held = [];
  let n = 0;
  let t0 = 0;
  let running = true;
  const loop = (async () => {
    while (running) {
      const p = c.paused.shift();
      if (!p) { await sleep(20); continue; }
      if (!/\/v1\/cube\/reduced_info/.test(p.request.url)) {
        await c.send('Fetch.continueRequest', { requestId: p.requestId }).catch(() => {});
        continue;
      }
      if (!t0) t0 = Date.now();
      const what = plan(n, Date.now() - t0);
      const fraction = Math.min(0.95, 0.07 * n);
      n++;
      seen.push(what);
      if (what === 'building') {
        await c.send('Fetch.fulfillRequest', { requestId: p.requestId, responseCode: 202,
          responseHeaders: [{ name: 'Content-Type', value: 'application/json' }],
          body: b64(JSON.stringify({ valid: false, preparing: true, progress: fraction, error: 'Building the cube\'s brick pyramid.' })) }).catch(() => {});
      } else if (what === 'reset') {
        await c.send('Fetch.failRequest', { requestId: p.requestId, errorReason: 'ConnectionReset' }).catch(() => {});
      } else if (what === 'refused') {
        await c.send('Fetch.failRequest', { requestId: p.requestId, errorReason: 'ConnectionRefused' }).catch(() => {});
      } else if (what === 'hang') {
        held.push(p.requestId);
      } else {
        await c.send('Fetch.continueRequest', { requestId: p.requestId }).catch(() => {});
      }
    }
  })();
  return {
    seen,
    elapsed: () => (t0 ? Date.now() - t0 : 0),
    async stop() {
      running = false;
      await loop;
      // The page gave up on a held request long ago (its deadline): let it go.
      for (const id of held) await c.send('Fetch.failRequest', { requestId: id, errorReason: 'Aborted' }).catch(() => {});
      for (const p of c.paused.splice(0)) await c.send('Fetch.continueRequest', { requestId: p.requestId }).catch(() => {});
    },
  };
}

/** The build's script: the first request and the polls 202; requests 3 and 4
 *  reset (a poll and its one resend: the page asks again by itself), request
 *  6 never answered (it ends at the page's deadline), then 202 until the wait
 *  is three poll deadlines long; then the backend's own answer. */
const BUILD = (n, ms) => {
  if (n === 3 || n === 4) return 'reset';
  if (n === 6) return 'hang';
  return ms < 3 * POLL_MS || n < 9 ? 'building' : 'real';
};

/** Requests paused before a run are let go, never dropped (a dropped one
 *  would hang its load: the first request has no deadline). */
async function release(c) {
  for (const p of c.paused.splice(0)) await c.send('Fetch.continueRequest', { requestId: p.requestId }).catch(() => {});
}

/** Samples, every 150 ms until `done` or `ms`: the status line, the notice on
 *  show (the app's room), whether the cube is on show. */
async function watch(c, done, ms) {
  const samples = [];
  const t0 = Date.now();
  while (Date.now() - t0 < ms) {
    const s = await c.evaluate(`(() => { const n = window.__vrShell.notices.current(); const S = window.__vrState;
      return { status: document.getElementById('load-status').textContent, notice: n ? n.id + ': ' + n.title : '',
        prep: S.prep ? Math.round(100 * S.prep.progress) : null, shown: !!S.cube, loading: S.loading, path: S.openPath || '' }; })()`);
    samples.push({ t: Date.now() - t0, ...s });
    if (await c.evaluate(done)) break;
    await sleep(150);
  }
  return samples;
}

/** The percentages a run showed, in order, without repeats. */
function shownPercents(samples, from) {
  const out = [];
  for (const s of samples) {
    const m = from(s).match(/Preparing .*?… (\d+) %/);
    if (m && Number(m[1]) !== out[out.length - 1]) out.push(Number(m[1]));
  }
  return out;
}

export async function runPyramidWaitChecks({ c, check, waitFor, api, base, token, cube }) {
  const results = {};
  await c.send('Fetch.enable', { patterns: [{ urlPattern: '*/v1/cube/reduced_info*', requestStage: 'Request' }] });
  try {
    // ── 1. The headset app, launched by the desktop on a cube being prepared ──
    const s = await api('POST', '/v1/sessions/new', token, { body: {} });
    const session = JSON.parse(s.body).session_id;
    const o = await api('POST', '/v1/datasets/open', token, { session, body: { path: cube } });
    const ds = JSON.parse(o.body).dataset_id;
    const L = await api('POST', '/v1/vr/launch', token, { session, body: { cube: { dataset_id: ds, path: cube } } });
    const ticket = JSON.parse(L.body).ticket;
    await release(c);
    const build = fakeBuild(c, BUILD);
    await c.send('Page.navigate', { url: `${base}/vr/?launch=immersive&debug&refine=0&xrfake&polltimeout=${POLL_MS}#ticket=${ticket}` });
    await waitFor(c, '!!window.__vrShell && !!window.__vrState', 30000);
    const samples = await watch(c, '!!window.__vrState.cube && window.__vrState.loading === 0', 60000);
    const waitMs = build.elapsed();
    await build.stop();
    // The room follows the page's timer (appTick): gone within a few ticks.
    await waitFor(c, `!/^app-room/.test((window.__vrShell.notices.current() || {}).id || '')`, 3000);
    const last = { ...(samples[samples.length - 1] || {}),
      notice: await c.evaluate(`(() => { const n = window.__vrShell.notices.current(); return n ? n.id + ': ' + n.title : ''; })()`) };
    const roomPct = shownPercents(samples, (x) => x.notice);
    const statusPct = shownPercents(samples, (x) => x.status);
    const failed = samples.find((x) => /could not be opened|did not answer/.test(`${x.notice} ${x.status}`));
    results.app = { waitMs, requests: build.seen.join(','), roomPct, statusPct, samples: samples.length };
    check('pyramid wait (app): the build outlasts the page\'s poll deadline, with a poll reset twice and a poll unanswered on the way',
      waitMs > 2 * POLL_MS && build.seen.filter((x) => x === 'reset').length === 2 && build.seen.includes('hang')
        && build.seen[build.seen.length - 1] === 'real',
      `${waitMs} ms; ${build.seen.join(',')}`);
    check('pyramid wait (app): the room in VR shows "Preparing … N %" growing for the whole build',
      roomPct.length >= 4 && roomPct.every((v, i) => i === 0 || v >= roomPct[i - 1]), roomPct.join(' → '));
    // The time shown moves every second, the unanswered poll's included.
    // Over the samples that show the build (not the transfer and upload after it).
    const during = samples.filter((x) => /\(\d+ s so far\)/.test(x.status));
    const spanMs = during.length ? during[during.length - 1].t - during[0].t : 0;
    const secs = [...new Set(during.map((x) => x.status.match(/\((\d+) s so far\)/)[1]))];
    check('pyramid wait (app): the time so far moves every second, also while a poll waits for its answer',
      spanMs > 2 * POLL_MS && secs.length >= Math.floor(spanMs / 1000), `${secs.length} values over ${spanMs} ms`);
    check('pyramid wait (app): never "could not be opened" or "did not answer"', !failed, failed ? `${failed.notice} | ${failed.status}` : '');
    check('pyramid wait (app): the cube is loaded once the build ends, the room gone',
      last.shown && last.path === cube && !/^app-room/.test(last.notice) && last.prep === null,
      JSON.stringify({ shown: last.shown, path: last.path === cube, notice: last.notice, prep: last.prep }));

    // ── 2. The page loading a path (the status line), the same build ──
    await c.send('Page.navigate', { url: `${base}/vr/?debug&refine=0&polltimeout=${POLL_MS}#token=${encodeURIComponent(token)}` });
    await waitFor(c, `!!window.__vrShell && window.__vrShell.life.state === 'ok'`, 30000);
    await release(c);
    const build2 = fakeBuild(c, BUILD);
    await c.evaluate(`(() => { const $ = (id) => document.getElementById(id); $('path').value = ${JSON.stringify(cube)}; $('load').click(); })()`);
    const samples2 = await watch(c, '!!window.__vrState.cube && window.__vrState.loading === 0', 60000);
    await build2.stop();
    // The status line's last words come right after the cube goes on show.
    await waitFor(c, `/upload/.test(document.getElementById('load-status').textContent)`, 5000);
    const last2 = { ...(samples2[samples2.length - 1] || {}),
      status: await c.evaluate(`document.getElementById('load-status').textContent`) };
    const pct2 = shownPercents(samples2, (x) => x.status);
    check('pyramid wait (page): the status line shows the build\'s progress, then the cube is loaded',
      pct2.length >= 4 && last2.shown && last2.path === cube && /upload/.test(last2.status), `${pct2.join(' → ')}; ${last2.status.split('\n')[0]}`);
    results.page = { waitMs: build2.elapsed(), requests: build2.seen.join(','), pct: pct2 };

    // ── 3. A backend that cannot be reached at the first request still says so ──
    await release(c);
    const down = fakeBuild(c, () => 'refused');
    await c.evaluate(`(() => { const $ = (id) => document.getElementById(id); $('path').value = ${JSON.stringify(cube)}; $('load').click(); })()`);
    const said = await waitFor(c, `/did not answer/.test(document.getElementById('load-status').textContent) && window.__vrState.loading === 0`, 15000);
    await down.stop();
    check('pyramid wait: a backend that cannot be reached at the first request ends the load and says so',
      !!said.value && down.seen.length === 2, `${said.ms} ms; ${await c.evaluate(`document.getElementById('load-status').textContent.split('\\n')[0]`)}; ${down.seen.join(',')}`);
  } finally {
    await c.send('Fetch.disable').catch(() => {});
  }
  return results;
}

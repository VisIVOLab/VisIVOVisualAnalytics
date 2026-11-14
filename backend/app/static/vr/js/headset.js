// The page's side of the headset hub (issue #237, backend #236): one page
// that the desktop drives and that reports back to it.
//
// - Presence: the page says hello (with its transport: adb, pair or lan),
//   then tells the hub what it shows (in VR or not, the cube on show, the
//   cube staged for the end of the session) whenever that changes.
// - Commands: one long poll at a time for the page's commands, acknowledged
//   with `after` once applied; a `show` loads that cube into this page.
// - Activity: look and clip (debounced, the latest of each per cube), each
//   product made here (saved first in the Workspace Exports, so the desktop
//   can open it), and the cube when it switches.
//
// Every request goes through the page's transport (pageFetch: direct HTTP, or
// the pairing's data channel inside the public loader). Never `keepalive` on
// the long poll (see share.js). Nothing here runs inside a frame: callers only
// record what changed, and the sends run from timers. A failure goes to the
// diagnostics, never to the user's tools.
//
// Pure helpers are exported for node's tests (backend/tests/vr/headset.test.mjs);
// createHub(ctx) is the controller.

import { apiUrl } from './base-path.js';
import { pageFetch } from './transport.js';
import { isUnsupported, instanceVerdict } from './share.js';

/** The longest a command poll waits on the backend (seconds; the hub caps it at 25). */
export const POLL_WAIT_S = 20;
/** The delays after a failed request of the loop (ms), the last one repeated. */
export const BACKOFF_MS = [1000, 2000, 5000, 10000, 30000];
/** Look and clip are sent this long after their last change (ms). */
export const DEBOUNCE_MS = 500;
/** The state is sent this long after its last change (ms): one post for a burst. */
export const STATE_DELAY_MS = 150;
/** A PV path is saved this long after its last point (ms): one file per path, not per point. */
export const PV_SETTLE_MS = 1500;
/** A save that met busy heavy slots (429) is tried again after these delays (ms). */
export const SAVE_RETRY_MS = [2000, 5000, 15000];
/** The hub refuses a payload over 16 KiB of JSON: the page stays under this. */
export const MAX_PAYLOAD_BYTES = 15 * 1024;
/** A request of the hub (not the long poll) at most (ms). */
export const REQUEST_TIMEOUT_MS = 15000;
/** A product's save at most (a moment of a large cube takes a while). */
export const SAVE_TIMEOUT_MS = 180000;
/** Where the page keeps its client id (this tab: a reload keeps its queued commands). */
export const CLIENT_KEY = 'visivo-vr-hub-client';

const CLIENT_RE = /^[A-Za-z0-9_.:-]{1,64}$/;

/** How the page reaches the backend: 'pair' in the public loader, 'adb' on
 *  this machine's loopback (a Quest through adb reverse, or the desktop's own
 *  browser), 'lan' for any other host (the local network, a portal). */
export function transportOf(loc, paired) {
  if (paired) return 'pair';
  const host = String((loc && loc.hostname) || '').replace(/^\[|\]$/g, '').toLowerCase();
  if (host === 'localhost' || host === '::1' || /^127\./.test(host) || host.endsWith('.localhost')) return 'adb';
  return 'lan';
}

/** What the desktop calls this page: the headset's name when the browser says it. */
export function headsetLabel(ua) {
  const s = String(ua || '');
  const quest = /Quest\s*(\d+S?|Pro)?/i.exec(s);
  if (quest) return quest[1] ? `Quest ${quest[1]}` : 'Quest';
  if (/Pico/i.test(s)) return 'Pico';
  if (/Wolvic/i.test(s)) return 'Wolvic';
  return 'Desktop browser';
}

/** A new client id: 'vr-' and 12 base-36 characters. */
export function newHubClient(rand = Math.random) {
  let s = 'vr-';
  for (let i = 0; i < 12; i++) s += Math.floor(rand() * 36).toString(36);
  return s;
}

/** This tab's client id, kept in `storage` (sessionStorage): the same across
 *  a reload of the tab (the hub keeps its queued commands), another for
 *  another tab. */
export function hubClientId(storage, rand = Math.random) {
  try {
    const kept = storage && storage.getItem(CLIENT_KEY);
    if (kept && CLIENT_RE.test(kept)) return kept;
  } catch { /* storage refused */ }
  const id = newHubClient(rand);
  try { if (storage) storage.setItem(CLIENT_KEY, id); } catch { /* this load only */ }
  return id;
}

/** The hub's cube record of what the page shows ({dataset_id, path, kind}),
 *  or null: a cube with no dataset or no file (the synthetic one) is none. */
export function cubeRecord(datasetId, path, kind = 'cube') {
  if (!datasetId || !path) return null;
  return { dataset_id: String(datasetId).slice(0, 128), path: String(path).slice(0, 4096), kind: kind || 'cube' };
}

/** The file name of a path. */
export function fileName(path) {
  return String(path || '').split(/[\\/]/).pop() || '';
}

/** A path's file name without its suffixes (.fits, .fits.gz, .bin.head). */
export function stemOf(path) {
  const name = fileName(path);
  const stem = name.replace(/(\.(fits?|fz|gz|bin|head|csv|tsv|txt|xml|vot|votable|tbl|ipac|speck))+$/i, '');
  return (stem || 'cube').replace(/[^A-Za-z0-9_.+-]+/g, '_').slice(0, 80);
}

/**
 * The clip box in voxels of the full cube, both ends included
 * ([[x0, x1], [y0, y1], [z0, z1]]), or null for the whole cube. `clip` is the
 * page's {min, max} in box coordinates (0..1 over the full cube); voxel i
 * spans i/n .. (i+1)/n, rounded as the Look page names the channels.
 */
export function voxelBox(clip, fullShape) {
  if (!clip || !Array.isArray(fullShape) || fullShape.length !== 3) return null;
  const box = [];
  let whole = true;
  for (let i = 0; i < 3; i++) {
    const n = Math.max(1, Math.round(Number(fullShape[i]) || 1));
    const a = Number(clip.min[i]), b = Number(clip.max[i]);
    if (!Number.isFinite(a) || !Number.isFinite(b)) return null;
    const lo = Math.min(n - 1, Math.max(0, Math.round(a * n)));
    const hi = Math.min(n - 1, Math.max(lo, Math.round(b * n) - 1));
    if (lo > 0 || hi < n - 1) whole = false;
    box.push([lo, hi]);
  }
  return whole ? null : box;
}

/** The page's clip ({min, max} in box coordinates) for a voxel box of the
 *  full cube (inclusive), as the desktop crops it: voxel i spans i/n..(i+1)/n.
 *  Null when the box is not one of this cube. */
export function clipOfBox(box, fullShape) {
  if (!Array.isArray(box) || box.length !== 3 || !Array.isArray(fullShape) || fullShape.length !== 3) return null;
  const min = [], max = [];
  for (let i = 0; i < 3; i++) {
    const n = Number(fullShape[i]);
    const r = box[i];
    if (!(n >= 1) || !Array.isArray(r) || r.length !== 2) return null;
    const lo = Number(r[0]), hi = Number(r[1]);
    if (!Number.isInteger(lo) || !Number.isInteger(hi) || lo < 0 || hi < lo || hi > n - 1) return null;
    min.push(lo / n);
    max.push((hi + 1) / n);
  }
  return { min, max };
}

const finite = (v) => (typeof v === 'number' && Number.isFinite(v) ? v : null);

/**
 * A `look` event's payload, under the names the desktop reads
 * (VrHeadsetHub.h parseLook): colormap; threshold and colour_max as data
 * values of the cube (not the page's 0..1); opacity; gamma; mode (0 emission,
 * 1 maximum intensity); z_stretch (the velocity axis stretch); dataset_id.
 */
export function lookPayload({ colormap, threshold, colourMax, opacity, gamma, mode, zStretch, datasetId }) {
  const p = { colormap: String(colormap || '') };
  const put = (k, v) => { const f = finite(v); if (f !== null) p[k] = f; };
  put('threshold', threshold);
  put('colour_max', colourMax);
  put('opacity', opacity);
  put('gamma', gamma);
  put('mode', mode);
  put('z_stretch', zStretch);
  if (datasetId) p.dataset_id = String(datasetId);
  return p;
}

/** A `clip` event's payload: the voxel box (null: the whole cube) and the dataset. */
export function clipPayload(box, datasetId) {
  const p = { box: box ? box.map((r) => [r[0], r[1]]) : null };
  if (datasetId) p.dataset_id = String(datasetId);
  return p;
}

/** The product kinds the hub accepts (backend PRODUCT_KINDS). */
export const PRODUCT_KINDS = ['moment', 'pv', 'spectrum', 'isolate', 'remove', 'catalogue', 'flow'];

/**
 * How a product made here is saved for the desktop: {route, body} of the
 * desktop's own save route, or null when it already has its file (Isolate
 * and Remove: mask_apply_bin wrote it) or is a file of the backend (a
 * catalogue, a velocity field). `detail`: {product, dataset_id, request}.
 * `stem`: the cube's file stem, so the file is named after the cube shown.
 */
export function savePlan(detail, stem = 'cube') {
  const d = detail || {};
  const req = d.request || {};
  if (d.product === 'moment' && d.dataset_id) {
    return { route: '/v1/products/moment_fits',
      body: { ...req, dataset_id: d.dataset_id, output_basename: `${stem}_vr_m${Number(req.moment_order) || 0}.fits` } };
  }
  if (d.product === 'pv' && d.dataset_id) {
    return { route: '/v1/products/pv_fits',
      body: { dataset_id: d.dataset_id, vertices: req.vertices, width_pixels: req.width_pixels, output_basename: `${stem}_vr_pv.fits` } };
  }
  if (d.product === 'spectrum' && d.dataset_id) {
    return { route: '/v1/products/spectrum_table',
      body: { dataset_id: d.dataset_id, x: req.x, y: req.y, radius: Number(req.radius) || 0, format: 'csv', output_basename: '' } };
  }
  return null;
}

/** The product's line on the desktop (at most 300 characters). */
export function productSummary(detail, cubeName, file) {
  const d = detail || {};
  const req = d.request || {};
  const on = cubeName ? ` of ${cubeName}` : '';
  let s;
  switch (d.product) {
    case 'moment': s = `Moment ${Number(req.moment_order) || 0} of the selection${on}`; break;
    case 'pv': s = `PV slice${on} (${Array.isArray(req.vertices) ? req.vertices.length : 0} points, width ${req.width_pixels || 1} px)`; break;
    case 'spectrum': s = `Spectrum at pixel (${req.x}, ${req.y})${on}`; break;
    case 'isolate': s = `Isolated cube${on}`; break;
    case 'remove': s = `Cube${on} with the selection removed`; break;
    case 'catalogue': s = `Catalogue ${fileName(d.path) || ''} over ${cubeName || 'the cube'}`.replace(/\s+/g, ' '); break;
    case 'flow': s = `Velocity field ${fileName(d.path)}`; break;
    default: s = String(d.label || d.product || 'Product');
  }
  if (file) s += ` → ${file}`;
  return s.slice(0, 300);
}

/**
 * A `product` event's payload, as the desktop reads it (VrHeadsetHub.h
 * parseProduct): product, file (a Workspace Exports filename; the backend
 * adds output_path), dataset_id, request (the API's names), recipe (the
 * visivo-recipe/1 record), label; `path` for a file of the backend that is
 * not in the exports (a catalogue, a velocity field).
 */
export function productPayload(detail, file, recipe) {
  const d = detail || {};
  const p = { product: d.product };
  if (file) p.file = String(file);
  else if (d.path) p.path = String(d.path);
  if (d.dataset_id) p.dataset_id = String(d.dataset_id);
  if (d.request && typeof d.request === 'object') p.request = d.request;
  if (recipe) p.recipe = recipe;
  if (d.label) p.label = String(d.label).slice(0, 200);
  return p;
}

/** JSON size in bytes (UTF-8). */
export function jsonBytes(v) {
  const s = JSON.stringify(v);
  return typeof TextEncoder !== 'undefined' ? new TextEncoder().encode(s).length : s.length;
}

/**
 * A payload kept under `max` bytes of JSON: the recipe's Provenance
 * parameters go first, then the recipe; {payload, trimmed} (trimmed: what
 * went, '' when nothing did). The file and the request are never dropped.
 */
export function fitPayload(payload, max = MAX_PAYLOAD_BYTES) {
  if (jsonBytes(payload) <= max) return { payload, trimmed: '' };
  if (payload.recipe && payload.recipe.parameters) {
    const p = { ...payload, recipe: { ...payload.recipe, parameters: {} } };
    if (jsonBytes(p) <= max) return { payload: p, trimmed: 'recipe parameters' };
  }
  if (payload.recipe) {
    const p = { ...payload };
    delete p.recipe;
    if (jsonBytes(p) <= max) return { payload: p, trimmed: 'recipe' };
  }
  return { payload: null, trimmed: 'too large' };
}

/** The query of the command poll. */
export function commandsQuery(client, after, wait = POLL_WAIT_S) {
  return new URLSearchParams({ client, after: String(Math.max(0, Math.floor(after) || 0)), wait: String(wait) });
}

/** A command the page can act on: a `show` with a dataset id and a path. */
export function commandUsable(cmd) {
  return !!cmd && Number.isInteger(cmd.seq) && cmd.seq > 0 && cmd.type === 'show'
    && typeof cmd.dataset_id === 'string' && !!cmd.dataset_id && typeof cmd.path === 'string' && !!cmd.path;
}

/**
 * The page's controller. ctx: {
 *   auth() -> {token, session} | null (no session yet: nothing is sent),
 *   transport ('adb' | 'pair' | 'lan'), label, client,
 *   onCommand(cmd) -> void|Promise (started, not finished: the load runs on),
 *   log(level, event, message) (the diagnostics), onClient(id) (a new client id to keep),
 *   fetch?, setTimeout?, clearTimeout? (tests) }.
 * The poll runs while started (an active page with a session); stop() (the
 * page stood down, or is closing) says goodbye to the hub.
 */
export function createHub(ctx) {
  const doFetch = ctx.fetch || ((...a) => pageFetch(...a));
  const later = ctx.setTimeout || ((f, ms) => setTimeout(f, ms));
  const cancel = ctx.clearTimeout || ((t) => clearTimeout(t));
  const log = (level, event, message) => { try { ctx.log && ctx.log(level, event, message); } catch { /* the log's own */ } };
  const st = {
    client: ctx.client,
    instance: '',       // the backend process the cursor belongs to
    after: 0,           // the commands applied so far (the poll's cursor)
    helloSession: '',   // the session the hello was said in ('' none yet)
    running: false,
    unsupported: false,
    fails: 0,
    error: '',
    sent: { look: 0, clip: 0, product: 0, cube: 0, state: 0 },
    commands: 0,
    saveFailures: 0,
    lastSaveMs: null,
    lastPostMs: null,
  };
  let loopId = 0;
  let waitTimer = null;
  let wake = null;                 // resolves the loop's backoff wait early (start, a new session)
  const inFlight = new Set();      // AbortControllers (stop() aborts them)
  let chain = Promise.resolve();   // the sends, one after the other (order kept)
  let stateWanted = null, stateKeySent = '', stateTimer = null;
  const pending = { look: null, clip: null };   // {key, cubePath, summary, payload, timer}
  const lastSent = { look: new Map(), clip: new Map() };   // per cube: the key last sent
  const products = new Map();      // key -> {detail, recipe, cubePath, cubeName, stem, timer}
  const applied = new Set();       // command seqs applied under st.instance
  let lastLogged = '';
  let runGen = 0;                  // bumped by start() and stop(): queued products of before are dropped
  let goodbye = Promise.resolve(); // the last DELETE: a new hello waits for it
  let goodbyeUnconfirmed = false;  // it may still reach the hub later: the next hello takes a new client id

  /** The Error of work that belongs to a run of the hub that has ended (not a failure). */
  const staleRun = () => Object.assign(new Error('the hub was stopped'), { stale: true });

  /** DELETE of this page in `session`; a hello after it waits for its answer.
   *  One that is not confirmed (no answer in time, a failure) could still
   *  arrive after a new hello and remove it: the next hello then uses a new
   *  client id. `unloading`: the request is to outlive a closing page. */
  function goodbyeRequest(a, session, unloading = false) {
    const path = apiUrl(`/v1/vr/headset?client=${encodeURIComponent(st.client)}`);
    const ac = typeof AbortController !== 'undefined' ? new AbortController() : null;
    const t = later(() => { if (ac) ac.abort(); }, REQUEST_TIMEOUT_MS);
    return Promise.resolve()
      .then(() => doFetch(path, { method: 'DELETE', headers: headers({ ...a, session }, false),
        signal: ac ? ac.signal : undefined, keepalive: unloading }))
      .then((resp) => { if (!resp || !(resp.status >= 200 && resp.status < 500)) goodbyeUnconfirmed = true; },
        () => { goodbyeUnconfirmed = true; })
      .finally(() => cancel(t));
  }

  /** A hello on its way: {a, done} (done: settled, whatever its outcome). */
  let helloPending = null;

  function authNow() {
    const a = ctx.auth && ctx.auth();
    return a && a.session ? a : null;
  }

  function headers(a, json) {
    const h = { 'X-Visivo-Token': a.token, 'X-Visivo-Session': a.session };
    if (json) h['Content-Type'] = 'application/json';
    return h;
  }

  /** A request with its own deadline; stop() aborts it. {status, body (parsed or text)}. */
  async function call(method, path, body, a, ms = REQUEST_TIMEOUT_MS) {
    const ac = typeof AbortController !== 'undefined' ? new AbortController() : null;
    let expired = false;
    const t = later(() => { expired = true; if (ac) ac.abort(); }, ms);
    if (ac) inFlight.add(ac);
    try {
      const resp = await doFetch(apiUrl(path), { method, headers: headers(a, body !== undefined),
        body: body === undefined ? undefined : JSON.stringify(body), signal: ac ? ac.signal : undefined });
      const text = await resp.text();
      let parsed = text;
      try { parsed = text ? JSON.parse(text) : null; } catch { /* plain text */ }
      return { status: resp.status, body: parsed, text };
    } catch (e) {
      throw expired ? new Error(`no answer in ${Math.round(ms / 1000)} s`) : e;
    } finally {
      cancel(t);
      if (ac) inFlight.delete(ac);
    }
  }

  function noteError(where, msg) {
    st.error = `${where}: ${msg}`.slice(0, 200);
    // The same failure again and again is said once.
    if (st.error !== lastLogged) { lastLogged = st.error; log('warning', 'hub-failed', st.error); }
  }

  /** The backend's instance in an answer: a restart resets the cursor. */
  function noteInstance(instance) {
    const v = instanceVerdict(st.instance, instance);
    if (v === 'reset') { st.after = 0; applied.clear(); log('info', 'hub-instance', 'the backend restarted: commands counted from 0'); }
    if (v !== 'same') st.instance = instance;
    return v;
  }

  /** A 404 of the hub: no hub on this backend (stop for good), or this page
   *  is unknown to it (said hello again at the next request). */
  function on404(r) {
    if (isUnsupported(r.status, r.text)) {
      st.unsupported = true;
      st.running = false;
      log('info', 'hub-unsupported', 'this backend has no headset hub (older than the page)');
      return 'unsupported';
    }
    st.helloSession = '';
    return 'hello';
  }

  async function hello(a, gen) {
    const req = call('POST', '/v1/vr/headset/hello', { client: st.client, transport: ctx.transport, label: String(ctx.label || '').slice(0, 80) }, a);
    // A stop while it is on its way takes back what it may register, after it
    // settles; the next hello waits for that (stop() sets the goodbye now).
    const mine = { a, done: req.then(() => {}, () => {}) };
    helloPending = mine;
    let r;
    try {
      r = await req;
    } catch (e) {
      if (gen !== runGen || !st.running) throw staleRun();
      throw e;
    } finally {
      if (helloPending === mine) helloPending = null;
    }
    if (gen !== runGen || !st.running) throw staleRun();
    if (r.status === 404 || r.status === 405) { on404(r); throw new Error(`hello: ${r.status}`); }
    if (r.status !== 200) throw new Error(`hello: ${r.status} ${String(r.text).slice(0, 80)}`);
    noteInstance(r.body && r.body.instance);
    st.helloSession = a.session;
    stateKeySent = '';           // the state goes again in the new session
    log('info', 'hub-hello', `${ctx.transport}, client ${st.client}`);
    return true;
  }

  /** Hello when this page is not known in the session yet. */
  async function ensureHello(a) {
    if (st.helloSession === a.session) return;
    // A goodbye still on its way would remove the registration this hello makes.
    const gen = runGen;
    await goodbye;
    if (gen !== runGen || !st.running) throw staleRun();
    if (goodbyeUnconfirmed) {
      goodbyeUnconfirmed = false;
      st.client = newHubClient();
      try { if (ctx.onClient) ctx.onClient(st.client); } catch { /* kept in memory only */ }
      log('info', 'hub-new-client', `the last goodbye was not confirmed: this page is ${st.client} from now on`);
    }
    if (st.helloSession && st.helloSession !== a.session) {
      // The page moved to another session (a load made a new one): the old one forgets it.
      const old = { ...a, session: st.helloSession };
      st.helloSession = '';
      call('DELETE', `/v1/vr/headset?client=${encodeURIComponent(st.client)}`, undefined, old).catch(() => {});
    }
    await hello(a, gen);
  }

  /** A post to the hub, saying hello first (and again once on a 404). */
  async function post(path, body) {
    const a = authNow();
    // A page that stood down (or has not started) tells the hub nothing.
    if (!a || st.unsupported || !st.running) return null;
    for (let attempt = 0; attempt < 2; attempt++) {
      await ensureHello(a);
      const t0 = Date.now();
      const r = await call('POST', path, body, a);
      if (r.status === 404 || r.status === 405) {
        if (on404(r) === 'unsupported') return null;
        continue;
      }
      if (r.status !== 200) throw new Error(`${r.status} ${typeof r.body === 'object' && r.body && r.body.detail ? JSON.stringify(r.body.detail).slice(0, 120) : String(r.text).slice(0, 120)}`);
      st.lastPostMs = Date.now() - t0;
      return r.body;
    }
    throw new Error('the hub does not know this page after a second hello');
  }

  /** Runs `fn` after the sends before it; its failure goes to the diagnostics. */
  function enqueue(where, fn) {
    chain = chain.then(fn).catch((e) => { if (!(e && e.stale)) noteError(where, e && e.message ? e.message : String(e)); });
    return chain;
  }

  // ── State ───────────────────────────────────────────────────────────────
  /** What the page shows now: {in_vr, cube, staged_cube}. Sent when it changed. */
  function noteState(s) {
    stateWanted = { in_vr: !!s.in_vr, cube: s.cube || null, staged_cube: s.staged_cube || null };
    if (JSON.stringify(stateWanted) === stateKeySent || stateTimer !== null) return;
    stateTimer = later(() => { stateTimer = null; flushState(); }, STATE_DELAY_MS);
  }

  function flushState() {
    if (!stateWanted || !st.running) return;
    enqueue('state', async () => {
      const want = stateWanted;
      const a = authNow();
      if (!a || st.unsupported) return;
      // A hello in another session must be followed by the state again.
      if (JSON.stringify(want) === stateKeySent && st.helloSession === a.session) return;
      const r = await post('/v1/vr/headset/state', { client: st.client, ...want });
      if (r) { stateKeySent = JSON.stringify(want); st.sent.state++; }
    });
  }

  // ── Activity ────────────────────────────────────────────────────────────
  /** One activity event, posted now (inside a send of the queue). */
  async function postActivity(kind, cubePath, summary, payload) {
    const fit = fitPayload(payload);
    if (!fit.payload) throw new Error(`the payload is larger than ${MAX_PAYLOAD_BYTES} bytes: not sent`);
    if (fit.trimmed) log('info', 'hub-trimmed', `${kind}: ${fit.trimmed} left out to keep it under ${MAX_PAYLOAD_BYTES} bytes`);
    const r = await post('/v1/vr/activity', { client: st.client, kind, cube_path: cubePath || null,
      summary: String(summary || '').slice(0, 300), payload: fit.payload });
    if (r) st.sent[kind]++;
  }

  /** One activity event, queued after the sends before it. */
  function activity(kind, cubePath, summary, payload) {
    return enqueue(kind, () => postActivity(kind, cubePath, summary, payload));
  }

  /** The look (`kind` 'look') or the clip ('clip') of the cube at `cubePath`
   *  changed: sent DEBOUNCE_MS after its last change, unless it is what was
   *  last sent for that cube. */
  function noteCoalesced(kind, cubePath, summary, payload) {
    if (!cubePath) return;
    const key = JSON.stringify(payload);
    const p = pending[kind];
    // Called as often as the page likes: the same value again changes nothing
    // (the wait is not restarted, nothing is sent twice).
    if (p ? (p.cubePath === cubePath && p.key === key) : lastSent[kind].get(cubePath) === key) return;
    if (p && p.cubePath !== cubePath) sendPending(kind);          // another cube's change goes as it was
    if (pending[kind]) cancel(pending[kind].timer);
    pending[kind] = { key, cubePath, summary, payload, timer: later(() => sendPending(kind), DEBOUNCE_MS) };
  }

  function sendPending(kind) {
    const p = pending[kind];
    if (!p) return;
    cancel(p.timer);
    pending[kind] = null;
    if (lastSent[kind].get(p.cubePath) === p.key) return;
    lastSent[kind].set(p.cubePath, p.key);
    activity(kind, p.cubePath, p.summary, p.payload);
  }

  /** What the page now holds as the look or clip of a cube without telling the
   *  desktop (a cube just installed, a box the desktop sent): later changes
   *  are compared with it. */
  function rebase(kind, cubePath, payload) {
    if (!cubePath) return;
    if (pending[kind] && pending[kind].cubePath === cubePath) { cancel(pending[kind].timer); pending[kind] = null; }
    lastSent[kind].set(cubePath, JSON.stringify(payload));
  }

  function look(cubePath, payload, summary = 'Look changed') { noteCoalesced('look', cubePath, summary, payload); }
  function clip(cubePath, payload, summary) {
    noteCoalesced('clip', cubePath, summary || (payload.box ? 'Clip box changed' : 'Clip box cleared'), payload);
  }

  /** The page switched cube: what was pending for the old one goes first. */
  function cube(cubePath, payload, summary) {
    if (!cubePath) return Promise.resolve();
    sendPending('look');
    sendPending('clip');
    return activity('cube', cubePath, summary || `Switched to ${fileName(cubePath)}`, payload);
  }

  /**
   * A product made here. `detail`: {product, dataset_id, request, file?, path?,
   * label}; `recipe`: its visivo-recipe/1 record; `where`: {cubePath,
   * cubeName, stem}; `key`: the product's identity (a PV path extended, a
   * moment pressed again: the same key, the latest wins). Saved with the
   * desktop's route when it has no file yet, then posted. A PV waits for its
   * path to settle.
   */
  function product(detail, recipe, where = {}, key = '') {
    if (!detail || !PRODUCT_KINDS.includes(detail.product)) return;
    const k = key || `${detail.product}-${Date.now()}-${Math.random()}`;
    const old = products.get(k);
    if (old) cancel(old.timer);
    // Fenced to this run of the hub and to the session it was made in: a
    // product of a page that stood down, or of another session, is not sent.
    const a = authNow();
    if (!st.running || !a) return;
    const entry = { detail, recipe, where, gen: runGen, session: a.session };
    entry.timer = later(() => { products.delete(k); enqueue('product', () => sendProduct(entry)); },
      detail.product === 'pv' ? PV_SETTLE_MS : 0);
    products.set(k, entry);
  }

  async function sendProduct({ detail, recipe, where, gen, session }) {
    const current = () => {
      const a = authNow();
      return gen === runGen && st.running && a && a.session === session ? a : null;
    };
    if (!current()) return;
    let file = detail.file || '';
    const plan = file ? null : savePlan(detail, where.stem || 'cube');
    if (plan) {
      const t0 = Date.now();
      let r = null;
      for (let i = 0; ; i++) {
        const a = current();
        if (!a) return;                   // stopped, or another session, meanwhile
        r = await call('POST', plan.route, plan.body, a, SAVE_TIMEOUT_MS);
        if (r.status !== 429 || i >= SAVE_RETRY_MS.length) break;
        await new Promise((res) => later(res, SAVE_RETRY_MS[i]));     // the heavy slots are busy: later
      }
      if (!current()) return;
      if (r.status !== 200 || !r.body || !r.body.valid) {
        st.saveFailures++;
        const why = r.body && (r.body.error || r.body.detail) ? JSON.stringify(r.body.error || r.body.detail) : r.status;
        throw new Error(`${detail.product} not saved for the desktop: ${String(why).slice(0, 120)}`);
      }
      st.lastSaveMs = Date.now() - t0;
      file = r.body.workspace_filename || fileName(r.body.output_path);
    }
    const payload = productPayload(detail, file, recipe);
    // Posted here, inside this send (activity() would queue it behind itself).
    await postActivity('product', where.cubePath || '', productSummary(detail, where.cubeName, file), payload);
  }

  // ── Commands ────────────────────────────────────────────────────────────
  async function pollOnce(id) {
    const a = authNow();
    if (!a) return 'idle';
    await ensureHello(a);
    if (id !== loopId) return 'stop';
    // The state goes before the first poll of a session (it may have changed meanwhile).
    flushState();
    const q = commandsQuery(st.client, st.after);
    const r = await call('GET', `/v1/vr/headset/commands?${q}`, undefined, a, (POLL_WAIT_S + 15) * 1000);
    if (id !== loopId) return 'stop';
    if (r.status === 404 || r.status === 405) return on404(r) === 'unsupported' ? 'stop' : 'again';
    if (r.status === 429) throw new Error('the hub\'s waiting polls are all taken');
    if (r.status !== 200 || !r.body) throw new Error(`${r.status} ${String(r.text).slice(0, 80)}`);
    // A restart: the cursor sent was the old process's, so its `next` is not one of this one.
    const restarted = noteInstance(r.body.instance) === 'reset';
    let next = st.after;
    for (const cmd of r.body.commands || []) {
      if (!Number.isInteger(cmd.seq)) continue;
      next = Math.max(next, cmd.seq);
      if (cmd.seq <= st.after || applied.has(cmd.seq)) continue;
      applied.add(cmd.seq);
      if (!commandUsable(cmd)) { log('warning', 'hub-command-ignored', `#${cmd.seq} ${String(cmd.type)}`); continue; }
      st.commands++;
      try { await ctx.onCommand(cmd); } catch (e) { noteError('command', e && e.message ? e.message : String(e)); }
      if (id !== loopId) return 'stop';
    }
    // Acknowledged by the next poll's `after`.
    st.after = restarted ? next : Math.max(next, Number.isInteger(r.body.next) ? r.body.next : 0);
    for (const s of applied) if (s <= st.after) applied.delete(s);
    return 'again';
  }

  async function loop(id) {
    while (st.running && id === loopId) {
      let next;
      try {
        next = await pollOnce(id);
        if (next === 'again' || next === 'stop') { st.fails = 0; if (st.error && next === 'again') st.error = ''; }
      } catch (e) {
        if (id !== loopId) return;
        st.fails++;
        noteError('commands', `${e && e.message ? e.message : e} (retrying)`);
        next = 'wait';
      }
      if (next === 'stop') { if (id === loopId) st.running = false; return; }
      if (next === 'wait' || next === 'idle') {
        const ms = next === 'idle' ? 1000 : BACKOFF_MS[Math.min(st.fails - 1, BACKOFF_MS.length - 1)];
        await new Promise((res) => { wake = res; waitTimer = later(res, ms); });
        waitTimer = null; wake = null;
      }
    }
  }

  /** Follows the hub (an active page). Again after stop(): a new hello. */
  function start() {
    if (st.running || st.unsupported) return;
    st.running = true;
    runGen++;
    st.helloSession = '';
    st.error = '';
    loop(++loopId);
  }

  /** The page stood down or is closing: no more polls, and the hub forgets it
   *  (best effort; `unloading`: the request is to outlive the page). */
  function stop({ unloading = false } = {}) {
    const wasKnown = st.helloSession;
    loopId++;
    runGen++;
    st.running = false;
    for (const ac of inFlight) { try { ac.abort(); } catch { /* done */ } }
    inFlight.clear();
    if (waitTimer !== null) { cancel(waitTimer); waitTimer = null; }
    if (wake) { wake(); wake = null; }
    if (stateTimer !== null) { cancel(stateTimer); stateTimer = null; }
    // What was waiting to be sent belongs to the run that ends.
    for (const e of products.values()) cancel(e.timer);
    products.clear();
    for (const k of ['look', 'clip']) if (pending[k]) { cancel(pending[k].timer); pending[k] = null; }
    st.helloSession = '';
    const a = authNow();
    // The goodbye, set now (a hello of a new run waits for it): for the
    // registration known, and for one a hello on its way may still make.
    const hp = helloPending;
    helloPending = null;
    const parts = [];
    if (wasKnown && a && !st.unsupported) parts.push(goodbyeRequest(a, wasKnown, unloading));
    if (hp && !st.unsupported) parts.push(hp.done.then(() => goodbyeRequest(hp.a, hp.a.session, unloading)));
    if (parts.length) goodbye = Promise.all(parts);
  }

  /** The page's session changed: the poll in flight (in the old session)
   *  ends now and a new one starts with a hello in the new session (the old
   *  one forgets the page: ensureHello). */
  function resync() {
    if (!st.running) return;
    loopId++;
    for (const ac of inFlight) { try { ac.abort(); } catch { /* done */ } }
    inFlight.clear();
    if (waitTimer !== null) { cancel(waitTimer); waitTimer = null; }
    if (wake) { wake(); wake = null; }
    loop(++loopId);
  }

  function hudLines() {
    if (st.unsupported) return ['hub: this backend has none'];
    if (!st.running && !st.sent.look && !st.sent.product && !st.error) return [];
    const s = st.sent;
    const out = [`hub ${st.running ? 'linked' : 'off'} (${ctx.transport}): commands ${st.commands}, sent look ${s.look} clip ${s.clip} products ${s.product} cubes ${s.cube}`
      + `${st.lastPostMs !== null ? `, post ${st.lastPostMs} ms` : ''}${st.lastSaveMs !== null ? `, save ${st.lastSaveMs} ms` : ''}`];
    if (st.error) out.push(st.error.slice(0, 70));
    return out;
  }

  return { st, start, stop, resync, noteState, look, clip, rebase, cube, product, hudLines,
    /** For the tests: waits for every send queued so far. */
    idle: () => chain };
}

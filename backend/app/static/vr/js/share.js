// Selections shared with the desktop (issue #188). The page and the desktop's
// cube viewer work in the same backend session (the desktop's pairing link,
// #172). A Kinematic Lasso or a box made here is published to the session's
// selection hub (/v1/share/selection); a selection the desktop sends is read
// from it with a long poll and drawn here, with the creator's record attached
// and no recipe step of its own (the side that made a selection records it).
//
// Never `keepalive` on these requests: a long poll that runs while the backend
// goes down froze headless pages for minutes with it.
//
// One invariant (as in the desktop's SharedSelectionLink): what is shown
// converges to the hub's latest entry for this cube. Each publish carries a
// nonce the hub echoes; reconcileDecision and confirmDraw are the rule, pure,
// exported for node's tests with the queries and notes; createShare(ctx) is
// the page's controller.

import { apiUrl } from './base-path.js';

/** The longest a poll waits on the backend (seconds); the backend caps it at 25. */
export const POLL_WAIT_S = 20;
/** The delays after a failed poll (ms), the last one repeated. */
export const BACKOFF_MS = [1000, 2000, 5000, 10000, 30000];
/** The wait before offering a selection again when the cube is not drawn yet (ms). */
export const NOT_READY_MS = 1000;

/** Whether a 404 says the backend has no hub at all (an older backend: the
 *  route is unknown, FastAPI's bare "Not Found"), rather than that the session
 *  or the cube is unknown, which a re-pairing or a reload recovers. */
export function isUnsupported(status, bodyText) {
  if (status === 405) return true;
  if (status !== 404) return false;
  try { return JSON.parse(bodyText).detail === 'Not Found'; } catch { return false; }
}

/** An id for this page's tab, so it ignores the echo of its own publications. */
export function newClientId(rand = Math.random) {
  let s = 'vr-';
  for (let i = 0; i < 12; i++) s += Math.floor(rand() * 36).toString(36);
  return s;
}

/** The query of POST /v1/share/selection. */
export function publishQuery({ datasetId, kind, client, origin, dims, meta }) {
  return new URLSearchParams({
    dataset_id: datasetId, kind, source: 'headset', client,
    origin_x: String(origin[0]), origin_y: String(origin[1]), origin_z: String(origin[2]),
    width: String(dims[0]), height: String(dims[1]), depth: String(dims[2]),
    meta: JSON.stringify(meta || {}),
  });
}

/** A box [[x0, x1], [y0, y1], [z0, z1]] (inclusive) as an origin and dims. */
export function boxGeometry(range) {
  return { origin: range.map((r) => r[0]), dims: range.map((r) => r[1] - r[0] + 1) };
}

/** What the page tells the desktop about a lasso it grew: the grow request
 *  under the API's names (the desktop's LassoGrowRequest, so an Apply there
 *  records this grow), the Provenance labels, the trim. */
export function lassoMeta(growRequest, parameters = {}, trimBox = 0) {
  return { grow: growRequest, parameters, trim_box: trimBox || 0, made_in: 'VR page' };
}

/** ...and about a box: its range (inclusive), and where its channels came from. */
export function boxMeta(range, parameters = {}) {
  return { box: range.map((r) => [r[0], r[1]]), parameters, made_in: 'VR page' };
}

/** A fresh nonce for a publish: carried in its meta and echoed by the hub,
 *  it tells "the selection on screen" from any other entry. */
export function newNonce(rand = Math.random) {
  let s = '';
  for (let i = 0; i < 16; i++) s += Math.floor(rand() * 36).toString(36);
  return s;
}

/** The nonce of a hub entry ('' when its publisher sent none). */
export function nonceOf(info) {
  return info && info.meta && typeof info.meta.nonce === 'string' ? info.meta.nonce : '';
}

const int3 = (a) => Array.isArray(a) && a.length === 3 && a.every((v) => Number.isInteger(v));

/** A hub entry the clients can read at all: an integer seq, a known kind,
 *  origin, dims and full_shape of three integers each (as the desktop's
 *  parseSharedSelectionInfo requires). */
export function entryReadable(e) {
  return !!e && Number.isInteger(e.seq) && (e.kind === 'lasso' || e.kind === 'box')
    && int3(e.origin) && int3(e.dims) && int3(e.full_shape);
}

/** Its box fits this cube: origin >= 0, dims > 0, the cube's shape when it is
 *  known (null, or any axis 0: unknown) equal to the entry's, the box inside
 *  the cube (the known shape, else the entry's own). As the desktop's
 *  sharedSelectionRefusal. */
export function entryFits(e, fullShape) {
  const known = Array.isArray(fullShape) && fullShape.length === 3 && fullShape.every((v) => Number(v) > 0);
  const full = known ? fullShape.map(Number) : e.full_shape;
  for (let i = 0; i < 3; i++) {
    if (known && e.full_shape[i] !== full[i]) return false;
    if (e.dims[i] <= 0 || e.origin[i] < 0 || e.origin[i] + e.dims[i] > full[i]) return false;
  }
  return true;
}

/**
 * The one rule both clients follow (issue #188): what is shown converges to
 * the hub's latest entry for this cube. `latest` is that entry (null: none),
 * `s` = {client, shownNonce (what is on screen, own or received), localNonce
 * (the nonce of this client's latest own selection), floorSeq (entries at or
 * below it were read and then replaced by a local selection, or were there
 * before this client followed the hub), fullShape}. In this order:
 *   'none'  — no readable entry, the one on screen, or (another client's)
 *             at or below the floor, or not of this cube;
 *   'skip'  — an older own selection: a newer local one is about to replace it;
 *   'own'   — this client's latest own selection, not on screen: draw it back;
 *   'fetch' — fetch its mask, confirm it is still the latest, draw it.
 * The cases are shared with the desktop's tests (backend/tests/vr/share_rule_cases.json).
 */
export function reconcileDecision(latest, s) {
  if (!entryReadable(latest)) return 'none';
  const nonce = nonceOf(latest);
  if (nonce && nonce === s.shownNonce) return 'none';
  if (latest.client === s.client) return nonce && nonce === s.localNonce ? 'own' : 'skip';
  if (latest.seq <= (s.floorSeq || 0)) return 'none';
  if (!entryFits(latest, s.fullShape)) return 'none';
  return 'fetch';
}

/** After the mask was fetched: draw only if that entry is still the hub's
 *  latest and nothing local happened meanwhile (no new local selection, no
 *  dismissal). */
export function confirmDraw(latestNow, fetchedSeq, before, now) {
  return !!latestNow && latestNow.seq === fetchedSeq
    && now.shownNonce === before.shownNonce && now.floorSeq === before.floorSeq;
}

/** A local selection that is not published (or a Clear) replaced what was on
 *  screen: the entries read so far stay off the screen; a newer one from
 *  another client still replaces it. */
export function noteLocal(s, seenSeq) {
  return { ...s, shownNonce: '', localNonce: '', floorSeq: Math.max(s.floorSeq || 0, seenSeq || 0) };
}

/** What a publish's HTTP status means: 'kept' (200); 'stale' (409, a later
 *  one is the hub's); 'retry' (429, the session's slots are busy: once, after
 *  a short delay); 'refused' (another 4xx: the hub will never hold it, e.g.
 *  over the voxel cap); 'unknown' (5xx, no answer, a timeout: the hub may
 *  have kept it). */
export function publishOutcomeOf(status, detail = '') {
  if (status === 200) return 'kept';
  // A 409 is 'cleared' when the cube's selection was cleared after this
  // publish arrived: it is then refused like a 4xx (stays local, not own).
  if (status === 409) return /^cleared:/.test(String(detail || '')) ? 'cleared' : 'stale';
  if (status === 429) return 'retry';
  if (status >= 400 && status < 500) return 'refused';
  return 'unknown';
}

/** The hub's backend instance against the one this client knows: 'adopt'
 *  (none known yet), 'same', or 'reset' (the backend restarted: seqs began
 *  again at 1, so the cursor, floor and nonces of the old one mean nothing). */
export function instanceVerdict(known, seen) {
  if (!seen) return 'same';
  if (!known) return 'adopt';
  return known === seen ? 'same' : 'reset';
}

function fmt(v) {
  if (v === null || v === undefined || !Number.isFinite(v)) return 'n/a';
  return Math.abs(v) >= 1e5 || (v !== 0 && Math.abs(v) < 1e-3) ? v.toExponential(3) : v.toPrecision(4);
}

/** The one-line note of a selection received: who made it, voxels, flux. */
export function receivedNote(info, own = false) {
  const s = info.stats || {};
  const who = own ? 'this page (back from the hub)' : info.source === 'desktop' ? 'the desktop' : 'another headset';
  const what = info.kind === 'box' ? 'box' : 'Kinematic Lasso';
  const flux = s.flux_jy_kms !== null && s.flux_jy_kms !== undefined ? `, flux ${fmt(s.flux_jy_kms)} Jy km/s` : '';
  return `${what} from ${who}: ${info.count} voxels${flux}`;
}

/** The mask route's answer as the page's lasso reads its own grow. */
export function parseMask(buf, headers) {
  const h = (k) => headers.get(k);
  const dims = [Number(h('X-Visivo-Width')), Number(h('X-Visivo-Height')), Number(h('X-Visivo-Depth'))];
  const origin = (h('X-Visivo-Origin') || '').split(',').map(Number);
  const n = dims[0] * dims[1] * dims[2];
  if (!(n > 0) || origin.length !== 3 || origin.some((v) => !Number.isFinite(v)) || buf.byteLength !== n * 4) {
    throw new Error(`bad shared mask: ${buf.byteLength} bytes for ${dims.join('x')}`);
  }
  return { mask: new Float32Array(buf), bytes: buf, dims, origin, count: Number(h('X-Visivo-Selected-Count')) };
}

/** A publish's longest wait for the hub's answer (ms): then it counts as
 *  settled with an unknown outcome, and the hub is read again. */
export const PUBLISH_TIMEOUT_MS = 30000;
/** A read of the hub's latest entry, and a mask, at most (ms). */
export const READ_TIMEOUT_MS = 30000;
export const MASK_TIMEOUT_MS = 120000;
/** The wait before the one retry of a publish answered 429 (ms). */
export const RETRY_429_MS = 1000;

/**
 * The page's controller. ctx: {
 *   backend() -> Backend|null (token and session headers),
 *   datasetId() -> string, fullShape() -> [w, h, d] | null,
 *   onReceive(info, maskParsed, own) -> false when the cube is not drawn yet,
 *   onChange() -> void (the HUD/panel line changed),
 *   fetch?, setTimeout?, clearTimeout?, clientId?, nonce? (tests) }.
 *
 * One invariant: what is shown converges to the hub's latest entry for this
 * cube. reconcile() reads that entry and acts (reconcileDecision); it runs
 * after every long poll answer (a new entry or the 20 s timeout, the safety
 * net), after every publish settles (answered, refused, failed or timed out)
 * and before anything received is drawn. The long poll never waits on a
 * publish. One exception: a publish the hub refuses for good (a 4xx other
 * than 409 and 429) stays local; it is no longer this page's "latest own".
 */
export function createShare(ctx) {
  const doFetch = ctx.fetch || ((...a) => fetch(...a));
  const later = ctx.setTimeout || ((f, ms) => setTimeout(f, ms));
  const cancel = ctx.clearTimeout || ((t) => clearTimeout(t));
  const makeNonce = ctx.nonce || (() => newNonce());
  const st = {
    client: ctx.clientId || newClientId(),
    instance: '',      // the backend instance the seqs below belong to
    lastSeq: 0,        // the long poll's cursor: the latest seq handled
    seenSeq: 0,        // the latest seq read from the hub, handled or not
    shownNonce: '',    // what is on screen (own or received)
    localNonce: '',    // this page's latest own selection
    floorSeq: 0,       // entries at or below: read and replaced locally, or there before
    running: false,
    dataset: '',
    note: '',          // the last thing said (sent / received / not shared)
    error: '',
    fails: 0,
    unsupported: false,
    sent: 0, received: 0,
    publishOutcome: '',    // the last publish: see publishOutcomeOf, or 'timeout' / 'failed'
    lastLatencyMs: null,   // publish -> answer of the hub, ms
    receiveMs: null,       // reconcile -> drawn, ms
  };
  let timer = null;
  let loopId = 0;
  let reconciling = null;   // the reconcile in flight (single-flight)
  let again = false;        // asked for again while one ran
  const inFlight = new Set();   // AbortControllers of the reads (stop() aborts them)

  const headers = (json) => {
    const b = ctx.backend();
    return b ? b.headers(json) : null;
  };
  const changed = () => ctx.onChange && ctx.onChange();

  /** A request with its own abort and timeout; stop() aborts it too. */
  async function timed(url, opts, ms) {
    const ac = typeof AbortController !== 'undefined' ? new AbortController() : null;
    let expired = false;
    const t = later(() => { expired = true; if (ac) ac.abort(); }, ms);
    if (ac) inFlight.add(ac);
    try {
      return await doFetch(apiUrl(url), { ...opts, signal: ac ? ac.signal : undefined });
    } catch (e) {
      throw expired ? new Error(`no answer in ${Math.round(ms / 1000)} s`) : e;
    } finally {
      cancel(t);
      if (ac) inFlight.delete(ac);
    }
  }

  /** The backend instance of an answer: on a change (a restart) every seq,
   *  floor and nonce of the old one is forgotten. */
  function noteInstance(instance) {
    const v = instanceVerdict(st.instance, instance);
    if (v === 'reset') {
      st.lastSeq = st.seenSeq = st.floorSeq = 0;
      st.shownNonce = st.localNonce = '';
    }
    if (v !== 'same') st.instance = instance;
    return v;
  }

  /** A local selection replaced what was on screen (noteLocal). */
  function dismiss() {
    Object.assign(st, noteLocal(st, st.seenSeq));
  }

  function publish(kind, { bytes = null, origin, dims, meta }) {
    const h = headers(false);
    const ds = ctx.datasetId();
    if (!h || !ds || st.unsupported) return Promise.resolve(null);
    // The local selection is on screen now: it is what this page shows, and
    // every entry read so far (even one whose mask is still on its way) is
    // older than it.
    const nonce = makeNonce();
    st.floorSeq = Math.max(st.floorSeq, st.seenSeq);
    st.shownNonce = nonce;
    st.localNonce = nonce;
    h['Content-Type'] = 'application/octet-stream';
    const url = `/v1/share/selection?${publishQuery({ datasetId: ds, kind, client: st.client, origin, dims, meta: { ...(meta || {}), nonce } })}`;
    const t0 = performance.now();
    const attempt = async (retried) => {
      let resp, text;
      try {
        resp = await timed(url, { method: 'POST', headers: h, body: bytes || new Uint8Array(0) }, PUBLISH_TIMEOUT_MS);
        text = await resp.text();
      } catch (e) {
        const timedOut = /no answer in/.test(String(e && e.message));
        st.note = timedOut ? 'sharing: no answer in 30 s (the hub is read again)'
          : `not shared with the desktop: ${e && e.message ? e.message : e}`;
        st.publishOutcome = timedOut ? 'timeout' : 'failed';
        return null;
      }
      let detail = text;
      try { detail = JSON.parse(text).detail || text; } catch { /* plain text */ }
      const outcome = publishOutcomeOf(resp.status, detail);
      if (outcome === 'retry' && !retried) {
        await new Promise((r) => later(r, RETRY_429_MS));
        return attempt(true);
      }
      st.publishOutcome = outcome;
      if (outcome === 'kept') {
        const r = JSON.parse(text);
        // A restart since the last read: forget the old seqs, but this
        // selection, kept by the new instance, is still the one on screen.
        if (noteInstance(r.instance) === 'reset') { st.shownNonce = nonce; st.localNonce = nonce; }
        st.sent++;
        st.lastLatencyMs = performance.now() - t0;
        st.note = `shared with the desktop (${r.count} voxels, ${st.lastLatencyMs.toFixed(0)} ms)`;
        return r;
      }
      if (outcome === 'stale') {
        st.note = 'not shared: a later selection replaced it';
      } else if (outcome === 'refused' || outcome === 'retry' || outcome === 'cleared') {
        // The hub will not hold it: it stays on screen, local; it is no
        // longer this page's "latest own", so the hub's own entries are older.
        if (st.localNonce === nonce) st.localNonce = '';
        st.note = outcome === 'cleared' ? 'not shared: the selection was cleared after it was sent'
          : `not shared: ${String(detail).slice(0, 120)}`;
      } else {
        st.note = `not shared with the desktop: ${resp.status} ${String(text).slice(0, 120)}`;
      }
      return null;
    };
    return (async () => {
      try {
        return await attempt(false);
      } finally {
        changed();
        // Settled, whatever the outcome (the server may have kept it even
        // when the answer was lost): read the hub and converge.
        if (st.running) reconcile().catch(() => {});
      }
    })();
  }

  async function readLatest(h, ds) {
    const q = new URLSearchParams({ dataset_id: ds, after: '0', wait: '0' });
    const resp = await timed(`/v1/share/selection?${q}`, { headers: h }, READ_TIMEOUT_MS);
    if (resp.status === 204) return null;
    if (resp.status === 404 || resp.status === 405) {
      const text = await resp.text();
      if (isUnsupported(resp.status, text)) {
        st.unsupported = true;
        const e = new Error('this backend does not share selections');
        e.unsupported = true;
        throw e;
      }
      throw new Error(`${resp.status} ${text.slice(0, 80)}`);
    }
    if (!resp.ok) throw new Error(`${resp.status}`);
    const latest = await resp.json();
    if (latest) noteInstance(latest.instance);
    if (latest && Number.isInteger(latest.seq)) st.seenSeq = Math.max(st.seenSeq, latest.seq);
    return latest;
  }

  /** Reads the hub's latest entry and makes the screen follow it. Returns
   *  'done' | 'notready' (the cube is not drawn yet: try again soon). */
  function reconcile() {
    if (reconciling) { again = true; return reconciling; }
    reconciling = (async () => {
      let out = 'done';
      do {
        again = false;
        out = await reconcileOnce();
      } while (again && st.running);
      return out;
    })().finally(() => { reconciling = null; });
    return reconciling;
  }

  /** An entry the page cannot use (unreadable, or a mask unlike its
   *  description): never asked for again. */
  function reject(seq) {
    if (!Number.isInteger(seq)) return;
    st.lastSeq = Math.max(st.lastSeq, seq);
    st.floorSeq = Math.max(st.floorSeq, seq);
  }

  async function reconcileOnce() {
    const h = headers(false);
    const ds = st.dataset;
    if (!h || !ds) return 'done';
    const id = loopId;
    const latest = await readLatest(h, ds);
    if (id !== loopId) return 'done';
    if (!latest) return 'done';
    if (!entryReadable(latest)) { reject(latest.seq); return 'done'; }
    const decision = reconcileDecision(latest, { ...st, fullShape: ctx.fullShape() });
    if (decision === 'none' || decision === 'skip') {
      st.lastSeq = Math.max(st.lastSeq, latest.seq);
      return 'done';
    }
    const before = { shownNonce: st.shownNonce, floorSeq: st.floorSeq };
    const t0 = performance.now();
    const mq = new URLSearchParams({ dataset_id: ds, seq: String(latest.seq) });
    const m = await timed(`/v1/share/selection/mask?${mq}`, { headers: h }, MASK_TIMEOUT_MS);
    if (id !== loopId) return 'done';
    if (m.status === 404) { again = true; return 'done'; }   // superseded meanwhile
    if (!m.ok) throw new Error(`${m.status}`);
    let parsed;
    try {
      parsed = parseMask(await m.arrayBuffer(), m.headers);
      if (parsed.count !== latest.count) throw new Error('count');
    } catch {
      reject(latest.seq);   // not the mask announced: skip it for good
      return 'done';
    }
    // Still the latest, and nothing local meanwhile (a lasso made while a
    // large mask was fetched must not be drawn over).
    const now = await readLatest(h, ds);
    if (id !== loopId) return 'done';
    if (!confirmDraw(now, latest.seq, before, st)) { again = true; return 'done'; }
    const own = decision === 'own';
    if (ctx.onReceive(latest, parsed, own) === false) return 'notready';
    st.shownNonce = nonceOf(latest);
    st.lastSeq = Math.max(st.lastSeq, latest.seq);
    if (!own) st.received++;
    st.receiveMs = performance.now() - t0;
    st.note = receivedNote(latest, own);
    changed();
    return 'done';
  }

  /** The long poll: one request waiting for an entry above the cursor. */
  async function pollOnce(id) {
    const h = headers(false);
    const ds = st.dataset;
    if (!h || !ds) return 'stop';
    const q = new URLSearchParams({ dataset_id: ds, after: String(st.lastSeq), wait: String(POLL_WAIT_S) });
    const resp = await timed(`/v1/share/selection?${q}`, { headers: h }, (POLL_WAIT_S + 10) * 1000);
    if (id !== loopId) return 'stop';
    if (resp.status === 404 || resp.status === 405) {
      const text = await resp.text();
      if (isUnsupported(resp.status, text)) {
        st.unsupported = true;
        st.error = 'this backend does not share selections';
        return 'stop';
      }
      // The session or the cube is unknown (a backend restart): retried, a
      // reload or a new pairing recovers it.
      throw new Error(`${resp.status} ${text.slice(0, 80)}`);
    }
    if (resp.status !== 204 && !resp.ok) throw new Error(`${resp.status}`);
    // Anything (a new entry, or the timeout: the safety net) -> reconcile.
    return (await reconcile()) === 'notready' ? 'notready' : 'again';
  }

  /** The first read of a link: the hub's current entry is taken as the
   *  cursor and the floor, never drawn (an entry made before this page
   *  followed the hub must not replace what is on screen). */
  async function prime(id) {
    const h = headers(false);
    if (!h || !st.dataset) return;
    const latest = await readLatest(h, st.dataset);
    if (id !== loopId || !latest || !Number.isInteger(latest.seq)) return;
    st.lastSeq = Math.max(st.lastSeq, latest.seq);
    st.floorSeq = Math.max(st.floorSeq, latest.seq);
  }

  async function loop(id) {
    let primed = false;
    while (st.running && id === loopId) {
      let next;
      try {
        if (!primed) { await prime(id); primed = true; next = 'again'; } else next = await pollOnce(id);
        st.fails = 0;
        if (st.error && next !== 'stop') { st.error = ''; changed(); }
      } catch (e) {
        if (id !== loopId) return;
        if (e && e.unsupported) { st.error = e.message; next = 'stop'; } else {
          st.fails++;
          st.error = `selection link: ${e && e.message ? e.message : e} (retrying)`;
          changed();
          next = 'wait';
        }
      }
      if (next === 'stop') { st.running = false; changed(); return; }
      if (next === 'wait' || next === 'notready') {
        const ms = next === 'notready' ? NOT_READY_MS : BACKOFF_MS[Math.min(st.fails - 1, BACKOFF_MS.length - 1)];
        await new Promise((res) => { timer = later(res, ms); });
        timer = null;
      }
    }
  }

  /** Follows the hub for this dataset (a new cube: a new loop). */
  function start(datasetId) {
    stop();
    if (!datasetId || st.unsupported) return;
    st.dataset = datasetId;
    st.instance = '';
    st.lastSeq = 0;
    st.seenSeq = 0;
    st.floorSeq = 0;
    st.shownNonce = '';
    st.localNonce = '';
    st.running = true;
    st.error = '';
    loop(++loopId);
  }

  function stop() {
    loopId++;
    st.running = false;
    st.dataset = '';
    for (const ac of inFlight) { try { ac.abort(); } catch { /* already done */ } }
    inFlight.clear();
    if (timer !== null) { cancel(timer); timer = null; }
  }

  function hudLines() {
    if (!st.dataset && !st.sent && !st.received && !st.error) return [];
    const parts = [`share ${st.running ? 'linked' : 'off'}: sent ${st.sent}, received ${st.received}`];
    if (st.lastLatencyMs !== null) parts.push(`publish ${st.lastLatencyMs.toFixed(0)} ms`);
    if (st.receiveMs !== null) parts.push(`receive ${st.receiveMs.toFixed(0)} ms`);
    const out = [parts.join('  ')];
    if (st.note) out.push(st.note);
    if (st.error) out.push(st.error);
    return out;
  }

  return { st, publish, dismiss, reconcile, start, stop, hudLines };
}

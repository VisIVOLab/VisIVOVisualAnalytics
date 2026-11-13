// The VR page's calls to the backend that serves it (same origin, under the
// page's own path prefix: base-path.js).
//
// The token: read once from the URL fragment (#token=...), which a browser
// never sends to the server, so it is not in any access log; then removed from
// the address bar and history entry. It is kept in sessionStorage (this tab
// only), or in localStorage when "remember on this device" is ticked. It can
// also be typed. It travels only in the X-Visivo-Token header (and in the
// render session's WebSocket address, as the backend requires there). The
// desktop's pairing link (issue #172) adds the session, dataset and path: see
// takePairing.

import { parsePairing, strippedAddress } from './pairing.js';
import { apiUrl } from './base-path.js';
import { pageFetch } from './transport.js';

const KEY = 'visivo-vr-token';

function store(kind) {
  try { return kind === 'local' ? window.localStorage : window.sessionStorage; } catch { return null; }
}

let fragmentError = '';
/** Why the token in the fragment could not be read ('' when it could). */
export function tokenError() { return fragmentError; }

const PAIR_KEY = 'visivo-vr-pairing';
let pairingCache = null;

/**
 * The pairing from the desktop (#token=…&session=…&dataset=…&path=…, issue
 * #172), read once: every pairing key is removed from the address bar and
 * the history entry. The token goes where a typed one goes; the session,
 * dataset and path are kept in sessionStorage (this tab only: a reload of
 * the tab continues, another tab or a later visit does not). Without a
 * pairing in the address, the one this tab kept. {token, session, dataset,
 * path, kind, box, fromAddress}.
 */
export function takePairing() {
  if (pairingCache) return pairingCache;
  const p = parsePairing(window.location.hash || '');
  if (p.present) {
    if (p.errors.length) {
      // A malformed %-escape must not stop the page loading.
      fragmentError = p.errors.includes('token')
        ? 'The token in the address (#token=…) is malformed: paste it into the field.'
        : `The link's ${p.errors.join(', ')} ${p.errors.length > 1 ? 'are' : 'is'} malformed: open the cube by hand.`;
    }
    window.history.replaceState(null, '', strippedAddress(window.location.pathname,
      window.location.search, window.location.hash));
    if (p.token) {
      try { store('session')?.setItem(KEY, p.token); } catch { /* private mode */ }
    }
    const kept = { session: p.session, dataset: p.dataset, path: p.path, kind: p.kind, box: p.box };
    try {
      if (kept.session || kept.dataset || kept.path) store('session')?.setItem(PAIR_KEY, JSON.stringify(kept));
      else store('session')?.removeItem(PAIR_KEY);
    } catch { /* storage refused: this load only */ }
    pairingCache = { token: p.token, ...kept, fromAddress: true };
    return pairingCache;
  }
  let kept = {};
  try { kept = JSON.parse(store('session')?.getItem(PAIR_KEY) || '{}') || {}; } catch { kept = {}; }
  const str = (v) => (typeof v === 'string' ? v : '');
  pairingCache = { token: '', session: str(kept.session), dataset: str(kept.dataset), path: str(kept.path),
    kind: str(kept.kind), box: str(kept.box), fromAddress: false };
  return pairingCache;
}

/** Forgets the tab's pairing (the user opened another cube by hand). */
export function forgetPairing() {
  try { store('session')?.removeItem(PAIR_KEY); } catch { /* nothing kept */ }
  pairingCache = { token: '', session: '', dataset: '', path: '', kind: '', box: '', fromAddress: false };
}

/** Token from the fragment, else from storage. Clears the pairing keys from the address. */
export function takeToken() {
  const p = takePairing();
  if (p.token) return p.token;
  try {
    return store('session')?.getItem(KEY) || store('local')?.getItem(KEY) || '';
  } catch { return ''; }
}

export function saveToken(token, remember) {
  try {
    store('session')?.setItem(KEY, token);
    if (remember) store('local')?.setItem(KEY, token);
    else store('local')?.removeItem(KEY);
  } catch { /* storage refused: the token lives in memory only */ }
}

/** An Error for a request: "<status>: <detail>", with `status` and `detail`;
 *  `network` when nothing answered (the backend down, the link lost). */
export function requestError(status, detail) {
  const e = new Error(status ? `${status}: ${detail}` : `network: ${detail}`);
  e.status = status;
  e.detail = detail;
  e.network = !status;
  return e;
}

/** Every Backend's failures and successes go to these hooks (lifecycle.js
 *  watches them): (err, path) and (path). Set by the page. */
export const requestHooks = { onFailure: null, onSuccess: null };

/** fetch(), with a rejected fetch turned into a `network` requestError
 *  (`timedOut` when its deadline ended it, `aborted` when the page cancelled
 *  it) and both outcomes reported to requestHooks. Non-ok responses are
 *  returned. `quiet`: a network failure is not reported here but marked
 *  `unreported` (with its `path`), for a caller that may send it again and
 *  reports only the failure that stays (retryOnNetworkError). */
export async function hookedFetch(path, init, quiet = false) {
  let resp;
  try {
    // Behind a reverse proxy the page's own path prefix goes first (#187).
    // Over the pairing's data channel when the page runs in the public
    // loader (transport.js, #213), else fetch() itself.
    resp = await pageFetch(apiUrl(path), init);
  } catch (err) {
    const timedOut = !!(init && init.signal && init.signal.aborted && init.signal.reason === TIMED_OUT);
    const e = requestError(0, timedOut ? 'the backend did not answer in time' : 'the backend did not answer');
    e.cause = err;
    e.timedOut = timedOut;
    e.path = path;
    if (init && init.signal && init.signal.aborted && !timedOut) e.aborted = true;   // cancelled by the page
    if (requestHooks.onFailure && !e.aborted) {
      if (quiet) e.unreported = true; else requestHooks.onFailure(e, path);
    }
    throw e;
  }
  if (resp.ok && requestHooks.onSuccess) requestHooks.onSuccess(path);
  return resp;
}

const TIMED_OUT = 'timeout';

/**
 * A request with a deadline over the whole exchange (headers and body): a
 * connection that hangs (a link half down, a backend that accepts and never
 * answers) ends as a `network` requestError after `timeoutMs` instead of
 * whenever the browser gives up. `signal` (optional) cancels it too (then the
 * error has `aborted`). `read(resp)` reads the body inside the deadline.
 */
export async function boundedRequest(path, init, timeoutMs, signal, read, quiet = false) {
  if (!(timeoutMs > 0) && !signal) {
    const resp = await hookedFetch(path, init, quiet);
    return read(resp);
  }
  const ctl = new AbortController();
  const onAbort = () => ctl.abort(signal.reason);
  if (signal) { if (signal.aborted) onAbort(); else signal.addEventListener('abort', onAbort, { once: true }); }
  const timer = timeoutMs > 0 ? setTimeout(() => ctl.abort(TIMED_OUT), timeoutMs) : null;
  try {
    const resp = await hookedFetch(path, { ...init, signal: ctl.signal }, quiet);
    try {
      // The deadline holds for the body too, whether or not this fetch
      // aborts a body read by itself.
      return await Promise.race([read(resp), new Promise((_, reject) => {
        if (ctl.signal.aborted) reject(new Error('aborted'));
        ctl.signal.addEventListener('abort', () => reject(new Error('aborted')), { once: true });
      })]);
    } catch (err) {
      if (err && err.status !== undefined) throw err;            // an HTTP error, as read() made it
      // The body did not come: the same as no answer at all.
      const e = requestError(0, ctl.signal.reason === TIMED_OUT ? 'the backend did not answer in time' : 'the backend did not answer');
      e.timedOut = ctl.signal.reason === TIMED_OUT;
      e.path = path;
      if (ctl.signal.aborted && ctl.signal.reason !== TIMED_OUT) e.aborted = true;
      e.cause = err;
      if (requestHooks.onFailure && !e.aborted) {
        if (quiet) e.unreported = true; else requestHooks.onFailure(e, path);
      }
      throw e;
    }
  } finally {
    if (timer) clearTimeout(timer);
    if (signal) signal.removeEventListener('abort', onAbort);
  }
}

/** The deadline of the lifecycle's check (/v1/vr/info), in ms. */
export const INFO_TIMEOUT_MS = 4000;

async function failed(resp, path) {
  const text = await resp.text().catch(() => '');
  let detail = text;
  try {
    const data = JSON.parse(text);
    detail = typeof data.detail === 'string' ? data.detail : JSON.stringify(data.detail ?? data);
  } catch { /* plain text */ }
  const e = requestError(resp.status, String(detail).slice(0, 300));
  if (requestHooks.onFailure) requestHooks.onFailure(e, path);
  return e;
}
/**
 * ``attempt(quiet)`` again, once, after a network error: nothing answered and
 * no deadline or cancellation ended it (the fetch's TypeError, as is or as
 * hookedFetch's `network` error; e.g. a keep-alive connection the server
 * closed as the request went out). Only for requests that change nothing on
 * the backend: on the Mac, the request for the reduced cube right after a
 * pyramid build failed so in one of three cold loads, and succeeded when sent
 * again. ``withinMs``: resend only when the error came that soon after
 * sending, i.e. before or while the request went out; a connection lost while
 * a long computation runs is not resent (the backend is still working on the
 * first; it also joins an identical second one). The first attempt runs
 * `quiet` (its failure is not reported to requestHooks); a failure that stays
 * is reported once, as hookedFetch would have. ``now`` is the clock (tests).
 */
export async function retryOnNetworkError(attempt, { waitMs = 500, withinMs = Infinity, now = () => performance.now() } = {}) {
  const report = (e) => {
    if (e && e.unreported) {
      e.unreported = false;
      if (requestHooks.onFailure) requestHooks.onFailure(e, e.path);
    }
    return e;
  };
  const t0 = now();
  try {
    return await attempt(true);
  } catch (e) {
    const cause = e && e.network ? e.cause : e;
    const resend = cause instanceof TypeError && !(e && (e.aborted || e.timedOut)) && now() - t0 <= withinMs;
    if (!resend) throw report(e);
    await new Promise((res) => setTimeout(res, waitMs));
    try {
      return await attempt(false);
    } catch (e2) {
      throw report(e2);
    }
  }
}

/** The error reduced_info answers with while the cube's brick pyramid is
 *  being built (HTTP 202): `preparing`, with `progress` (0..1). No caller can
 *  take that answer for the cube's description by mistake. */
export function preparingError(progress) {
  const e = new Error(`the backend is preparing the cube's brick pyramid (${(100 * (progress || 0)).toFixed(0)} %)`);
  e.preparing = true;
  e.progress = Number.isFinite(progress) ? progress : 0;
  return e;
}

/** How soon a failed reduced_bin may be resent (its error came before it was sent). */
export const RESEND_WITHIN_MS = 2000;

export class Backend {
  constructor(token) {
    this.token = token;
    this.session = '';
    // A deadline for every JSON request of this handle (0: none). The page's
    // loads have none (a large cube can take long to open); the recovery's
    // handle has one, so a hung connection cannot stall it.
    this.timeoutMs = 0;
  }

  headers(json = true) {
    const h = { 'X-Visivo-Token': this.token };
    if (this.session) h['X-Visivo-Session'] = this.session;
    if (json) h['Content-Type'] = 'application/json';
    return h;
  }

  async _json(method, path, body) {
    return boundedRequest(path, {
      method, headers: this.headers(body !== undefined),
      body: body === undefined ? undefined : JSON.stringify(body),
    }, this.timeoutMs, null, async (resp) => {
      if (!resp.ok) throw await failed(resp, path);
      const text = await resp.text();
      try { return JSON.parse(text); } catch { return { detail: text }; }
    });
  }

  /** GET /v1/vr/info (public; with the token, also whether it is accepted),
   *  within `timeoutMs` (INFO_TIMEOUT_MS), cancelled by `signal`. */
  async info({ signal = null, timeoutMs = INFO_TIMEOUT_MS } = {}) {
    return boundedRequest('/v1/vr/info', { headers: this.token ? { 'X-Visivo-Token': this.token } : {}, cache: 'no-store' },
      timeoutMs, signal, async (resp) => {
        if (!resp.ok) throw await failed(resp, '/v1/vr/info');
        return resp.json();
      });
  }

  /** POST /v1/vr/log: {status, retryAfterS}, never throws for an HTTP error. */
  async log(body) {
    // Not keepalive: a log line is not worth outliving the page, and keepalive
    // requests go through the browser process with a small shared quota.
    const resp = await pageFetch(apiUrl('/v1/vr/log'), { method: 'POST', headers: this.headers(true), body: JSON.stringify(body) });
    return { status: resp.status, retryAfterS: Number(resp.headers.get('Retry-After')) || 0 };
  }

  async ping() {
    const resp = await pageFetch(apiUrl('/v1/ping'));
    return resp.ok;
  }

  /** Uses the desktop's session (the pairing) for every request. */
  adoptSession(id) { this.session = id || ''; }

  async newSession() {
    const r = await this._json('POST', '/v1/sessions/new', {});
    this.session = r.session_id;
    return this.session;
  }

  async listFiles(path) {
    const q = encodeURIComponent(path || '');
    return this._json('GET', `/v1/files/list?path=${q}`);
  }

  async open(path) {
    if (!this.session) await this.newSession();
    const r = await this._json('POST', '/v1/datasets/open', { path });
    if (!r.valid) throw new Error(r.error || 'The dataset could not be opened.');
    if (r.session_id) this.session = r.session_id;
    if (r.kind !== 'cube') throw new Error(`Not a cube (${r.kind}): the VR page shows cubes.`);
    return r;
  }

  // ── Analysis on the full cube (issue #171) ────────────────────────────────

  /** `signal`: an AbortSignal, to cancel a request a newer one replaces. */
  async _binary(path, body, contentType = 'application/json', signal = undefined) {
    const headers = this.headers(false);
    headers['Content-Type'] = contentType;
    const resp = await hookedFetch(path, { method: 'POST', headers, body, signal });
    if (!resp.ok) throw await failed(resp, path);
    return { resp, buf: await resp.arrayBuffer() };
  }

  /** Full-resolution voxels x0..x1, y0..y1, z0..z1 (inclusive), float32. */
  async subvolume(datasetId, x0, x1, y0, y1, z0, z1) {
    const { resp, buf } = await this._binary('/v1/cube/subvolume_bin',
      JSON.stringify({ dataset_id: datasetId, x0, x1, y0, y1, z0, z1 }));
    const h = (k) => Number(resp.headers.get(k));
    return { values: new Float32Array(buf), dims: [h('X-Visivo-Width'), h('X-Visivo-Height'), h('X-Visivo-Depth')], origin: [x0, y0, z0] };
  }

  /** The Kinematic Lasso (the desktop's kinematic_select_bin): the mask over
   *  the selection's bounding box, its origin and the voxel count. */
  async kinematicSelect(datasetId, growRequest) {
    const t0 = performance.now();
    const { resp, buf } = await this._binary('/v1/cube/kinematic_select_bin',
      JSON.stringify({ dataset_id: datasetId, ...growRequest }));
    const h = (k) => resp.headers.get(k);
    const dims = [Number(h('X-Visivo-Width')), Number(h('X-Visivo-Height')), Number(h('X-Visivo-Depth'))];
    const origin = (h('X-Visivo-Origin') || '0,0,0').split(',').map(Number);
    return {
      mask: new Float32Array(buf), bytes: buf, dims, origin,
      count: Number(h('X-Visivo-Selected-Count')),
      trimBox: Number(h('X-Visivo-Trim-Box') || 0),
      ms: performance.now() - t0,
    };
  }

  /** Statistics of an explicit mask on the full cube (mask_stats_bin). */
  maskStats(datasetId, maskBytes, origin, dims) {
    const q = new URLSearchParams({
      dataset_id: datasetId, origin_x: origin[0], origin_y: origin[1], origin_z: origin[2],
      width: dims[0], height: dims[1], depth: dims[2],
    });
    return this._binary(`/v1/cube/mask_stats_bin?${q}`, maskBytes, 'application/octet-stream')
      .then(({ buf }) => JSON.parse(new TextDecoder().decode(buf)));
  }

  /** Statistics of every voxel of a box (mask_stats_bin with box=true). */
  boxStats(datasetId, boxRequest) {
    const q = new URLSearchParams({ dataset_id: datasetId, ...boxRequest });
    return this._binary(`/v1/cube/mask_stats_bin?${q}`, new Uint8Array(0), 'application/octet-stream')
      .then(({ buf }) => JSON.parse(new TextDecoder().decode(buf)));
  }

  spectralProbe(datasetId, probeRequest) {
    return this._json('POST', '/v1/spectral/probe', { dataset_id: datasetId, ...probeRequest });
  }

  pixelToSky(datasetId, x, y, z) {
    return this._json('POST', '/v1/datasets/pixel_to_sky', { dataset_id: datasetId, x, y, z });
  }

  /** reduced_info; while a pyramid builds the page asks again every second,
   *  so one request is given at most 2 minutes. */
  /** reduced_info. While the cube's brick pyramid is built the backend
   *  answers 202: that throws preparingError (`preparing`, `progress`), never
   *  returned as the cube's description. The deadline is the handle's
   *  (`timeoutMs`, none by default: opening a large cube can take long), or
   *  `opts.timeoutMs` (the page's polls during a build). */
  reducedInfo(req, opts = {}) {
    const timeoutMs = opts.timeoutMs > 0 ? opts.timeoutMs : this.timeoutMs;
    return retryOnNetworkError((quiet) => boundedRequest('/v1/cube/reduced_info', {
      method: 'POST', headers: this.headers(true), body: JSON.stringify(req),
    }, timeoutMs, null, async (resp) => {
      if (!resp.ok) throw await failed(resp, '/v1/cube/reduced_info');
      const text = await resp.text();
      let data;
      try { data = JSON.parse(text); } catch { data = { detail: text }; }
      if (resp.status === 202 || (data && data.preparing)) throw preparingError(data && data.progress);
      return data;
    }, quiet));
  }

  /** The reduced cube: {data: Uint8Array | Uint16Array, dims, lo, hi, ...}.
   *  At most `timeoutMs` until its headers (30 min: a whole-cube pass of a
   *  large cube takes minutes), so a request that never answers ends the
   *  load with a message (a `timedOut` network error, reported to the
   *  lifecycle like any other); the body then streams without a deadline.
   *  Sent once more only when it failed within RESEND_WITHIN_MS (before it
   *  went out). */
  async reducedCube(req, onProgress, timeoutMs = 1800000) {
    const t0 = performance.now();
    let resp;
    try {
      resp = await retryOnNetworkError(async (quiet) => {
        const ctl = new AbortController();
        const timer = setTimeout(() => ctl.abort(TIMED_OUT), timeoutMs);
        try {
          return await hookedFetch('/v1/cube/reduced_bin', {
            method: 'POST', headers: this.headers(true), body: JSON.stringify(req), signal: ctl.signal,
          }, quiet);
        } finally {
          clearTimeout(timer);          // the headers came (or not): the body has no deadline
        }
      }, { withinMs: RESEND_WITHIN_MS });
    } catch (e) {
      if (e && e.timedOut) {
        e.message = `the backend gave no reduced cube within ${(timeoutMs / 60000).toFixed(0)} min: load it again`;
      }
      throw e;
    }
    if (!resp.ok) throw await failed(resp, '/v1/cube/reduced_bin');
    if (resp.status === 202) {
      // The brick pyramid is not ready (reduced_info waits for it first).
      throw new Error('the cube\'s brick pyramid is still being built: load it again');
    }
    const h = (k) => resp.headers.get(k);
    const total = Number(h('Content-Length')) || 0;
    const tHeaders = performance.now();
    let buf;
    if (resp.body && onProgress && total) {
      const reader = resp.body.getReader();
      const out = new Uint8Array(total);
      let got = 0;
      for (;;) {
        const { done, value } = await reader.read();
        if (done) break;
        out.set(value, got);
        got += value.length;
        onProgress(got / total);
      }
      buf = out.buffer;
    } else {
      buf = await resp.arrayBuffer();
    }
    const encoding = h('X-Visivo-Scalar-Type');
    const dims = [Number(h('X-Visivo-Width')), Number(h('X-Visivo-Height')), Number(h('X-Visivo-Depth'))];
    const n = dims[0] * dims[1] * dims[2];
    const data = encoding === 'float16' ? new Uint16Array(buf, 0, n) : new Uint8Array(buf, 0, n);
    const thr = h('X-Visivo-Threshold');
    return {
      data, dims, encoding,
      lo: Number(h('X-Visivo-Range-Min')), hi: Number(h('X-Visivo-Range-Max')),
      factors: (h('X-Visivo-Bin-Factors') || '1,1,1').split(',').map(Number),
      fullShape: (h('X-Visivo-Full-Shape') || '').split(',').map(Number),
      mode: h('X-Visivo-Bin-Mode') || '',
      reducedThreshold: thr ? Number(thr) : null,
      bunit: h('X-Visivo-Bunit') || '',
      source: h('X-Visivo-Source') || 'cube',
      pyramidLevel: h('X-Visivo-Pyramid-Level') ? Number(h('X-Visivo-Pyramid-Level')) : null,
      fileRevision: h('X-Visivo-File-Revision') || '',
      bytes: buf.byteLength,
      timing: { serverMs: tHeaders - t0, totalMs: performance.now() - t0 },
    };
  }
}

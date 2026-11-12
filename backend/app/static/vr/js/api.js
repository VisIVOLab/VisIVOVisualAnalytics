// The VR page's calls to the backend that serves it (same origin).
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
 * path, fromAddress}.
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
    const kept = { session: p.session, dataset: p.dataset, path: p.path };
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
  pairingCache = { token: '', session: str(kept.session), dataset: str(kept.dataset), path: str(kept.path), fromAddress: false };
  return pairingCache;
}

/** Forgets the tab's pairing (the user opened another cube by hand). */
export function forgetPairing() {
  try { store('session')?.removeItem(PAIR_KEY); } catch { /* nothing kept */ }
  pairingCache = { token: '', session: '', dataset: '', path: '', fromAddress: false };
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

/** fetch(), with a rejected fetch turned into a `network` requestError and
 *  both outcomes reported to requestHooks. Non-ok responses are returned. */
export async function hookedFetch(path, init) {
  let resp;
  try {
    resp = await fetch(path, init);
  } catch (err) {
    const timedOut = init && init.signal && init.signal.aborted && init.signal.reason === TIMED_OUT;
    const e = requestError(0, timedOut ? 'the backend did not answer in time' : 'the backend did not answer');
    e.cause = err;
    if (init && init.signal && init.signal.aborted && !timedOut) e.aborted = true;   // cancelled by the page
    if (requestHooks.onFailure && !e.aborted) requestHooks.onFailure(e, path);
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
export async function boundedRequest(path, init, timeoutMs, signal, read) {
  if (!(timeoutMs > 0) && !signal) {
    const resp = await hookedFetch(path, init);
    return read(resp);
  }
  const ctl = new AbortController();
  const onAbort = () => ctl.abort(signal.reason);
  if (signal) { if (signal.aborted) onAbort(); else signal.addEventListener('abort', onAbort, { once: true }); }
  const timer = timeoutMs > 0 ? setTimeout(() => ctl.abort(TIMED_OUT), timeoutMs) : null;
  try {
    const resp = await hookedFetch(path, { ...init, signal: ctl.signal });
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
      if (ctl.signal.aborted && ctl.signal.reason !== TIMED_OUT) e.aborted = true;
      e.cause = err;
      if (requestHooks.onFailure && !e.aborted) requestHooks.onFailure(e, path);
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
    const resp = await fetch('/v1/vr/log', { method: 'POST', headers: this.headers(true), body: JSON.stringify(body) });
    return { status: resp.status, retryAfterS: Number(resp.headers.get('Retry-After')) || 0 };
  }

  async ping() {
    const resp = await fetch('/v1/ping');
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

  async _binary(path, body, contentType = 'application/json') {
    const headers = this.headers(false);
    headers['Content-Type'] = contentType;
    const resp = await hookedFetch(path, { method: 'POST', headers, body });
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

  reducedInfo(req) {
    return this._json('POST', '/v1/cube/reduced_info', req);
  }

  /** The reduced cube: {data: Uint8Array | Uint16Array, dims, lo, hi, ...}. */
  async reducedCube(req, onProgress) {
    const t0 = performance.now();
    const resp = await hookedFetch('/v1/cube/reduced_bin', {
      method: 'POST', headers: this.headers(true), body: JSON.stringify(req),
    });
    if (!resp.ok) throw await failed(resp, '/v1/cube/reduced_bin');
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
      bytes: buf.byteLength,
      timing: { serverMs: tHeaders - t0, totalMs: performance.now() - t0 },
    };
  }
}

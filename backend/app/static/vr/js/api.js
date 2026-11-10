// The VR page's calls to the backend that serves it (same origin).
//
// The token: read once from the URL fragment (#token=...), which a browser
// never sends to the server, so it is not in any access log; then removed from
// the address bar and history entry. It is kept in sessionStorage (this tab
// only), or in localStorage when "remember on this device" is ticked. It can
// also be typed. It travels only in the X-Visivo-Token header.

const KEY = 'visivo-vr-token';

function store(kind) {
  try { return kind === 'local' ? window.localStorage : window.sessionStorage; } catch { return null; }
}

let fragmentError = '';
/** Why the token in the fragment could not be read ('' when it could). */
export function tokenError() { return fragmentError; }

/** Token from the fragment, else from storage. Clears the fragment. */
export function takeToken() {
  const hash = window.location.hash || '';
  const m = hash.match(/(?:^#|&)token=([^&]+)/);
  if (m) {
    let token = '';
    try {
      token = decodeURIComponent(m[1]);
    } catch {
      // A malformed %-escape must not stop the page loading.
      fragmentError = 'The token in the address (#token=…) is malformed: paste it into the field.';
    }
    if (token) {
      try { store('session')?.setItem(KEY, token); } catch { /* private mode */ }
    }
    const rest = hash.replace(/(?:^#|&)token=[^&]+/, '').replace(/^&/, '');
    window.history.replaceState(null, '', window.location.pathname + window.location.search
      + (rest ? `#${rest}` : ''));
    return token;
  }
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

export class Backend {
  constructor(token) {
    this.token = token;
    this.session = '';
  }

  headers(json = true) {
    const h = { 'X-Visivo-Token': this.token };
    if (this.session) h['X-Visivo-Session'] = this.session;
    if (json) h['Content-Type'] = 'application/json';
    return h;
  }

  async _json(method, path, body) {
    const resp = await fetch(path, {
      method, headers: this.headers(body !== undefined),
      body: body === undefined ? undefined : JSON.stringify(body),
    });
    const text = await resp.text();
    let data;
    try { data = JSON.parse(text); } catch { data = { detail: text }; }
    if (!resp.ok) {
      const detail = typeof data.detail === 'string' ? data.detail : JSON.stringify(data.detail ?? data);
      throw new Error(`${resp.status}: ${detail}`);
    }
    return data;
  }

  async ping() {
    const resp = await fetch('/v1/ping');
    return resp.ok;
  }

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

  reducedInfo(req) {
    return this._json('POST', '/v1/cube/reduced_info', req);
  }

  /** The reduced cube: {data: Uint8Array | Uint16Array, dims, lo, hi, ...}. */
  async reducedCube(req, onProgress) {
    const t0 = performance.now();
    const resp = await fetch('/v1/cube/reduced_bin', {
      method: 'POST', headers: this.headers(true), body: JSON.stringify(req),
    });
    if (!resp.ok) throw new Error(`${resp.status}: ${await resp.text()}`);
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

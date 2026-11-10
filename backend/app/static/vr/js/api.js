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

  // ── Analysis on the full cube (issue #171) ────────────────────────────────

  async _binary(path, body, contentType = 'application/json') {
    const headers = this.headers(false);
    headers['Content-Type'] = contentType;
    const resp = await fetch(path, { method: 'POST', headers, body });
    if (!resp.ok) throw new Error(`${resp.status}: ${(await resp.text()).slice(0, 300)}`);
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

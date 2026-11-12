// HTTP and WebSockets over the pairing's data channels (issue #213; the
// framing is pair-wire.js). Two pieces, both independent of where the
// channels come from (a real RTCDataChannel in the public loader, a
// MessagePort to the loader in the page, a fake in the tests):
//
// - ApiClient: fetch() over the `api` channel. It answers a native Response
//   whose body streams as the chunks arrive, so the page's callers read it as
//   they read any fetch. A failure of the link rejects with a TypeError, as a
//   fetch does when nothing answers; an AbortSignal rejects with an
//   AbortError and sends {"t":"cancel"}. api.js turns these into its
//   `network` / `timedOut` / `aborted` errors exactly as for a direct fetch.
// - ChannelWebSocket: a WebSocket over one `ws` channel (readyState, protocol,
//   binaryType, send, close, the four events).
//
// Unit-tested in backend/tests/vr/pair-transport.test.mjs. Shipped byte for
// byte with the public loader as well (web/vr-loader/js/pair-transport.js).

import {
  PacedSender, bodyChunks, decodeChunk, toBytes, WsReassembler, wsMessageFrames, wsControlFrame,
  WS_CLOSE,
} from './pair-wire.js';

/** Header names the page never sends over the pairing: the bridge
 *  authenticates for it (the backend's token never reaches the headset). */
const DROPPED_HEADERS = new Set(['x-visivo-token']);

/** Response headers as a plain object, from an object or a list of pairs. */
function headerPairs(h) {
  if (!h) return [];
  if (Array.isArray(h)) return h.filter((p) => Array.isArray(p) && p.length === 2).map(([k, v]) => [String(k), String(v)]);
  if (typeof h === 'object') return Object.entries(h).map(([k, v]) => [k, Array.isArray(v) ? v.join(', ') : String(v)]);
  return [];
}

/** The request's headers as a plain object (lower-case names), without the token. */
export function requestHeaders(h) {
  const out = {};
  if (!h) return out;
  const put = (k, v) => { const n = String(k).toLowerCase(); if (!DROPPED_HEADERS.has(n)) out[n] = String(v); };
  if (typeof h.forEach === 'function' && !Array.isArray(h)) h.forEach((v, k) => put(k, v));
  else if (Array.isArray(h)) for (const [k, v] of h) put(k, v);
  else for (const [k, v] of Object.entries(h)) put(k, v);
  return out;
}

function abortError(signal) {
  const r = signal && signal.reason;
  if (r instanceof Error && r.name === 'AbortError') return r;
  try { return new DOMException('The operation was aborted.', 'AbortError'); } catch {
    const e = new Error('The operation was aborted.'); e.name = 'AbortError'; return e;
  }
}

const NULL_BODY = new Set([101, 103, 204, 205, 304]);

/**
 * fetch() over an `api` channel. `send(frame)` puts a frame on the channel
 * (a PacedSender, or a relay to one); frames that arrive go to receive().
 * With a PacedSender, `sender` (optional) lets a cancel take back the
 * request's frames still queued and go out first.
 * Request ids start at `idStart` and step by `idStep`, so two clients can
 * share one channel (the loader's own asset requests take the even ids, the
 * page's the odd ones). `isOpen()` false: requests fail at once, as a fetch
 * to a backend that is down.
 */
export class ApiClient {
  constructor(send, { idStart = 1, idStep = 1, isOpen = () => true, sender = null } = {}) {
    this._send = send;
    this.sender = sender;
    this.nextId = idStart;
    this.idStep = idStep;
    this.isOpen = isOpen;
    this.pending = new Map();   // id -> {resolve, reject, controller, status, signal, onAbort}
    this.stats = { requests: 0, bytesIn: 0, bytesOut: 0, cancels: 0 };
  }

  _id() {
    const id = this.nextId;
    this.nextId = (this.nextId + this.idStep) >>> 0;
    if (this.nextId === 0) this.nextId = this.idStep;
    return id;
  }

  /** fetch(path, init): `path` relative to the backend root, query included. */
  fetch(path, init = {}) {
    const signal = init.signal || null;
    if (signal && signal.aborted) return Promise.reject(abortError(signal));
    if (!this.isOpen()) return Promise.reject(new TypeError('the link to the computer is down'));
    let bytes = null;
    const method = String(init.method || 'GET').toUpperCase();
    try {
      if (init.body !== undefined && init.body !== null) bytes = toBytes(init.body);
    } catch (err) { return Promise.reject(err); }
    const id = this._id();
    this.stats.requests++;
    return new Promise((resolve, reject) => {
      const p = { resolve, reject, controller: null, status: 0, signal, onAbort: null, done: false };
      this.pending.set(id, p);
      if (signal) {
        p.onAbort = () => this._abort(id, abortError(signal));
        signal.addEventListener('abort', p.onAbort, { once: true });
      }
      this._send(JSON.stringify({ t: 'req', id, method, path: String(path), headers: requestHeaders(init.headers), body: !!bytes }));
      if (bytes) {
        for (const f of bodyChunks(id, bytes)) this._send(f);
        this.stats.bytesOut += bytes.byteLength;
      }
    });
  }

  _finish(id) {
    const p = this.pending.get(id);
    if (!p) return null;
    this.pending.delete(id);
    p.done = true;
    if (p.signal && p.onAbort) p.signal.removeEventListener('abort', p.onAbort);
    return p;
  }

  _fail(id, err) {
    const p = this._finish(id);
    if (!p) return;
    if (p.controller) { try { p.controller.error(err); } catch { /* closed */ } } else p.reject(err);
  }

  _abort(id, err) {
    if (!this.pending.has(id)) return;
    this.stats.cancels++;
    const cancel = JSON.stringify({ t: 'cancel', id });
    if (this.sender) {
      this.sender.unqueue(id);
      this.sender.urgent(cancel);
    } else this._send(cancel);
    this._fail(id, err);
  }

  /** A frame from the channel (text or ArrayBuffer). */
  receive(data) {
    if (typeof data === 'string') {
      let m;
      try { m = JSON.parse(data); } catch { return; }
      if (!m || !Number.isInteger(m.id)) return;
      const p = this.pending.get(m.id);
      if (!p) return;
      if (m.t === 'res' && !p.controller) this._respond(m.id, p, m);
      else if (m.t === 'err') this._fail(m.id, new TypeError(String(m.message || 'the bridge could not reach the backend')));
      else if (m.t === 'cancel') this._fail(m.id, new TypeError('the backend cancelled the request'));
      return;
    }
    const c = decodeChunk(data);
    if (!c) return;
    const p = this.pending.get(c.id);
    if (!p || !p.controller) return;     // a chunk after a cancel, or before its res: dropped
    this.stats.bytesIn += c.payload.byteLength;
    if (!p.nullBody && c.payload.byteLength) {
      try { p.controller.enqueue(c.payload.slice()); } catch { /* cancelled by the reader */ }
    }
    if (c.last) {
      this._finish(c.id);
      try { p.controller.close(); } catch { /* cancelled */ }
    }
  }

  _respond(id, p, m) {
    const status = Number(m.status) || 0;
    const client = this;
    const stream = new ReadableStream({
      start(controller) { p.controller = controller; },
      cancel() { client._abort(id, new TypeError('body cancelled')); },
    });
    p.nullBody = NULL_BODY.has(status);
    let resp;
    try {
      resp = new Response(p.nullBody ? null : stream, { status: status >= 200 && status <= 599 ? status : 502, headers: headerPairs(m.headers) });
    } catch (err) {
      this._abort(id, err);
      return;
    }
    p.status = status;
    p.resolve(resp);
  }

  /** The link is gone: every request in flight fails as a fetch to a vanished server. */
  failAll(why = 'the link to the computer dropped') {
    for (const id of [...this.pending.keys()]) this._fail(id, new TypeError(why));
  }
}

const CONNECTING = 0, OPEN = 1, CLOSING = 2, CLOSED = 3;

/**
 * A WebSocket over one `ws` channel. `channel` is created by the caller
 * (label `ws`, protocol JSON {"path","protocols"}) and has send(),
 * bufferedAmount, readyState ('connecting'|'open'|'closing'|'closed'),
 * close() and the 'open', 'message', 'close' and 'bufferedamountlow' events.
 * The socket opens when the backend's open frame (0x03) arrives, not when the
 * channel does: that is when the backend accepted it.
 */
export class ChannelWebSocket extends EventTarget {
  static CONNECTING = CONNECTING;
  static OPEN = OPEN;
  static CLOSING = CLOSING;
  static CLOSED = CLOSED;

  constructor(url, channel) {
    super();
    this.url = String(url);
    this.protocol = '';
    this.extensions = '';
    this.binaryType = 'blob';
    this.readyState = CONNECTING;
    this.onopen = null; this.onmessage = null; this.onclose = null; this.onerror = null;
    this.channel = channel;
    this.sender = new PacedSender(channel);
    this.rx = new WsReassembler();
    this._closeSent = false;
    this._closeTimer = null;
    try { channel.binaryType = 'arraybuffer'; } catch { /* fixed */ }
    channel.addEventListener('message', (e) => this._onFrame(e.data));
    channel.addEventListener('close', () => this._closed(1006, '', false));
  }

  get bufferedAmount() {
    const q = this.sender.queue.reduce((n, f) => n + f.byteLength, 0);
    return q + (this.channel.bufferedAmount || 0);
  }

  _emit(type, init) {
    const ev = type === 'message' ? new MessageEvent('message', init)
      : type === 'close' ? Object.assign(new Event('close'), init) : new Event(type);
    const h = this[`on${type}`];
    this.dispatchEvent(ev);
    if (typeof h === 'function') { try { h.call(this, ev); } catch (err) { setTimeout(() => { throw err; }); } }
  }

  _onFrame(data) {
    if (this.readyState === CLOSED || typeof data === 'string') return;
    const r = this.rx.push(data);
    if (!r) return;
    if (r.kind === 'error') { this._failNow(1002, r.why); return; }
    if (r.kind === 'open') {
      if (this.readyState !== CONNECTING) return;
      this.protocol = r.protocol;
      this.readyState = OPEN;
      this._emit('open');
      return;
    }
    if (r.kind === 'close') {
      // The backend closed (or echoed ours): echo once, then the channel goes.
      if (!this._closeSent) { this._closeSent = true; this.sender.send(wsControlFrame(WS_CLOSE, { code: r.code, reason: r.reason })); }
      this._closed(r.code, r.reason, true);
      return;
    }
    if (this.readyState !== OPEN) return;
    let data_ = r.data;
    if (r.kind === 'binary' && this.binaryType === 'blob' && typeof Blob !== 'undefined') data_ = new Blob([r.data]);
    this._emit('message', { data: data_ });
  }

  _failNow(code, reason) {
    try { this.channel.close(); } catch { /* closed */ }
    this._closed(code === 1002 ? 1006 : code, reason, false, true);
  }

  _closed(code, reason, wasClean, error = false) {
    if (this.readyState === CLOSED) return;
    this.readyState = CLOSED;
    clearTimeout(this._closeTimer);
    // Let the echo go out before the channel closes.
    setTimeout(() => { this.sender.close(); try { this.channel.close(); } catch { /* closed */ } }, 0);
    // As a browser's WebSocket: an abnormal closure fires 'error' first.
    if (error || !wasClean) this._emit('error');
    this._emit('close', { code, reason, wasClean });
  }

  send(data) {
    if (this.readyState === CONNECTING) {
      const e = new Error("Failed to execute 'send' on 'WebSocket': Still in CONNECTING state.");
      e.name = 'InvalidStateError';
      throw e;
    }
    if (this.readyState !== OPEN) return;
    if (typeof Blob !== 'undefined' && data instanceof Blob) {
      throw new TypeError('a Blob cannot be sent over the pairing: send an ArrayBuffer');
    }
    for (const f of wsMessageFrames(data)) this.sender.send(f);
  }

  close(code = 1000, reason = '') {
    if (this.readyState === CLOSING || this.readyState === CLOSED) return;
    if (this.readyState === CONNECTING) {
      // The backend has not accepted it yet: the channel just goes.
      this._failNow(1006, '');
      return;
    }
    this.readyState = CLOSING;
    this._closeSent = true;
    this.sender.send(wsControlFrame(WS_CLOSE, { code, reason: String(reason).slice(0, 123) }));
    // A backend that never echoes: closed anyway after a while.
    this._closeTimer = setTimeout(() => this._closed(1006, '', false), 5000);
  }
}
for (const [k, v] of Object.entries({ CONNECTING, OPEN, CLOSING, CLOSED })) ChannelWebSocket.prototype[k] = v;

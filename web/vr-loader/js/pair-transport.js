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

/** Methods a dropped link may send again after a resume (they change
 *  nothing). A request of another method may say it changes nothing too,
 *  with `idempotent: true` in its init (a field fetch() itself ignores). */
const IDEMPOTENT = new Set(['GET', 'HEAD']);

/**
 * fetch() over an `api` channel. `send(frame)` puts a frame on the channel
 * (a PacedSender, or a relay to one); frames that arrive go to receive().
 * With a PacedSender, `sender` (optional) lets a cancel take back the
 * request's frames still queued and go out first.
 * Request ids start at `idStart` and step by `idStep`, so two clients can
 * share one channel (the loader's own asset requests take the even ids, the
 * page's the odd ones). `isOpen()` false: requests fail at once, as a fetch
 * to a backend that is down.
 *
 * A dropped link that may come back (the loader resumes it): hold() keeps
 * the GET and HEAD requests in flight (and those marked `idempotent`) and
 * fails the others; resend() sends
 * the kept ones again once the link is back, and their callers see one
 * answer. A body that had begun goes on where it stopped only when both
 * answers carry the same strong ETag and Content-Length (the same bytes);
 * otherwise, and when the second body ends short, the body fails (a network
 * error the caller can retry). failAll() fails everything, kept or not.
 */
export class ApiClient {
  constructor(send, { idStart = 1, idStep = 1, isOpen = () => true, sender = null } = {}) {
    this._send = send;
    this.sender = sender;
    this.nextId = idStart;
    this.idStep = idStep;
    this.isOpen = isOpen;
    this.pending = new Map();   // id -> request state (see fetch)
    this.held = [];             // requests kept over a drop, to send again
    this.stats = { requests: 0, bytesIn: 0, bytesOut: 0, cancels: 0, resent: 0 };
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
    this.stats.requests++;
    return new Promise((resolve, reject) => {
      const p = {
        id: 0, resolve, reject, controller: null, status: 0, length: null, etag: null, signal, onAbort: null,
        method, path: String(path), headers: requestHeaders(init.headers), bytes,
        idempotent: IDEMPOTENT.has(method) || init.idempotent === true,
        gotRes: false, delivered: 0, skip: 0,
      };
      if (signal) {
        p.onAbort = () => this._abort(p, abortError(signal));
        signal.addEventListener('abort', p.onAbort, { once: true });
      }
      this._start(p);
    });
  }

  /** Sends request `p` under a new id. */
  _start(p) {
    p.id = this._id();
    p.gotRes = false;
    this.pending.set(p.id, p);
    this._send(JSON.stringify({ t: 'req', id: p.id, method: p.method, path: p.path, headers: p.headers, body: !!p.bytes }));
    if (p.bytes) {
      for (const f of bodyChunks(p.id, p.bytes)) this._send(f);
      this.stats.bytesOut += p.bytes.byteLength;
    }
  }

  _finish(p) {
    if (this.pending.get(p.id) === p) this.pending.delete(p.id);
    const i = this.held.indexOf(p);
    if (i >= 0) this.held.splice(i, 1);
    if (p.signal && p.onAbort) p.signal.removeEventListener('abort', p.onAbort);
    p.onAbort = null;
  }

  _fail(p, err) {
    this._finish(p);
    if (p.controller) { try { p.controller.error(err); } catch { /* closed */ } } else p.reject(err);
  }

  _abort(p, err) {
    const live = this.pending.get(p.id) === p;
    if (!live && !this.held.includes(p)) return;
    this.stats.cancels++;
    if (live) {
      // A held request is not on the wire: nothing to cancel there.
      const cancel = JSON.stringify({ t: 'cancel', id: p.id });
      if (this.sender) {
        this.sender.unqueue(p.id);
        this.sender.urgent(cancel);
      } else this._send(cancel);
    }
    this._fail(p, err);
  }

  /** A frame from the channel (text or ArrayBuffer). */
  receive(data) {
    if (typeof data === 'string') {
      let m;
      try { m = JSON.parse(data); } catch { return; }
      if (!m || !Number.isInteger(m.id)) return;
      const p = this.pending.get(m.id);
      if (!p) return;
      if (m.t === 'res' && !p.gotRes) this._respond(p, m);
      else if (m.t === 'err') this._fail(p, new TypeError(String(m.message || 'the bridge could not reach the backend')));
      else if (m.t === 'cancel') this._fail(p, new TypeError('the backend cancelled the request'));
      return;
    }
    const c = decodeChunk(data);
    if (!c) return;
    const p = this.pending.get(c.id);
    if (!p || !p.gotRes) return;     // a chunk after a cancel, or before its res: dropped
    this.stats.bytesIn += c.payload.byteLength;
    let payload = c.payload;
    if (p.skip) {
      // Sent again after a drop: what the caller already has is not repeated.
      const n = Math.min(p.skip, payload.byteLength);
      p.skip -= n;
      payload = payload.subarray(n);
    }
    if (!p.nullBody && payload.byteLength) {
      p.delivered += payload.byteLength;
      try { p.controller.enqueue(payload.slice()); } catch { /* cancelled by the reader */ }
    }
    if (c.last) {
      if (p.resent && !p.nullBody && (p.skip > 0 || (p.length !== null && p.delivered !== Number(p.length)))) {
        // The answer sent again ended before the caller had all of it.
        this._fail(p, new TypeError('the answer ended short after the link to the computer came back'));
        return;
      }
      this._finish(p);
      try { p.controller.close(); } catch { /* cancelled */ }
    }
  }

  _respond(p, m) {
    const status = Number(m.status) || 0;
    const headers = headerPairs(m.headers);
    const header = (name) => { const h = headers.find(([k]) => k.toLowerCase() === name); return h ? h[1].trim() : null; };
    const length = header('content-length');
    const etag = header('etag');
    if (p.controller) {
      // The answer to a request sent again after a drop, whose caller has the
      // first answer already: it must be the same answer, and when part of
      // its body was delivered, provably the same bytes.
      const strong = (t) => !!t && !/^W\//.test(t);
      const same = status === p.status && length === p.length
        && (p.delivered === 0 || (strong(etag) && etag === p.etag && length !== null));
      if (!same) {
        this._abort(p, new TypeError('the answer changed while the link to the computer was down'));
        return;
      }
      p.gotRes = true;
      return;
    }
    const client = this;
    const stream = new ReadableStream({
      start(controller) { p.controller = controller; },
      cancel() { client._abort(p, new TypeError('body cancelled')); },
    });
    // A HEAD answer may name a length but never has a body.
    p.nullBody = NULL_BODY.has(status) || p.method === 'HEAD';
    let resp;
    try {
      resp = new Response(p.nullBody ? null : stream, { status: status >= 200 && status <= 599 ? status : 502, headers });
    } catch (err) {
      p.controller = null;
      this._abort(p, err);
      return;
    }
    p.gotRes = true;
    p.status = status;
    p.length = length;
    p.etag = etag;
    p.resolve(resp);
  }

  /** The link dropped and may come back: GET and HEAD requests in flight are
   *  kept for resend(), the others fail as a fetch to a vanished server. */
  hold(why = 'the link to the computer dropped') {
    for (const p of [...this.pending.values()]) {
      this.pending.delete(p.id);
      if (p.idempotent) this.held.push(p);
      else this._fail(p, new TypeError(why));
    }
  }

  /** The link is back: the kept requests go again (new ids). */
  resend() {
    const again = this.held.splice(0);
    for (const p of again) {
      p.skip = p.delivered;
      p.resent = true;
      this.stats.resent++;
      this._start(p);
    }
  }

  /** The link is gone for good: every request fails, kept ones too. */
  failAll(why = 'the link to the computer dropped') {
    for (const p of [...this.pending.values(), ...this.held]) this._fail(p, new TypeError(why));
    this.held = [];
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

// The framing of the pairing's WebRTC data channels (issue #213, contract
// `visivo-pair/1` in issue #210, with its amendment 1). Pure functions and a
// paced sender, no browser API beyond TextEncoder: unit-tested in
// backend/tests/vr/pair-transport.test.mjs.
//
// - `api` (HTTP over the channel): JSON text frames for the control messages
//   ({"t":"req"|"res"|"cancel"|"err", "id", ...}); binary frames for body
//   chunks: a 4-byte big-endian request id, 1 flag byte (bit 0 = last
//   chunk), then at most 16 KiB of payload. A body always ends with a chunk
//   flagged last, which may be empty.
// - `ws` (one channel per page WebSocket): every frame binary; byte 0 the
//   type (0x01 text, 0x02 binary, 0x03 open, 0x04 close), byte 1 the flags
//   (bit 0 = last fragment), then at most 16 KiB of payload. A message is one
//   or more frames of its type; open and close are one frame flagged last.
// - Both: a sender pauses while the channel's bufferedAmount is above 1 MiB.
//
// This file is also shipped, byte for byte, with the public loader
// (web/vr-loader/js/pair-wire.js): a test checks that the copies agree.

export const CHUNK_BYTES = 16 * 1024;
export const HIGH_WATER_BYTES = 1024 * 1024;

export const WS_TEXT = 0x01;
export const WS_BINARY = 0x02;
export const WS_OPEN = 0x03;
export const WS_CLOSE = 0x04;
export const FLAG_LAST = 0x01;

const enc = new TextEncoder();
const dec = new TextDecoder();

/** Bytes of a request or message body: a string (UTF-8), an ArrayBuffer, a
 *  view of one, or nothing (an empty array). */
export function toBytes(body) {
  if (body === undefined || body === null) return new Uint8Array(0);
  if (typeof body === 'string') return enc.encode(body);
  if (body instanceof ArrayBuffer || Object.prototype.toString.call(body) === '[object ArrayBuffer]') return new Uint8Array(body);
  if (ArrayBuffer.isView(body)) return new Uint8Array(body.buffer, body.byteOffset, body.byteLength);
  if (body instanceof URLSearchParams) return enc.encode(body.toString());
  throw new TypeError(`a body of type ${Object.prototype.toString.call(body)} cannot go over the pairing`);
}

/** One api body chunk: id (uint32), last flag, payload (≤ CHUNK_BYTES). */
export function encodeChunk(id, last, payload) {
  const p = payload || new Uint8Array(0);
  if (p.byteLength > CHUNK_BYTES) throw new RangeError('chunk payload over 16 KiB');
  const out = new Uint8Array(5 + p.byteLength);
  new DataView(out.buffer).setUint32(0, id >>> 0, false);
  out[4] = last ? FLAG_LAST : 0;
  out.set(p, 5);
  return out.buffer;
}

/** {id, last, payload: Uint8Array} of an api body chunk, or null when too short. */
export function decodeChunk(buf) {
  const u = buf instanceof Uint8Array ? buf : new Uint8Array(buf);
  if (u.byteLength < 5) return null;
  const id = new DataView(u.buffer, u.byteOffset, u.byteLength).getUint32(0, false);
  return { id, last: (u[4] & FLAG_LAST) !== 0, payload: u.subarray(5) };
}

/** The api frames of a body: chunks of at most 16 KiB, the last flagged
 *  (an empty body is one empty chunk flagged last). */
export function bodyChunks(id, bytes) {
  const out = [];
  const n = bytes.byteLength;
  if (!n) return [encodeChunk(id, true, null)];
  for (let off = 0; off < n; off += CHUNK_BYTES) {
    const end = Math.min(n, off + CHUNK_BYTES);
    out.push(encodeChunk(id, end === n, bytes.subarray(off, end)));
  }
  return out;
}

/** The request id an api frame belongs to (text or binary), or null. */
export function frameId(data) {
  if (typeof data === 'string') {
    try { const m = JSON.parse(data); return Number.isInteger(m && m.id) ? m.id : null; } catch { return null; }
  }
  const c = decodeChunk(data);
  return c ? c.id : null;
}

/** One ws frame: type, last flag, payload (≤ CHUNK_BYTES). */
export function encodeWsFrame(type, last, payload) {
  const p = payload || new Uint8Array(0);
  if (p.byteLength > CHUNK_BYTES) throw new RangeError('ws fragment payload over 16 KiB');
  const out = new Uint8Array(2 + p.byteLength);
  out[0] = type;
  out[1] = last ? FLAG_LAST : 0;
  out.set(p, 2);
  return out.buffer;
}

/** {type, last, payload: Uint8Array} of a ws frame, or null when too short. */
export function decodeWsFrame(buf) {
  const u = buf instanceof Uint8Array ? buf : new Uint8Array(buf);
  if (u.byteLength < 2) return null;
  return { type: u[0], last: (u[1] & FLAG_LAST) !== 0, payload: u.subarray(2) };
}

/** The ws frames of a message (text: a string; binary: bytes), fragmented. */
export function wsMessageFrames(data) {
  const text = typeof data === 'string';
  const bytes = toBytes(data);
  const type = text ? WS_TEXT : WS_BINARY;
  const out = [];
  const n = bytes.byteLength;
  if (!n) return [encodeWsFrame(type, true, null)];
  for (let off = 0; off < n; off += CHUNK_BYTES) {
    const end = Math.min(n, off + CHUNK_BYTES);
    out.push(encodeWsFrame(type, end === n, bytes.subarray(off, end)));
  }
  return out;
}

/** A control frame (open or close), JSON in one frame flagged last. */
export function wsControlFrame(type, obj) {
  return encodeWsFrame(type, true, enc.encode(JSON.stringify(obj)));
}

export function decodeJson(bytes) {
  try { return JSON.parse(dec.decode(bytes)); } catch { return null; }
}

export function decodeText(bytes) { return dec.decode(bytes); }

/**
 * Reassembles ws messages from frames. push(frame) returns
 * {kind: 'text'|'binary', data} for a complete message, {kind: 'open', protocol},
 * {kind: 'close', code, reason}, null while a message is incomplete, or
 * {kind: 'error', why} for a frame that breaks the framing.
 */
export class WsReassembler {
  constructor() { this.type = 0; this.parts = []; this.bytes = 0; }

  push(buf) {
    const f = decodeWsFrame(buf);
    if (!f) return { kind: 'error', why: 'a frame shorter than its header' };
    if (f.type === WS_OPEN || f.type === WS_CLOSE) {
      if (this.parts.length) return { kind: 'error', why: 'a control frame inside a fragmented message' };
      const o = decodeJson(f.payload) || {};
      if (f.type === WS_OPEN) return { kind: 'open', protocol: typeof o.protocol === 'string' ? o.protocol : '' };
      return { kind: 'close', code: Number.isInteger(o.code) ? o.code : 1005, reason: typeof o.reason === 'string' ? o.reason : '' };
    }
    if (f.type !== WS_TEXT && f.type !== WS_BINARY) return { kind: 'error', why: `unknown frame type ${f.type}` };
    if (this.parts.length && f.type !== this.type) return { kind: 'error', why: 'a fragment of another type inside a message' };
    this.type = f.type;
    if (!f.last) {
      // Copied: the frame's buffer is not ours to keep a view of.
      this.parts.push(f.payload.slice());
      this.bytes += f.payload.byteLength;
      return null;
    }
    let whole;
    if (!this.parts.length) whole = f.payload;
    else {
      whole = new Uint8Array(this.bytes + f.payload.byteLength);
      let off = 0;
      for (const p of this.parts) { whole.set(p, off); off += p.byteLength; }
      whole.set(f.payload, off);
    }
    this.parts = [];
    this.bytes = 0;
    const type = this.type;
    this.type = 0;
    if (type === WS_TEXT) return { kind: 'text', data: dec.decode(whole) };
    return { kind: 'binary', data: whole.byteOffset === 0 && whole.byteLength === whole.buffer.byteLength ? whole.buffer : whole.slice().buffer };
  }
}

/**
 * Sends frames on a data channel (or anything with send(), bufferedAmount and
 * a 'bufferedamountlow' event), in order, pausing while bufferedAmount is
 * above `high` (1 MiB). A timer polls as well, for channels that never fire
 * the event. `onError(err)` gets a send that threw (the channel closed).
 */
export class PacedSender {
  constructor(channel, { high = HIGH_WATER_BYTES, pollMs = 50, onError = null } = {}) {
    this.channel = channel;
    this.high = high;
    this.pollMs = pollMs;
    this.onError = onError;
    this.queue = [];
    this.waiting = false;
    this.timer = null;
    this.sentBytes = 0;
    this.pauses = 0;
    try { channel.bufferedAmountLowThreshold = high; } catch { /* not settable */ }
    this._low = () => this.flush();
    if (channel.addEventListener) channel.addEventListener('bufferedamountlow', this._low);
  }

  get queued() { return this.queue.length; }

  send(frame) {
    this.queue.push(frame);
    if (!this.waiting) this.flush();
  }

  flush() {
    this.waiting = false;
    if (this.timer) { clearTimeout(this.timer); this.timer = null; }
    while (this.queue.length) {
      if ((this.channel.bufferedAmount || 0) > this.high) {
        this.waiting = true;
        this.pauses++;
        this.timer = setTimeout(() => this.flush(), this.pollMs);
        return;
      }
      const f = this.queue.shift();
      try {
        this.channel.send(f);
        this.sentBytes += typeof f === 'string' ? f.length : f.byteLength;
      } catch (err) {
        this.queue = [];
        if (this.onError) this.onError(err);
        return;
      }
    }
  }

  /** Removes the queued api frames of request `id` (it was cancelled: what
   *  has not gone out yet never will). Returns how many were removed. */
  unqueue(id) {
    const before = this.queue.length;
    this.queue = this.queue.filter((f) => frameId(f) !== id);
    return before - this.queue.length;
  }

  /** Sends `frame` before everything queued (a cancel). */
  urgent(frame) {
    this.queue.unshift(frame);
    if (!this.waiting) this.flush();
  }

  /** Drops what is queued (the channel is gone). */
  close() {
    this.queue = [];
    if (this.timer) { clearTimeout(this.timer); this.timer = null; }
    this.waiting = false;
    if (this.channel.removeEventListener) this.channel.removeEventListener('bufferedamountlow', this._low);
  }
}

// The public loader's pure decisions (issue #213): where the pairing service
// is, the code in the address, the app's address in the iframe, what a
// failure means for the link. No browser API: unit-tested in
// backend/tests/vr/pair-loader.test.mjs.

/** True for a loopback host name (a loader on the developer's machine). */
export function isLoopbackHost(hostname) {
  const h = String(hostname || '').replace(/^\[|\]$/g, '').toLowerCase();
  return h === 'localhost' || h === '::1' || /^127\./.test(h);
}

/**
 * The pairing service's base address (ws:// or wss://, no trailing slash).
 * `configured`: config.json's "pair_service", absolute (ws/wss/http/https)
 * or relative to config.json (`configUrl`), which follows the loader's
 * directory. `override`: ?pair_service=, honoured only on a loopback loader
 * (development): a public loader cannot be pointed elsewhere by a link.
 * '' when there is none.
 */
export function serviceUrl(configured, override, loc, configUrl) {
  const pick = override && isLoopbackHost(loc.hostname) ? override : configured;
  if (!pick || typeof pick !== 'string') return '';
  let u;
  try { u = new URL(pick, configUrl || loc.href); } catch { return ''; }
  if (u.protocol === 'https:') u.protocol = 'wss:';
  else if (u.protocol === 'http:') u.protocol = 'ws:';
  if (u.protocol !== 'wss:' && u.protocol !== 'ws:') return '';
  // A plain ws:// service only for a loopback loader (development).
  if (u.protocol === 'ws:' && !isLoopbackHost(loc.hostname) && !isLoopbackHost(u.hostname)) return '';
  u.hash = '';
  return u.href.replace(/\/+$/, '');
}

/** The service's host, for the diagnostics (no path, no query). */
export function serviceHost(url) {
  try { return new URL(url).host; } catch { return ''; }
}

/** A code as typed: spaces and dashes out, upper case for a pairing code
 *  (6 characters); a resume code is kept as it is. */
export function normaliseCode(text) {
  const t = String(text || '').trim().replace(/[\s-]+/g, '');
  return t.length <= 8 ? t.toUpperCase() : t;
}

/** {code, rest}: the #pair=CODE of the loader's address, and the fragment
 *  without it (to put back in the address bar). */
export function pairFromHash(hash) {
  const text = String(hash || '').replace(/^#/, '');
  let code = '';
  const keep = [];
  for (const part of text.split('&')) {
    if (!part) continue;
    if (part.startsWith('pair=')) {
      if (!code) { try { code = decodeURIComponent(part.slice(5)); } catch { code = ''; } }
    } else keep.push(part);
  }
  return { code: normaliseCode(code), rest: keep.join('&') };
}

/** The loader's query passed on to the app (its own pair_service left out). */
export function appQuery(search) {
  const p = new URLSearchParams(String(search || ''));
  p.delete('pair_service');
  p.delete('transport');
  const rest = p.toString().replace(/=(?=&|$)/g, '');
  return `?transport=pair${rest ? `&${rest}` : ''}`;
}

/**
 * The app's fragment from the welcome: the values the desktop's link puts
 * after #, the token excepted (the bridge authenticates). `params` (contract
 * amendment 1) carries all of them; session, dataset and path also stand on
 * their own.
 */
export function appFragment(welcome) {
  const w = welcome || {};
  const vals = { ...(w.params && typeof w.params === 'object' ? w.params : {}) };
  for (const k of ['session', 'dataset', 'path']) if (w[k] && !vals[k]) vals[k] = w[k];
  delete vals.token;
  const parts = [];
  for (const [k, v] of Object.entries(vals)) {
    if (v === undefined || v === null || v === '') continue;
    if (!/^[A-Za-z0-9_-]{1,32}$/.test(k)) continue;
    parts.push(`${k}=${encodeURIComponent(String(v))}`);
  }
  return parts.join('&');
}

/** The app's address in the iframe, relative to the loader's directory. */
export function appAddress(tag, search, welcome) {
  const frag = appFragment(welcome);
  return `app/${tag}/vr/${appQuery(search)}${frag ? `#${frag}` : ''}`;
}

/**
 * What a failure of a join means: 'retry' (later, with the same code),
 * 'lost' (the code is no good: a new one is needed). `reason` is the
 * service's error reason, or the loader's own: 'timeout', 'ice-failed',
 * 'service-unreachable', 'bye'.
 */
export function failureKind(reason) {
  switch (reason) {
    case 'unknown-code': case 'expired': case 'used': case 'rejected': case 'bye':
      return 'lost';
    default:
      return 'retry';
  }
}

/** The wait before resume attempt `n` (1, 2, …), in ms: 1, 2, 4, 8, then 15 s. */
export function resumeDelayMs(n, reason = '') {
  const base = Math.min(15000, 1000 * 2 ** Math.max(0, n - 1));
  return reason === 'rate-limited' ? Math.max(base, 30000) : base;
}

/** How long a dropped link is resumed before the page asks for a new code. */
export const RESUME_BUDGET_MS = 3 * 60 * 1000;

/** The user's words for a reason. */
export function reasonText(reason) {
  return {
    'unknown-code': 'This code is not known: check it, or ask the computer for a new one',
    expired: 'This code has expired: ask the computer for a new one',
    used: 'This code was already used: ask the computer for a new one',
    'rate-limited': 'Too many attempts: wait a minute, then try again',
    'host-gone': 'The computer is not connected to the pairing service (is VisIVO Visual Analytics running?)',
    rejected: 'The computer refused this headset',
    timeout: 'The computer did not answer in time',
    'ice-failed': 'No network path to the computer was found',
    'service-unreachable': 'The pairing service cannot be reached',
    'not-configured': 'This loader has no pairing service configured (config.json)',
    bye: 'The computer ended the pairing',
    'no-resume': 'The computer gave this headset no resume code, so the link cannot be resumed: enter a new code',
    'resume-expired': 'The pairing could not be resumed in time',
  }[reason] || `The pairing failed (${reason || 'unknown reason'})`;
}

/** The user's words for a `bye` from the backend (its reason). */
export function byeText(reason) {
  return {
    disconnected: 'The computer disconnected this headset',
    'backend-stopping': 'VisIVO Visual Analytics on the computer was closed',
  }[reason] || `The computer ended the pairing${reason ? ` (${reason})` : ''}`;
}

/** A resume code kept for this tab: {code, expiresAt}, or null when absent or stale. */
export function keptResume(text, now = Date.now()) {
  try {
    const r = JSON.parse(text || 'null');
    if (r && typeof r.code === 'string' && r.code && Number(r.expiresAt) > now + 5000) return { code: r.code, expiresAt: Number(r.expiresAt) };
  } catch { /* nothing kept */ }
  return null;
}

/** The ICE candidate pair in use, from getStats() reports (an iterable of
 *  stats objects): {candidates: 'host/srflx udp', rttMs} or {}. */
export function linkFigures(reports) {
  const all = [...reports];
  const byId = new Map(all.map((r) => [r.id, r]));
  const transport = all.find((r) => r.type === 'transport' && r.selectedCandidatePairId);
  let pair = transport ? byId.get(transport.selectedCandidatePairId) : null;
  if (!pair) pair = all.find((r) => r.type === 'candidate-pair' && (r.selected || r.nominated) && r.state === 'succeeded');
  if (!pair) return {};
  const l = byId.get(pair.localCandidateId) || {};
  const r = byId.get(pair.remoteCandidateId) || {};
  const out = { candidates: `${l.candidateType || '?'}/${r.candidateType || '?'} ${l.protocol || ''}${l.relayProtocol ? ` via ${l.relayProtocol} relay` : ''}`.trim() };
  if (Number.isFinite(pair.currentRoundTripTime)) out.rttMs = 1000 * pair.currentRoundTripTime;
  return out;
}

/**
 * The page's api request ids on the wire. Each page document numbers its
 * requests from 1, and a reloaded page is a new document while the link (and
 * the backend's view of its ids) stays: the loader gives every page request a
 * wire id of its own (odd, never reused while the link lives), sends the
 * answer only to the document that asked, and lets a replaced document's
 * requests be cancelled. `doc` is any key naming a document (its port).
 */
export class AppIdMap {
  constructor({ start = 1, step = 2 } = {}) {
    this.next = start;
    this.start = start;
    this.step = step;
    this.toWire = new Map();   // doc -> Map(pageId -> wireId)
    this.toPage = new Map();   // wireId -> {doc, id}
  }

  _alloc() {
    const id = this.next;
    this.next = (this.next + this.step) >>> 0;
    if (this.next < this.step) this.next = this.start;
    return id;
  }

  _end(wire) {
    const p = this.toPage.get(wire);
    if (!p) return;
    this.toPage.delete(wire);
    const m = this.toWire.get(p.doc);
    if (m) { m.delete(p.id); if (!m.size) this.toWire.delete(p.doc); }
  }

  /** A frame from document `doc`: {frame, wire, cancel} for the wire, or null (dropped). */
  outgoing(doc, data) {
    if (typeof data === 'string') {
      let msg;
      try { msg = JSON.parse(data); } catch { return null; }
      if (!msg || !Number.isInteger(msg.id)) return null;
      let m = this.toWire.get(doc);
      let wire;
      if (msg.t === 'req') {
        wire = this._alloc();
        if (!m) { m = new Map(); this.toWire.set(doc, m); }
        const old = m.get(msg.id);
        if (old !== undefined) this.toPage.delete(old);
        m.set(msg.id, wire);
        this.toPage.set(wire, { doc, id: msg.id });
      } else {
        wire = m && m.get(msg.id);
        if (wire === undefined) return null;
        if (msg.t === 'cancel') this._end(wire);
      }
      return { frame: JSON.stringify({ ...msg, id: wire }), wire, cancel: msg.t === 'cancel' };
    }
    const u = new Uint8Array(data);
    if (u.byteLength < 5) return null;
    const m = this.toWire.get(doc);
    const page = new DataView(u.buffer, u.byteOffset, u.byteLength).getUint32(0, false);
    const wire = m && m.get(page);
    if (wire === undefined) return null;
    const out = u.slice();
    new DataView(out.buffer).setUint32(0, wire, false);
    return { frame: out.buffer, wire, cancel: false };
  }

  /** A frame from the backend for a page request: {doc, frame} with the
   *  page's own id, or null (a request that ended or was forgotten). */
  incoming(data) {
    if (typeof data === 'string') {
      let msg;
      try { msg = JSON.parse(data); } catch { return null; }
      const p = msg && this.toPage.get(msg.id);
      if (!p) return null;
      if (msg.t === 'err' || msg.t === 'cancel') this._end(msg.id);
      return { doc: p.doc, frame: JSON.stringify({ ...msg, id: p.id }) };
    }
    const u = new Uint8Array(data);
    if (u.byteLength < 5) return null;
    const wire = new DataView(u.buffer, u.byteOffset, u.byteLength).getUint32(0, false);
    const p = this.toPage.get(wire);
    if (!p) return null;
    if (u[4] & 1) this._end(wire);
    const out = u.byteOffset === 0 && u.byteLength === u.buffer.byteLength ? u : u.slice();
    new DataView(out.buffer, out.byteOffset, out.byteLength).setUint32(0, p.id, false);
    return { doc: p.doc, frame: out.buffer };
  }

  /** Forgets document `doc`; returns the wire ids of its requests still open (to cancel). */
  forget(doc) {
    const m = this.toWire.get(doc);
    if (!m) return [];
    const wires = [...m.values()];
    for (const w of wires) this.toPage.delete(w);
    this.toWire.delete(doc);
    return wires;
  }

  /** The link changed: every request is gone. */
  clear() { this.toWire.clear(); this.toPage.clear(); }

  get open() { return this.toPage.size; }
}

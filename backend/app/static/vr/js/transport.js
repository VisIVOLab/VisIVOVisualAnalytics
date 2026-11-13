// How the page reaches its backend (issue #213): the single place every
// request and WebSocket of the page goes through (api.js hookedFetch,
// Backend.log/ping, share.js, refine-client.js).
//
// - Direct (the default): fetch() and WebSocket on the page's own origin,
//   exactly as before: the page served by the backend at <prefix>/vr/, reached
//   through adb reverse (localhost) or a cluster's HTTPS portal.
// - Paired: the page runs inside the public loader (web/vr-loader/), in a
//   same-origin iframe opened at app/<tag>/vr/?transport=pair. The loader
//   holds the WebRTC link to the backend's bridge; this module talks to it
//   through a MessagePort: the `api` frames are relayed as they are (this
//   page's requests take the odd ids), and every WebSocket gets a `ws`
//   channel of its own, created by the loader and relayed over a port of its
//   own. The backend's token never reaches the page: the bridge
//   authenticates, so X-Visivo-Token headers and token subprotocols are
//   dropped here.
//
// Port messages, page -> loader: {k:'api', data} (an api frame), {k:'ws',
// protocol, port} (open a ws channel), {k:'new-code'} (the user asks to pair
// again), {k:'stats', n}. Loader -> page: {k:'api', data}, {k:'link', state:
// 'up'|'down'|'lost', reason, info}, {k:'stats', n, stats}. A ws port: {k:
// 'open'|'close'} and {k:'data', data} both ways.

import { ApiClient, ChannelWebSocket } from './pair-transport.js';
import { basePrefix } from './base-path.js';

/** What the token field holds in paired mode (dropped before it is sent):
 *  the page's code paths that need "a token" work unchanged. */
export const PAIRED_TOKEN = 'visivo-paired-no-token';

/** True for an address the loader opens the page at (?transport=pair). */
export function pairedRequested(search) {
  try { return new URLSearchParams(String(search || '')).get('transport') === 'pair'; } catch { return false; }
}

/**
 * The backend path (relative to its root, query included) of an address the
 * page made with apiUrl()/wsUrl(), or null when it is not under the page's
 * prefix on the page's own host. In paired mode the prefix is the loader's
 * app/<tag> directory, which stands for the backend's root.
 */
export function backendPath(url, loc) {
  let u;
  try { u = new URL(String(url), loc.href); } catch { return null; }
  if (u.host !== loc.host) return null;
  const prefix = basePrefix(loc.pathname);
  if (prefix && !(u.pathname === prefix || u.pathname.startsWith(`${prefix}/`))) return null;
  const rest = u.pathname.slice(prefix.length) || '/';
  return `${rest}${u.search}`;
}

/** A WebSocket's path and protocols as sent over the pairing: no token in
 *  the query, no token subprotocol. */
export function socketRequest(path, protocols) {
  const [p, q = ''] = String(path).split('?');
  const params = new URLSearchParams(q);
  params.delete('token');
  const query = params.toString();
  const list = (Array.isArray(protocols) ? protocols : protocols ? [protocols] : [])
    .map(String).filter((s) => !s.startsWith('visivo-token.'));
  return { path: `${p}${query ? `?${query}` : ''}`, protocols: list };
}

/** A data channel's face over a MessagePort to the loader's relay. */
export class PortChannel extends EventTarget {
  constructor(port) {
    super();
    this.port = port;
    this.readyState = 'connecting';
    this.bufferedAmount = 0;          // the loader paces the real channel
    this.binaryType = 'arraybuffer';
    port.onmessage = (e) => {
      const m = e.data || {};
      if (m.k === 'open' && this.readyState === 'connecting') { this.readyState = 'open'; this.dispatchEvent(new Event('open')); }
      else if (m.k === 'data') this.dispatchEvent(new MessageEvent('message', { data: m.data }));
      else if (m.k === 'close') this._gone();
    };
  }

  send(data) {
    if (this.readyState === 'closed') throw new Error('the channel is closed');
    this.port.postMessage({ k: 'data', data }, data instanceof ArrayBuffer ? [data] : []);
  }

  close() {
    if (this.readyState === 'closed') return;
    try { this.port.postMessage({ k: 'close' }); } catch { /* gone */ }
    this._gone();
  }

  _gone() {
    if (this.readyState === 'closed') return;
    this.readyState = 'closed';
    this.dispatchEvent(new Event('close'));
    try { this.port.close(); } catch { /* closed */ }
  }
}

// ── The page's state ───────────────────────────────────────────────────────
const win = typeof window !== 'undefined' ? window : null;
const paired = !!(win && win.parent && win.parent !== win && pairedRequested(win.location.search));

const link = {
  port: null,
  state: paired ? 'connecting' : 'direct',
  reason: '',
  info: {},            // from the loader: service host, ICE, RTT, build
  listeners: [],
  client: null,
  ready: null,
  stats: null,
  statsN: 0,
};

/** True when the page runs in the public loader over the pairing. */
export function isPaired() { return paired; }

/** Called with (state, reason) when the pairing's link changes: 'up',
 *  'down' (the loader is resuming), 'lost' (a new code is needed). */
export function onLinkChange(fn) { link.listeners.push(fn); }

function setLink(state, reason = '', info = null) {
  const changed = state !== link.state || reason !== link.reason;
  link.state = state;
  link.reason = reason;
  if (info) link.info = info;
  if (link.client) {
    // A drop the loader resumes: GET and HEAD requests in flight are kept and
    // sent again once the link is back; the others fail (as a fetch to a
    // vanished server). A pairing lost for good fails them all.
    if (state === 'up') link.client.resend();
    else if (state === 'lost') link.client.failAll('the pairing with the computer is lost');
    else link.client.hold('the link to the computer dropped');
  }
  if (changed) for (const fn of link.listeners) { try { fn(state, reason); } catch { /* a listener's own error */ } }
}

function connectToLoader() {
  link.ready = new Promise((resolve) => {
    const onMsg = (e) => {
      if (e.source !== win.parent || e.origin !== win.location.origin) return;
      if (!e.data || e.data.visivoPair !== 'port' || !e.ports || !e.ports[0]) return;
      win.removeEventListener('message', onMsg);
      clearInterval(hello);
      const port = e.ports[0];
      link.port = port;
      link.client = new ApiClient((frame) => {
        port.postMessage({ k: 'api', data: frame }, frame instanceof ArrayBuffer ? [frame] : []);
      }, { idStart: 1, idStep: 2, isOpen: () => link.state === 'up' });
      port.onmessage = (ev) => {
        const m = ev.data || {};
        if (m.k === 'api') link.client.receive(m.data);
        else if (m.k === 'link') setLink(m.state, m.reason || '', m.info || null);
        else if (m.k === 'stats') link.stats = { ...m.stats, at: Date.now() };
      };
      // The link's state comes with the port, so a request made at once knows it.
      setLink(e.data.state || 'down', e.data.reason || '', e.data.info || null);
      resolve();
    };
    win.addEventListener('message', onMsg);
    // Asked until the loader answers: it may still be wiring its side. The
    // nonce names this document, so the loader answers it once.
    const nonce = Array.from(crypto.getRandomValues(new Uint8Array(12)), (b) => b.toString(16).padStart(2, '0')).join('');
    const say = () => win.parent.postMessage({ visivoPair: 'hello', nonce }, win.location.origin);
    const hello = setInterval(say, 250);
    say();
  });
  // Diagnostics: the link's figures, refreshed while paired.
  setInterval(() => { if (link.port) link.port.postMessage({ k: 'stats', n: ++link.statsN }); }, 5000);
}
if (paired) connectToLoader();

/** Waits (at most `ms`) for the loader's port; false when it did not come. */
async function portReady(ms = 10000) {
  if (link.port) return true;
  let t;
  const late = new Promise((r) => { t = setTimeout(() => r(false), ms); });
  const ok = await Promise.race([link.ready.then(() => true), late]);
  clearTimeout(t);
  return ok;
}

/** fetch() for the page's backend requests (an address from apiUrl()). */
export async function pageFetch(url, init = {}) {
  if (!paired) return fetch(url, init);
  if (!(await portReady())) throw new TypeError('the public loader did not answer');
  const path = backendPath(url, win.location);
  if (path === null) throw new TypeError(`not a backend address: ${url}`);
  return link.client.fetch(path, init);
}

/** new WebSocket() for the page's backend sockets (an address from wsUrl()). */
export function openSocket(url, protocols) {
  if (!paired) return new WebSocket(url, protocols);
  const path = backendPath(url, win.location);
  const req = socketRequest(path === null ? '/' : path, protocols);
  const mc = new MessageChannel();
  const ch = new PortChannel(mc.port1);
  const ws = new ChannelWebSocket(url, ch);
  if (path === null || link.state !== 'up') {
    // No link (or not a backend address): it fails as a socket to a server that is down.
    setTimeout(() => ch._gone(), 0);
    return ws;
  }
  link.port.postMessage({ k: 'ws', protocol: JSON.stringify(req), port: mc.port2 }, [mc.port2]);
  return ws;
}

/** The user asks to pair again (the 'pair-lost' notice's action). */
export function requestNewCode() {
  if (link.port) link.port.postMessage({ k: 'new-code' });
}

/** One line for the diagnostics: which transport, and how it is doing. */
export function transportInfo() {
  if (!paired) {
    const loc = win ? win.location : { protocol: '', host: '' };
    return `direct HTTP (${loc.protocol}//${loc.host})`;
  }
  const i = link.info || {};
  const s = link.stats || {};
  const parts = [`WebRTC data channel via the pairing service ${i.service || '?'}`, `link ${link.state}${link.reason ? ` (${link.reason})` : ''}`];
  if (s.candidates) parts.push(`ICE ${s.candidates}`);
  if (Number.isFinite(s.rttMs)) parts.push(`round trip ${s.rttMs.toFixed(0)} ms`);
  if (s.resumes) parts.push(`resumed ${s.resumes}×`);
  if (link.client) {
    const c = link.client.stats;
    parts.push(`${c.requests} requests, ${(c.bytesIn / 1e6).toFixed(1)} MB in, ${(c.bytesOut / 1e6).toFixed(1)} MB out, ${c.cancels} cancelled`);
  }
  return parts.join('; ');
}

/** For the tests and the debug hooks. */
export function linkState() { return { paired, state: link.state, reason: link.reason, info: link.info, stats: link.stats }; }

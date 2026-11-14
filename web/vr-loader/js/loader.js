// The public loader (issue #213): pairs the headset with a backend through
// the pairing service, holds the WebRTC link, and runs the VR page fetched
// from that backend in a same-origin iframe.
//
// Why an iframe: the peer connection must outlive the page it serves. The VR
// page reloads itself (a new build, a lost GPU), and a top-level navigation
// would drop the link and its data channels. The loader stays; the page lives
// in <iframe allow="xr-spatial-tracking"> on the same origin, so WebXR is
// allowed to it by the permissions policy (same-origin frames are, and the
// attribute says so explicitly), and its "Enter VR" press is a user
// activation inside that frame.
//
// Why a service worker: the page's files are ES modules that import each
// other by relative address. The worker (sw.js) answers every request under
// app/<tag>/ by asking this loader, which fetches GET /<path> from the
// backend over the `api` channel; files under vr/b/<build>/ are kept in
// Cache Storage named after that build, so a page of the backend's build
// loads from the cache and never mixes two builds.
//
// The page talks to this loader through a MessagePort (see the protocol in
// backend/app/static/vr/js/transport.js); its api frames are relayed with
// their ids mapped to wire ids of their own (odd, AppIdMap: a reloaded page
// numbers its requests from 1 again), the loader's own requests take the even
// ids.

import { ApiClient } from './pair-transport.js';
import { PacedSender, frameId } from './pair-wire.js';
import {
  serviceUrl, serviceHost, normaliseCode, pairFromHash, appAddress, failureKind, resumeDelayMs,
  RESUME_BUDGET_MS, reasonText, byeText, keptResume, linkFigures, AppIdMap, launchMode,
} from './loader-core.js';

const $ = (id) => document.getElementById(id);
const RESUME_KEY = 'visivo-vr-loader-resume';
const JOIN_TIMEOUT_MS = 20000;
/** How long one of the page's files may take, resumes included (sw.js waits 30 s). */
const ASSET_BUDGET_MS = 25000;

const state = {
  service: '',
  // The app's directory for this tab (app/<tag>/): 8 random characters.
  tag: Array.from(crypto.getRandomValues(new Uint8Array(8)), (b) => 'abcdefghijklmnopqrstuvwxyz0123456789'[b % 36]).join(''),
  appNonce: '',           // the page document the port was given to
  link: null,             // the live Link, or null
  linkState: 'none',      // 'none' | 'connecting' | 'up' | 'down' | 'lost'
  reason: '',
  welcome: null,
  resume: null,           // {code, expiresAt}
  appPort: null,
  appWindow: null,
  resumes: 0,
  resuming: false,
  resumeGen: 0,           // bumped by a new pairing: a resume loop of before stops
};

function setStatus(text, level = '') {
  const s = $('status');
  s.textContent = text;
  s.className = level;
}

function store() { try { return window.sessionStorage; } catch { return null; } }

function keepResume(welcome) {
  // No resume code (the service gave none in time): a drop then asks for a
  // new code at once; an older code is not reused for this pairing.
  if (!welcome || !welcome.resume_code || !(Number(welcome.resume_expires_in) > 0)) { dropResume(); return; }
  const expiresAt = Date.now() + 1000 * (Number(welcome.resume_expires_in) || 0);
  state.resume = { code: String(welcome.resume_code), expiresAt };
  try { store()?.setItem(RESUME_KEY, JSON.stringify(state.resume)); } catch { /* this load only */ }
}

function dropResume() {
  state.resume = null;
  try { store()?.removeItem(RESUME_KEY); } catch { /* nothing kept */ }
}

// ── The link: signalling, the peer, the channels ───────────────────────────

/**
 * One WebRTC link: joins the pairing service with `code`, sets up the peer
 * and the `ctl` and `api` channels, says hello (with `resume` when it
 * resumes) and resolves with the backend's welcome. Rejects with an Error
 * whose `reason` is the service's or one of the loader's own.
 */
class Link {
  constructor(service, onDrop) {
    this.service = service;
    this.onDrop = onDrop;
    this.pc = null; this.ctl = null; this.api = null; this.sig = null;
    this.sender = null;
    this.client = null;
    this.relays = new Set();
    this.ids = new AppIdMap({ start: 1, step: 2 });
    this.dropped = false;
    this.welcome = null;
  }

  connect(code, resume) {
    return new Promise((resolve, reject) => {
      let settled = false;
      const fail = (reason, detail = '') => {
        if (settled) return;
        settled = true;
        clearTimeout(timer);
        this.close(`join failed: ${reason}`);
        const e = new Error(detail || reasonText(reason));
        e.reason = reason;
        reject(e);
      };
      const timer = setTimeout(() => fail('timeout'), JOIN_TIMEOUT_MS);
      let sig;
      try { sig = new WebSocket(`${this.service}/v1/join`); } catch { fail('service-unreachable'); return; }
      this.sig = sig;
      const say = (type, body) => { if (sig.readyState === 1) sig.send(JSON.stringify({ v: 1, type, ...body })); };
      sig.onerror = () => { if (!this.pc || !this.welcome) fail('service-unreachable'); };
      sig.onclose = () => { if (!this.welcome && !settled && !this.pc) fail('service-unreachable'); };
      sig.onmessage = async (ev) => {
        let m;
        try { m = JSON.parse(ev.data); } catch { return; }
        if (!m || m.v !== 1) return;
        try {
          if (m.type === 'ice_servers' && !this.pc) {
            await this._peer(m.ice_servers || [], (c) => say('ice', { candidate: c }), resume, (w) => {
              if (settled) return;
              settled = true;
              clearTimeout(timer);
              try { sig.close(); } catch { /* closed */ }
              resolve(w);
            }, fail);
            const offer = await this.pc.createOffer();
            await this.pc.setLocalDescription(offer);
            say('join', { code, offer: this.pc.localDescription.sdp });
          } else if (m.type === 'answer' && this.pc) {
            await this.pc.setRemoteDescription({ type: 'answer', sdp: m.sdp });
          } else if (m.type === 'ice' && this.pc && m.candidate) {
            await this.pc.addIceCandidate(m.candidate);
          } else if (m.type === 'error') {
            fail(m.reason || 'rejected');
          }
        } catch (err) {
          fail('ice-failed', String(err && err.message || err));
        }
      };
    });
  }

  async _peer(iceServers, sendIce, resume, onWelcome, fail) {
    const pc = new RTCPeerConnection({ iceServers });
    this.pc = pc;
    pc.onicecandidate = (e) => { if (e.candidate) sendIce(e.candidate.toJSON ? e.candidate.toJSON() : e.candidate); };
    pc.onconnectionstatechange = () => {
      const s = pc.connectionState;
      if (s === 'failed' || s === 'closed') {
        if (!this.welcome) fail('ice-failed'); else this._drop('the link failed');
      } else if (s === 'disconnected') {
        // Often back within seconds (a Wi-Fi roam): given 5 s.
        clearTimeout(this._disc);
        this._disc = setTimeout(() => { if (pc.connectionState === 'disconnected') this._drop('the link was interrupted'); }, 5000);
      }
    };
    const ctl = pc.createDataChannel('ctl', { ordered: true });
    const api = pc.createDataChannel('api', { ordered: true });
    api.binaryType = 'arraybuffer';
    this.ctl = ctl; this.api = api;
    this.sender = new PacedSender(api, { onError: () => this._drop('the api channel failed') });
    this.client = new ApiClient((f) => this.sender.send(f),
      { idStart: 2, idStep: 2, sender: this.sender, isOpen: () => api.readyState === 'open' && !this.dropped });
    ctl.onopen = () => ctl.send(JSON.stringify({ t: 'hello', resume: resume || null }));
    ctl.onmessage = (e) => {
      let m;
      try { m = JSON.parse(e.data); } catch { return; }
      if (m.t === 'welcome' && !this.welcome) { this.welcome = m; onWelcome(m); }
      else if (m.t === 'bye') this._drop(m.reason || 'the computer ended the pairing', true);
    };
    // Which channel, in what state of the connection: what tells a channel
    // the bridge refused from a link that went.
    const gone = (label) => () => {
      // Closed by this loader (closedBy), or by the other side / the network.
      if (this.welcome) this._drop(`the ${label} channel closed (connection ${pc.connectionState}${this.closedBy ? `, closed here: ${this.closedBy}` : ', not closed here'})`);
    };
    ctl.onclose = gone('ctl');
    api.onclose = gone('api');
    api.onmessage = (e) => this._route(e.data);
  }

  /** An api frame from the backend: the loader's (even id) or a page
   *  document's (odd), which goes to that document only. */
  _route(data) {
    const id = frameId(data);
    if (id === null) return;
    if (id % 2 === 0) { this.client.receive(data); return; }
    const r = this.ids.incoming(data);
    if (!r || r.doc !== state.appPort) return;
    r.doc.postMessage({ k: 'api', data: r.frame }, r.frame instanceof ArrayBuffer ? [r.frame] : []);
  }

  /** An api frame from page document `doc`; a cancel takes back what of its
   *  request is still queued and goes out first. */
  fromApp(doc, data) {
    if (this.dropped || !this.sender) return;
    const r = this.ids.outgoing(doc, data);
    if (!r) return;
    if (r.cancel) {
      this.sender.unqueue(r.wire);
      this.sender.urgent(r.frame);
    } else this.sender.send(r.frame);
  }

  /** Page document `doc` is gone (a reload): its requests are cancelled and
   *  its WebSockets closed. */
  forgetDoc(doc) {
    for (const wire of this.ids.forget(doc)) {
      if (this.sender && !this.dropped) {
        this.sender.unqueue(wire);
        this.sender.urgent(JSON.stringify({ t: 'cancel', id: wire }));
      }
    }
    for (const r of [...this.relays]) if (r.doc === doc) r.end();
  }

  /** A `ws` channel for page document `doc`, relayed over `port`. */
  openWs(protocol, port, doc) {
    if (this.dropped || !this.pc) { port.postMessage({ k: 'close' }); port.close(); return; }
    let dc;
    try { dc = this.pc.createDataChannel('ws', { ordered: true, protocol }); } catch {
      port.postMessage({ k: 'close' }); port.close(); return;
    }
    dc.binaryType = 'arraybuffer';
    const sender = new PacedSender(dc, { onError: () => { try { dc.close(); } catch { /* closed */ } } });
    const relay = { dc, port, sender, doc };
    this.relays.add(relay);
    let closed = false;
    const end = () => {
      if (closed) return;
      closed = true;
      this.relays.delete(relay);
      sender.close();
      try { port.postMessage({ k: 'close' }); } catch { /* gone */ }
      try { port.close(); } catch { /* closed */ }
      try { dc.close(); } catch { /* closed */ }
    };
    relay.end = end;
    dc.onopen = () => port.postMessage({ k: 'open' });
    dc.onmessage = (e) => port.postMessage({ k: 'data', data: e.data }, e.data instanceof ArrayBuffer ? [e.data] : []);
    dc.onclose = end;
    port.onmessage = (e) => {
      const m = e.data || {};
      if (m.k === 'data' && dc.readyState !== 'closed') {
        // Sent once the channel is open (queued until then).
        if (dc.readyState === 'open') sender.send(m.data);
        else dc.addEventListener('open', () => sender.send(m.data), { once: true });
      } else if (m.k === 'close') end();
    };
  }

  _drop(why, bye = false) {
    if (this.dropped) return;
    this.dropped = true;
    this.close(`dropped: ${why}`);
    if (this.onDrop) this.onDrop(why, bye);
  }

  close(why = 'replaced') {
    if (!this.closedBy) this.closedBy = why;
    clearTimeout(this._disc);
    for (const r of [...this.relays]) r.end();
    this.ids.clear();
    if (this.client) this.client.failAll('the link to the computer dropped');
    if (this.sender) this.sender.close();
    for (const x of [this.ctl, this.api, this.pc, this.sig]) { try { if (x) x.close(); } catch { /* closed */ } }
  }

  async figures() {
    if (!this.pc) return {};
    try { return linkFigures((await this.pc.getStats()).values()); } catch { return {}; }
  }
}

// ── The link's life: pair, resume, ask for a new code ──────────────────────

function tellApp(extra = {}) {
  if (!state.appPort) return;
  state.appPort.postMessage({ k: 'link', state: state.linkState, reason: state.reason, info: linkInfo(), ...extra });
}

function linkInfo() {
  return { service: serviceHost(state.service), build: state.welcome ? state.welcome.page_build : '', resumes: state.resumes };
}

function setLinkState(s, reason = '') {
  // For a debugging session (DevTools, or a CDP-driven check): the link's story.
  console.info(`[vr-loader] ${new Date().toISOString()} link ${s}${reason ? `: ${reason}` : ''}`);
  state.linkState = s;
  state.reason = reason;
  tellApp();
  $('link-state').textContent = s === 'up' ? `paired (${serviceHost(state.service)})` : s;
}

/** Pairs with a code typed or from #pair=: a new app in the iframe. */
async function pair(code) {
  if (!state.service) { showForm(reasonText('not-configured'), 'error'); return; }
  if (state.link) { state.link.onDrop = null; state.link.close('a new pairing'); state.link = null; }
  state.resuming = false;
  state.resumeGen++;
  setLinkState('connecting');
  setStatus('Connecting to the computer…');
  $('connect').disabled = true;
  const link = new Link(state.service, onDrop);
  try {
    const welcome = await link.connect(code, null);
    state.link = link;
    state.welcome = welcome;
    keepResume(welcome);
    keepBuild(welcome.page_build);
    setLinkState('up');
    openApp(welcome);
  } catch (err) {
    setLinkState(state.appWindow ? 'lost' : 'none', err.reason || '');
    showForm(err.message, 'error');
  } finally {
    $('connect').disabled = false;
  }
}

/** The link dropped: resumed with the resume code, else a new code. */
function onDrop(why, bye) {
  if (!state.link) return;
  state.link = null;
  if (bye) {
    dropResume();
    setLinkState('lost', byeText(why));
    return;
  }
  setLinkState('down', why);
  resumeLoop();
}

async function resumeLoop() {
  if (state.resuming) return;
  state.resuming = true;
  const gen = ++state.resumeGen;
  const mine = () => state.resuming && gen === state.resumeGen;
  const t0 = Date.now();
  let n = 0;
  let reason = '';
  while (mine()) {
    const r = state.resume;
    if (!r) { reason = 'no-resume'; break; }
    if (r.expiresAt <= Date.now()) { reason = 'expired'; break; }
    n++;
    await new Promise((res) => setTimeout(res, n === 1 ? 300 : resumeDelayMs(n - 1, reason)));
    if (!mine()) return;
    const link = new Link(state.service, onDrop);
    try {
      const welcome = await link.connect(r.code, r.code);
      if (!mine()) { link.onDrop = null; link.close('a new pairing took over the resume'); return; }
      state.link = link;
      state.resuming = false;
      state.welcome = welcome;
      state.resumes++;
      keepResume(welcome);
      keepBuild(welcome.page_build);
      setLinkState('up');
      if (!state.appWindow) openApp(welcome);
      return;
    } catch (err) {
      reason = err.reason || '';
      if (failureKind(reason) === 'lost') break;
      if (Date.now() - t0 > RESUME_BUDGET_MS) { reason = 'resume-expired'; break; }
      if (!mine()) return;
      setLinkState('down', `${reasonText(reason)}; trying again (attempt ${n})`);
    }
  }
  // A new pairing took over meanwhile: its state is not this loop's to set.
  if (gen !== state.resumeGen) return;
  state.resuming = false;
  dropResume();
  setLinkState('lost', reasonText(reason));
  // Without a page on show (a reloaded loader), the loader asks itself; with
  // one, the page's 'pair-lost' notice does (its action opens the form).
  if ($('app').hidden) showForm(reasonText(reason), 'error');
}

// ── The app in the iframe ──────────────────────────────────────────────────

function openApp(welcome) {
  const frame = $('app');
  // Launched from the installed app's icon: the page is told (?launch=app), so
  // it tries to enter VR by itself once its cube is on show (issue #237).
  frame.src = appAddress(state.tag, window.location.search, welcome,
    launchMode(typeof window.matchMedia === 'function' ? (q) => window.matchMedia(q) : null));
  frame.hidden = false;
  $('home').hidden = true;
  state.appWindow = frame.contentWindow;
}

window.addEventListener('message', (e) => {
  const frame = $('app');
  if (e.origin !== window.location.origin || e.source !== frame.contentWindow) return;
  if (!e.data || e.data.visivoPair !== 'hello') return;
  // The page says hello until it has its port: a repeat from the same
  // document is ignored (a second port would close the one it took).
  const nonce = String(e.data.nonce || '');
  if (nonce && nonce === state.appNonce && state.appPort) return;
  state.appNonce = nonce;
  // A (re)loaded page: a fresh port; the previous document's requests are
  // cancelled and its WebSockets closed.
  if (state.appPort) {
    if (state.link) state.link.forgetDoc(state.appPort);
    try { state.appPort.close(); } catch { /* closed */ }
  }
  const mc = new MessageChannel();
  state.appPort = mc.port1;
  state.appWindow = frame.contentWindow;
  mc.port1.onmessage = (ev) => onAppMessage(ev, mc.port1);
  frame.contentWindow.postMessage({ visivoPair: 'port', state: state.linkState === 'connecting' ? 'down' : state.linkState,
    reason: state.reason, info: linkInfo() }, window.location.origin, [mc.port2]);
});

async function onAppMessage(ev, doc) {
  if (doc !== state.appPort) return;           // a replaced document
  const m = ev.data || {};
  if (m.k === 'api') { if (state.link) state.link.fromApp(doc, m.data); }
  else if (m.k === 'ws') {
    const port = ev.ports && ev.ports[0] ? ev.ports[0] : m.port;
    if (!port) return;
    if (state.link) state.link.openWs(m.protocol, port, doc); else { port.postMessage({ k: 'close' }); port.close(); }
  } else if (m.k === 'new-code') showForm('Enter the new code the computer shows.', '');
  else if (m.k === 'stats') {
    const fig = state.link ? await state.link.figures() : {};
    if (state.appPort) state.appPort.postMessage({ k: 'stats', n: m.n, stats: { ...fig, resumes: state.resumes, state: state.linkState } });
  }
}

// ── The service worker: the app's files ───────────────────────────────────

async function registerWorker() {
  if (!('serviceWorker' in navigator)) throw new Error('This browser has no service workers: the loader cannot run the page.');
  navigator.serviceWorker.addEventListener('message', onWorkerMessage);
  await navigator.serviceWorker.register('sw.js', { scope: './' });
  await navigator.serviceWorker.ready;
}

function keepBuild(build) {
  if (!build) return;
  navigator.serviceWorker.ready.then((reg) => reg.active && reg.active.postMessage({ t: 'keep-build', build })).catch(() => {});
}

/** The worker asks for a file of the app (GET over the api channel). */
async function onWorkerMessage(e) {
  const m = e.data || {};
  const reply = e.ports && e.ports[0];
  if (m.t !== 'get' || !reply) return;
  if (m.tag !== state.tag) { reply.postMessage({ notMine: true }); return; }
  const text = (status, msg) => reply.postMessage({ status, headers: [['content-type', 'text/plain']],
    body: new TextEncoder().encode(msg).buffer });
  // A GET of the page's files: sent again when the link drops under it and
  // comes back (a resume). The whole of it (waits, requests, reads) within
  // ASSET_BUDGET_MS, under the worker's own 30 s, so the worker always gets
  // this answer rather than its timeout.
  const ac = new AbortController();
  const timer = setTimeout(() => ac.abort(), ASSET_BUDGET_MS);
  let last = 'no link to the computer';
  try {
    while (!ac.signal.aborted && state.linkState !== 'lost' && state.linkState !== 'none') {
      if (!(state.link && state.linkState === 'up')) { await new Promise((r) => setTimeout(r, 100)); continue; }
      try {
        const resp = await state.link.client.fetch(m.path, { method: 'GET', signal: ac.signal });
        const body = await resp.arrayBuffer();
        if (ac.signal.aborted) break;
        reply.postMessage({ status: resp.status, headers: [...resp.headers], body }, [body]);
        return;
      } catch (err) {
        last = String(err.message || err);
        if (!(err instanceof TypeError)) break;          // an abort (the budget) or a bad answer
        await new Promise((r) => setTimeout(r, 100));
      }
    }
  } finally {
    clearTimeout(timer);
  }
  text(ac.signal.aborted ? 504 : state.link ? 502 : 503, ac.signal.aborted ? 'the computer did not send the file in time' : last);
}

// ── The page ───────────────────────────────────────────────────────────────

function showForm(message, level) {
  $('pair-form').hidden = false;
  $('home').hidden = false;
  $('home').classList.toggle('over', !$('app').hidden);
  setStatus(message || 'Enter the code shown on the computer.', level);
  $('code').focus();
}

async function loadConfig() {
  const url = new URL('config.json', window.location.href);
  try {
    const r = await fetch(url, { cache: 'no-store' });
    return { cfg: r.ok ? await r.json() : {}, url: url.href };
  } catch { return { cfg: {}, url: url.href }; }
}

async function start() {
  const { cfg, url } = await loadConfig();
  const override = new URLSearchParams(window.location.search).get('pair_service') || '';
  state.service = serviceUrl(cfg.pair_service, override, window.location, url);
  $('service').textContent = state.service ? serviceHost(state.service) : 'not configured';
  $('pair-form').addEventListener('submit', (e) => {
    e.preventDefault();
    const code = normaliseCode($('code').value);
    $('code').value = '';
    if (!code) { setStatus('Enter the code shown on the computer.', 'error'); return; }
    $('pair-form').hidden = true;
    pair(code);
  });
  $('close-form').addEventListener('click', () => { if (!$('app').hidden) $('home').hidden = true; });
  try { await registerWorker(); } catch (err) { setStatus(err.message, 'error'); return; }
  const { code, rest } = pairFromHash(window.location.hash);
  if (code || window.location.hash) {
    // The code leaves the address bar (and the history entry) at once.
    window.history.replaceState(null, '', `${window.location.pathname}${window.location.search}${rest ? `#${rest}` : ''}`);
  }
  if (!state.service) { showForm(reasonText('not-configured'), 'error'); return; }
  if (code) { $('pair-form').hidden = true; pair(code); return; }
  const kept = keptResume(store()?.getItem(RESUME_KEY));
  if (kept) {
    // A reload of this tab: the pairing goes on without a code.
    state.resume = kept;
    $('pair-form').hidden = true;
    setStatus('Reconnecting to the computer…');
    state.link = null;
    setLinkState('down', 'the loader was reloaded');
    resumeLoop();
    return;
  }
  showForm('', '');
}

// The page's own lifecycle: a browser that freezes or hides the page (Quest
// Browser sent to the background, a headless browser under load) can close
// its peer connections; the link's story then says why.
state.lifecycle = [];
for (const type of ['freeze', 'resume', 'visibilitychange', 'pagehide', 'pageshow']) {
  const target = type === 'pagehide' || type === 'pageshow' ? window : document;
  target.addEventListener(type, () => {
    const what = type === 'visibilitychange' ? `visibility ${document.visibilityState}` : type;
    state.lifecycle.push({ t: Date.now(), what });
    if (state.lifecycle.length > 20) state.lifecycle.shift();
    console.info(`[vr-loader] ${new Date().toISOString()} page ${what}`);
  });
}

window.__vrLoader = { state, pair };
start();

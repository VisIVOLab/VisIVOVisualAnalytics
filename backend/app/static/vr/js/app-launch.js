// The headset app (issue #255): the VR page packaged as an immersive Horizon
// OS app (tools/quest-app). The desktop starts it with ONE launch whose
// address is /vr/?launch=immersive#ticket=…: a one-time, short-lived ticket,
// never the token (Android keeps a launch's address in dumpsys). The page
// takes the ticket out of the address at once, trades it for the token, the
// desktop's session and the cube (POST /v1/vr/launch/exchange), and enters VR
// at load, before the cube, with the loading state shown inside VR. Launched
// from the app's icon (no ticket) it enters an empty room that says how to
// send a cube from the computer.
//
// Leaving VR ends the app (the browser closes it with the session), so the
// way out is labelled as quitting, and the page never ends its session to
// make way for another page: a second launch's page hands its grant to the
// page in VR over the one-page channel (single-page.js) instead.
//
// Pure, with fetch and the clock passed in: unit-tested in
// backend/tests/vr/app-launch.test.mjs.

/** The query of the app's own launches (app.json startPath). */
export const IMMERSIVE = 'immersive';
/** The label of the way out of VR when leaving it ends the app. */
export const QUIT_LABEL = 'Quit VisIVO VR';
/** The deadline of the exchange (it answers in milliseconds over adb). */
export const EXCHANGE_TIMEOUT_MS = 8000;
/** The message a stood-down page sends the page in VR with its launch. */
export const GRANT_MESSAGE = 'launch-grant';

const TICKET = /^[A-Za-z0-9_-]{32,128}$/;
const LAUNCH = /^[A-Za-z0-9_-]{8,32}$/;

/** The page was started as the immersive app (`launch=immersive`). */
export function launchedImmersive(search) {
  try { return new URLSearchParams(String(search || '')).get('launch') === IMMERSIVE; } catch { return false; }
}

/** The page runs in the headset app opened by its QR-code scanner (issue
 *  #383: the public loader, paired by code): leaving VR goes back to the
 *  app's scanner, it does not quit. The notices' `app` is then this value
 *  instead of true. */
export const APP_PAIRED = 'paired';

/** The way out of VR, as the user reads it: quitting the app (the desktop's
 *  USB launch), or leaving VR (a page, or the app back to its scanner). */
export function exitLabel(app) { return app && app !== APP_PAIRED ? QUIT_LABEL : 'Exit VR'; }

/** A ticket as the backend issues it (else ''). */
export function cleanTicket(text) {
  const s = String(text || '').trim();
  return TICKET.test(s) ? s : '';
}

/**
 * The exchange's answer checked: {token, session, launch, cube} with cube
 * {dataset_id, path, kind, box} or null; null when it is not one.
 */
export function validGrant(g) {
  if (!g || typeof g !== 'object') return null;
  const str = (v, max) => (typeof v === 'string' && v.length > 0 && v.length <= max ? v : '');
  const token = str(g.token, 4096), session = str(g.session, 256), launch = str(g.launch, 32);
  if (!token || !session || !LAUNCH.test(launch)) return null;
  let cube = null;
  if (g.cube && typeof g.cube === 'object') {
    const dataset = str(g.cube.dataset_id, 128), path = str(g.cube.path, 4096);
    if (!dataset || !path) return null;
    const kind = str(g.cube.kind, 32) || 'cube';
    const box = Array.isArray(g.cube.box) ? g.cube.box : null;
    // A sky catalogue's placement, the desktop window's (issue #260).
    cube = { dataset_id: dataset, path, kind, box };
    if (g.cube.sky && typeof g.cube.sky === 'object') cube.sky = g.cube.sky;
  } else if (g.cube != null) {
    return null;
  }
  return { token, session, launch, cube };
}

/**
 * Trades `ticket` for the grant, once. `fetch(url, init)` and `url` (the
 * exchange route under the page's prefix) are passed in. Resolves to the
 * grant; rejects with an Error whose `kind` is 'expired' (404: unknown, used
 * or expired), 'busy' (429), 'network' (no answer in time) or 'failed'.
 */
export async function exchangeTicket(ticket, { fetch, url, timeoutMs = EXCHANGE_TIMEOUT_MS, setTimer = setTimeout, clearTimer = clearTimeout }) {
  const t = cleanTicket(ticket);
  if (!t) throw Object.assign(new Error('the launch ticket is malformed'), { kind: 'expired' });
  const ac = typeof AbortController === 'function' ? new AbortController() : null;
  // One deadline over the whole exchange, the body included: a body that
  // never comes must not keep the app out of VR.
  let expired = false;
  let onExpire = null;
  const deadline = new Promise((_, reject) => { onExpire = reject; });
  deadline.catch(() => {});
  const timer = setTimer(() => {
    expired = true;
    if (ac) ac.abort();
    onExpire(Object.assign(new Error('the backend did not answer the launch'), { kind: 'network' }));
  }, timeoutMs);
  let resp, body = null;
  try {
    try {
      resp = await Promise.race([fetch(url, { method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ ticket: t }), cache: 'no-store', credentials: 'omit', signal: ac ? ac.signal : undefined }), deadline]);
    } catch {
      throw Object.assign(new Error('the backend did not answer the launch'), { kind: 'network' });
    }
    if (resp.status === 404) throw Object.assign(new Error('this launch has expired or was already used'), { kind: 'expired', status: 404 });
    if (resp.status === 429) throw Object.assign(new Error('too many launches in a minute'), { kind: 'busy', status: 429 });
    if (!resp.ok) throw Object.assign(new Error(`the backend refused the launch (${resp.status})`), { kind: 'failed', status: resp.status });
    try { body = await Promise.race([resp.json(), deadline]); } catch {
      if (expired) throw Object.assign(new Error('the backend did not answer the launch'), { kind: 'network' });
      body = null;
    }
  } finally {
    clearTimer(timer);
  }
  const grant = validGrant(body);
  if (!grant) throw Object.assign(new Error('the backend\'s answer to the launch is not one'), { kind: 'failed' });
  return grant;
}

/**
 * What the room says, from the app's state: {kind, ctx} for the page's
 * notices (lifecycle.js noticeFor 'app-…'), or null when nothing is to be
 * said (a cube on show, not the app). `s`: {immersive, exchanging,
 * error ({kind, message}), connected (a token), loading, shown, name,
 * origin, prep}. `prep`: the brick pyramid build the load waits for
 * ({progress, ms, silentMs}, issue #329), shown while loading.
 */
export function roomState(s) {
  if (!s || !s.immersive || s.shown) return null;
  if (s.exchanging) return { kind: 'app-loading', ctx: { name: '', origin: s.origin } };
  if (s.error) return { kind: 'app-failed', ctx: { why: s.error.kind || 'failed', detail: String(s.error.message || ''), origin: s.origin } };
  if (s.loading) {
    return { kind: 'app-loading', ctx: { name: s.name || '', origin: s.origin, ...(s.prep ? { prep: s.prep } : {}) } };
  }
  return { kind: 'app-empty', ctx: { connected: !!s.connected, origin: s.origin } };
}

/** The host:port the app reached the computer at (the page's own origin). */
export function originLabel(loc) {
  const host = String((loc && loc.host) || '');
  return host || 'localhost';
}

/** A grant message for the page in VR (to: its page id). */
export function grantMessage(to, grant) {
  return { type: GRANT_MESSAGE, to: String(to || ''), grant };
}

/** The grant of a message addressed to page `me`, checked; else null. */
export function grantFromMessage(m, me) {
  if (!m || typeof m !== 'object' || m.type !== GRANT_MESSAGE || !m.to || m.to !== me) return null;
  return validGrant(m.grant);
}

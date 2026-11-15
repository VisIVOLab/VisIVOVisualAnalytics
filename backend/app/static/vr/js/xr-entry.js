// Fewer Enter VR presses (issue #237). WebXR's requestSession needs a user
// gesture (a transient activation of the page's own frame), with two ways
// round it on the Quest:
//
// - An installed web app launched from its icon: Meta's guidance for
//   PWAs is to request the session right after load, which the Quest Browser
//   accepts as if the launch were the gesture. The page tries it once, as
//   soon as its first cube is on show, when it was launched as an app; a
//   refusal (no activation) is quiet, and the Enter VR button stays.
//   Inside the public loader the page is an iframe of the installed app:
//   the loader tells it so (?launch=app), but whether the browser lets the
//   iframe use the launch is the browser's call: the refusal is the fallback.
// - The immersive headset app (#255, `launch=immersive`): the browser grants
//   requestSession with no gesture at any time (measured on a Quest 3, #254),
//   so the page enters at load, before its cube (the loading state is shown
//   in VR), once its launch is settled (the ticket exchanged or refused).
// - navigator.xr.offerSession (Quest Browser; Chromium behind a flag): the
//   page offers a session and the browser shows its own Enter VR (in the
//   address bar), which is the gesture. Feature-detected, never assumed.
//   Issue #291: an offer cannot be taken back (the API has no withdraw; a
//   second offerSession only replaces the first, measured on a Quest 3), and
//   a Quest Browser holding one was seen to refuse the page's own
//   requestSession ("already an active, immersive XRSession"; Quest Browser
//   157 granted it in every state tried later). The page always shows its own
//   Enter VR, so it offers only where that button is not the way in: when
//   its address asks for it (`?offer`, for a host that hides the button).
//   The page's button then ignores a pending offer, a refusal says what to
//   do, and an offered session that comes while the page has one is ended.
//
// Pure: unit-tested in backend/tests/vr/xr-entry.test.mjs.

/** The page was launched as an installed app: the loader said so
 *  (`launch=app` in the page's query), the headset app started it
 *  (`launch=immersive`, #255), or the page itself is shown as one
 *  (display-mode standalone or fullscreen). `matchMedia`: window.matchMedia. */
export function launchedAsApp(search, matchMedia) {
  try {
    const launch = new URLSearchParams(String(search || '')).get('launch');
    if (launch === 'app' || launch === 'immersive') return true;
  } catch { /* a malformed query */ }
  if (typeof matchMedia !== 'function') return false;
  try {
    return ['standalone', 'fullscreen'].some((m) => matchMedia(`(display-mode: ${m})`).matches);
  } catch { return false; }
}

/** The loader's launch mode for the page's address: 'app' when the loader
 *  itself runs as the installed app. */
export function loaderLaunch(matchMedia) {
  return launchedAsApp('', matchMedia) ? 'app' : '';
}

/** The browser offers sessions (navigator.xr.offerSession). */
export function canOffer(xr) {
  return !!xr && typeof xr.offerSession === 'function';
}

/** Try entering VR by itself now? Once per page, launched as an app, with a
 *  headset, on the page in use, outside VR, a cube on show, no benchmark
 *  being prepared, no other entry under way. The immersive headset app
 *  (#255, `immersive`) enters before its cube, once `settled` (its launch
 *  ticket exchanged or refused, or none to exchange). */
export function shouldAutoEnter({ launched, tried, vrSupported, active, inXR, cubeReady, busy = false, immersive = false, settled = false }) {
  const ready = immersive ? settled : cubeReady;
  return !!(launched && !tried && vrSupported && active && !inXR && ready && !busy);
}

/** A refusal for want of a user gesture (or of the permission): the button
 *  is the way then, nothing went wrong. */
export function isActivationRefusal(err) {
  if (!err) return false;
  const name = String(err.name || '');
  if (name === 'SecurityError' || name === 'NotAllowedError') return true;
  return /user (activation|gesture)|activation|gesture/i.test(String(err.message || ''));
}

/** The page offers a session only when its address asks for it (`?offer`,
 *  issue #291): its own Enter VR is the way in everywhere else. */
export function offerWanted(search) {
  try { return new URLSearchParams(String(search || '')).has('offer'); } catch { return false; }
}

/** Offer a session now? With offerSession, asked for (`wanted`, #291), a
 *  headset, on the page in use, outside VR and not entering, none offered
 *  already, offers not stopped. */
export function shouldOffer({ offerable, wanted, vrSupported, active, inXR, offered, stopped = false }) {
  return !!(offerable && wanted && vrSupported && active && !inXR && !offered && !stopped);
}

/** The page's own requestSession refused because the browser holds this
 *  page's offer (issue #291): an InvalidStateError while an offer is pending
 *  and the page has no session of its own. */
export function isOfferConflict(err, { offerPending, inXR }) {
  return !!(err && err.name === 'InvalidStateError' && offerPending && !inXR);
}

/** What the page says when Enter VR fails: what happened and what to do
 *  instead, never a bare error (issue #291). `kind`: offer-conflict,
 *  offer-raced, activation, permission, unsupported, busy, paused, failed. */
export function enterFailure(err, { offerPending = false, inXR = false, offerRaced = false } = {}) {
  const name = String((err && err.name) || '');
  const msg = String((err && err.message) || err || 'unknown error').slice(0, 160);
  if (isOfferConflict(err, { offerPending, inXR })) {
    return { kind: 'offer-conflict', text: 'The browser keeps its own Enter VR for this page and refused the page\'s button: '
      + 'press the browser\'s Enter VR (the headset button in its address bar), or reload the page without ?offer and press Enter VR.' };
  }
  // The browser's Enter VR answered while the page's request was under way:
  // the page closed that session (never two), and the browser refused its own.
  if (offerRaced && name === 'InvalidStateError') {
    return { kind: 'offer-raced', text: 'The browser\'s own Enter VR started a session at the same moment (it was closed): press Enter VR again.' };
  }
  // The page's own refusals (enterXR) already say what to do.
  if (!name || name === 'Error') {
    if (/this page is paused/.test(msg)) return { kind: 'paused', text: `${msg}: press Use this window, then Enter VR.` };
    if (/already running/.test(msg)) return { kind: 'busy', text: 'A VR session of this page is already running or starting: wait for it, or leave VR first.' };
  }
  if (name === 'InvalidStateError') {
    return { kind: 'busy', text: 'The browser has another VR session open (another tab or app): leave it, then press Enter VR again.' };
  }
  if (name === 'NotAllowedError' || name === 'SecurityError' || isActivationRefusal(err)) {
    const denied = /permission|denied|blocked/i.test(msg);
    return { kind: denied ? 'permission' : 'activation',
      text: denied ? 'The browser does not allow VR for this site: allow it in the site\'s settings, then press Enter VR.'
        : 'The browser wants a press on the page itself: press Enter VR again.' };
  }
  if (name === 'NotSupportedError') return { kind: 'unsupported', text: 'This browser or headset cannot start a VR session: open the page in the headset\'s browser.' };
  if (!name || name === 'Error') return { kind: 'failed', text: msg };
  return { kind: 'failed', text: `Enter VR failed (${name}: ${msg}): reload the page and press Enter VR; Copy diagnostics has the details.` };
}

/** Who holds the page's "a WebXR session runs" flag (issue #305): the
 *  session the page renders (its owner) and any session the page refused
 *  that has not ended yet (an offered one, #237). The flag stays up while
 *  any of them lives (no 3-D texture is allocated then, #229), and a late
 *  `end` lets go of that session only: a refused offer ending after the
 *  page's own session started never clears the flag under it.
 *  `onChange(active, owner)` mirrors the state (R.xrActive, R.xrSession). */
export class SessionFlag {
  constructor(onChange = () => {}) {
    this.owner = null;
    this.refused = new Set();
    this.onChange = onChange;
  }

  get active() { return !!this.owner || this.refused.size > 0; }

  /** The session the page renders from now on. */
  own(session) {
    this.refused.delete(session);
    this.owner = session;
    this.onChange(this.active, this.owner);
  }

  /** A session the page refused: the flag is held until its end. */
  holdRefused(session) {
    if (!session || session === this.owner) return;
    this.refused.add(session);
    this.onChange(this.active, this.owner);
  }

  /** A session ended: 'owner', 'refused', or '' (one the flag never held, or an end already seen). */
  ended(session) {
    let was = '';
    if (session && session === this.owner) { this.owner = null; was = 'owner'; }
    if (this.refused.delete(session)) was = was || 'refused';
    if (was) this.onChange(this.active, this.owner);
    return was;
  }
}

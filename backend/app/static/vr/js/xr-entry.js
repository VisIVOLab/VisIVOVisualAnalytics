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

/** Offer a session now? With offerSession, a headset, on the page in use,
 *  outside VR, none offered already. */
export function shouldOffer({ offerable, vrSupported, active, inXR, offered }) {
  return !!(offerable && vrSupported && active && !inXR && !offered);
}

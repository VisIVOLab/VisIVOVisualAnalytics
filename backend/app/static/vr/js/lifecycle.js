// The page's lifecycle (epic #166): is the backend there, is it the backend
// this page was built for, is the token still accepted, did the backend
// restart (its sessions and datasets are then gone). One cheap public call,
// GET /v1/vr/info (with the token header), answers all four; it is made at
// start, every few seconds while all is well and with a backoff while the
// backend cannot be reached. What to tell the user in each case, and the way
// out, is in noticeFor(): the same text is shown on the 2-D page and on the
// headset's message card (notice.js), and the user guide's troubleshooting
// section is written from it.
//
// Pure, with the network and the clock passed in: unit-tested in
// backend/tests/vr/lifecycle.test.mjs.

/** The page API this page speaks (backend/app/routers/vr.py VR_API). */
export const PAGE_VR_API = 1;

/** The build this module was served under (/vr/b/<build>/js/…), or '' when
 *  it was loaded from an unversioned address (an older index page). */
export function pageBuild(moduleUrl) {
  const m = String(moduleUrl || '').match(/\/vr\/b\/([A-Za-z0-9_-]{4,64})\//);
  return m ? m[1] : '';
}

/**
 * The page against the backend's /v1/vr/info answer.
 * {ok: true} or {ok: false, kind: 'reload' | 'update-app', why}:
 * - the page's API older than the backend accepts: 'reload' (a reload gets
 *   the backend's own page);
 * - the page's API newer than the backend's: 'update-app' (the page came
 *   from a newer backend and this one is older: update the application);
 * - same API, another build: 'reload' (the backend was upgraded under an
 *   open tab; the page still works, but its code is yesterday's).
 */
export function compareVersions(page, info) {
  if (!info || typeof info !== 'object') return { ok: true };
  const api = Number(page.api), max = Number(info.vr_api), min = Number(info.vr_api_min ?? info.vr_api);
  if (Number.isFinite(min) && api < min) {
    return { ok: false, kind: 'reload', why: `page API ${api}, the backend needs ${min} or later` };
  }
  if (Number.isFinite(max) && api > max) {
    return { ok: false, kind: 'update-app', why: `page API ${api}, the backend offers ${max}` };
  }
  if (page.build && info.build && page.build !== info.build) {
    return { ok: false, kind: 'reload', why: `page build ${page.build}, the backend serves ${info.build}`, soft: true };
  }
  return { ok: true };
}

/**
 * What a failed request means for the page. `err` is the Backend's Error:
 * message "<status>: <detail>", or `network: true` (fetch rejected: nothing
 * answered). Kinds: 'network', 'auth' (401/403), 'dataset-gone' (404 on a
 * dataset), 'session-gone' (404/400 on a session), 'busy' (429/503),
 * 'server' (other 5xx), 'other'.
 */
export function classifyFailure(err) {
  if (!err) return 'other';
  if (err.network || err.name === 'TypeError' && /fetch|network|load failed/i.test(String(err.message))) return 'network';
  const m = String(err.message || err).match(/^(\d{3}):\s*(.*)$/s);
  const code = Number(err.status || (m && m[1]) || 0);
  const detail = String(err.detail ?? (m ? m[2] : ''));
  if (code === 401 || code === 403) return 'auth';
  if (code === 404 && /session not found/i.test(detail)) return 'session-gone';
  if (code === 400 && /session/i.test(detail)) return 'session-gone';
  if (code === 404 && /dataset/i.test(detail)) return 'dataset-gone';
  if (code === 429 || code === 503) return 'busy';
  if (code >= 500) return 'server';
  return 'other';
}

/** Doubling waits between retries, capped: 1, 2, 4, 5, 5… seconds (a check
 *  is one tiny GET, and the user is waiting for the backend to come back). */
export class Backoff {
  constructor({ base = 1000, max = 5000, factor = 2 } = {}) {
    this.base = base; this.max = max; this.factor = factor; this.attempts = 0;
  }
  next() {
    const ms = Math.min(this.max, this.base * this.factor ** this.attempts);
    this.attempts++;
    return ms;
  }
  reset() { this.attempts = 0; }
}

export function isLoopback(hostname) {
  const h = String(hostname || '').replace(/^\[|\]$/g, '').toLowerCase();
  return h === 'localhost' || h === '::1' || /^127\./.test(h);
}

/** A standalone headset's browser (the Quest's, or another Android headset's). */
export function isHeadsetBrowser(ua) {
  return /OculusBrowser|Quest|Pico|Wolvic/i.test(String(ua || ''));
}

/**
 * The notice for a lifecycle state: {id, level, title, lines, actions,
 * sticky, badge}. `actions` are {id, label} the page wires: 'retry',
 * 'reload', 'exit-vr', 'dismiss', 'hide', 'reopen', 'enter-vr'. A sticky
 * notice stays while its condition lasts; 'hide' folds it into `badge`, a
 * short line on the panel, until the condition changes. `ctx`: {loopback,
 * headset, inVR, attempts, why, detail, status}.
 */
export function noticeFor(kind, ctx = {}) {
  const exit = ctx.inVR ? [{ id: 'exit-vr', label: 'Exit VR' }] : [];
  const hide = { id: 'hide', label: 'Hide' };
  const desktopReopen = 'On the computer: Tools → Views → Open in VR Headset… → Open on Headset.';
  switch (kind) {
    case 'unreachable': {
      const tries = ctx.attempts > 1 ? ` (attempt ${ctx.attempts})` : '';
      const lines = ctx.loopback && ctx.headset ? [
        'The headset cannot reach the backend on the computer.',
        'If the USB cable was unplugged, or the headset or adb restarted, the link (adb reverse) is gone:',
        desktopReopen,
        'If the backend is restarting, this clears by itself.',
      ] : [
        'The backend does not answer: it is restarting, stopped, or the network is down.',
        'Start the desktop application (it starts the backend), or check the network.',
      ];
      lines.push(`Trying again by itself every few seconds${tries}. The cube stays on show and moves meanwhile.`);
      return { id: 'unreachable', level: 'error', title: 'Backend not reachable', lines,
        actions: [{ id: 'retry', label: 'Retry now' }, hide, ...exit], sticky: true,
        badge: 'Backend not reachable: retrying' };
    }
    case 'http-error': {
      const proxy = ctx.status >= 502 && ctx.status <= 504;
      return { id: 'http-error', level: 'error', title: proxy ? 'The backend is not answering behind its proxy' : 'The backend answered with an error',
        lines: [proxy ? `The address answers (HTTP ${ctx.status}) but the backend behind it does not: it is restarting or stopped.`
          : `The backend answered HTTP ${ctx.status} to the page's check.`,
        'Trying again by itself every few seconds. The cube stays on show and moves meanwhile.'],
        actions: [{ id: 'retry', label: 'Retry now' }, hide, ...exit], sticky: true,
        badge: `Backend error (HTTP ${ctx.status}): retrying` };
    }
    case 'restarted':
      return { id: 'restarted', level: 'info', title: 'The backend restarted',
        lines: ['Its sessions ended, so the cube was opened again by its path.',
          'The view is as you left it; repeat the last selection if it did not finish.'],
        actions: [{ id: 'dismiss', label: 'OK' }], autoHideMs: 8000 };
    case 'reopened':
      return { id: 'reopened', level: 'info', title: 'The cube was opened again',
        lines: ['The backend no longer held this cube (the desktop\'s session ended, or it was cleared), so it was opened again by its path.',
          'Repeat the last selection if it did not finish.'],
        actions: [{ id: 'dismiss', label: 'OK' }], autoHideMs: 8000 };
    case 'reopen-failed':
      return { id: 'reopen-failed', level: 'error', title: 'The cube could not be opened again',
        lines: [`The backend answered: ${ctx.detail || 'an error'}.`,
          'The cube on show still works for looking at it; selections and refinement need it opened again.',
          desktopReopen],
        actions: [{ id: 'reopen', label: 'Try again' }, ...exit, { id: 'dismiss', label: 'Close' }], sticky: true };
    case 'auth':
      return { id: 'auth', level: 'error', title: 'The backend refused the token',
        lines: ['The token in this page is no longer accepted: the backend was restarted with a new token, or it expired.',
          desktopReopen,
          'Without the desktop: paste the token into the page\'s Backend token field, then press Open and load.'],
        actions: [{ id: 'retry', label: 'Check again' }, hide, ...exit], sticky: true,
        badge: 'Token refused: re-open from the desktop' };
    case 'reload':
      return { id: 'reload', level: 'warning', title: ctx.soft ? 'The backend was updated' : 'This page is out of date',
        lines: [ctx.soft ? 'The backend now serves a newer version of this page. Reload it to use the new code.'
          : 'The backend no longer works with this version of the page. Reload it.',
        ...(ctx.why ? [`(${ctx.why})`] : []),
        'After a reload, open the cube again from the desktop if it does not load by itself.'],
        actions: [{ id: 'reload', label: 'Reload page' }, ctx.soft ? { id: 'dismiss', label: 'Later' } : hide, ...exit],
        sticky: !ctx.soft, badge: 'This page is out of date: reload' };
    case 'update-app':
      return { id: 'update-app', level: 'error', title: 'Update the application',
        lines: ['This page is newer than the backend it is talking to.',
          'Update VisIVO Visual Analytics on the computer (or reload this page from the updated backend).',
          ...(ctx.why ? [`(${ctx.why})`] : [])],
        actions: [{ id: 'reload', label: 'Reload page' }, hide, ...exit], sticky: true,
        badge: 'Update the application' };
    case 'context-lost':
      return { id: 'context-lost', level: 'error', title: 'The graphics were reset',
        lines: ['The browser took the GPU away from this page (too much memory, or another app).',
          'Waiting for it to come back; the cube is uploaded again by itself.'],
        actions: [{ id: 'reload', label: 'Reload page' }], sticky: true };
    case 'context-failed':
      return { id: 'context-failed', level: 'error', title: 'The graphics did not come back',
        lines: ['The browser did not give the GPU back to this page.',
          'Reload the page: a cube opened from the desktop opens again by itself.',
          ...(ctx.detail ? [`(${ctx.detail})`] : [])],
        actions: [{ id: 'reload', label: 'Reload page' }], sticky: true };
    case 'context-restored':
      return { id: 'context-restored', level: 'info', title: 'Graphics restored',
        lines: ['The cube was uploaded again.', ...(ctx.wasInVR ? ['The VR session had to end: press Enter VR to go back in.'] : [])],
        actions: [...(ctx.wasInVR ? [{ id: 'enter-vr', label: 'Enter VR' }] : []), { id: 'dismiss', label: 'OK' }], autoHideMs: 12000 };
    case 'xr-ended':
      return { id: 'xr-ended', level: 'warning', title: 'The VR session ended',
        lines: ['The headset or the browser ended it (the system menu, the headset taken off, or the browser stopping it).',
          'The cube and the settings are kept: press Enter VR to go back in.'],
        actions: [{ id: 'enter-vr', label: 'Enter VR' }, { id: 'dismiss', label: 'OK' }], autoHideMs: 30000 };
    case 'budget':
      return { id: 'budget', level: 'error', title: 'This cube is too large for this device',
        lines: [ctx.detail || 'It would not fit in the memory this device gives a web page.',
          'Choose a larger binning (Spatial bin, Spectral bin) or a smaller Max side, then Open and load.'],
        actions: [{ id: 'dismiss', label: 'OK' }], sticky: true };
    default:
      return { id: kind, level: 'info', title: String(kind), lines: ctx.lines || [], actions: [{ id: 'dismiss', label: 'OK' }] };
  }
}

/**
 * The page's loads, newest wins. Every load takes a ticket when it starts,
 * with the path it was started for (read from the page then, never again);
 * a load that finishes after a newer one started is dropped, so loads that
 * finish out of order never put the older cube on show, and the recipes name
 * the file that is actually shown.
 */
export class LoadTracker {
  constructor() { this.latest = 0; }
  /** A new load of `path` ('' for a cube that is not a backend file). */
  begin(path) { this.latest++; return { id: this.latest, path: String(path || '') }; }
  /** Is `ticket` still the newest load? */
  isCurrent(ticket) { return !!ticket && ticket.id === this.latest; }
}

/** The Error a superseded load ends with (the page says nothing about it). */
export function supersededLoad() {
  const e = new Error('superseded by a newer load');
  e.superseded = true;
  return e;
}

/**
 * Opens the cube on show again after the backend lost its dataset (a restart,
 * the desktop's session ended), without touching the page. `snap` is taken by
 * the caller BEFORE any await: {path, fullShape, fileRevision, gen} (gen: the page's load
 * generation, bumped by every load). `io`: {open(path), newSession(),
 * reducedInfo(openAnswer), classify(err), loadGen()}. After every await the
 * generation is checked: when a load ran meanwhile, its cube is the one on
 * show and the answer is {ok: false, superseded: true}, to be ignored.
 * Otherwise {ok: true, open, info}, or {ok: false, detail} when the file's
 * shape/revision changed or its revision cannot be verified. Errors are thrown.
 */
export async function reopenCube(snap, io) {
  const superseded = { ok: false, superseded: true, detail: 'superseded by a newer load' };
  const stale = () => io.loadGen() !== snap.gen;
  let open;
  try {
    open = await io.open(snap.path);
  } catch (err) {
    if (stale()) return superseded;
    if (io.classify(err) !== 'session-gone') throw err;
    await io.newSession();
    if (stale()) return superseded;
    open = await io.open(snap.path);
  }
  if (stale()) return superseded;
  const info = await io.reducedInfo(open);
  if (stale()) return superseded;
  if (String(info.full_shape) !== String(snap.fullShape)) {
    return { ok: false, detail: `the file now has shape ${(info.full_shape || []).join('×')}, not ${(snap.fullShape || []).join('×')}: open it again` };
  }
  if (!snap.fileRevision || !info.file_revision || snap.fileRevision !== info.file_revision) {
    return { ok: false, detail: 'The file changed, or its revision cannot be verified: open it again to reload its voxels and coordinates.' };
  }
  return { ok: true, open, info };
}

/**
 * Watches the backend through `fetchInfo()` (resolves with /v1/vr/info's
 * answer; rejects with an Error marked `network` when nothing answers).
 * States: 'starting', 'ok', 'down', 'auth' (token refused), 'mismatch'
 * (another page build or API). `onChange(ev)` gets {state, prev, info,
 * restarted (the backend's instance changed since the last answer), version
 * (compareVersions' answer), retryInMs, attempts}.
 * `report(err)` lets any request say what it saw: a network failure starts
 * the backoff at once instead of at the next heartbeat.
 */
export class Lifecycle {
  constructor({ fetchInfo, page, onChange = () => {}, timers = globalThis, heartbeatMs = 5000, backoff, probeTimeoutMs = 5000 } = {}) {
    this.fetchInfo = fetchInfo;
    this.page = page || { build: '', api: PAGE_VR_API };
    this.onChange = onChange;
    this.timers = timers;
    this.heartbeatMs = heartbeatMs;
    this.backoff = backoff || new Backoff();
    this.state = 'starting';
    this.info = null;
    this.instance = null;
    this.timer = null;
    this.inFlight = null;
    this.stopped = true;
    this.nextAt = 0;
    this.lastVersion = { ok: true };
    // A check fetchInfo does not end by itself in this long (it has its own,
    // shorter, deadline) is ended here: the watch never waits on one.
    this.probeTimeoutMs = probeTimeoutMs;
    this.controller = null;
  }

  start() { this.stopped = false; this.probe(); }

  stop() {
    this.stopped = true;
    if (this.timer) this.timers.clearTimeout(this.timer);
    this.timer = null;
  }

  _schedule(ms) {
    if (this.stopped) return;
    if (this.timer) this.timers.clearTimeout(this.timer);
    this.nextAt = Date.now() + ms;
    this.timer = this.timers.setTimeout(() => { this.timer = null; this.probe(); }, ms);
  }

  /** A request elsewhere failed: a network failure checks now. */
  report(err) {
    const kind = classifyFailure(err);
    if ((kind === 'network' || kind === 'auth') && this.state !== 'down' && !this.inFlight) this.probe();
    return kind;
  }

  /** The user pressed Retry: a check now, the backoff kept (not reset). A
   *  check still pending is cancelled and a new one made: Retry always asks
   *  again. */
  retryNow() {
    if (!this.inFlight) return this.probe();
    if (this.controller) this.controller.abort('retry');
    return this.inFlight.then(() => this.probe());
  }

  _set(state, extra = {}) {
    const prev = this.state;
    this.state = state;
    // A listener's mistake must not stop the watch (the next check is
    // scheduled before this is called anyway).
    try { this.onChange({ state, prev, info: this.info, ...extra }); } catch (err) {
      if (globalThis.console) console.error('lifecycle listener failed', err);
    }
  }

  /**
   * One check now (or the one under way). Never rejects. The next check is
   * always scheduled before anyone is told, so nothing a listener does can
   * stop the watch:
   * - nothing answers (`network`): 'down', with the backoff;
   * - an HTTP error: 404 means a backend older than this page (no
   *   /v1/vr/info: 'mismatch', update the application); 401/403 'auth';
   *   anything else (a proxy's 502, a 500) 'down' with its `status`, and the
   *   backoff;
   * - an answer: 'ok', 'auth' (token refused) or 'mismatch'.
   */
  probe() {
    if (this.inFlight) return this.inFlight;
    let settle;
    this.inFlight = new Promise((resolve) => { settle = resolve; });
    const controller = typeof AbortController !== 'undefined' ? new AbortController() : null;
    this.controller = controller;
    const run = async () => {
      let info;
      let guard = null;
      try {
        // fetchInfo gets the signal; whatever it does with it, the check
        // ends after probeTimeoutMs as "nothing answered".
        info = await Promise.race([
          this.fetchInfo({ signal: controller ? controller.signal : null }),
          new Promise((_, reject) => {
            guard = this.timers.setTimeout(() => {
              if (controller) controller.abort('timeout');
              const e = new Error('network: no answer in time');
              e.network = true; e.status = 0;
              reject(e);
            }, this.probeTimeoutMs);
          }),
          ...(controller ? [new Promise((_, reject) => {
            controller.signal.addEventListener('abort', () => {
              if (controller.signal.reason === 'retry') { const e = new Error('cancelled for a retry'); e.cancelled = true; reject(e); }
            }, { once: true });
          })] : []),
        ]);
        this.timers.clearTimeout(guard);
      } catch (err) {
        if (guard) this.timers.clearTimeout(guard);
        if (err && err.cancelled) return;              // Retry: a new check follows at once
        const status = err && err.status > 0 ? err.status : 0;
        if (status === 404) {
          this._schedule(this.heartbeatMs);
          this._set('mismatch', { error: err, version: { ok: false, kind: 'update-app',
            why: 'this backend has no /v1/vr/info: it is older than this page' } });
        } else if (status === 401 || status === 403) {
          this._schedule(this.heartbeatMs);
          this._set('auth', { error: err });
        } else {
          const ms = this.backoff.next();
          this._schedule(ms);
          this._set('down', { error: err, status, retryInMs: ms, attempts: this.backoff.attempts });
        }
        return;
      }
      this.backoff.reset();
      this._schedule(this.heartbeatMs);
      const restarted = this.instance !== null && !!info.instance && info.instance !== this.instance;
      if (info.instance) this.instance = info.instance;
      this.info = info;
      const version = compareVersions(this.page, info);
      this.lastVersion = version;
      let state = 'ok';
      if (!version.ok && !version.soft) state = 'mismatch';
      else if (info.token_ok === false) state = 'auth';
      // Not checked this time (the backend rate-limited the token check): a
      // refused token stays refused until a check says otherwise.
      else if (info.token_ok === null && this.state === 'auth') state = 'auth';
      this._set(state, { restarted, version });
    };
    const clear = () => { if (this.controller === controller) this.controller = null; };
    run().catch(() => {}).finally(() => { clear(); this.inFlight = null; settle(); });
    return this.inFlight;
  }
}

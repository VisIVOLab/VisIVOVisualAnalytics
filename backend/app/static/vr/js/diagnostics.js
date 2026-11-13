// Diagnostics for support (epic #166): the page's errors and lifecycle events
// (uncaught errors, unhandled rejections, context loss, the XR session's end,
// reconnections, a frame-rate summary) kept in a short ring and sent, a few at
// a time, to the backend's log (POST /v1/vr/log); and the "Copy diagnostics"
// text: versions, GPU limits, settings and the last errors.
//
// Nothing secret leaves the page: scrubText applies the backend's own rules
// (backend/app/routers/vr.py scrub, which applies them again):
//   1. %-escapes are decoded first (a pairing link writes path=%2FUsers%2F…);
//   2. every known token becomes <token>;
//   3. the value of any key containing "token" or "session" (session_id,
//      sessionId, access_token, X-Visivo-Token, JSON "token": "…"), quoted or
//      not, before = or :, becomes <redacted> when it looks like a secret
//      (6+ characters with a digit, 8+ hex, or 16+ of any kind);
//   4. the id in /v1/render/<id>/… becomes <id>;
//   5. paths keep only their last component: file:// URLs, the path of a URL
//      with a host (except the backend's /v1/… and /vr/… routes), Windows paths
//      (C:\…, C:/…, \\server\share\…) and POSIX paths (/…, ~/…); spaces inside
//      directory names only up to the first file name with an extension,
//      without one, in any directory name another separator follows; a path
//      ending at a home directory (/Users/<n>, /home/<n>, C:\Users\<n>)
//      gives …/<user>;
//   6. control and format characters (Unicode Cc, Cf, Zl, Zp) become spaces.
// The same cases are checked on both sides from backend/tests/vr/scrub_cases.json.
//
// Pure, with the network and the clock passed in: unit-tested in
// backend/tests/vr/diagnostics.test.mjs and quality.test.mjs.

const ESCAPES = /(?:%[0-9A-Fa-f]{2})+/g;
// A key: a whole word of at most 64 characters naming a token or a session,
// found by a bounded lookahead (linear: an unbounded [\w-]*token[\w-]* took
// seconds on a long "xtoken…" run). Only such keys match, so "host:port/…"
// cannot swallow a ?token= behind it.
const KEY_VALUE = /(["']?)(?<![\w-])(?=[\w-]{0,60}?(?:token|session))([\w-]{1,64})(?![\w-])\1(\s*[=:]\s*)("[^"]*"|'[^']*'|[^\s&#,;'"}\]]+)/gi;
const SECRET_KEY = /token|session/i;
/** What the pattern rules see at most (after the known secrets are replaced). */
const SCRUB_WINDOW = 1200;
const RENDER_ID = /(\/v1\/render\/(?:queue\/)?)([0-9A-Fa-f-]{8,})/g;
// file:// URLs go through the path rules: their path is marked, the mark removed at the end.
const MARK = '\ue000';
const FILE_PREFIX = /\bfile:\/\/\/?/gi;
const HOST_URL = /\b(?!file:)([a-z][a-z0-9+.-]*:\/\/[^\s/"'<>?#]+)(\/[^\s"'<>?#]*)?/gi;
// The same patterns as vr.py, built from the same pieces.
const SEG_SP = String.raw`[^/\s"'<>]+(?: [^/\s"'<>]+)*`;
const SEG_SP_WIN = String.raw`[^\\/\s"'<>|]+(?: [^\\/\s"'<>|]+)*`;
const EXT_NAME = String.raw`[^/\\\s"'<>|]+\.[A-Za-z0-9]{1,8}\b`;
const WIN_START = String.raw`(?:\b[A-Za-z]:[\\/]|\\\\)`;
const POSIX_START = String.raw`(?<![\w.:/\\…<>])~?/(?!v1/|vr/)`;
const WIN_PATH_EXT = new RegExp(`${WIN_START}(?:${SEG_SP_WIN}[\\\\/])*?(${EXT_NAME})`, 'g');
// Without an extension: a home directory's name may hold spaces and becomes
// <user> when the path ends there; elsewhere spaced names are taken whole too.
const WIN_PATH = new RegExp(String.raw`${WIN_START}(?:[Uu]sers[\\/](${SEG_SP_WIN})(?:[\\/](?:${SEG_SP_WIN}[\\/])*([^\\/\s"'<>|]*))?|(?:${SEG_SP_WIN}[\\/])*([^\\/\s"'<>|]*))`, 'g');
const WIN_USER_DIR = /^(?:[A-Za-z]:[\\/]|\\\\)[Uu]sers[\\/][^\\/]+$/;
const POSIX_USER_DIR = /^~?\/(?:Users|home)\/[^/]+$/;
const POSIX_PATH_EXT = new RegExp(`${POSIX_START}(?:${SEG_SP}/)+?(${EXT_NAME})`, 'g');
const POSIX_PATH = new RegExp(String.raw`${POSIX_START}(?:(?:Users|home)/(${SEG_SP})(?:/(?:${SEG_SP}/)*([^/\s"'<>]*))?|(?:${SEG_SP}/)+([^/\s"'<>]*))`, 'g');

const pathSub = (sep) => (_, user, last, other) => (user !== undefined ? `…${sep}${last || '<user>'}` : `…${sep}${other}`);
const extSub = (sep, userDir) => (all, base) => (userDir.test(all) ? `…${sep}<user>` : `…${sep}${base}`);

function looksSecret(value) {
  const v = String(value).replace(/^["']|["']$/g, '');
  return (v.length >= 6 && /\d/.test(v)) || (v.length >= 8 && /^[0-9A-Fa-f-]+$/.test(v)) || v.length >= 16;
}
const CONTROLS = /[\p{Cc}\p{Cf}\p{Zl}\p{Zp}]/gu;

function decodeEscapes(text) {
  return text.replace(ESCAPES, (run) => {
    try { return decodeURIComponent(run); } catch { return run; }
  });
}

function urlPath(_, base, path = '') {
  if (!path || path === '/' || path.startsWith('/v1/') || path.startsWith('/vr/')) return base + path;
  const last = path.replace(/\/+$/, '').split('/').pop();
  return `${base}/…/${last}`;
}

/** `text` safe for a log or a support request (the rules above). */
export function scrubText(text, secrets = []) {
  let out = decodeEscapes(String(text ?? '')).replace(CONTROLS, ' ').split(MARK).join(' ');
  // 1. the known secrets over the whole text (linear), 2. cut, 3. the patterns.
  for (const s of secrets) {
    if (s && String(s).length >= 6) out = out.split(String(s)).join('<token>');
  }
  out = out.slice(0, SCRUB_WINDOW);
  out = out.replace(KEY_VALUE, (all, q, k, sep, v) => (SECRET_KEY.test(k) && looksSecret(v) ? `${q}${k}${q}${sep}<redacted>` : all));
  out = out.replace(RENDER_ID, (_, pre) => `${pre}<id>`);
  out = out.replace(HOST_URL, urlPath);
  out = out.replace(FILE_PREFIX, `file://${MARK}/`);
  out = out.replace(WIN_PATH_EXT, extSub('\\', WIN_USER_DIR));
  out = out.replace(WIN_PATH, pathSub('\\'));
  out = out.replace(POSIX_PATH_EXT, extSub('/', POSIX_USER_DIR));
  out = out.replace(POSIX_PATH, pathSub('/'));
  out = out.split(MARK).join('');
  return out.replace(/ {2,}/g, ' ').trim();
}

/** The last `max` events: {t, level, event, message, count}. A repeat of the
 *  last event counts up instead of filling the ring. */
export class DiagLog {
  constructor({ max = 40, now = () => Date.now(), secrets = () => [] } = {}) {
    this.max = max; this.now = now; this.secrets = secrets;
    this.items = [];
    this.listeners = [];
  }

  add(level, event, message = '') {
    const msg = scrubText(message, this.secrets()).slice(0, 600);
    const last = this.items[this.items.length - 1];
    if (last && last.event === event && last.message === msg && last.level === level) {
      last.count++;
      last.t = this.now();
      return last;
    }
    const e = { t: this.now(), level, event, message: msg, count: 1 };
    this.items.push(e);
    while (this.items.length > this.max) this.items.shift();
    for (const f of this.listeners) f(e);
    return e;
  }

  onAdd(f) { this.listeners.push(f); }
  errors(n = 10) { return this.items.filter((e) => e.level === 'error' || e.level === 'warning').slice(-n); }
}

/**
 * Sends events to the backend's log, batched: at most `maxPerPost` a post,
 * one post every `intervalMs` at most, and no more than `maxPerMinute` a
 * minute (the backend has its own, stricter-or-equal, limit). Debug events
 * stay local. A 429 waits its Retry-After; a 401/403 stops sending until
 * `resume()` (a new token); a network failure keeps the queue (capped) for
 * the next try.
 * `post(body)` resolves with {status, retryAfterS}; it is the page's fetch.
 */
export class LogSender {
  constructor({ post, build = '', userAgent = '', now = () => Date.now(), timers = globalThis,
    intervalMs = 3000, maxPerPost = 20, maxPerMinute = 40, maxQueue = 60 } = {}) {
    Object.assign(this, { post, build, userAgent, now, timers, intervalMs, maxPerPost, maxPerMinute, maxQueue });
    this.queue = [];
    this.sentTimes = [];
    this.timer = null;
    this.blockedUntil = 0;
    this.stopped = false;
    this.busy = false;
    this.sent = 0;
    this.droppedCount = 0;
  }

  enqueue(e) {
    if (e.level === 'debug') return;
    this.queue.push({ level: e.level, event: e.event, message: e.count > 1 ? `${e.message} (×${e.count})` : e.message, t: e.t });
    while (this.queue.length > this.maxQueue) { this.queue.shift(); this.droppedCount++; }
    this._arm();
  }

  resume() { this.stopped = false; this._arm(); }

  _arm(delay = this.intervalMs) {
    if (this.timer || this.stopped || !this.queue.length) return;
    this.timer = this.timers.setTimeout(() => { this.timer = null; this.flush(); }, Math.max(0, delay));
  }

  /** The number of events allowed now by the per-minute cap. */
  _allowance(t) {
    this.sentTimes = this.sentTimes.filter((s) => t - s < 60000);
    return Math.max(0, this.maxPerMinute - this.sentTimes.length);
  }

  async flush() {
    if (this.busy || this.stopped || !this.queue.length) return;
    const t = this.now();
    if (t < this.blockedUntil) { this._arm(this.blockedUntil - t); return; }
    const n = Math.min(this.maxPerPost, this._allowance(t), this.queue.length);
    if (n <= 0) { this._arm(Math.max(1000, 60000 - (t - this.sentTimes[0]))); return; }
    const batch = this.queue.slice(0, n);
    this.busy = true;
    let r;
    try {
      r = await this.post({ build: this.build, user_agent: this.userAgent, events: batch });
    } catch {
      r = { status: 0 };
    } finally {
      this.busy = false;
    }
    if (r.status >= 200 && r.status < 300) {
      this.queue.splice(0, n);
      for (let i = 0; i < n; i++) this.sentTimes.push(t);
      this.sent += n;
    } else if (r.status === 429) {
      this.blockedUntil = this.now() + 1000 * (r.retryAfterS || 30);
    } else if (r.status === 401 || r.status === 403) {
      this.stopped = true;              // the token is refused: the lifecycle says so
      return;
    } else if (r.status === 413 || r.status === 422) {
      this.queue.splice(0, n);          // this batch will never be accepted
      this.droppedCount += n;
    } else {
      this.blockedUntil = this.now() + 10000;   // the backend is not there: keep them
    }
    this._arm();
  }
}

/** Quantiles and mean of frame intervals (ms) → an FPS summary line. */
export function fpsSummary(intervals, targetHz) {
  const a = intervals.filter((v) => v > 0 && v < 1000);
  if (!a.length) return 'no frames';
  const s = [...a].sort((x, y) => x - y);
  const mean = a.reduce((p, v) => p + v, 0) / a.length;
  const q = (f) => s[Math.min(s.length - 1, Math.floor(f * s.length))];
  const target = targetHz ? 1000 / targetHz : 0;
  const missed = target ? a.filter((v) => v > 1.5 * target).length / a.length : 0;
  return `${(1000 / mean).toFixed(1)} fps mean over ${a.length} frames, interval p50 ${q(0.5).toFixed(1)} ms p95 ${q(0.95).toFixed(1)} ms`
    + (target ? `, display ${targetHz} Hz, missed ${(100 * missed).toFixed(1)} %` : '');
}

/**
 * The text "Copy diagnostics" puts on the clipboard. `d`: {page: {build,
 * api}, backend: /v1/vr/info answer or null, lifecycle state, userAgent,
 * gpu: {renderer, vendor, maxTex3D, maxTexture, timerQuery}, xr: {...},
 * settings, cube, fps, quality, errors: DiagLog items, secrets}.
 */
export function diagnosticsReport(d) {
  const lines = [];
  const kv = (k, v) => lines.push(`${k}: ${v === undefined || v === null || v === '' ? 'n/a' : v}`);
  lines.push('VisIVO VR page diagnostics');
  kv('time', d.time);
  kv('page build', d.page && d.page.build);
  kv('page API', d.page && d.page.api);
  const b = d.backend;
  kv('backend', b ? `version ${b.backend_version}, page build ${b.build}, API ${b.vr_api_min}–${b.vr_api}, instance ${b.instance}` : 'not reached');
  kv('connection', d.lifecycle);
  kv('transport', d.transport);
  kv('address', d.address);
  kv('browser', d.userAgent);
  const g = d.gpu || {};
  kv('GPU', g.renderer);
  kv('GPU limits', `3-D texture ${g.maxTex3D}, 2-D texture ${g.maxTexture}, timer query ${g.timerQuery ? 'yes' : 'no'}, device memory ${g.deviceMemoryGB ?? 'n/a'} GB`);
  kv('WebXR', d.xr);
  kv('cube', d.cube);
  kv('frame rate', d.fps);
  kv('quality', d.quality);
  // Issue #226: how the text is drawn in the headset and the page's GPU textures.
  if (d.text !== undefined) kv('text in the headset', d.text);
  if (d.memory !== undefined) kv('GPU memory', d.memory);
  kv('refinement', d.refine);
  kv('settings', d.settings ? JSON.stringify(d.settings) : '');
  lines.push('last events:');
  const evs = (d.errors || []).slice(-25);
  if (!evs.length) lines.push('  (none)');
  for (const e of evs) {
    lines.push(`  ${new Date(e.t).toISOString().slice(11, 19)} ${e.level} ${e.event}${e.count > 1 ? ` ×${e.count}` : ''}: ${e.message}`);
  }
  // Line by line: scrubbing turns newlines into spaces and looks at a bounded
  // window, so the report as one string would come out as one cut line.
  return lines.map((l) => scrubText(l, d.secrets || [])).join('\n');
}

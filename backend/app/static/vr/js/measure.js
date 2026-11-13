// Recording the headset's measurements (issue #224): one sample a second,
// events with their own times, and a header naming the device, the setup,
// the units and the definitions; sent in batches to the backend
// (POST /v1/vr/measurements), which keeps one JSON-lines file per record.
//
// Every figure is computed by the code that computes what the page shows:
// the frame figures by volume.js FrameStats (the wrist's FPS, frame mean and
// 95th percentile, missed %, CPU and GPU time), over the frames of the
// sample's own window instead of the wrist's last 240; pose→frame and
// stop→refined by refine-client.js (its Rolling quantiles, the same values
// the wrist shows). The raw values each sample's figures come from are kept
// with it (frame intervals, pose→frame times), so a report can pool them
// over a phase with the same definitions.
//
// No token, no path, no personal data: the record holds the dataset's id
// (not its file), the user agent and the GPU's name, and numbers.
//
// Pure, with the clock and the network passed in: unit-tested in
// backend/tests/vr/measure.test.mjs.

import { FrameStats } from './volume.js';

export const MEASURE_FORMAT = 'visivo-vr-measure/1';
export const SAMPLE_MS = 1000;
/** Frame intervals kept per sample at most (a 1 s window at 120 Hz is 120). */
export const MAX_INTERVALS = 400;
/** pose→frame values kept per sample at most. */
export const MAX_POSE_FRAME = 200;

/** What each field means, written into every record's header. */
export const UNITS = {
  t: 'ms since the record started (the page clock, performance.now)',
  dur: 'ms covered by the sample (its window: the frames since the previous sample)',
  'frames.n': 'frames drawn in the window',
  'frames.fps': 'frames per second: 1000 / mean frame interval',
  'frames.mean, frames.p50, frames.p95': 'frame interval in ms (requestAnimationFrame time between consecutive frames)',
  'frames.missed_pct': '% of frame intervals longer than 1.5 × 1000 / target_hz; null without a display rate (desktop browser)',
  'frames.cpu_mean': 'ms of page work per frame (frame callback start to the end of its GL calls)',
  'frames.gpu_mean': 'ms of GPU time per frame from the GL timer query (EXT_disjoint_timer_query_webgl2); null without it',
  'frames.cpu_n, frames.gpu_n': 'how many values cpu_mean and gpu_mean are over (a frame may have no GPU result)',
  'frames.target_hz': 'display rate in Hz (XRSession.frameRate); 0 in a desktop browser',
  iv: 'the window\'s frame intervals in ms, rounded to 0.01 ms: the values the frame figures are computed from',
  'wrist.*': 'the numbers on the wrist at the sample\'s time (the last 240 frames), as shown',
  'display.eye': 'eye buffer size in pixels (XRWebGLLayer framebuffer width × height, both eyes)',
  'display.scale': 'framebufferScaleFactor in use; chosen_scale: the one chosen on the page',
  'refine.pose_frame': 'pose sent → refined frame decoded and ready to show, in ms: n, p50, p95 over the refiner\'s last 120 frames (the wrist\'s pose->frame)',
  'refine.pf': 'the pose→frame times (ms) of the frames that arrived during the window',
  'refine.stop_first, refine.stop_full': 'the wrist\'s last stop->refined (first) and (full), in ms',
  'video.*': 'the WebRTC video link of the refined frames (getStats): kbps, decode_ms and jitter_ms per frame, rtt_ms of the selected candidate pair, ice: local/remote candidate types',
  'pair.*': 'the pairing data channel (direct links: absent): rtt_ms, ice, resumes, bytes in and out',
  look: 'a short hash of the look (transfer function, crop, stretch): equal hashes, equal look',
  rec_ms: 'ms the page spent building the previous sample',
  'event refined-first, refined-full': 'ms from the end of a motion (moving → slow or still) to the first refined frame shown / shown at full weight: the wrist\'s stop->refined',
  'event load-end': 'ms from the load\'s start to the cube on show; mb: bytes transferred / 1e6',
};

export const DEFINITIONS = {
  quantiles_frames: 'frame p50, p95: sorted[floor(q × (n − 1))] (volume.js FrameStats.stats)',
  quantiles_refine: 'pose→frame p50, p95: sorted[min(n − 1, floor(q × n))] (refine.js Rolling.quantile)',
  missed: 'a frame interval above 1.5 × the display\'s frame time (quality.js, volume.js, diagnostics.js)',
  protocol: 'proto.condition: what is tested; proto.phase: what the viewer is asked to do; proto.part: lead (the prompt is read, not measured) or measure',
};

const r2 = (v) => (Number.isFinite(v) ? Math.round(v * 100) / 100 : null);
const r1 = (v) => (Number.isFinite(v) ? Math.round(v * 10) / 10 : null);

/** A record's id: UTC date and time of its start and 8 random hex digits. */
export function recordId(date, randomHex) {
  const p = (n, w = 2) => String(n).padStart(w, '0');
  const d = `${date.getUTCFullYear()}${p(date.getUTCMonth() + 1)}${p(date.getUTCDate())}`;
  const t = `${p(date.getUTCHours())}${p(date.getUTCMinutes())}${p(date.getUTCSeconds())}`;
  return `${d}-${t}-${String(randomHex).toLowerCase().replace(/[^0-9a-f]/g, '').padEnd(8, '0').slice(0, 8)}`;
}
export const RECORD_ID = /^\d{8}-\d{6}-[0-9a-f]{8}$/;

/** Which link the page uses, by kind only (no host name, no address). */
export function linkKind({ paired = false, hostname = '' } = {}) {
  if (paired) return { kind: 'paired', host: 'pairing service' };
  const h = String(hostname).toLowerCase().replace(/^\[|\]$/g, '');
  if (h === 'localhost' || h === '::1' || /^127\./.test(h)) return { kind: 'direct', host: 'loopback' };
  if (/^10\./.test(h) || /^192\.168\./.test(h) || /^172\.(1[6-9]|2\d|3[01])\./.test(h) || /\.local$/.test(h) || /^f[cd]/.test(h)) {
    return { kind: 'direct', host: 'lan' };
  }
  return { kind: 'direct', host: 'remote' };
}

/** A short, stable hash of a string (FNV-1a, 32 bits, hex). */
export function shortHash(text) {
  let h = 0x811c9dc5;
  const s = String(text);
  for (let i = 0; i < s.length; i++) {
    h ^= s.charCodeAt(i);
    h = Math.imul(h, 0x01000193) >>> 0;
  }
  return h.toString(16).padStart(8, '0');
}

/**
 * The frame figures of a window, by FrameStats (the wrist's code): {n, fps,
 * mean, p50, p95, missed_pct, cpu_mean, gpu_mean, target_hz}.
 */
export function frameFigures(fs, targetHz) {
  const s = fs.summary(targetHz ? 1000 / targetHz : 0);
  const iv = s.interval;
  return {
    n: s.n, fps: r2(s.fps), mean: iv ? r2(iv.mean) : null, p50: iv ? r2(iv.p50) : null, p95: iv ? r2(iv.p95) : null,
    missed_pct: targetHz ? r2(100 * s.missedFraction) : null,
    cpu_mean: s.cpu ? r2(s.cpu.mean) : null, gpu_mean: s.gpu ? r2(s.gpu.mean) : null,
    // How many values each mean is over (FrameStats keeps them apart: a frame
    // may have no GPU result yet).
    cpu_n: fs.cpu.length, gpu_n: fs.gpu.length, target_hz: targetHz || 0,
  };
}

/**
 * The values a Rolling (refine.js) received since `seen` pushes, given its
 * running total of pushes: at most the ones it still holds.
 */
export function newValues(rolling, total, seen) {
  const n = Math.max(0, Math.min(total - seen, rolling.v.length));
  return n ? rolling.v.slice(rolling.v.length - n) : [];
}

/**
 * Sends the record's lines in batches: every `intervalMs`, or sooner when
 * `batchLines` are waiting; at most `maxQueue` lines are kept unsent (the
 * oldest samples go first: events and the header are kept). A failure never
 * throws to the page: 429 waits its Retry-After, 401/403 waits for resume()
 * (a new token), 413/422 drop the batch, 507 (the backend cannot store the
 * record: disk full, directory not writable, record over its size) stops
 * storing for this record, anything else retries later with the same batch
 * number (the backend ignores a batch it already wrote).
 * `post({record, seq, lines})` resolves with {status, retryAfterS, body}.
 */
export class MeasureSender {
  constructor({ post, now = () => Date.now(), timers = globalThis, intervalMs = 5000, batchLines = 60, maxQueue = 900,
    maxBatchBytes = 192 * 1024 } = {}) {
    Object.assign(this, { post, now, timers, intervalMs, batchLines, maxQueue, maxBatchBytes });
    this.record = '';
    this.queue = [];            // {line (object), text (JSON), kind}
    this.inflight = null;       // {seq, items}
    this.seq = 0;
    this.timer = null;
    this.blockedUntil = 0;
    this.paused = false;
    this.stored = true;
    this.status = 'off';
    this.file = '';
    this.sentLines = 0;
    this.dropped = 0;
    this.lastError = '';
  }

  start(record) {
    // A batch of the previous record still in flight is that record's: its
    // answer must not touch this one (gen).
    this.gen = (this.gen || 0) + 1;
    if (this.timer) { this.timers.clearTimeout(this.timer); this.timer = null; }
    this.busy = false;
    this.record = record;
    this.queue = []; this.inflight = null; this.seq = 0; this.sentLines = 0; this.dropped = 0;
    this.blockedUntil = 0; this.paused = false; this.stored = true; this.file = ''; this.lastError = '';
    this.status = 'recording';
  }

  push(line) {
    if (!this.record || !this.stored) return;
    let text;
    try { text = JSON.stringify(line); } catch { this.dropped++; return; }
    if (text.length > 32 * 1024) { this.dropped++; return; }
    this.queue.push({ text, kind: line.k });
    while (this.queue.length > this.maxQueue) {
      const i = this.queue.findIndex((q) => q.kind === 's');
      this.queue.splice(i < 0 ? 0 : i, 1);
      this.dropped++;
    }
    if (this.queue.length >= this.batchLines) this._arm(0);
    else this._arm();
  }

  resume() { this.paused = false; this._arm(0); }

  _arm(delay = this.intervalMs) {
    if (this.paused || !this.stored || !this.queue.length && !this.inflight) return;
    if (this.timer && delay > 0) return;
    if (this.timer) this.timers.clearTimeout(this.timer);
    this.timer = this.timers.setTimeout(() => { this.timer = null; this.flush(); }, Math.max(0, delay));
  }

  /** The next batch: the one that failed (same number), or new lines. */
  _batch() {
    if (this.inflight) return this.inflight;
    const items = [];
    let bytes = 0;
    while (items.length < this.batchLines && items.length < this.queue.length) {
      const q = this.queue[items.length];
      if (items.length && bytes + q.text.length > this.maxBatchBytes) break;
      items.push(q);
      bytes += q.text.length + 1;
    }
    if (!items.length) return null;
    this.queue.splice(0, items.length);
    this.inflight = { seq: this.seq++, items };
    return this.inflight;
  }

  flush() {
    if (this.busy) return this.flushing;
    this.flushing = this._flush();
    return this.flushing;
  }

  async _flush() {
    if (this.paused || !this.stored || !this.record) return;
    const t = this.now();
    if (t < this.blockedUntil) { this._arm(this.blockedUntil - t); return; }
    const b = this._batch();
    if (!b) return;
    const gen = this.gen;
    this.busy = true;
    let r;
    try {
      r = await this.post({ record: this.record, seq: b.seq, lines: `[${b.items.map((q) => q.text).join(',')}]` });
    } catch (err) {
      r = { status: 0, body: { detail: String((err && err.message) || err) } };
    } finally {
      if (gen === this.gen) this.busy = false;
    }
    if (gen !== this.gen) return;           // a new record started meanwhile
    const detail = (r.body && (r.body.detail || r.body.error)) || '';
    if (r.status >= 200 && r.status < 300) {
      this.inflight = null;
      this.sentLines += b.items.length;
      if (r.body && r.body.file) this.file = r.body.file;
      this.lastError = '';
      this.status = 'recording';
    } else if (r.status === 429) {
      this.blockedUntil = this.now() + 1000 * (r.retryAfterS || 10);
    } else if (r.status === 401 || r.status === 403) {
      this.paused = true;
      this.status = 'paused: the token is refused';
      return;
    } else if (r.status === 413 || r.status === 422) {
      this.inflight = null;
      this.dropped += b.items.length;
      this.lastError = `batch refused (${r.status}${detail ? `: ${detail}` : ''})`;
    } else if (r.status === 507) {
      this.inflight = null;
      this.stored = false;
      this.dropped += b.items.length + this.queue.length;
      this.queue = [];
      this.status = `not stored: ${detail || 'the backend cannot write the record'}`;
      this.lastError = this.status;
      return;
    } else {
      this.blockedUntil = this.now() + 5000;
      this.lastError = r.status ? `HTTP ${r.status}` : 'the backend did not answer';
    }
    this._arm(this.queue.length >= this.batchLines ? 0 : undefined);
  }

  /** Whatever is waiting goes now (the record stops); never waits more
   *  than `maxMs` in all (a backend that does not answer keeps the rest). */
  async drain(maxTries = 3, maxMs = 5000) {
    const until = Date.now() + maxMs;
    const bounded = (p) => Promise.race([p, new Promise((r) => { const h = this.timers.setTimeout(r, Math.max(0, until - Date.now())); p.then(() => this.timers.clearTimeout(h), () => {}); })]);
    if (this.busy) await bounded(Promise.resolve(this.flushing));     // the batch in flight first
    for (let i = 0; i < maxTries && Date.now() < until && !this.busy && (this.queue.length || this.inflight) && this.stored && !this.paused; i++) {
      if (this.timer) { this.timers.clearTimeout(this.timer); this.timer = null; }
      this.blockedUntil = 0;
      await bounded(this.flush());
    }
  }
}

/**
 * The recorder: fed every frame (frame()), it builds a sample when its
 * window reaches `sampleMs` (tick()) or when cut() is called (a protocol
 * phase begins: no sample spans two phases); events go out as they come.
 * `snapshot()` gives the page's state for a sample (see buildSample).
 */
export class MeasureRecorder {
  constructor({ sender, snapshot, now = () => performance.now(), wall = () => new Date(), sampleMs = SAMPLE_MS } = {}) {
    Object.assign(this, { sender, snapshot, now, wall, sampleMs });
    this.on = false;
    this.window = new FrameStats(MAX_INTERVALS);
    this.iv = [];
    this.t0 = 0;
    this.winStart = 0;
    this.samples = 0;
    this.events = 0;
    this.lastBuildMs = 0;
    this.buildMs = [];
    this.proto = null;           // {condition, phase, part} while a protocol runs
    this.pfSeen = 0;
  }

  /** Starts a record: its header first. Returns the record id. */
  start(header, id) {
    this.on = true;
    this.t0 = this.now();
    this.winStart = this.t0;
    this.window = new FrameStats(MAX_INTERVALS);
    this.iv = [];
    this.samples = 0; this.events = 0; this.buildMs = [];
    this.id = id;
    this.sender.start(id);
    this.sender.push({ k: 'h', format: MEASURE_FORMAT, record: id, started: this.wall().toISOString(), sample_ms: this.sampleMs,
      ...header, units: UNITS, definitions: DEFINITIONS });
    return id;
  }

  async stop(reason = 'stopped') {
    if (!this.on) return;
    this.cut();
    this.event('record-stop', { reason, samples: this.samples });
    this.on = false;
    await this.sender.drain();
  }

  /** One drawn frame (the same three numbers the wrist's FrameStats gets). */
  frame(intervalMs, cpuMs, gpuMs) {
    if (!this.on) return;
    this.window.push(intervalMs, cpuMs, gpuMs);
    if (intervalMs > 0 && intervalMs < 1000 && this.iv.length < MAX_INTERVALS) this.iv.push(r2(intervalMs));
  }

  /** Once a frame, after frame(): a sample when the window is full. */
  tick() {
    if (!this.on) return;
    if (this.now() - this.winStart >= this.sampleMs) this.cut();
  }

  /** The window so far as a sample (if it holds a frame), and a new window. */
  cut() {
    if (!this.on) return;
    const t = this.now();
    if (this.window.buf.length || this.iv.length) {
      const b0 = this.now();
      let line = null;
      try {
        line = buildSample({ t: t - this.t0, dur: t - this.winStart, window: this.window, iv: this.iv,
          proto: this.proto, buildMs: this.lastBuildMs, ...this.snapshot() });
      } catch (err) {
        line = { k: 'e', t: r1(t - this.t0), name: 'sample-failed', data: { error: String((err && err.message) || err).slice(0, 200) } };
      }
      this.sender.push(line);
      if (line.k === 's') this.samples++;
      this.lastBuildMs = this.now() - b0;
      this.buildMs.push(this.lastBuildMs);
      if (this.buildMs.length > 120) this.buildMs.shift();
    }
    this.window = new FrameStats(MAX_INTERVALS);
    this.iv = [];
    this.winStart = t;
  }

  /** An event with its own time: {k: 'e', t, name, data, proto}. */
  event(name, data = {}) {
    if (!this.on) return;
    this.events++;
    this.sender.push({ k: 'e', t: r1(this.now() - this.t0), name, data, ...(this.proto ? { proto: { ...this.proto } } : {}) });
  }

  setProto(p) { this.proto = p ? { ...p } : null; }
}

/**
 * One sample from the window and the page's state. `s`: {t, dur, window
 * (FrameStats), iv, proto, buildMs, xr: 'vr'|'ar'|null, mode, tool, targetHz,
 * wrist (FrameStats.summary of the wrist), display {hz, eye, scale,
 * chosen_scale, foveation}, render {spv, spv_chosen, governor, max_steps},
 * refine (refineFigures), video, pair, cube, look, ui}.
 */
export function buildSample(s) {
  const w = s.wrist;
  return {
    k: 's', t: r1(s.t), dur: r1(s.dur),
    ...(s.proto ? { proto: { ...s.proto } } : {}),
    xr: s.xr || null, mode: s.mode || null, tool: s.tool || null,
    frames: frameFigures(s.window, s.targetHz),
    iv: s.iv.slice(0, MAX_INTERVALS),
    wrist: w ? { fps: r2(w.fps), mean: w.interval ? r2(w.interval.mean) : null, p95: w.interval ? r2(w.interval.p95) : null,
      missed_pct: s.targetHz ? r2(100 * w.missedFraction) : null } : null,
    display: s.display || null,
    render: s.render || null,
    refine: s.refine || null,
    video: s.video || null,
    pair: s.pair || null,
    cube: s.cube || null,
    look: s.look || null,
    ui: s.ui || null,
    rec_ms: r2(s.buildMs || 0),
  };
}

/**
 * The refinement's part of a sample from Refiner.summary() and its state:
 * `r`: {enabled, suspended, status, motion, weight, transport (rtc.summary()),
 * mode, scale, quality, depth, latency {n, p50, p95}, remoteFps,
 * lastStopFirst, lastStopFull, lastServer, lastKB, window}, `pf`: the new
 * pose→frame values.
 */
export function refineFigures(r, pf = []) {
  if (!r) return null;
  const tr = r.transport || {};
  const via = !r.enabled || r.suspended ? null : tr.state === 'video' ? 'webrtc' : 'jpeg';
  return {
    on: !!r.enabled, suspended: r.suspended || null, status: r.status, motion: r.motion, weight: r2(r.weight),
    via, mode: r.mode, scale: r.scale, quality: r.quality, depth: !!r.depth,
    remote_fps: r1(r.remoteFps),
    pose_frame: r.latency ? { n: r.latency.n, p50: r1(r.latency.p50), p95: r1(r.latency.p95) } : null,
    pf: pf.slice(-MAX_POSE_FRAME).map(r1),
    stop_first: r1(r.lastStopFirst), stop_full: r1(r.lastStopFull),
    server_ms: r.lastServer ? r1(r.lastServer.total) : null, render_ms: r.lastServer ? r1(r.lastServer.render) : null,
    kb: r1(r.lastKB), window: r.window ?? null,
  };
}

/** The WebRTC video's figures from RtcStereo.summary() (null when it is not on). */
export function videoFigures(tr) {
  if (!tr || tr.state !== 'video') return null;
  const b = tr.bitrate || {};
  return { kbps: r1(b.kbps), decode_ms: r2(b.decodeMs), jitter_ms: r2(b.jitterMs), frames_decoded: b.framesDecoded ?? null,
    frames_dropped: b.framesDropped ?? null, bytes: b.bytes ?? null, rtt_ms: r2(b.rttMs), ice: b.ice || null };
}

/** The pairing data channel's figures (transport.js linkFigures()), or null on a direct link. */
export function pairFigures(f) {
  if (!f || !f.paired) return null;
  return { state: f.state, ice: f.candidates || null, rtt_ms: r1(f.rttMs), resumes: f.resumes ?? null,
    bytes_in: f.bytesIn ?? null, bytes_out: f.bytesOut ?? null, requests: f.requests ?? null };
}

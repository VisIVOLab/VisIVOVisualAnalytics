// Refinement (issue #170): the pure logic behind blending the backend's
// full-quality stereo frames into the headset's own rendering. No DOM, no GL:
// node runs the tests (backend/tests/vr/refine.test.mjs).
//
// - parseVXR1 / inflate / decodeDepth: the stereo frame of the render session
//   (docs/backend-api/render.md, "Stereo frames for a headset").
// - poseDelta / MotionDetector: how fast the eye moves relative to the cube,
//   and when the view counts as moving, slow or still.
// - PoseBook: the poses sent, until a frame answers them (or a newer one does:
//   the backend renders only the newest of the poses waiting for it).
// - reprojection math: a frame rendered for one eye pose, drawn for another.
// - transferFunction / cropBox: the page's look, as the session's commands.

import * as M from './mat4.js';

// ── The wire ────────────────────────────────────────────────────────────────

const MAGIC = [0x56, 0x58, 0x52, 0x31];     // "VXR1"

/** True when a binary message is a stereo frame (else it is a mono JPEG). */
export function isVXR1(buf) {
  const b = new Uint8Array(buf instanceof ArrayBuffer ? buf : buf.buffer, buf.byteOffset || 0, Math.min(4, buf.byteLength));
  return b.length === 4 && MAGIC.every((v, i) => b[i] === v);
}

/**
 * A stereo frame message: {header, color: Uint8Array (JPEG), depth:
 * Uint8Array (zlib) or null}. Views into the message, no copies. Throws on a
 * message that is not one, or whose payloads run past its end.
 */
export function parseVXR1(buf) {
  const ab = buf instanceof ArrayBuffer ? buf : buf.buffer;
  const base = buf instanceof ArrayBuffer ? 0 : buf.byteOffset;
  const total = buf.byteLength;
  if (total < 8 || !isVXR1(buf)) throw new Error('not a VXR1 stereo frame');
  const n = new DataView(ab, base, total).getUint32(4, true);
  if (8 + n > total) throw new Error('VXR1 header runs past the message');
  const header = JSON.parse(new TextDecoder().decode(new Uint8Array(ab, base + 8, n)));
  const start = 8 + n;
  const part = (p) => {
    if (!p) return null;
    const off = Number(p.offset), len = Number(p.length);
    if (!(off >= 0 && len >= 0 && start + off + len <= total)) throw new Error('VXR1 payload runs past the message');
    return new Uint8Array(ab, base + start + off, len);
  };
  return { header, color: part(header.color), depth: part(header.depth) };
}

/** zlib (RFC 1950) bytes inflated, with the platform's DecompressionStream. */
export async function inflate(bytes) {
  const stream = new Blob([bytes]).stream().pipeThrough(new DecompressionStream('deflate'));
  return new Uint8Array(await new Response(stream).arrayBuffer());
}

/**
 * The depth payload (inflated) as distances in metres along the eye's -z,
 * rows top first, both eyes side by side; 0 where the ray had no sample.
 * q = 0: none; else z_near + (q - 1) / 65534 * (z_far - z_near).
 */
export function decodeDepth(raw, width, height, zNear, zFar) {
  const n = width * height;
  if (raw.byteLength < 2 * n) throw new Error(`depth of ${raw.byteLength} bytes for ${width} x ${height}`);
  const dv = new DataView(raw.buffer, raw.byteOffset, 2 * n);
  const out = new Float32Array(n);
  const k = (zFar - zNear) / 65534;
  for (let i = 0; i < n; i++) {
    const q = dv.getUint16(2 * i, true);
    out[i] = q === 0 ? 0 : zNear + (q - 1) * k;
  }
  return out;
}

// ── Motion ──────────────────────────────────────────────────────────────────

function rotationPart(a) {
  const s = Math.cbrt(Math.abs(det3(a))) || 1;
  return [a[0] / s, a[1] / s, a[2] / s, a[4] / s, a[5] / s, a[6] / s, a[8] / s, a[9] / s, a[10] / s];
}

function det3(a) {
  return a[0] * (a[5] * a[10] - a[9] * a[6]) - a[4] * (a[1] * a[10] - a[9] * a[2]) + a[8] * (a[1] * a[6] - a[5] * a[2]);
}

/**
 * How far apart two eye poses are relative to the cube. Each is the
 * model-view matrix A = view * model (model space to eye). Returns the angle
 * between the two eye orientations in degrees, and the distance between the
 * two eye positions in the cube, in metres at the second pose's scale.
 */
export function poseDelta(a, b) {
  const ra = rotationPart(a), rb = rotationPart(b);
  // trace(Ra^T Rb), column-major 3x3
  let tr = 0;
  for (let c = 0; c < 3; c++) for (let r = 0; r < 3; r++) tr += ra[c * 3 + r] * rb[c * 3 + r];
  const angle = Math.acos(Math.max(-1, Math.min(1, (tr - 1) / 2))) * 180 / Math.PI;
  const ia = M.invert(a), ib = M.invert(b);
  if (!ia || !ib) return { angleDeg: angle, metres: Infinity };
  const scale = Math.cbrt(Math.abs(det3(b))) || 1;          // metres per model unit
  const d = Math.hypot(ia[12] - ib[12], ia[13] - ib[13], ia[14] - ib[14]);
  return { angleDeg: angle, metres: d * scale };
}

// Thresholds on the eye's motion relative to the cube (angular deg/s,
// translational m/s). Head tracking held still jitters at about 0.5-2 deg/s
// and a few mm/s; slow, deliberate looking around is 5-20 deg/s; a turn of
// the head or a grab of the cube is 50-300 deg/s.
export const MOTION_DEFAULTS = {
  still: { degS: 3, mS: 0.01 },     // below: still (held for holdMs)
  slow: { degS: 15, mS: 0.06 },     // below: slow (held for holdMs); refinement asked for
  move: { degS: 30, mS: 0.15 },     // above: moving (at once); refinement dropped
  holdMs: 60,
  tauMs: 35,                        // smoothing of the velocities
};

/**
 * The view's motion state from the eye pose of each displayed frame:
 * 'moving', 'slow' or 'still', with hysteresis (enter slow below `slow`,
 * leave it above `move`). `stoppedAt` is the time the view last left
 * 'moving' (the start of "stop to refined").
 */
export class MotionDetector {
  constructor(opts = {}) {
    this.o = { ...MOTION_DEFAULTS, ...opts };
    this.reset();
  }

  reset() {
    this.state = 'moving';
    this.prev = null;
    this.tPrev = 0;
    this.degS = Infinity;
    this.mS = Infinity;
    this.belowSince = { slow: null, still: null };
    this.stoppedAt = null;
    this.dtPrev = 0;
  }

  /** One displayed frame: its time (ms) and the eye's model-view matrix. */
  push(tMs, a) {
    const o = this.o;
    if (this.prev && tMs <= this.tPrev) return this.state;
    const gap = this.prev ? tMs - this.tPrev : Infinity;
    // A pause (the page hidden, the headset off the face) starts over, as
    // moving: a gap of more than 250 ms that is also more than three times the
    // interval before it, so a page drawn slowly (under 4 frames a second)
    // still reaches still.
    if (!this.prev || (gap > 250 && gap > 3 * (this.dtPrev || 0))) {
      const dtPrev = this.prev ? gap : 0;
      if (this.prev) this.reset();
      this.prev = Float32Array.from(a);
      this.tPrev = tMs;
      this.dtPrev = dtPrev;
      return this.state;
    }
    const dt = gap;
    this.dtPrev = dt;
    const d = poseDelta(this.prev, a);
    const w = d.angleDeg / (dt / 1000), v = d.metres / (dt / 1000);
    const k = 1 - Math.exp(-dt / o.tauMs);
    this.degS = Number.isFinite(this.degS) ? this.degS + k * (w - this.degS) : w;
    this.mS = Number.isFinite(this.mS) ? this.mS + k * (v - this.mS) : v;
    this.prev = Float32Array.from(a);
    this.tPrev = tMs;
    const under = (t) => this.degS < t.degS && this.mS < t.mS;
    for (const lvl of ['slow', 'still']) {
      if (under(o[lvl])) { if (this.belowSince[lvl] === null) this.belowSince[lvl] = tMs; }
      else this.belowSince[lvl] = null;
    }
    const held = (lvl) => this.belowSince[lvl] !== null && tMs - this.belowSince[lvl] >= o.holdMs;
    const was = this.state;
    if (!under(o.move)) this.state = 'moving';
    else if (held('still')) this.state = 'still';
    else if (was === 'moving') this.state = held('slow') ? 'slow' : 'moving';
    else if (was === 'still') this.state = under({ degS: 2 * o.still.degS, mS: 2 * o.still.mS }) ? 'still' : 'slow';
    else this.state = 'slow';
    if (was === 'moving' && this.state !== 'moving') this.stoppedAt = tMs;
    return this.state;
  }
}

// ── Poses sent ──────────────────────────────────────────────────────────────

/**
 * The poses sent, by id, until answered. A frame for pose k answers every
 * pose up to k (the backend renders the newest pose waiting; the others are
 * merged into it and never get a frame of their own).
 */
export class PoseBook {
  constructor(capacity = 64) {
    this.capacity = capacity;
    this.nextId = 1;
    this.pending = new Map();
    this.skipped = 0;
  }

  /** Records a pose; returns its id. */
  add(record) {
    const id = this.nextId++;
    this.pending.set(id, { ...record, id });
    while (this.pending.size > this.capacity) this.pending.delete(this.pending.keys().next().value);
    return id;
  }

  /** The pose a frame answers (null for an unknown or stale id); drops it and
   *  every older pose. */
  take(id) {
    const rec = this.pending.get(id) || null;
    for (const k of [...this.pending.keys()]) {
      if (k < id) { this.pending.delete(k); this.skipped++; }
    }
    this.pending.delete(id);
    return rec;
  }

  /** Poses sent less than `timeoutMs` ago and not answered yet. */
  outstanding(nowMs, timeoutMs = 500) {
    let n = 0;
    for (const r of this.pending.values()) if (nowMs - r.tSent < timeoutMs) n++;
    return n;
  }

  clear() { this.pending.clear(); }
}

// ── Reprojection ────────────────────────────────────────────────────────────

/**
 * The matrix from the eye space a frame was rendered in to the current eye
 * space: E = (viewNow * modelNow) * inverse(viewFrame * modelFrame). The
 * picture is attached to the cube, so a grab of the cube moves it too. E is
 * a rotation, a translation and a uniform scale.
 */
export function reprojectionMatrix(viewFrame, modelFrame, viewNow, modelNow) {
  const inv = M.invert(M.multiply(viewFrame, modelFrame));
  return M.multiply(M.multiply(viewNow, modelNow), inv);
}

/**
 * The eye-space point of one pixel of a frame: image coordinates (u, v) in
 * 0..1 with v down from the top row, at distance d metres along -z, through
 * the frame's projection (its inverse given).
 */
export function unprojectPixel(invProj, u, v, d) {
  const p = M.transformPoint(invProj, [2 * u - 1, 1 - 2 * v, -1]);
  const k = d / -p[2];
  return [p[0] * k, p[1] * k, -d];
}

/** Where a pixel of the frame lands now: normalised device coordinates. */
export function reprojectPixel(invProjFrame, E, projNow, u, v, d) {
  const x = unprojectPixel(invProjFrame, u, v, d);
  return M.transformPoint(M.multiply(projNow, E), x);
}

/** Distance along -z of the cube's centre in the frame's eye: the depth of a
 *  pixel whose ray had no sample (a plane through the cube). */
export function centreDistance(viewFrame, modelFrame) {
  const c = M.transformPoint(M.multiply(viewFrame, modelFrame), [0, 0, 0]);
  return Math.max(1e-3, -c[2]);
}

// ── Whether a frame may be shown ─────────────────────────────────────────────

export const SHOW_DEFAULTS = {
  maxDeg: 6,          // eye orientation changed more than this since the frame's pose: drop
  maxMetres: 0.04,    // eye moved more than this relative to the cube: drop
  fadeInMs: 120,
  fadeOutMs: 60,
};

/**
 * The weight of the refined frame this displayed frame, from the last one,
 * moving toward 1 while the frame may be shown and toward 0 otherwise.
 */
export function fadeStep(weight, show, dtMs, o = SHOW_DEFAULTS) {
  if (show) return Math.min(1, weight + dtMs / o.fadeInMs);
  return Math.max(0, weight - dtMs / o.fadeOutMs);
}

/** May a frame rendered for model-view `aFrame` be shown at `aNow`? */
export function frameUsable(aFrame, aNow, motionState, o = SHOW_DEFAULTS) {
  if (motionState === 'moving') return false;
  const d = poseDelta(aFrame, aNow);
  return d.angleDeg <= o.maxDeg && d.metres <= o.maxMetres;
}

/** Has the eye moved enough since the last pose sent to send a new one? */
export function poseChanged(aSent, aNow, minDeg = 0.05, minMetres = 0.0005) {
  if (!aSent) return true;
  const d = poseDelta(aSent, aNow);
  return d.angleDeg > minDeg || d.metres > minMetres;
}

// ── The page's look as session commands ─────────────────────────────────────

/**
 * The page's colour and opacity mapping as `set_transfer_functions` points
 * on the full cube's values, so the backend's full-resolution frame looks
 * like the headset's reduced one. `p`: thrNorm, cmaxNorm, gamma, opacity, mode
 * (0 emission, 1 maximum), lo, hi (the reduced cube's normalisation), factors
 * (binning), lut (256 x RGBA8). The page's emission sample over one reduced
 * voxel has opacity 1 - exp(-opacity (0.04 + x)); a full-resolution voxel is
 * 1/f of that path, f the geometric mean of the binning factors (VTK's
 * opacity is per voxel: unit distance 1). Values below the threshold are
 * transparent; above the colour maximum, the top of the map.
 */
export function transferFunction(p, n = 48) {
  const span = p.hi - p.lo;
  const toData = (t) => p.lo + t * span;
  const thr = p.thrNorm, cmax = Math.max(p.cmaxNorm, thr + 1e-3);
  const f = Math.cbrt((p.factors || [1, 1, 1]).reduce((a, b) => a * b, 1)) || 1;
  const eps = Math.max(Math.abs(span) * 1e-6, 1e-12);
  const lutAt = (L) => {
    const x = Math.max(0, Math.min(255, L * 255));
    const i = Math.min(254, Math.floor(x)), t = x - i;
    return [0, 1, 2].map((c) => ((1 - t) * p.lut[i * 4 + c] + t * p.lut[(i + 1) * 4 + c]) / 255);
  };
  const color = [], opacity = [];
  const x0 = toData(thr);
  opacity.push([x0 - eps, 0]);
  for (let j = 0; j <= n; j++) {
    const L = j / n;
    const x = toData(thr + Math.pow(L, 1 / p.gamma) * (cmax - thr));
    const a = p.mode === 1 ? 0.25 + 0.75 * L : 1 - Math.exp(-p.opacity * (0.04 + L) / f);
    color.push([x, ...lutAt(L)]);
    opacity.push([x, Math.min(1, a)]);
  }
  return { type: 'set_transfer_functions', color, opacity, blend_mode: p.mode === 1 ? 1 : 0 };
}

/** The clipping box as `set_crop` (box fractions, x = NAXIS1); null box when whole. */
export function cropBox(clip) {
  const whole = clip.min.every((v) => v <= 0) && clip.max.every((v) => v >= 1);
  return { type: 'set_crop', box: whole ? null : { min: [...clip.min], max: [...clip.max] } };
}

/** Why the backend would refuse this clipping box (its crop_planes rule), or ''. */
export function cropError(clip) {
  const ok = (v) => typeof v === 'number' && Number.isFinite(v);
  if (!clip || clip.min.length !== 3 || clip.max.length !== 3 || ![...clip.min, ...clip.max].every(ok)) {
    return 'not three finite numbers per corner';
  }
  for (let i = 0; i < 3; i++) {
    if (!(clip.min[i] >= 0 && clip.min[i] < clip.max[i] && clip.max[i] <= 1)) return `axis ${'xyz'[i]} is not 0 <= min < max <= 1`;
  }
  return '';
}

// ── Statistics ──────────────────────────────────────────────────────────────

/** Rolling window of samples (times or values) for the HUD. */
export class Rolling {
  constructor(n = 120) { this.n = n; this.v = []; this.total = 0; }
  /** `total` counts every push (a recorder takes the values new since it last looked). */
  push(x) { this.v.push(x); this.total++; if (this.v.length > this.n) this.v.shift(); }
  get length() { return this.v.length; }
  quantile(q) {
    if (!this.v.length) return NaN;
    const s = [...this.v].sort((a, b) => a - b);
    return s[Math.min(s.length - 1, Math.floor(q * s.length))];
  }
  /** Events per second over the last `windowMs`, for a series of times (ms). */
  rate(nowMs, windowMs = 2000) {
    const recent = this.v.filter((t) => nowMs - t <= windowMs);
    if (recent.length < 2) return 0;
    return (recent.length - 1) / ((recent[recent.length - 1] - recent[0]) / 1000);
  }
}

/** A token a WebSocket subprotocol can carry (RFC 6455: an HTTP token). */
export function subprotocolSafe(token) {
  return typeof token === 'string' && /^[A-Za-z0-9!#$%&'*+.^_`|~-]+$/.test(token);
}

/**
 * Does this create_session error mean "no render sessions here" (give up for
 * this cube) rather than "try again" ? 404/503 "… not available", or a bare
 * 404 "Not Found" (a backend without the render routes). A 404 "Unknown
 * dataset_id" is not: the dataset is reopened and the refiner connects again.
 */
export function renderUnavailable(message) {
  const m = String(message || '');
  return /^(404|503):.*not available/i.test(m) || /^404: (Not Found)?$/.test(m.trim());
}

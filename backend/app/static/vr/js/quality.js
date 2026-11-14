// Safe performance defaults (epic #166): the frame-rate governor's second
// stage, the eye-buffer scale, what the panel says when quality was lowered,
// and the memory budget a cube must fit before anything is allocated.
//
// The first stage is volume.js Governor (samples per voxel, every frame).
// When that is already at its floor and frames are still missed, the eye
// buffer is made smaller, one step at a time (never below 0.85×, issue #226),
// at most every few seconds; it
// is never raised again within the session (a layer swap costs a hitch, and
// an up-and-down oscillation would cost one every few seconds); the next
// entry into VR starts again from the chosen resolution.
//
// Pure: unit-tested in backend/tests/vr/quality.test.mjs.

/** The eye-buffer scale factors (framebufferScaleFactor, absolute: 1 is the
 *  headset's recommended resolution) the governor steps down through, from
 *  the one chosen on the page. Issue #226: the first Quest 3 session showed
 *  0.7× made the text and the cube blurry for a few frames per second gained;
 *  the eye buffer is now the last resort and never goes below 0.85×. */
export const EYE_SCALE_STEPS = [1.25, 1, 0.85];
export const EYE_SCALE_FLOOR = 0.85;

/** The display rate the page asks for by default (issue #226): 72 Hz leaves
 *  13.9 ms a frame, so the cube keeps more samples per voxel than at 90 Hz. */
export const DEFAULT_RATE_HZ = 72;

/** The order the governor gives quality up in, for the diagnostics and the
 *  measurement records (issue #226). */
export function qualityPolicy({ targetHz, spvMin = 0.25, spvChosen = 1, eyeChosen = 1 } = {}) {
  return `${targetHz ? `${Math.round(targetHz)} Hz` : 'display rate'}; samples/voxel first (${spvChosen} down to ${spvMin}), `
    + `then the eye buffer as a last resort (${eyeChosen}× down to ${Math.min(eyeChosen, EYE_SCALE_FLOOR)}× at most)`;
}

export class EyeBufferGovernor {
  constructor({ start = 1, holdMs = 3000, missLimit = 0.1, steps = EYE_SCALE_STEPS } = {}) {
    this.start = start;
    this.scale = start;
    this.holdMs = holdMs;
    this.missLimit = missLimit;
    this.steps = steps;
    this.reset(start);
  }

  /** A new VR session: from `start` again, and enabled again (a layer swap
   *  that failed in the last session does not count against this one). */
  reset(start = this.start) {
    this.start = start;
    this.scale = start;
    this.disabled = false;
    this.windowMs = 0;
    this.missed = 0;
    this.frames = 0;
    this.lowered = 0;
  }

  /** The next smaller step under `s`, or s when there is none. */
  nextDown(s) {
    const below = this.steps.filter((v) => v < s - 1e-6 && v >= EYE_SCALE_FLOOR - 1e-6);
    return below.length ? Math.max(...below) : s;
  }

  /**
   * One frame: its interval, the display's, and whether the samples per
   * voxel are at their floor. Returns the eye-buffer scale to use (a change
   * when it differs from the last one).
   */
  update(intervalMs, targetMs, spvAtFloor) {
    if (!(intervalMs > 0) || !(targetMs > 0) || intervalMs > 250) return this.scale;
    this.windowMs += intervalMs;
    this.frames++;
    if (intervalMs > targetMs * 1.5) this.missed++;
    if (this.windowMs < this.holdMs) return this.scale;
    const missedFraction = this.missed / this.frames;
    this.windowMs = 0; this.missed = 0; this.frames = 0;
    if (spvAtFloor && missedFraction > this.missLimit && !this.disabled) {
      const next = this.nextDown(this.scale);
      if (next < this.scale) { this.scale = next; this.lowered++; }
    }
    return this.scale;
  }
}

/**
 * Has the samples-per-voxel governor (volume.js Governor) gone as low as it
 * can? Only then may the eye-buffer stage act (issue #226: samples per voxel
 * are given up first, all the way to their floor; until #226 the eye buffer
 * was lowered from 0.5 samples per voxel on).
 */
export function spvAtFloor({ value, min }) {
  return value <= min * 1.02;
}

/**
 * What the panel says about quality: '' while it is as chosen, else
 * "Quality lowered to hold 90 Hz: 0.55 samples/voxel, eye buffer 0.85×".
 */
export function qualityNote({ governorOn, spv, spvChosen, eyeScale, eyeChosen, targetHz }) {
  if (!governorOn) return '';
  const parts = [];
  if (spv < spvChosen * 0.95) parts.push(`${spv.toFixed(2)} samples/voxel (of ${spvChosen})`);
  if (eyeScale !== undefined && eyeChosen !== undefined && eyeScale < eyeChosen - 1e-6) parts.push(`eye buffer ${eyeScale}× (of ${eyeChosen}×)`);
  if (!parts.length) return '';
  return `Quality lowered to hold ${targetHz ? `${Math.round(targetHz)} Hz` : 'the frame rate'}: ${parts.join(', ')}`;
}

const MiB = 1024 * 1024;

/**
 * How much GPU texture this device may give one cube. A standalone headset
 * shares its memory between the system, the browser and the page, and a tab
 * that goes over is killed with no message, so its share is kept small:
 * 256 MiB on a headset with 8 GB (navigator.deviceMemory, which the browser
 * reports rounded and capped at 8), 128 MiB with 4 GB or less; a computer's
 * browser 1 GiB. These are conservative guesses until measured on a Quest 3.
 */
export function textureLimitBytes({ headset = false, deviceMemoryGB } = {}) {
  const gb = Number.isFinite(deviceMemoryGB) && deviceMemoryGB > 0 ? deviceMemoryGB : 8;
  if (headset) return gb <= 4 ? 128 * MiB : 256 * MiB;
  return gb <= 4 ? 512 * MiB : 1024 * MiB;
}

/**
 * Does a cube of `dims` voxels and `encoding` fit, before it is read or
 * allocated? {ok, textureBytes, gpuBytes, peakBytes, limitBytes, reason}.
 * On the GPU (gpuBytes, what is checked against the device's share) the cube
 * keeps its texture, 1 (8-bit) or 2 (16-bit float) bytes a voxel, the
 * selection mask's storage over the same grid (one byte a voxel, allocated
 * with the cube so that no 3-D texture is allocated in VR, issue #229) and
 * the empty-space grid (1/512 of the voxels, 4 bytes each). While it loads
 * the page also holds the download (peakBytes). The GPU's 3-D texture side
 * limit is checked first. With `capacity` (the renderer's fixed capacity,
 * issue #235) it also says whether the cube goes into it (`inCapacity`: no
 * new GPU memory, shown at once in VR) and what the capacity holds
 * (`capacityBytes`); a cube that does not is still checked alone, since the
 * page gives the capacity back when both would not fit.
 */
export function cubeBudget({ dims, encoding }, { maxTex3D = 2048, headset = false, deviceMemoryGB, capacity = null } = {}) {
  const [w, h, d] = dims.map(Number);
  const nvox = w * h * d;
  const bpv = encoding === 'float16' ? 2 : 1;
  const textureBytes = nvox * bpv;
  const macro = Math.ceil(w / 8) * Math.ceil(h / 8) * Math.ceil(d / 8) * 4;
  const gpuBytes = textureBytes + nvox + macro;
  const peakBytes = textureBytes + gpuBytes;
  const limitBytes = textureLimitBytes({ headset, deviceMemoryGB });
  // Issue #235: a cube that fits the page's fixed capacity is written into it
  // (no new GPU memory, and it can be shown in VR); one that does not is
  // held in storage of its own (gpuBytes) and waits for the end of a session.
  const inCapacity = !!capacity && capacity.encoding === encoding && (capacity.cell ?? 8) === 8
    && [w, h, d].every((v, i) => v <= capacity.dims[i]);
  const capacityBytes = capacity ? capacity.totalBytes : 0;
  const mb = (b) => `${(b / MiB).toFixed(0)} MiB`;
  if (Math.max(w, h, d) > maxTex3D) {
    return { ok: false, textureBytes, gpuBytes, peakBytes, limitBytes, inCapacity, capacityBytes,
      reason: `${w}×${h}×${d} exceeds this GPU's 3-D texture limit (${maxTex3D} per side).` };
  }
  if (gpuBytes > limitBytes) {
    return { ok: false, textureBytes, gpuBytes, peakBytes, limitBytes, inCapacity, capacityBytes,
      reason: `${w}×${h}×${d} ${bpv === 2 ? '16-bit' : '8-bit'} needs ${mb(gpuBytes)} of GPU memory (${mb(textureBytes)} of texture, `
        + `${mb(nvox)} for the selection mask; ${mb(peakBytes)} while loading); `
        + `this ${headset ? 'headset' : 'browser'} allows ${mb(limitBytes)} for one cube.` };
  }
  return { ok: true, textureBytes, gpuBytes, peakBytes, limitBytes, inCapacity, capacityBytes, reason: '' };
}

/**
 * GPU bytes of a fixed capacity of `dims` voxels (issue #235): two banks
 * (the cube on show and the next one), each a texture of `encoding` and an
 * empty-space grid (R32F, one cell per `cell`^3 voxels), and one selection
 * mask (R8) shared by them. {volumeBytes, macroBytes, maskBytes, totalBytes},
 * the first two for one bank.
 */
export function capacityBytes(dims, encoding, cell = 8) {
  const nvox = dims[0] * dims[1] * dims[2];
  const volumeBytes = nvox * (encoding === 'float16' ? 2 : 1);
  const macroBytes = dims.reduce((a, v) => a * Math.ceil(v / cell), 1) * 4;
  return { volumeBytes, macroBytes, maskBytes: nvox, totalBytes: 2 * (volumeBytes + macroBytes) + nvox };
}

/** The smallest capacity side considered: below it, no capacity (every cube in VR waits). */
export const CAPACITY_MIN_SIDE = 32;

/**
 * The fixed capacity the page allocates before any VR session (issue #235),
 * so that a cube that arrives in VR is written into storage that already
 * exists (on a Quest 3 a 3-D allocation during a session resets the GPU
 * process, #229). Per axis the page's cube budget: `side` (the Max side
 * the page asks the backend for, which automatic binning keeps every axis
 * under) up to the GPU's 3-D texture limit, widened to `atLeast` (the cube
 * about to be shown, when it is larger) when the two banks of that still fit
 * this device's share (`limitBytes`); else the plain side; else the largest
 * side, a multiple of 8, whose two banks fit; null below CAPACITY_MIN_SIDE.
 * {dims, encoding, cell, grown, shrunk, ...capacityBytes}.
 */
export function planCapacity({ side, encoding = 'uint8', atLeast = null, maxTex3D = 2048, limitBytes, cell = 8 }) {
  const s0 = Math.max(1, Math.min(Math.floor(Number(side) || 256), maxTex3D));
  const base = [s0, s0, s0];
  const plan = (dims, extra) => ({ dims, encoding, cell, grown: false, shrunk: false, ...extra, ...capacityBytes(dims, encoding, cell) });
  const fits = (dims) => capacityBytes(dims, encoding, cell).totalBytes <= limitBytes;
  if (atLeast) {
    const grown = base.map((v, i) => Math.min(maxTex3D, Math.max(v, Number(atLeast[i]) || 0)));
    if (grown.some((v, i) => v !== base[i]) && fits(grown)) return plan(grown, { grown: true });
  }
  if (fits(base)) return plan(base);
  for (let s = Math.floor((s0 - 1) / 8) * 8; s >= CAPACITY_MIN_SIDE; s -= 8) {
    if (fits([s, s, s])) return plan([s, s, s], { shrunk: true });
  }
  return null;
}

/** Is `a` the same capacity as `b` (dims, encoding, cell)? */
export function sameCapacity(a, b) {
  if (!a || !b) return !a && !b;
  return a.encoding === b.encoding && (a.cell ?? 8) === (b.cell ?? 8) && a.dims.every((v, i) => v === b.dims[i]);
}

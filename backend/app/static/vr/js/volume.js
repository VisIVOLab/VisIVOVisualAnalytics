// Shader-independent volume logic of the VR page: decoding the backend's
// reduced cube, the empty-space-skipping grid, axis ticks in world
// coordinates, the explicit velocity scale, the frame-rate governor and a
// synthetic cube for benchmarks. No DOM, no GL: node runs the tests.

// ── Values ──────────────────────────────────────────────────────────────────
// The shader works on a normalised value n: data = lo + n (hi - lo), blank < 0.
// uint8 codes (/v1/cube/reduced_bin): 0 blank, 1..255 = n in [0, 1].
// float16: n itself, blank = -1.

export function codeToNorm(code) {
  return code === 0 ? -1 : (code - 1) / 254;
}

let HALF_TABLE = null;
/** float16 bits -> number, through a 64 K table built once. */
export function halfToFloat(bits) {
  if (!HALF_TABLE) {
    HALF_TABLE = new Float32Array(65536);
    for (let h = 0; h < 65536; h++) {
      const s = h & 0x8000 ? -1 : 1, e = (h >> 10) & 0x1f, f = h & 0x3ff;
      let v;
      if (e === 0) v = s * Math.pow(2, -14) * (f / 1024);
      else if (e === 31) v = f ? NaN : s * Infinity;
      else v = s * Math.pow(2, e - 15) * (1 + f / 1024);
      HALF_TABLE[h] = v;
    }
  }
  return HALF_TABLE[bits];
}

/** A normalised value as the shader reads it from a voxel of `data`. */
export function voxelNorm(data, encoding, i) {
  return encoding === 'uint8' ? codeToNorm(data[i]) : halfToFloat(data[i]);
}

export function normToData(n, lo, hi) { return lo + n * (hi - lo); }
export function dataToNorm(v, lo, hi) { return hi > lo ? (v - lo) / (hi - lo) : 0; }

/**
 * Empty-space-skipping grid: the largest normalised value in each cell of
 * `cell`^3 voxels, widened by one voxel on every side because trilinear
 * filtering reads the neighbours. Blank-only cells hold -1.
 * Returns {data: Float32Array, dims: [cx, cy, cz]}.
 */
export function buildMacrocells(data, dims, encoding, cell = 8) {
  const [w, h, d] = dims;
  const cx = Math.ceil(w / cell), cy = Math.ceil(h / cell), cz = Math.ceil(d / cell);
  // Per-voxel maxima reduced first along x into a (w/cell) grid per row with
  // the one-voxel apron, then along y and z: three passes instead of 27 reads.
  const rowMax = new Float32Array(cx * h * d).fill(-1);
  const norm = encoding === 'uint8'
    ? (() => { const t = new Float32Array(256); for (let c = 0; c < 256; c++) t[c] = codeToNorm(c); return t; })()
    : null;
  for (let z = 0; z < d; z++) {
    for (let y = 0; y < h; y++) {
      const base = (z * h + y) * w, out = (z * h + y) * cx;
      for (let c = 0; c < cx; c++) {
        const x0 = Math.max(0, c * cell - 1), x1 = Math.min(w - 1, (c + 1) * cell);
        let m = -1;
        if (norm) {
          for (let x = x0; x <= x1; x++) { const v = norm[data[base + x]]; if (v > m) m = v; }
        } else {
          for (let x = x0; x <= x1; x++) { const v = halfToFloat(data[base + x]); if (v > m) m = v; }
        }
        rowMax[out + c] = m;
      }
    }
  }
  const colMax = new Float32Array(cx * cy * d).fill(-1);
  for (let z = 0; z < d; z++) {
    for (let c = 0; c < cy; c++) {
      const y0 = Math.max(0, c * cell - 1), y1 = Math.min(h - 1, (c + 1) * cell);
      for (let x = 0; x < cx; x++) {
        let m = -1;
        for (let y = y0; y <= y1; y++) { const v = rowMax[(z * h + y) * cx + x]; if (v > m) m = v; }
        colMax[(z * cy + c) * cx + x] = m;
      }
    }
  }
  const out = new Float32Array(cx * cy * cz).fill(-1);
  for (let c = 0; c < cz; c++) {
    const z0 = Math.max(0, c * cell - 1), z1 = Math.min(d - 1, (c + 1) * cell);
    for (let y = 0; y < cy; y++) {
      for (let x = 0; x < cx; x++) {
        let m = -1;
        for (let z = z0; z <= z1; z++) { const v = colMax[(z * cy + y) * cx + x]; if (v > m) m = v; }
        out[(c * cy + y) * cx + x] = m;
      }
    }
  }
  return { data: out, dims: [cx, cy, cz] };
}

/** The `q` quantile (0..1) of the non-blank normalised values at or above
 *  `above`, from a stride sample of about `target` voxels. */
export function normQuantile(data, encoding, q, target = 2_000_000, above = 0) {
  const step = Math.max(1, Math.floor(data.length / target));
  const hist = new Uint32Array(4096);
  let n = 0;
  for (let i = 0; i < data.length; i += step) {
    const v = voxelNorm(data, encoding, i);
    if (!(v >= 0) || v < above) continue;
    hist[Math.min(4095, Math.floor(Math.min(v, 1) * 4095))]++;
    n++;
  }
  if (!n) return 1;
  const want = q * n;
  let acc = 0;
  for (let b = 0; b < 4096; b++) { acc += hist[b]; if (acc >= want) return (b + 1) / 4096; }
  return 1;
}

// ── Axes in world coordinates ───────────────────────────────────────────────

/** "Nice" tick values covering [a, b] (either order), about `n` of them. */
export function niceTicks(a, b, n = 5) {
  const lo = Math.min(a, b), hi = Math.max(a, b);
  if (!(hi > lo)) return [lo];
  const raw = (hi - lo) / Math.max(1, n);
  const mag = Math.pow(10, Math.floor(Math.log10(raw)));
  const norm = raw / mag;
  const step = (norm < 1.5 ? 1 : norm < 3 ? 2 : norm < 7 ? 5 : 10) * mag;
  const out = [];
  for (let v = Math.ceil(lo / step) * step; v <= hi + step * 1e-9; v += step) {
    out.push(Math.abs(v) < step * 1e-9 ? 0 : v);
  }
  return out.length ? out : [lo];
}

export function tickStep(values) {
  return values.length > 1 ? Math.abs(values[1] - values[0]) : 0;
}

/**
 * Texture coordinate t in [0, 1] of the world value `v` along an axis whose
 * reduced voxel i (centre at t = (i + 0.5) / N) has world coordinate
 * world[i]. The array may increase or decrease (RA, a frequency-ordered
 * velocity axis); nulls are skipped. null when `v` is outside the axis.
 */
export function worldToTex(world, v) {
  const n = world.length;
  for (let i = 0; i + 1 < n; i++) {
    const a = world[i], b = world[i + 1];
    if (a === null || b === null) continue;
    if ((v - a) * (v - b) <= 0 && a !== b) {
      const f = (v - a) / (b - a);
      return (i + f + 0.5) / n;
    }
  }
  if (n === 1 && world[0] === v) return 0.5;
  return null;
}

// ── Longitudes across 0° ────────────────────────────────────────────────────
// A right ascension (or galactic or ecliptic longitude) axis crossing 0° comes
// from the backend as it is on the sky, e.g. [359.8, 359.9, 0, 0.1, 0.2].
// Interpolated or ticked as plain numbers it would run backwards through 180°.
// It is unwrapped first (each value within 180° of the one before), worked on
// as a continuous line, and every value given back is wrapped to [0, 360).

/** Is this reduced_info axis a longitude in degrees? */
export function isLongitudeAxis(axis) {
  if (!axis) return false;
  const unit = String(axis.unit || 'deg').toLowerCase();
  if (unit !== 'deg' && unit !== '') return false;
  return [axis.name, axis.ctype].some((s) => /^(RA|GLON|ELON)(-|$)/i.test(String(s || '').trim()));
}

/** The values made continuous: each within 180° of the previous finite one
 *  (nulls kept as nulls). */
export function unwrapDegrees(values) {
  let prev = null;
  return values.map((v) => {
    if (v === null || !Number.isFinite(v)) return v;
    let u = v;
    if (prev !== null) u -= 360 * Math.round((u - prev) / 360);
    prev = u;
    return u;
  });
}

/** v in [0, 360). */
export function wrap360(v) {
  if (v === null || v === undefined || !Number.isFinite(v)) return v;
  const w = ((v % 360) + 360) % 360;
  return Math.abs(w - 360) < 1e-9 || Math.abs(w) < 1e-12 ? 0 : w;
}

/** The world coordinate of an axis at texture coordinate t: texToWorld, made
 *  right for a longitude crossing 0°. */
export function axisTexToWorld(axis, world, t) {
  if (!isLongitudeAxis(axis) || world !== axis.world) return texToWorld(world, t);
  const v = texToWorld(unwrapDegrees(world), t);
  return v !== null && v !== undefined && (v < 0 || v >= 360) ? wrap360(v) : v;
}

/** World coordinate at texture coordinate t (linear between voxel centres,
 *  clamped to the first and last centre). */
export function texToWorld(world, t) {
  const n = world.length;
  const x = Math.min(n - 1, Math.max(0, t * n - 0.5));
  const i = Math.min(n - 2, Math.floor(x));
  if (n === 1) return world[0];
  const a = world[i], b = world[i + 1];
  if (a === null || b === null) return a ?? b;
  return a + (b - a) * (x - i);
}

function decimalsFor(step) {
  return step > 0 ? Math.max(0, Math.min(6, -Math.floor(Math.log10(step) + 1e-9))) : 2;
}

/**
 * Ticks of one axis of /v1/cube/reduced_info: [{t, label}], plus the title.
 * The z axis is labelled in km/s when the backend gives a velocity.
 */
export function axisTicks(axis, which, n = 5) {
  let world = axis.world, unit = axis.unit || '', title = axis.name || which;
  if (which === 'z' && Array.isArray(axis.velocity_kms) && axis.velocity_kms.length) {
    world = axis.velocity_kms;
    unit = 'km/s';
    title = 'Velocity';
  }
  // A longitude crossing 0° is ticked on its unwrapped line and labelled wrapped.
  const lon = which !== 'z' && isLongitudeAxis(axis);
  if (lon) world = unwrapDegrees(world);
  const finite = world.filter((v) => v !== null && Number.isFinite(v));
  if (!finite.length) return { title, unit, ticks: [] };
  const lo = Math.min(...finite), hi = Math.max(...finite);
  const values = niceTicks(lo, hi, n);
  const step = tickStep(values);
  const dec = decimalsFor(step);
  const ticks = [];
  for (const v of values) {
    const t = worldToTex(world, v);
    // Only values off [0, 360) are wrapped: the others are left exactly as they were.
    const shown = lon && (v < 0 || v >= 360) ? wrap360(v) : v;
    if (t !== null) ticks.push({ t, value: shown, label: shown.toFixed(dec) });
  }
  return { title, unit, ticks };
}

export function axisTitle(axisTicksResult) {
  const { title, unit } = axisTicksResult;
  return unit ? `${title} (${unit})` : title;
}

/**
 * Model-space extents of the box (longest side = 1): the cube's full-resolution
 * pixels on each axis (a partial last block is drawn at its true width); the
 * spectral axis is stretched by `zStretch` channels per spatial pixel. Pixels
 * are drawn square on the sky even when CDELT1 and CDELT2 differ.
 */
export function boxExtent(fullShape, zStretch = 1) {
  const ex = fullShape[0], ey = fullShape[1], ez = fullShape[2] * zStretch;
  const m = Math.max(ex, ey, ez);
  return [ex / m, ey / m, ez / m];
}

/**
 * Box coordinate (0..1 over the full extent) -> texture coordinate: the texture
 * spans whole blocks (reduced * factor pixels), the box only the cube's pixels.
 */
export function texScale(fullShape, factors, reducedShape) {
  return [0, 1, 2].map((i) => fullShape[i] / (reducedShape[i] * factors[i]));
}

/** 0-based full-resolution pixel of each reduced voxel's centre along one axis
 *  (a partial last block is centred on the pixels it holds): the backend's
 *  cube_reduce.block_centres, where its world coordinates are evaluated. */
export function blockCentres(nFull, factor) {
  const out = [];
  for (let s = 0; s < nFull; s += factor) out.push(0.5 * (s + Math.min(s + factor, nFull) - 1));
  return out;
}

/**
 * Texture-index coordinate t (voxel i's centre at (i + 0.5) / N_reduced, the
 * coordinate worldToTex/texToWorld use) -> box coordinate u (0..1 over the
 * cube's full-resolution pixels, pixel p's centre at (p + 0.5) / N_full),
 * through the block centres, so a partial last block is placed on its pixels.
 */
export function texToBox(t, nFull, factor) {
  const c = blockCentres(nFull, factor);
  const nr = c.length;
  if (nr === 1) return 0.5;
  const x = t * nr - 0.5;
  const i = Math.max(0, Math.min(nr - 2, Math.floor(x)));
  const px = c[i] + (c[i + 1] - c[i]) * (x - i);
  return (px + 0.5) / nFull;
}

/** Inverse of texToBox. */
export function boxToTex(u, nFull, factor) {
  const c = blockCentres(nFull, factor);
  const nr = c.length;
  if (nr === 1) return 0.5;
  const px = u * nFull - 0.5;
  let i = 0;
  while (i < nr - 2 && px > c[i + 1]) i++;
  const x = i + (px - c[i]) / (c[i + 1] - c[i]);
  return (x + 0.5) / nr;
}

/**
 * The explicit scale of the velocity axis against the sky, as a sentence:
 * one channel is drawn as `zStretch` spatial pixels.
 */
export function velocityScaleText(info, zStretch = 1) {
  const z = info.axes && info.axes.z;
  const pix = info.pixel_scale_arcsec;
  const dv = z && z.channel_width_kms;
  const fz = info.factors ? info.factors[2] : 1;
  const sky = pix ? `${(pix * zStretch).toFixed(1)}\u2033 of sky` : `${zStretch} pixel(s)`;
  const held = `; a drawn voxel holds ${fz} channel${fz === 1 ? '' : 's'}`;
  if (dv) {
    const perArcmin = pix ? (dv / (pix * zStretch)) * 60 : null;
    return `Velocity scale: 1 full-resolution channel = ${dv.toFixed(2)} km/s drawn as ${sky}`
      + (perArcmin ? ` (${perArcmin.toFixed(1)} km/s per arcmin of sky)` : '')
      + `, stretch \u00d7${zStretch.toFixed(2)}${held}`;
  }
  return `Spectral scale: 1 full-resolution channel drawn as ${sky}, stretch \u00d7${zStretch.toFixed(2)}${held}`;
}

/** The same scale, short enough for a label over the box. */
export function velocityScaleShort(info, zStretch = 1) {
  const z = info.axes && info.axes.z;
  const pix = info.pixel_scale_arcsec;
  const dv = z && z.channel_width_kms;
  const fz = info.factors ? info.factors[2] : 1;
  const sky = pix ? `${(pix * zStretch).toFixed(1)}\u2033` : `${zStretch.toFixed(2)} px`;
  const per = fz > 1 ? `, ${fz} per voxel` : '';
  return dv ? `depth: 1 channel ${dv.toFixed(2)} km/s = ${sky} (\u00d7${zStretch.toFixed(2)}${per})`
    : `depth: 1 channel = ${sky} (\u00d7${zStretch.toFixed(2)}${per})`;
}

/** Reduction factors that fit a cube in `maxDim` voxels per side, the
 *  backend's rule (app/cube_reduce.auto_factors), for the UI preview. */
export function autoFactors(fullShape, maxDim = 256) {
  const [w, h, d] = fullShape;
  const fxy = Math.max(1, Math.ceil(Math.max(w, h) / maxDim));
  const fz = Math.max(1, Math.ceil(d / maxDim));
  return [fxy, fxy, fz];
}

export function reducedShape(fullShape, factors) {
  return [0, 1, 2].map((i) => Math.ceil(fullShape[i] / factors[i]));
}

/**
 * Watches a brick pyramid build through its progress: see(progress, now) is
 * '' while it moves, 'stalled' when the fraction has not grown for `stallMs`,
 * 'timeout' past `totalMs` in all. A build of a few hundred GB takes minutes
 * (about 45 s for 23 GiB on PLEIADI, 14 s for 11 GiB on an M5 Max), so the
 * defaults (5 min still, 2 h in all) only catch a build that is gone.
 */
export function buildWatch(stallMs = 300000, totalMs = 7200000) {
  let start = null, last = null, best = -1;
  return {
    see(progress, now) {
      if (start === null) { start = now; last = now; }
      const p = Number.isFinite(progress) ? progress : 0;
      if (p > best + 1e-6) { best = p; last = now; }
      if (now - start > totalMs) return 'timeout';
      if (now - last > stallMs) return 'stalled';
      return '';
    },
    progress() { return Math.max(0, best); },
  };
}

/** What the binning label adds for a reduced cube read from a brick pyramid
 *  level (its voxels cover the same blocks; their values are means of the
 *  level's means), '' for one reduced from the whole cube. */
export function sourceNote(cube) {
  return cube && cube.source === 'pyramid' && Number.isInteger(cube.pyramidLevel)
    ? `, pyramid level ${cube.pyramidLevel}` : '';
}

// ── Sampling ────────────────────────────────────────────────────────────────

/**
 * Ray-march step in texture space units of the longest reduced axis, and the
 * largest number of samples a ray can take (across the box diagonal), for
 * `samplesPerVoxel` samples per reduced voxel.
 */
export function stepInfo(dims, samplesPerVoxel) {
  const maxDim = Math.max(...dims);
  const spv = Math.max(0.05, samplesPerVoxel);
  const maxSteps = Math.ceil(Math.hypot(dims[0], dims[1], dims[2]) * spv);
  return { voxelsPerStep: 1 / spv, maxSteps, maxDim };
}

/**
 * Frame-rate governor: lowers the samples per voxel when frames are missed,
 * raises them slowly when every frame is on time. Frame intervals are read
 * from the XR frame callbacks; a missed frame shows as an interval of about
 * twice the refresh period.
 */
export class Governor {
  constructor({ min = 0.25, max = 2.0, start = 1.0, upAfterMs = 2000 } = {}) {
    this.min = min; this.max = max; this.value = start; this.upAfterMs = upAfterMs;
    this.goodMs = 0; this.missed = 0;
  }

  /** Feed one frame interval; returns the samples-per-voxel to use. */
  update(intervalMs, targetMs) {
    if (!(intervalMs > 0) || !(targetMs > 0) || intervalMs > 250) return this.value;
    if (intervalMs > targetMs * 1.5) {
      this.missed++;
      this.goodMs = 0;
      if (this.missed >= 3) {
        this.value = Math.max(this.min, this.value * 0.85);
        this.missed = 0;
      }
    } else {
      this.missed = Math.max(0, this.missed - 1);
      this.goodMs += intervalMs;
      if (this.goodMs >= this.upAfterMs) {
        this.value = Math.min(this.max, this.value * 1.1);
        this.goodMs = 0;
      }
    }
    return this.value;
  }
}

/** Rolling frame statistics: mean / p95 of the intervals over a window. */
export class FrameStats {
  constructor(size = 180) { this.size = size; this.buf = []; this.cpu = []; this.gpu = []; }
  push(intervalMs, cpuMs, gpuMs) {
    if (intervalMs > 0 && intervalMs < 1000) this.buf.push(intervalMs);
    if (cpuMs >= 0) this.cpu.push(cpuMs);
    if (gpuMs >= 0) this.gpu.push(gpuMs);
    for (const a of [this.buf, this.cpu, this.gpu]) while (a.length > this.size) a.shift();
  }
  static stats(a) {
    if (!a.length) return null;
    const s = [...a].sort((x, y) => x - y);
    const mean = a.reduce((p, v) => p + v, 0) / a.length;
    return { mean, p50: s[Math.floor(0.5 * (s.length - 1))], p95: s[Math.floor(0.95 * (s.length - 1))] };
  }
  summary(targetMs) {
    const f = FrameStats.stats(this.buf);
    const missed = targetMs ? this.buf.filter((v) => v > targetMs * 1.5).length : 0;
    return {
      fps: f ? 1000 / f.mean : 0,
      interval: f,
      cpu: FrameStats.stats(this.cpu),
      gpu: FrameStats.stats(this.gpu),
      missedFraction: this.buf.length ? missed / this.buf.length : 0,
      n: this.buf.length,
    };
  }
}

// ── Synthetic cube (benchmarks without a backend) ──────────────────────────

/**
 * An n^3 uint8 cube that looks like an HI field: a rotating disc whose
 * line-of-sight velocity shifts with position (a tilted "S"), a few compact
 * sources, and noise. Codes as /v1/cube/reduced_bin's uint8 encoding.
 */
export function syntheticCube(n, seed = 1) {
  const out = new Uint8Array(n * n * n);
  let s = seed >>> 0;
  const rnd = () => { s = (s * 1664525 + 1013904223) >>> 0; return s / 4294967296; };
  // Noise: around code 30 (sd ~6 codes), the emission above it.
  for (let i = 0; i < out.length; i++) {
    out[i] = 30 + Math.floor((rnd() + rnd() + rnd() - 1.5) * 12);
  }
  const c = (n - 1) / 2, r0 = 0.35 * n, vrot = 0.25 * n, sig = 0.03 * n + 1;
  for (let y = 0; y < n; y++) {
    for (let x = 0; x < n; x++) {
      const dx = x - c, dy = (y - c) / 0.6;   // inclined disc
      const r = Math.hypot(dx, dy);
      if (r > r0) continue;
      const vlos = c + vrot * (dx / Math.max(r, 1)) * Math.min(1, r / (0.15 * n));
      const amp = 200 * Math.exp(-r / (0.2 * n));
      const z0 = Math.max(0, Math.floor(vlos - 3 * sig)), z1 = Math.min(n - 1, Math.ceil(vlos + 3 * sig));
      for (let z = z0; z <= z1; z++) {
        const g = amp * Math.exp(-0.5 * ((z - vlos) / sig) ** 2);
        const i = (z * n + y) * n + x;
        out[i] = Math.min(255, out[i] + g);
      }
    }
  }
  for (let k = 0; k < 6; k++) {
    const bx = rnd() * n, by = rnd() * n, bz = rnd() * n, br = (0.02 + 0.03 * rnd()) * n;
    const x0 = Math.max(0, Math.floor(bx - 2 * br)), x1 = Math.min(n - 1, Math.ceil(bx + 2 * br));
    const y0 = Math.max(0, Math.floor(by - 2 * br)), y1 = Math.min(n - 1, Math.ceil(by + 2 * br));
    const z0 = Math.max(0, Math.floor(bz - 2 * br)), z1 = Math.min(n - 1, Math.ceil(bz + 2 * br));
    for (let z = z0; z <= z1; z++) for (let y = y0; y <= y1; y++) for (let x = x0; x <= x1; x++) {
      const g = 120 * Math.exp(-((x - bx) ** 2 + (y - by) ** 2 + (z - bz) ** 2) / (2 * br * br));
      const i = (z * n + y) * n + x;
      out[i] = Math.min(255, out[i] + g);
    }
  }
  for (let i = 0; i < out.length; i++) if (out[i] === 0) out[i] = 1;  // 0 is blank
  return out;
}

/** reduced_info-like metadata for the synthetic cube. */
export function syntheticInfo(n) {
  const world = (a, b) => Array.from({ length: n }, (_, i) => a + (b - a) * (i + 0.5) / n);
  return {
    full_shape: [n, n, n], factors: [1, 1, 1], reduced_shape: [n, n, n],
    default_threshold: 0.25, bunit: 'arbitrary', pixel_scale_arcsec: null,
    axes: {
      x: { name: 'x', unit: 'pixel', world: world(0, n) },
      y: { name: 'y', unit: 'pixel', world: world(0, n) },
      z: { name: 'channel', unit: '', world: world(0, n), velocity_kms: null },
    },
    synthetic: true,
  };
}

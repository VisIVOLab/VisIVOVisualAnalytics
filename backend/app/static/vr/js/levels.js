// The cube's value scale (issue #258): the threshold in multiples of the noise
// σ, the colour-scale stretches, the histogram of the values with its
// percentiles, and the info line (rest frequency, channel width, beam). Pure:
// no DOM, no GL; node runs the tests (backend/tests/vr/levels.test.mjs).
//
// σ is the full cube's (reduced_info's noise_sigma: the desktop's Estimate
// Noise rule over the whole field), in the cube's unit, and a threshold of
// n σ is the value n·σ (about zero, as the desktop's 3σ moment mask), compared
// with the drawn cube's values as the threshold always is.
//
// The stretches are the desktop's (src/vtk/LutScale: Linear, Log, Sqrt,
// Square, Power t^γ), applied to t = (v − threshold) / (colour max −
// threshold): the colour AND the opacity of a sample follow the stretched t
// (the emission model has one ramp). The log is the desktop's
// log10(v / min) / log10(max / min) over [threshold, colour max], written as
// log(1 + a t) / log(1 + a) with a = max/min − 1; a threshold at or below
// zero takes a = 1000 (astropy's LogStretch default). The shader
// (renderer.js stretchFn) and the refined frames' transfer function
// (refine.js) use stretchValue / stretchInverse below.

import { voxelNorm, dataToNorm, normToData, valueText, niceTicks } from './volume.js';

export const STRETCHES = ['linear', 'log', 'sqrt', 'square', 'power'];
export const STRETCH_LABELS = { linear: 'Linear', log: 'Log', sqrt: 'Sqrt', square: 'Square', power: 'Power' };
export const STRETCH_TIPS = {
  linear: 'Linear: colour and opacity rise evenly from the threshold to the colour max',
  log: 'Log: log10(v / threshold) / log10(max / threshold), lifts faint emission most',
  sqrt: 'Sqrt: √t, lifts faint emission',
  square: 'Square: t², brings out the brightest emission',
  power: 'Power: t^γ, the exponent γ below',
};
/** The shader's code of each stretch (renderer.js uStretch). */
export const STRETCH_CODE = { linear: 0, log: 1, sqrt: 2, square: 3, power: 4 };
export const DEFAULT_STRETCH = 'sqrt';      // the page's look before #258 (gamma 0.5)
export const DEFAULT_GAMMA = 2.0;           // the desktop's Power default
export const GAMMA_MIN = 0.1, GAMMA_MAX = 10;
export const LOG_A_DEFAULT = 1000;
export const LOG_A_MAX = 1e5;

/** The threshold presets in σ, and the percentile presets (of the drawn cube's values). */
export const SIGMA_PRESETS = [1, 2, 3, 5, 10];
export const PERCENTILE_PRESETS = [90, 99, 99.9, 99.99];

/** The threshold and colour sliders' scale: the slider at s (0..1000) is the
 *  normalised value (s/1000)², so the faint end has room. */
export function sliderToNorm(s) { return (s / 1000) ** 2; }
export function normToSlider(n) { return Math.round(Math.sqrt(Math.max(0, Math.min(1, n))) * 1000); }

const isStretch = (s) => STRETCHES.includes(s);
export const stretchOf = (s) => (isStretch(s) ? s : DEFAULT_STRETCH);
export const clampGamma = (g) => (Number.isFinite(g) && g > 0 ? Math.min(GAMMA_MAX, Math.max(GAMMA_MIN, g)) : DEFAULT_GAMMA);

/** The power exponent one step up (dir +1) or down (−1): ×/÷ 1.25, rounded
 *  to two decimals, within [GAMMA_MIN, GAMMA_MAX]. */
export function stepGamma(g, dir) {
  const v = clampGamma(g) * (dir > 0 ? 1.25 : 0.8);
  return clampGamma(Math.round(v * 100) / 100);
}

/**
 * The stretch's parameter: the log's `a` (from the threshold and the colour
 * max in data units) or the power's γ; 0 for the others.
 */
export function stretchParam(stretch, { gamma = DEFAULT_GAMMA, thrData = NaN, cmaxData = NaN } = {}) {
  const s = stretchOf(stretch);
  if (s === 'power') return clampGamma(gamma);
  if (s !== 'log') return 0;
  if (thrData > 0 && cmaxData > thrData) return Math.min(LOG_A_MAX, Math.max(1e-3, cmaxData / thrData - 1));
  return LOG_A_DEFAULT;
}

/** The stretched value of t (0..1), as the shader computes it. */
export function stretchValue(t, stretch, a) {
  const x = Math.max(0, Math.min(1, Number.isFinite(t) ? t : 0));
  switch (stretchOf(stretch)) {
    case 'log': return Math.log1p(a * x) / Math.log1p(a);
    case 'sqrt': return Math.sqrt(x);
    case 'square': return x * x;
    case 'power': return Math.pow(x, a);
    default: return x;
  }
}

/** The t (0..1) whose stretched value is L: stretchValue's inverse. */
export function stretchInverse(L, stretch, a) {
  const y = Math.max(0, Math.min(1, Number.isFinite(L) ? L : 0));
  switch (stretchOf(stretch)) {
    case 'log': return Math.expm1(y * Math.log1p(a)) / a;
    case 'sqrt': return y * y;
    case 'square': return Math.sqrt(y);
    case 'power': return Math.pow(y, 1 / a);
    default: return y;
  }
}

// ── σ ──────────────────────────────────────────────────────────────────────

/** A usable σ (finite, positive) or null. */
export const sigmaOf = (info) => {
  const s = info ? Number(info.noise_sigma) : NaN;
  return Number.isFinite(s) && s > 0 ? s : null;
};

/** The threshold `thrData` as a multiple of σ, or null without σ. */
export function sigmaMultiple(thrData, sigma) {
  return sigma && Number.isFinite(thrData) ? thrData / sigma : null;
}

const kText = (n) => (Math.abs(n) < 10 ? n.toFixed(1) : n.toFixed(0));

/** '= 3.2 σ' (one decimal under 10, none above), '' without σ. σ is the
 *  full-resolution cube's noise. */
export function sigmaText(thrData, sigma) {
  const n = sigmaMultiple(thrData, sigma);
  if (n === null) return '';
  return `= ${kText(n)} σ`;
}

/**
 * The same threshold in the drawn cube's noise (review of #258): σ is the
 * full-resolution noise, but the threshold is compared with the drawn,
 * binned cube. Mean binning of N = fx·fy·fz voxels lowers independent noise
 * to σ/√N, so k σ is about k√N of the drawn cube's noise (fewer where the
 * beam correlates neighbouring pixels: an upper estimate). Max binning
 * draws block maxima, whose noise is not σ scaled: said so. '' at full
 * resolution or without a multiple. `short`: the Display line's wording.
 */
export function drawnSigmaText(k, factors, mode, short = false) {
  if (!Number.isFinite(k)) return '';
  const n = (factors || [1, 1, 1]).reduce((a, b) => a * (b > 0 ? b : 1), 1);
  if (!(n > 1)) return '';
  if (mode === 'max') return short ? 'max binning: not σ-scaled' : 'max binning: drawn noise not σ scaled';
  return `≈${kText(k * Math.sqrt(n))}σ ${short ? 'drawn' : 'of the drawn cube'}`;
}

/**
 * The normalised threshold (0..1 over the drawn cube's [lo, hi]) of k σ:
 * {norm, clamped} (clamped: k σ lies outside the cube's values, the nearest
 * end is taken), or null without σ. `scale`: the cube's (volume.js
 * dataToNorm; on 'log', lo and hi are log10 values).
 */
export function sigmaThreshold(k, sigma, lo, hi, scale = 'linear') {
  if (!sigma || !(hi > lo)) return null;
  const n = dataToNorm(k * sigma, lo, hi, scale);
  const norm = Math.max(0, Math.min(1, n));
  return { norm, clamped: norm !== n };
}

// ── The histogram ──────────────────────────────────────────────────────────

/** Bins of the float encoding's histogram (uint8 has one bin per code). */
export const FLOAT_BINS = 2048;

/**
 * The histogram of the drawn cube's normalised values (blank voxels left
 * out), from a stride sample of about `target` voxels: {counts (Float64Array),
 * edges (Float64Array, counts.length + 1, in 0..1), n (voxels counted)}. On
 * the uint8 encoding each code is a bin centred on its value (codes are
 * (code − 1)/254), so the bins hold no empty gaps between codes.
 */
export function normHistogram(data, encoding, target = 4_000_000) {
  const len = data ? data.length : 0;
  const step = Math.max(1, Math.floor(len / Math.max(1, target)));
  const u8 = encoding === 'uint8';
  const nb = u8 ? 255 : FLOAT_BINS;
  const counts = new Float64Array(nb);
  const edges = new Float64Array(nb + 1);
  if (u8) {
    for (let k = 0; k <= nb; k++) edges[k] = Math.max(0, Math.min(1, (k - 0.5) / 254));
  } else {
    for (let k = 0; k <= nb; k++) edges[k] = k / nb;
  }
  let n = 0;
  for (let i = 0; i < len; i += step) {
    const v = voxelNorm(data, encoding, i);
    if (!(v >= 0)) continue;
    const b = u8 ? Math.round(Math.min(v, 1) * 254) : Math.min(nb - 1, Math.floor(Math.min(v, 1) * nb));
    counts[b]++;
    n++;
  }
  return { counts, edges, n };
}

/** The q quantile (0..1) of the histogram's values at or above `above`
 *  (normalised), interpolated inside its bin; null when nothing is counted. */
export function histQuantile(h, q, above = 0) {
  if (!h || !h.n) return null;
  const { counts, edges } = h;
  let total = 0;
  const part = (k) => {
    const a = edges[k], b = edges[k + 1];
    if (b <= above) return 0;
    if (a >= above || !(b > a)) return counts[k];
    return counts[k] * (b - above) / (b - a);
  };
  for (let k = 0; k < counts.length; k++) total += part(k);
  if (!(total > 0)) return null;
  const want = Math.max(0, Math.min(1, q)) * total;
  let acc = 0;
  for (let k = 0; k < counts.length; k++) {
    const c = part(k);
    if (!c) continue;
    if (acc + c >= want) {
      const a = Math.max(edges[k], above), b = edges[k + 1];
      return a + (b - a) * Math.max(0, Math.min(1, (want - acc) / c));
    }
    acc += c;
  }
  return edges[counts.length];
}

// ── Percentiles from a sorted sample ────────────────────────────────────────
// The histogram's linear bins are for the picture only: on data spanning many
// decades (a simulation's density) the 90, 99 and 99.9 % points all fall in
// its first bins (review of #258). The percentiles come from a sorted stride
// sample of the values instead, exact on the sample at any dynamic range.

/** Values in the sorted sample: enough for the 99.99th percentile to stand
 *  on about a hundred values (4 MB, sorted once in the prep worker). */
export const SORTED_SAMPLE = 1 << 20;

/** The non-blank normalised values of a stride sample of about `target`
 *  voxels, sorted (Float32Array). */
export function sortedSample(data, encoding, target = SORTED_SAMPLE) {
  const len = data ? data.length : 0;
  const step = Math.max(1, Math.floor(len / Math.max(1, target)));
  const out = new Float32Array(Math.ceil(len / step));
  let n = 0;
  for (let i = 0; i < len; i += step) {
    const v = voxelNorm(data, encoding, i);
    if (v >= 0) out[n++] = Math.min(v, 1);
  }
  return out.subarray(0, n).sort();
}

/** The first index of the sorted `s` whose value is >= v (or > v with `strict`). */
function lowerBound(s, v, strict = false) {
  let lo = 0, hi = s.length;
  while (lo < hi) {
    const mid = (lo + hi) >> 1;
    if (strict ? s[mid] <= v : s[mid] < v) lo = mid + 1; else hi = mid;
  }
  return lo;
}

/** The q quantile (0..1) of the sorted sample's values at or above `above`,
 *  interpolated between neighbours; null when there are none. */
export function sampleQuantile(s, q, above = 0) {
  if (!s || !s.length) return null;
  const i0 = lowerBound(s, above);
  const m = s.length - i0;
  if (m <= 0) return null;
  const pos = i0 + Math.max(0, Math.min(1, q)) * (m - 1);
  const a = Math.floor(pos), t = pos - a;
  return a + 1 < s.length ? s[a] + t * (s[a + 1] - s[a]) : s[a];
}

/** The fraction (0..1) of the sorted sample's values above `norm`; null when empty. */
export function fractionAbove(s, norm) {
  if (!s || !s.length) return null;
  return (s.length - lowerBound(s, norm, true)) / s.length;
}

/**
 * The σ multiples to mark on the histogram: {k, at (the slider's fraction),
 * label}, for the multiples in `ks` (in order of preference) inside the
 * cube's values, leaving out one closer than `minGap` to a mark already kept,
 * sorted along the axis.
 */
export function sigmaMarks(sigma, lo, hi, ks = [3, 5, 1, 10], minGap = 0.06, scale = 'linear') {
  if (!sigma || !(hi > lo)) return [];
  const out = [];
  for (const k of ks) {
    const n = dataToNorm(k * sigma, lo, hi, scale);
    if (!(n >= 0 && n <= 1)) continue;
    const at = Math.sqrt(n);
    if (out.some((m) => Math.abs(m.at - at) < minGap)) continue;
    out.push({ k, at, label: `${k}σ` });
  }
  return out.sort((a, b) => a.at - b.at);
}

/** The normalised threshold at percentile p (0..100) of the drawn cube's
 *  values, from the sorted sample (sortedSample). */
export const percentileThreshold = (s, p) => sampleQuantile(s, p / 100, 0);

/**
 * The histogram drawn along the slider's scale (normToSlider): `n` columns
 * over the slider's 0..1, each the density of values in its span (counts per
 * unit of the slider), the bins spread by overlap so a bin wider than a
 * column fills its columns evenly. Returns a Float64Array of n.
 */
export function sliderDensity(h, n = 160) {
  const out = new Float64Array(n);
  if (!h || !h.n) return out;
  const { counts, edges } = h;
  for (let k = 0; k < counts.length; k++) {
    const c = counts[k];
    if (!c) continue;
    const fa = Math.sqrt(Math.max(0, edges[k])), fb = Math.sqrt(Math.max(0, edges[k + 1]));
    if (!(fb > fa)) { out[Math.min(n - 1, Math.floor(fa * n))] += c * n; continue; }
    const ja = Math.floor(fa * n), jb = Math.min(n - 1, Math.floor(fb * n - 1e-12));
    for (let j = ja; j <= jb; j++) {
      const lo = Math.max(fa, j / n), hi = Math.min(fb, (j + 1) / n);
      if (hi > lo) out[j] += c * (hi - lo) / (fb - fa) * n;
    }
  }
  return out;
}

// ── The colour bar of a simulation volume (issue #317) ─────────────────────

/**
 * The colour bar of a volume (colour-bar.js drawColourBar's `bar`): from the
 * threshold to the colour max, in values (never their logarithms), its
 * ticks the decades between them on a log-scaled cube (nice linear values
 * otherwise), each placed where the shader puts its colour: the stretch of
 * (n − thr) / (cmax − thr), n the value's normalised code. `p`: thrNorm,
 * cmaxNorm, lo, hi, scale (the cube's), stretch, stretchA, title, unit.
 */
export function volumeBar(p) {
  const thr = p.thrNorm, cmax = Math.max(p.cmaxNorm, thr + 1e-3);
  const scale = p.scale === 'log' ? 'log' : 'linear';
  const val = (n) => normToData(n, p.lo, p.hi, scale);
  const a = val(thr), b = val(cmax);
  const log = scale === 'log';
  const at = (v) => {
    const n = dataToNorm(v, p.lo, p.hi, scale);
    if (!Number.isFinite(n)) return -1;
    const t = (n - thr) / (cmax - thr);
    return t < -1e-9 || t > 1 + 1e-9 ? t : stretchValue(t, p.stretch, p.stretchA);
  };
  let ticks = [];
  if (Number.isFinite(a) && Number.isFinite(b) && b > a) {
    if (log && a > 0) {
      for (let e = Math.ceil(Math.log10(a)); e <= Math.floor(Math.log10(b)); e++) ticks.push(10 ** e);
      // Fewer than two decades inside: the two ends.
      if (ticks.length < 2) ticks = [a, b];
    } else {
      ticks = niceTicks(a, b, 4);
    }
  }
  const unit = p.unit ? ` [${p.unit}]` : '';
  return { lo: a, hi: b, log, at, ticks, title: `${p.title || 'value'}${unit}${log ? ', log scale' : ''}` };
}

// ── The info line ───────────────────────────────────────────────────────────

const g4 = (v) => {
  const a = Math.abs(v);
  return a !== 0 && (a < 1e-3 || a >= 1e5) ? v.toExponential(2) : String(Number(v.toPrecision(4)));
};

/** A frequency in Hz as text: MHz below 10 GHz (HI as 1420.406 MHz, as
 *  radio astronomers write it), GHz above (CO as 115.271202 GHz). */
export function freqText(hz) {
  if (!Number.isFinite(hz) || hz <= 0) return '';
  if (hz >= 1e10) return `${(hz / 1e9).toFixed(6).replace(/0+$/, '').replace(/\.$/, '')} GHz`;
  if (hz >= 1e6) return `${(hz / 1e6).toFixed(3)} MHz`;
  if (hz >= 1e3) return `${(hz / 1e3).toFixed(3)} kHz`;
  return `${g4(hz)} Hz`;
}

/** The full-resolution channel width: {kms} from the velocity axis, else
 *  {native, unit} from the drawn channels' spacing over their binning. */
export function channelWidth(info) {
  const z = info && info.axes && info.axes.z;
  if (!z) return null;
  if (Number.isFinite(z.channel_width_kms) && z.channel_width_kms > 0) return { kms: z.channel_width_kms };
  const w = Array.isArray(z.world) ? z.world.filter(Number.isFinite) : [];
  const fz = (info.factors && info.factors[2]) || 1;
  if (w.length > 1 && z.unit && z.unit !== 'channel') {
    return { native: Math.abs(w[w.length - 1] - w[0]) / (w.length - 1) / fz, unit: z.unit };
  }
  return null;
}

/** 'beam 15.0″ × 12.0″, PA 33°' (the arcsec as the header's BMAJ/BMIN), '' without one. */
export function beamText(beam) {
  if (!beam || !Number.isFinite(beam.major_arcsec) || !Number.isFinite(beam.minor_arcsec)) return '';
  const pa = Number.isFinite(beam.pa_deg) ? `, PA ${beam.pa_deg.toFixed(0)}°` : '';
  return `beam ${beam.major_arcsec.toFixed(1)}″ × ${beam.minor_arcsec.toFixed(1)}″${pa}${beam.per_channel ? ' (central channel)' : ''}`;
}

/**
 * A simulation volume's info line (issue #317): its axes and their unit, the
 * grid spacing, and the value scale of the drawn cube (`cube`: its scale and
 * value range, when on show): no channel, rest frequency or beam.
 */
export function simulationLine(info, cube = null) {
  const ax = (info && info.axes) || {};
  const names = ['x', 'y', 'z'].map((k) => (ax[k] && ax[k].name) || k.toUpperCase());
  const unit = (ax.x && ax.x.unit) || '';
  const parts = [`Simulation volume: ${names.join(', ')}${unit ? ` in ${unit}` : ''}`];
  const sp = Array.isArray(info.spacing) ? info.spacing.map((v) => Math.abs(Number(v))) : [];
  if (sp.length === 3 && sp.every((v) => Number.isFinite(v) && v > 0)) {
    parts.push(sp.every((v) => v === sp[0]) ? `voxel ${valueText(sp[0])}` : `voxel ${sp.map((v) => valueText(v)).join(' × ')}`);
  }
  if (cube && cube.scale === 'log') {
    const range = Number.isFinite(cube.valueMin) && Number.isFinite(cube.valueMax)
      ? ` ${valueText(cube.valueMin)} to ${valueText(cube.valueMax)}${cube.bunit ? ` ${cube.bunit}` : ''}` : '';
    const blank = cube.nonpositive > 0 ? `, ${cube.nonpositive} voxels ≤ 0 blank` : '';
    parts.push(`log scale${range}${blank}`);
  }
  return parts.join(' · ');
}

/**
 * The cube's info line: rest frequency (named HI when it is the HI line),
 * the full-resolution channel width (km/s when the cube has a velocity axis;
 * the drawn cube's when binned along the band), the beam when the header has
 * one. Pieces joined by ' · '; '' for a cube without any of them. A missing
 * rest frequency is said only on a frequency axis. A simulation volume has
 * its own (simulationLine); a cube drawn on a log scale says so.
 */
export function infoLine(info, cube = null) {
  if (!info) return '';
  if (info.simulation) return simulationLine(info, cube);
  const parts = [];
  const rest = freqText(info.rest_frequency_hz);
  const z = (info.axes && info.axes.z) || {};
  // Without RESTFRQ, said only on a frequency axis (where it would give the velocities).
  if (rest) parts.push(`${info.rest_line ? `${info.rest_line} ` : ''}rest ${rest}`);
  else if (/hz$/i.test(String(z.unit || ''))) parts.push('no rest frequency');
  const cw = channelWidth(info);
  const fz = (info.factors && info.factors[2]) || 1;
  if (cw && cw.kms) parts.push(`channel ${cw.kms.toFixed(2)} km/s${fz > 1 ? ` (drawn ×${fz}: ${(cw.kms * fz).toFixed(1)})` : ''}`);
  else if (cw) parts.push(`channel ${cw.unit === 'Hz' ? freqText(cw.native) : `${g4(cw.native)} ${cw.unit}`}`);
  const b = beamText(info.beam);
  if (b) parts.push(b);
  if (cube && cube.scale === 'log') parts.push('log scale');
  return parts.join(' · ');
}

/** The velocity convention line (Levels page), '' when there is no velocity axis. */
export function conventionLine(info) {
  const z = info && info.axes && info.axes.z;
  if (!z || !z.velocity_convention) return '';
  return `velocity ${z.velocity_convention}${z.specsys ? `, ${z.specsys}` : ''}`;
}

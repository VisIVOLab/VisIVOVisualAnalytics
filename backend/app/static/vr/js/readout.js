// The cursor readout and the controller labels (issue #201).
//
// The readout follows the primary ray: where it meets the drawn emission (the
// selection's hover pick, already made every other frame), the sky position
// and velocity of that full-resolution pixel (selection.js worldOfPixel: the
// axis labels' interpolation, the same numbers as the tick labels) and the
// value drawn there (the reduced cube's, at its binning) with the cube's unit.
// It is text from a few array reads, recomputed only when the point changes.
//
// The labels are a small card above each controller listing what its
// controls do now (controls.js labelLines), the mode on the primary's.
// Pure text and placement: tested by node (backend/tests/vr/controls.test.mjs).

import * as M from './mat4.js';
import { boxToFullPixel, worldOfPixel, formatRA, formatDec, sampleNorm } from './selection.js';
import { normToData } from './volume.js';

function fmt(v, digits = 4) {
  if (v === null || v === undefined || !Number.isFinite(v)) return 'n/a';
  const a = Math.abs(v);
  if (a !== 0 && (a < 1e-3 || a >= 1e5)) return v.toExponential(digits - 1);
  return v.toPrecision(digits);
}

/**
 * The readout's lines for box point `p` (0..1 per axis) of the cube on show:
 * [mode line, sky position, velocity (or spectral value), value with unit].
 * `cube`: {data, dims, encoding, lo, hi, bunit, factors}; `info`: reduced_info;
 * `texScale`: the page's. Without a point, the mode line alone.
 */
export function readoutLines(modeText, p, { info, cube, texScale } = {}) {
  const out = [modeText];
  if (!p || !info || !cube) return out;
  const px = boxToFullPixel(p, info.full_shape);
  const w = worldOfPixel(info, px);
  if (w.xName === 'RA' && Number.isFinite(w.x) && Number.isFinite(w.y)) out.push(`RA ${formatRA(w.x)}  Dec ${formatDec(w.y)}`);
  else out.push(`${w.xName} ${fmt(w.x, 7)} ${w.xUnit || ''}  ${w.yName} ${fmt(w.y, 7)} ${w.yUnit || ''}`.trim());
  const conv = info.axes && info.axes.z && info.axes.z.velocity_convention;
  out.push(w.velocity !== null && w.velocity !== undefined && Number.isFinite(w.velocity)
    ? `v ${w.velocity.toFixed(2)} km/s${conv ? ` (${conv})` : ''}  ch ${px[2]}`
    : `${info.axes.z.name || 'spectral'} ${fmt(w.spectral, 7)} ${w.spectralUnit || ''}  ch ${px[2]}`);
  const n = sampleNorm({ data: cube.data, dims: cube.dims, encoding: cube.encoding, texScale }, p);
  const binned = cube.factors && cube.factors.some((f) => f > 1) ? `  (binned ${cube.factors.join('×')})` : '';
  out.push(n < 0 || !Number.isFinite(n) ? `blank${binned}` : `${fmt(normToData(n, cube.lo, cube.hi))} ${cube.bunit || ''}${binned}`.replace(/\s+\(/, ' ('));
  return out;
}

// The readout card (issue #226, point 4): on the Quest its 30-pixel text on a
// 4-cm card was under half a degree high, a blur at arm's length. It is now
// sized by the angle its text makes at the eye: the body text's em is at
// least READOUT_MIN_DEG high at every distance from READOUT_NEAR to
// READOUT_FAR (the card grows with distance), bright on a near-opaque dark
// card with a light edge (contrast over 15:1), and drawn as a composition
// layer where the browser has them (xr-layers.js), so the compositor shows it
// at the display's own resolution.
export const READOUT_PX = [720, 236];
/** The card's size in metres at 1 m (scaled by readoutScale with distance). */
export const READOUT_SIZE = [0.288, 0.0944];
/** Text sizes on the card's canvas (pixels): the mode line and the body lines. */
export const READOUT_FONT = { title: 34, body: 40, pitch: 52, top: 30 };
export const READOUT_MIN_DEG = 0.9;
export const READOUT_NEAR = 0.3;
export const READOUT_FAR = 4;
export const READOUT_COLORS = { bg: [6, 8, 12], bgPinned: [34, 26, 8], text: [255, 255, 255], title: [255, 210, 122], edge: [159, 208, 255] };
export const LABEL_PX = [512, 256];
export const LABEL_SIZE = [0.12, 0.06];

/** The card's scale at `dist` metres from the eye: the same angle from near to far. */
export function readoutScale(dist) {
  return Math.min(READOUT_FAR, Math.max(READOUT_NEAR, dist));
}

/** The angle (degrees) at the eye of a font of `fontPx` canvas pixels on a
 *  card of `cardPx` pixels and `cardM` metres high, scaled by `scale`, at `dist`. */
export function textAngleDeg(fontPx, cardPx, cardM, scale, dist) {
  const h = (fontPx / cardPx) * cardM * scale;
  return (2 * Math.atan(h / 2 / Math.max(1e-6, dist)) * 180) / Math.PI;
}

/** WCAG contrast ratio of two sRGB colours [r, g, b] (0..255). */
export function contrastRatio(a, b) {
  const lum = (c) => {
    const [r, g, bb] = c.map((v) => { const s = v / 255; return s <= 0.03928 ? s / 12.92 : ((s + 0.055) / 1.055) ** 2.4; });
    return 0.2126 * r + 0.7152 * g + 0.0722 * bb;
  };
  const [hi, lo] = [lum(a), lum(b)].sort((x, y) => y - x);
  return (hi + 0.05) / (lo + 0.05);
}

/**
 * The readout card's pose: just above the point `at` (world), facing the eye,
 * scaled with distance so its text keeps its angular size (readoutScale).
 */
export function readoutModel(at, eye) {
  const dist = Math.hypot(at[0] - eye[0], at[1] - eye[1], at[2] - eye[2]);
  const s = readoutScale(dist);
  const p = [at[0], at[1] + (READOUT_SIZE[1] * s) / 2 + 0.012 * s, at[2]];
  return M.multiply(faceEye(p, eye), M.scaling(s));
}

/** A pose at p facing eye, upright. */
export function faceEye(p, eye) {
  let z = M.sub(eye, p);
  if (Math.hypot(z[0], z[2]) < 1e-6) z = [0, 0, 1];
  z = M.normalize(z);
  const x = M.normalize(M.cross([0, 1, 0], z));
  const y = M.cross(z, x);
  return new Float32Array([x[0], x[1], x[2], 0, y[0], y[1], y[2], 0, z[0], z[1], z[2], 0, p[0], p[1], p[2], 1]);
}

/** The label card's pose: above the controller whose grip pose is `grip`,
 *  facing the eye. `lift` raises it (the numbers sit above the labels). */
export function labelModel(grip, eye, lift = 0) {
  const p = M.transformPoint(grip, [0, 0.07 + lift, 0.0]);
  return faceEye(p, eye);
}

// ── Drawing ─────────────────────────────────────────────────────────────────

/** The readout card. */
export function drawReadout(c, lines, pinned = false) {
  const [W, H] = READOUT_PX;
  const rgb = (v, a = 1) => `rgba(${v[0]}, ${v[1]}, ${v[2]}, ${a})`;
  const F = READOUT_FONT;
  c.clearRect(0, 0, W, H);
  c.fillStyle = rgb(pinned ? READOUT_COLORS.bgPinned : READOUT_COLORS.bg, 0.95);
  c.fillRect(0, 0, W, H);
  c.strokeStyle = rgb(pinned ? READOUT_COLORS.title : READOUT_COLORS.edge, 0.9);
  c.lineWidth = 4;
  c.strokeRect(2, 2, W - 4, H - 4);
  c.textBaseline = 'middle';
  lines.slice(0, 4).forEach((l, i) => {
    c.fillStyle = rgb(i === 0 ? READOUT_COLORS.title : READOUT_COLORS.text);
    c.font = i === 0 ? `bold ${F.title}px -apple-system, system-ui, sans-serif` : `600 ${F.body}px ui-monospace, Menlo, monospace`;
    c.fillText(l, 16, F.top + i * F.pitch, W - 32);
  });
}

/** A label card: `title` (the mode, on the primary's) then [name, text] rows. */
export function drawLabels(c, title, rows) {
  const [W, H] = LABEL_PX;
  c.clearRect(0, 0, W, H);
  c.fillStyle = 'rgba(10, 14, 22, 0.78)';
  c.fillRect(0, 0, W, H);
  c.textBaseline = 'middle';
  let y = 24;
  if (title) {
    c.fillStyle = '#ffd27a';
    c.font = 'bold 28px -apple-system, system-ui, sans-serif';
    c.fillText(title, 12, y);
    y += 36;
  }
  c.font = '26px -apple-system, system-ui, sans-serif';
  for (const [name, text] of rows.slice(0, title ? 6 : 7)) {
    c.fillStyle = '#9fd0ff';
    c.fillText(name, 12, y);
    c.fillStyle = '#e6e9ef';
    c.fillText(text, 170, y);
    y += 32;
  }
}

// The colour bar of a point table or a velocity field (issue #259): the
// colour map over the colour window, its ticks, and the column with its unit,
// drawn on a 2-D canvas that stands beside the box, turned to the eyes. It is
// uploaded only when what it shows changes (a 2-D texture: nothing 3-D during
// a session, #229). Every text goes through text-fit.js. Pure drawing on a 2-D
// context: node runs the tests (backend/tests/vr/sim-controls.test.mjs) and
// validation/vr_text_fit.mjs measures its texts.

import { fitLine, measureWith } from './text-fit.js';

export const BAR_PX = [640, 150];
/** Its size in the room (metres): about the readout's text size at arm's length. */
export const BAR_SIZE = [0.26, 0.26 * BAR_PX[1] / BAR_PX[0]];
const FONT = '-apple-system, system-ui, sans-serif';
const PAD = 24, BAR_Y = 62, BAR_H = 30;

/** A tick's label: a decade as 1e11, else three significant digits. */
export function tickLabel(v) {
  const a = Math.abs(v);
  const e = a > 0 ? Math.log10(a) : 0;
  if (a !== 0 && Math.abs(e - Math.round(e)) < 1e-9 && Math.abs(Math.round(e)) >= 3) return `${v < 0 ? '-' : ''}1e${Math.round(e)}`;
  return a !== 0 && (a < 1e-2 || a >= 1e4) ? v.toExponential(1) : String(+v.toPrecision(3));
}

/** Where tick value v sits along the bar (0..1) for the window [a, b] (log: on its logarithm). */
export function tickAt(v, a, b, log) {
  if (log && a > 0 && b > 0 && v > 0) return (Math.log10(v) - Math.log10(a)) / (Math.log10(b) - Math.log10(a));
  return (v - a) / (b - a);
}

/**
 * Draws the bar: `bar` {lut (256 × RGBA8), lo, hi (the window's data values,
 * its two ends), log, title (colourBarTitle), ticks (data values)}.
 */
export function drawColourBar(c, bar) {
  const [W, H] = BAR_PX;
  c.clearRect(0, 0, W, H);
  c.fillStyle = 'rgba(12, 16, 24, 0.88)';
  c.fillRect(0, 0, W, H);
  const text = (s, x, y, maxW, { font = `24px ${FONT}`, color = '#e6e9ef', align = 'left' } = {}) => {
    const r = fitLine(measureWith(c), font, String(s), maxW);
    c.font = r.font; c.fillStyle = color; c.textAlign = align; c.textBaseline = 'middle';
    c.fillText(r.text, x, y);
    c.textAlign = 'left';
  };
  text(bar.title || '', PAD, 30, W - 2 * PAD, { font: `600 26px ${FONT}` });
  const x0 = PAD, len = W - 2 * PAD;
  const lut = bar.lut;
  // The map as a gradient of 33 stops (one fill), as the menu's swatches.
  const g = typeof c.createLinearGradient === 'function' ? c.createLinearGradient(x0, 0, x0 + len, 0) : null;
  if (g && typeof g.addColorStop === 'function') {
    for (let s = 0; s <= 32; s++) {
      const k = Math.round((s / 32) * 255) * 4;
      g.addColorStop(s / 32, `rgb(${lut[k]}, ${lut[k + 1]}, ${lut[k + 2]})`);
    }
    c.fillStyle = g;
  } else {
    c.fillStyle = `rgb(${lut[512]}, ${lut[513]}, ${lut[514]})`;
  }
  c.fillRect(x0, BAR_Y, len, BAR_H);
  const { lo, hi, log } = bar;
  const ticks = (bar.ticks || []).filter((v) => {
    const t = tickAt(v, lo, hi, log);
    return t >= -1e-9 && t <= 1 + 1e-9;
  });
  // Labels at most one per 120 px.
  const placed = [];
  for (const v of ticks) {
    const x = x0 + tickAt(v, lo, hi, log) * len;
    c.fillStyle = '#e6e9ef';
    c.fillRect(x - 1, BAR_Y + BAR_H, 2, 8);
    if (placed.some((p) => Math.abs(p - x) < 120)) continue;
    placed.push(x);
    text(tickLabel(v), Math.max(x0 + 50, Math.min(x0 + len - 50, x)), BAR_Y + BAR_H + 26, 100,
      { font: `22px ${FONT}`, color: '#b9c2d0', align: 'center' });
  }
}

/** The bar's key: redrawn and uploaded only when it changes. */
export const barKey = (bar, cmap) => JSON.stringify([cmap, bar.lo, bar.hi, bar.log, bar.title, bar.ticks]);

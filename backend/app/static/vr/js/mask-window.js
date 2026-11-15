// The Mask window (issue #282): a label mask over the cube, used with the
// primary ray and trigger. Show: how the mask is drawn (off, the cube with
// the sources' outline, only the masked emission, the residuals) and the
// labels at the centroids. Source: Pick (the next trigger on the cube picks
// the source under the ray), step through the sources, Visit, a New source.
// Refine: the Kinematic Lasso's New / Add / Subtract on the picked source,
// Undo. Save: the refined mask to the desktop. Layout pure (unit-tested in
// backend/tests/vr/mask-layer.test.mjs); every text through text-fit.js.

import { fitLine, measureWith, wrapFit } from './text-fit.js';
import { MODES, MODE_NAMES } from './mask-layer.js';

export const MASK_PX = [1024, 720];
export const MASK_SIZE = [MASK_PX[0] / 2133, MASK_PX[1] / 2133];

const FONT = '-apple-system, system-ui, sans-serif';
const C = {
  bg: 'rgba(12, 16, 24, 0.94)', head: 'rgba(30, 38, 54, 0.96)', fg: '#e6e9ef', muted: '#9aa3b2', note: '#b9c2d0',
  btn: '#1d2433', btnHover: '#2b3650', on: '#2f7fd8', onFg: '#ffffff', edge: '#4a5670', off: '#5b6476', offBg: '#161b26',
  armed: '#a8620f', warn: '#ffd27a', err: '#ff8a8a',
};
const X0 = 200, RIGHT = 1004, WH = 60;
const ROWS = [88, 200, 312, 424];
const LABELS = ['Show', 'Source', 'Refine', 'Save'];
const span = (y, n, k, x0 = X0, right = RIGHT, gap = 10) => {
  const w = (right - x0 - gap * (n - 1)) / n;
  return [x0 + k * (w + gap), y, w, WH];
};

/**
 * The widgets for the view: {loaded, mode, labels, selected (an ID or 0),
 * picking, newSource, seedMode, canUndo, canSave, nSources}.
 */
export function maskLayout(v) {
  const none = v.loaded ? '' : 'No mask: send one from the desktop, or load one on the page.';
  const noSel = none || (v.selected ? '' : 'Pick a source first.');
  const w = [];
  MODES.forEach((m, k) => w.push({ id: `mode:${m}`, kind: 'button', rect: span(ROWS[0], 5, k), label: MODE_NAMES[m], group: 'mode', value: m,
    on: v.loaded && v.mode === m, disabled: none }));
  w.push({ id: 'labels', kind: 'button', rect: span(ROWS[0], 5, 4), label: 'Labels', on: v.loaded && !!v.labels, disabled: none });
  w.push({ id: 'pick', kind: 'button', rect: span(ROWS[1], 5, 0), label: v.picking ? 'Picking' : 'Pick', armed: !!v.picking, disabled: none });
  w.push({ id: 'src-prev', kind: 'button', rect: span(ROWS[1], 5, 1), label: '◂', disabled: none || (v.nSources ? '' : 'The mask is empty.') });
  w.push({ id: 'src-next', kind: 'button', rect: span(ROWS[1], 5, 2), label: '▸', disabled: none || (v.nSources ? '' : 'The mask is empty.') });
  w.push({ id: 'visit', kind: 'button', rect: span(ROWS[1], 5, 3), label: 'Visit', disabled: noSel });
  w.push({ id: 'new-source', kind: 'button', rect: span(ROWS[1], 5, 4), label: 'New', on: !!v.newSource, disabled: none });
  [['new', 'Replace'], ['add', 'Add'], ['carve', 'Subtract']].forEach(([m, label], k) => {
    w.push({ id: `seed:${m}`, kind: 'button', rect: span(ROWS[2], 4, k), label, group: 'seed', value: m,
      on: v.loaded && (!!v.selected || !!v.newSource) && v.seedMode === m, disabled: none || (v.selected || v.newSource ? '' : 'Pick a source, or New for one.') });
  });
  w.push({ id: 'undo-edit', kind: 'button', rect: span(ROWS[2], 4, 3), label: 'Undo', disabled: none || (v.canUndo ? '' : 'Nothing to undo.') });
  w.push({ id: 'save', kind: 'button', rect: span(ROWS[3], 2, 0), label: 'Save to desktop', disabled: none || (v.canSave ? '' : 'Nothing edited yet.') });
  w.push({ id: 'deselect', kind: 'button', rect: span(ROWS[3], 2, 1), label: 'Deselect', disabled: noSel });
  return w;
}

function roundRect(c, x, y, w, h, r) {
  c.beginPath();
  c.moveTo(x + r, y);
  c.arcTo(x + w, y, x + w, y + h, r);
  c.arcTo(x + w, y + h, x, y + h, r);
  c.arcTo(x, y + h, x, y, r);
  c.arcTo(x, y, x + w, y, r);
  c.closePath();
}

function text(c, s, x, y, maxW, { color = C.muted, font = `22px ${FONT}`, align = 'left' } = {}) {
  const r = fitLine(measureWith(c), font, String(s ?? ''), maxW);
  c.font = r.font;
  c.fillStyle = color;
  c.textBaseline = 'middle';
  c.textAlign = align;
  c.fillText(r.text, x, y);
  c.textAlign = 'left';
}

function drawButton(c, w, hover) {
  const [x, y, ww, hh] = w.rect;
  const off = !!w.disabled;
  c.fillStyle = w.armed && !off ? C.armed : w.on && !off ? C.on : off ? C.offBg : hover ? C.btnHover : C.btn;
  roundRect(c, x, y, ww, hh, 10);
  c.fill();
  if (hover && !off) { c.strokeStyle = C.edge; c.lineWidth = 2; c.stroke(); }
  c.fillStyle = off ? C.off : w.on || w.armed ? C.onFg : C.fg;
  c.textAlign = 'center';
  c.textBaseline = 'middle';
  const r = fitLine(measureWith(c), `26px ${FONT}`, w.label || '', ww - 16);
  c.font = r.font;
  c.fillText(r.text, x + ww / 2, y + hh / 2 + 1);
  c.textAlign = 'left';
}

/**
 * Draws the window: `v` as maskLayout's plus title, info, notes {show,
 * source, refine, save} (the line under each row), status [[text, colour]];
 * the line of the row pointed at is the button's reason when greyed.
 */
export function drawMask(c, layout, v, ui = {}) {
  const [W, H] = MASK_PX;
  c.clearRect(0, 0, W, H);
  c.fillStyle = C.bg;
  roundRect(c, 0, 0, W, H, 18);
  c.fill();
  c.fillStyle = C.head;
  roundRect(c, 0, 0, W, 72, 18);
  c.fill();
  text(c, v.title || 'Mask', 20, 24, W - 40, { color: C.fg, font: `bold 26px ${FONT}` });
  text(c, v.info || '', 20, 54, W - 40, { color: C.note, font: `20px ${FONT}` });
  const pointed = layout.find((w) => w.id === ui.hover) || null;
  const keys = ['show', 'source', 'refine', 'save'];
  LABELS.forEach((label, i) => {
    text(c, label, 20, ROWS[i] + WH / 2, X0 - 30, { color: C.fg, font: `28px ${FONT}` });
    const inRow = layout.filter((w) => w.rect[1] === ROWS[i]);
    const here = pointed && inRow.includes(pointed) ? pointed : null;
    const why = here && here.disabled ? here.disabled : '';
    const line = why || ((v.notes || {})[keys[i]] || '');
    if (line) text(c, line, X0, ROWS[i] + WH + 20, RIGHT - X0, { color: why ? C.warn : C.note });
  });
  for (const w of layout) drawButton(c, w, ui.hover === w.id);
  const top = ROWS[3] + WH + 60;
  const measureIn = measureWith(c);
  let y = top;
  for (const [s, color] of v.status || []) {
    for (const piece of wrapFit(measureIn, `22px ${FONT}`, s, W - 40, 2, [1]).lines) {
      if (y > H - 24) break;
      text(c, piece, 20, y, W - 40, { color: color || C.note });
      y += 30;
    }
  }
}

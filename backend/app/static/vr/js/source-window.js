// The Sources window (issue #279): the source list of the catalogue over the
// cube as a window of the headset (windows.js), used with the primary ray
// and trigger like the hand menu. Column headings sort, a row is chosen and
// visited, Prev / Next step through the sorted list, the page buttons page a
// large catalogue, the flag and note buttons classify the current source,
// Send writes the VOTable for the desktop. The layout is a pure function of
// what it shows (unit-tested in backend/tests/vr/source-list.test.mjs); every
// text goes through text-fit.js (validation/vr_text_fit.mjs).

import { fitLine, measureWith } from './text-fit.js';
import { positionText, velocityText, positionNames, flagColour, pageCount } from './source-list.js';

export const SRC_PX = [1024, 920];
/** Metres: the in-VR panel's 2133 px/m, so its 22-28 px text reads as the menu's. */
export const SRC_SIZE = [SRC_PX[0] / 2133, SRC_PX[1] / 2133];
export const ROWS_PER_PAGE = 10;

const FONT = '-apple-system, system-ui, sans-serif';
const MONO = 'ui-monospace, Menlo, monospace';
const C = {
  bg: 'rgba(12, 16, 24, 0.94)', head: 'rgba(30, 38, 54, 0.96)', fg: '#e6e9ef', muted: '#9aa3b2',
  btn: '#1d2433', btnHover: '#2b3650', on: '#2f7fd8', onFg: '#ffffff', edge: '#4a5670', off: '#5b6476', offBg: '#161b26',
  row: 'rgba(255,255,255,0.03)', rowHover: '#24304a', rowCur: '#2f4f86', warn: '#ffd27a', err: '#ff8a8a', note: '#b9c2d0',
};
const X = 12, RIGHT = 1012;
const HEAD_Y = 76, HEAD_H = 46, ROW_Y = 130, ROW_H = 46, ROW_STEP = 50;
const NAV_Y = ROW_Y + ROWS_PER_PAGE * ROW_STEP + 8, BTN_H = 56;
const FLAG_Y = NAV_Y + BTN_H + 12, NOTE_Y = FLAG_Y + BTN_H + 12, STATUS_Y = NOTE_Y + BTN_H + 26;
/** The columns: [key, x, width]. */
export const COLUMNS = [['name', X, 352], ['lon', 372, 200], ['lat', 580, 180], ['velocity', 768, 124], ['flag', 900, RIGHT - 900]];

const span = (y, n, k, x0 = X, right = RIGHT, gap = 10) => {
  const w = (right - x0 - gap * (n - 1)) / n;
  return [x0 + k * (w + gap), y, w, BTN_H];
};

/**
 * The window's widgets for the view: {page, pages, rows (this page's),
 * current (key), currentRow (the row or null), sort: {key, dir}, flagSet,
 * noteSet, mark ({flag, note} of the current), frame, canSend}. Each
 * {id, kind, rect, label, on, disabled}; the rows are buttons `row:<key>`
 * (their names may be cut: `free`).
 */
export function sourceLayout(v) {
  const w = [];
  const names = positionNames(v.frame);
  const head = { name: 'Name', lon: names[0], lat: names[1], velocity: 'v km/s', flag: 'Flag' };
  for (const [key, x, width] of COLUMNS) {
    const arrow = v.sort && v.sort.key === key ? (v.sort.dir > 0 ? ' ▲' : ' ▼') : '';
    w.push({ id: `sort:${key}`, kind: 'button', rect: [x, HEAD_Y, width - 8, HEAD_H], label: `${head[key]}${arrow}`,
      on: !!arrow, small: true });
  }
  (v.rows || []).forEach((r, k) => {
    w.push({ id: `row:${r.key}`, kind: 'button', rect: [X, ROW_Y + k * ROW_STEP, RIGHT - X, ROW_H], row: r,
      on: r.key === v.current, free: true, label: '' });
  });
  const none = v.currentRow ? '' : 'Choose a source first.';
  const empty = v.total ? '' : 'No sources.';
  w.push({ id: 'prev', kind: 'button', rect: span(NAV_Y, 6, 0), label: '◂ Prev', disabled: empty });
  w.push({ id: 'visit', kind: 'button', rect: span(NAV_Y, 6, 1), label: 'Visit', disabled: empty || none });
  w.push({ id: 'next', kind: 'button', rect: span(NAV_Y, 6, 2), label: 'Next ▸', disabled: empty });
  w.push({ id: 'page-prev', kind: 'button', rect: span(NAV_Y, 6, 3), label: '◂ Page', disabled: v.pages > 1 ? '' : 'One page only.' });
  w.push({ id: 'page-next', kind: 'button', rect: span(NAV_Y, 6, 4), label: 'Page ▸', disabled: v.pages > 1 ? '' : 'One page only.' });
  w.push({ id: 'found-remove', kind: 'button', rect: span(NAV_Y, 6, 5), label: 'Remove',
    disabled: v.currentRow && v.currentRow.origin === 'user' ? '' : 'Only a source you found can be removed.' });
  const flags = v.flagSet || [];
  const nf = flags.length + 1;
  flags.forEach((f, k) => w.push({ id: `flag:${f}`, kind: 'button', rect: span(FLAG_Y, nf, k), label: f, group: 'flag', value: f,
    on: !!v.mark && v.mark.flag === f, disabled: none, colour: flagColour(f, flags) }));
  w.push({ id: 'send', kind: 'button', rect: span(FLAG_Y, nf, nf - 1), label: 'Send',
    disabled: v.canSend ? '' : 'Nothing to send: flag a source or find one first.' });
  const notes = v.noteSet || [];
  notes.forEach((n, k) => w.push({ id: `note:${n}`, kind: 'button', rect: span(NOTE_Y, Math.max(notes.length, 3), k), label: n,
    group: 'note', value: n, on: !!v.mark && v.mark.note === n, disabled: none, small: true }));
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
  return r;
}

const css = (rgb) => `rgb(${rgb.map((v) => Math.round(255 * v)).join(',')})`;

function drawRow(c, w, v, hover) {
  const [x, y, ww, hh] = w.rect;
  const r = w.row;
  c.fillStyle = w.on ? C.rowCur : hover ? C.rowHover : C.row;
  roundRect(c, x, y, ww, hh, 8);
  c.fill();
  const cy = y + hh / 2 + 1;
  const [lon, lat] = positionText(r);
  const mark = v.marks ? v.marks(r.key) : { flag: '' };
  const fg = w.on ? C.onFg : C.fg;
  const mono = `22px ${MONO}`;
  const nameCol = r.origin === 'user' ? '#7fe9ff' : fg;
  text(c, r.label, COLUMNS[0][1] + 10, cy, COLUMNS[0][2] - 22, { color: nameCol, font: `24px ${FONT}` });
  text(c, lon, COLUMNS[1][1] + 4, cy, COLUMNS[1][2] - 12, { color: fg, font: mono });
  text(c, lat, COLUMNS[2][1] + 4, cy, COLUMNS[2][2] - 12, { color: fg, font: mono });
  text(c, velocityText(r.velocity), COLUMNS[3][1] + COLUMNS[3][2] - 14, cy, COLUMNS[3][2] - 18, { color: fg, font: mono, align: 'right' });
  if (mark.flag) {
    const col = flagColour(mark.flag, v.flagSet || []) || [0.8, 0.8, 0.8];
    c.beginPath();
    c.arc(COLUMNS[4][1] + 12, cy, 8, 0, 2 * Math.PI);
    c.fillStyle = css(col);
    c.fill();
    text(c, mark.flag, COLUMNS[4][1] + 26, cy, COLUMNS[4][2] - 34, { color: fg, font: `20px ${FONT}` });
  }
}

function drawButton(c, w, hover) {
  const [x, y, ww, hh] = w.rect;
  const off = !!w.disabled;
  c.fillStyle = w.on && !off ? C.on : off ? C.offBg : hover ? C.btnHover : C.btn;
  roundRect(c, x, y, ww, hh, 10);
  c.fill();
  if (hover && !off) { c.strokeStyle = C.edge; c.lineWidth = 2; c.stroke(); }
  // A flag's button carries its colour, the one its sources are drawn in.
  let dx = 0;
  if (w.colour) {
    c.beginPath();
    c.arc(x + 22, y + hh / 2, 9, 0, 2 * Math.PI);
    c.fillStyle = off ? C.off : css(w.colour);
    c.fill();
    dx = 26;
  }
  c.fillStyle = off ? C.off : w.on ? C.onFg : C.fg;
  c.textAlign = 'center';
  c.textBaseline = 'middle';
  const r = fitLine(measureWith(c), `${w.small ? 22 : 26}px ${FONT}`, w.label || '', ww - 16 - dx);
  c.font = r.font;
  c.fillText(r.text, x + dx + (ww - dx) / 2, y + hh / 2 + 1);
  c.textAlign = 'left';
}

/**
 * Draws the window. `v` as sourceLayout's, plus title, info, status ([text,
 * colour]), marks(key) -> {flag, note}; `ui` {hover}. The line under the
 * buttons is the tooltip of the button pointed at (why it is greyed), else
 * the status.
 */
export function drawSources(c, layout, v, ui = {}) {
  const [W, H] = SRC_PX;
  c.clearRect(0, 0, W, H);
  c.fillStyle = C.bg;
  roundRect(c, 0, 0, W, H, 18);
  c.fill();
  c.fillStyle = C.head;
  roundRect(c, 0, 0, W, 66, 18);
  c.fill();
  text(c, v.title || 'Sources', 20, 24, 640, { color: C.fg, font: `bold 26px ${FONT}` });
  text(c, `page ${(v.page || 0) + 1} of ${v.pages || 1}`, RIGHT - 8, 24, 300, { align: 'right' });
  text(c, v.info || '', 20, 52, RIGHT - 28, { color: C.note, font: `20px ${FONT}` });
  for (const w of layout) {
    const hover = ui.hover === w.id;
    if (w.row) drawRow(c, w, v, hover); else drawButton(c, w, hover);
  }
  if (!(v.rows || []).length) text(c, v.total ? '' : (v.empty || 'No catalogue over the cube.'), 24, ROW_Y + ROW_H / 2, RIGHT - 48, { color: C.muted, font: `24px ${FONT}` });
  const pointed = layout.find((w) => w.id === ui.hover && !w.row);
  const line = pointed && pointed.disabled ? [pointed.disabled, C.warn] : (v.status || ['', C.muted]);
  text(c, line[0], 20, STATUS_Y, RIGHT - 28, { color: line[1] || C.muted, font: `22px ${FONT}` });
}

/** The page the rows of `total` are shown on, kept inside the pages. */
export function clampPage(page, total) {
  const n = pageCount(total, ROWS_PER_PAGE);
  return ((page % n) + n) % n;
}

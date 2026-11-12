// The hand menu (issue #201): one menu, held open by a button of the
// secondary hand (or its palm turned up) and drawn next to that wrist,
// closed when the button is let go. The primary hand's ray and trigger press
// its items. Tabs: Mode (modes and tools), Look (the former control panel:
// colour map, threshold, opacity, slab, refinement), Analysis (the former
// analysis panel: lasso seeds, Isolate/Remove, moment, PV path), Layers
// (points, catalogue, flow: the former strip), Settings and Help. In a
// desktop browser the same menu stands in the 3-D view and the mouse uses it.
//
// The Look and Analysis pages reuse vr-panel.js's and tools-panel.js's
// layouts and drawing as they were. Layout, placement and the page logic are
// pure (backend/tests/vr/hand-menu.test.mjs); drawMenu needs a 2-D context.

import * as M from './mat4.js';
import * as P from './vr-panel.js';
import * as TP from './tools-panel.js';
import { stripLayout } from './layers.js';
import { MODES, MODE_NAMES, SUBTOOLS, SUB_NAMES } from './controls.js';

export const TAB_H = 96;
export const PAGE_PX = [1024, 832];
export const MENU_PX = [1024, TAB_H + PAGE_PX[1]];
/** 2800 px/m: 0.37 × 0.33 m; the 22-32 px text is 0.8-1.1 cm high, about
 *  1.0-1.4° at the 0.45 m a menu by the wrist is seen from. */
export const MENU_PPM = 2800;
export const MENU_SIZE = [MENU_PX[0] / MENU_PPM, MENU_PX[1] / MENU_PPM];

export const TABS = [
  ['mode', 'Mode'], ['look', 'Look'], ['analysis', 'Analysis'], ['layers', 'Layers'], ['settings', 'Settings'], ['help', 'Help'],
];
export const PAGES = TABS.map((t) => t[0]);

const WH = 64, X0 = 240, RIGHT = 1004;

function tabLayout() {
  const gap = 8, x0 = 12, n = TABS.length;
  const w = (MENU_PX[0] - 2 * x0 - gap * (n - 1)) / n;
  return TABS.map(([id, label], i) => ({ id: `tab:${id}`, kind: 'button', rect: [x0 + i * (w + gap), 12, w, TAB_H - 20], label, group: 'tab', value: id }));
}

const span = (y, n, k, x0 = X0, right = RIGHT, gap = 10, h = WH) => {
  const w = (right - x0 - gap * (n - 1)) / n;
  return [x0 + k * (w + gap), y, w, h];
};

/** The Mode page: modes, the tools of each, the clipping resets, undo and reset view. */
export function modeLayout() {
  const w = [];
  MODES.forEach((m, k) => w.push({ id: `mode:${m}`, kind: 'button', rect: span(24, 3, k, 20, RIGHT, 12, 88), label: MODE_NAMES[m], group: 'mode', value: m }));
  SUBTOOLS.select.forEach((t, k) => w.push({ id: `sub:select:${t}`, kind: 'button', rect: span(160, 4, k), label: SUB_NAMES.select[t], group: 'sub-select', value: t }));
  w.push({ id: 'sel-clear', kind: 'button', rect: span(160, 4, 3), label: 'Clear' });
  SUBTOOLS.clip.forEach((t, k) => w.push({ id: `sub:clip:${t}`, kind: 'button', rect: span(256, 4, k), label: SUB_NAMES.clip[t], group: 'sub-clip', value: t }));
  w.push({ id: 'clip-all', kind: 'button', rect: span(256, 4, 2), label: 'All channels' });
  w.push({ id: 'clip-reset', kind: 'button', rect: span(256, 4, 3), label: 'Reset box' });
  w.push({ id: 'undo', kind: 'button', rect: span(352, 3, 0), label: 'Undo' });
  w.push({ id: 'reset-view', kind: 'button', rect: span(352, 3, 1), label: 'Reset view' });
  w.push({ id: 'menu-pin', kind: 'button', rect: span(352, 3, 2), label: 'Keep open' });
  return w;
}

export const SETTINGS_ROWS = [
  ['leftHanded', 'Left-handed (left hand points)'],
  ['labels', 'Labels by the buttons'],
  ['haptics', 'Haptics (pulses)'],
  ['hud', 'Numbers on the wrist'],
  ['governor', 'Hold the frame rate'],
  ['refine', 'Refine with the full cube'],
  ['refineDepth', 'Reproject refined frames with depth'],
  ['refineScale', 'Refined frame size'],
];

/** The Settings page: one toggle (or a stepping button) per row. */
export function settingsLayout() {
  return SETTINGS_ROWS.map(([key], i) => ({
    id: `set:${key}`, kind: key === 'refineScale' ? 'button' : 'toggle', rect: [784, 20 + i * 84, 200, WH], key,
  }));
}

/** The Help page: the guide and the control maps of the other modes. */
export function helpLayout() {
  const w = MODES.map((m, k) => ({ id: `help-mode:${m}`, kind: 'button', rect: span(20, 4, k, 20), label: MODE_NAMES[m], group: 'help-mode', value: m }));
  w.push({ id: 'help-guide', kind: 'button', rect: span(20, 4, 3, 20), label: 'Guide' });
  return w;
}

/** The widgets of a page, in page pixels (y from the top of the page). */
export function pageLayout(page, { layerButtons = [] } = {}) {
  if (page === 'mode') return modeLayout();
  if (page === 'look') return P.panelLayout().filter((w) => w.id !== 'drag' && w.id !== 'guide');
  if (page === 'analysis') return TP.toolsLayout();
  if (page === 'layers') return stripLayout(layerButtons).map((w) => ({ ...w, rect: [w.rect[0], 60, w.rect[2], 84] }));
  if (page === 'settings') return settingsLayout();
  if (page === 'help') return helpLayout();
  return [];
}

/** The whole menu's widgets in menu pixels: the tabs, then the page's shifted under them. */
export function menuLayout(page, opts) {
  const shift = (w) => ({ ...w, rect: [w.rect[0], w.rect[1] + TAB_H, w.rect[2], w.rect[3]], page: true });
  return [...tabLayout(), ...pageLayout(page, opts).map(shift)];
}

/**
 * The menu's pose by the secondary hand at `hand` (position), facing the eye
 * `eye`: above the hand and toward the body's middle (`inward` = +1 when the
 * menu goes to the hand's right, the left hand's case), upright.
 */
export function menuModel(hand, eye, headRight = [1, 0, 0], inward = 1, size = MENU_SIZE) {
  const r = M.normalize([headRight[0], 0, headRight[2]]);
  const p = M.add(hand, M.add([0, 0.05 + size[1] / 2, 0], M.scale3(r, inward * (size[0] / 2 - 0.04))));
  let z = M.sub(eye, p);
  if (Math.hypot(z[0], z[2]) < 1e-6) z = [0, 0, 1];
  z = M.normalize(z);
  let x = M.cross([0, 1, 0], z);
  if (Math.hypot(...x) < 1e-6) x = r;
  x = M.normalize(x);
  const y = M.cross(z, x);
  return new Float32Array([x[0], x[1], x[2], 0, y[0], y[1], y[2], 0, z[0], z[1], z[2], 0, p[0], p[1], p[2], 1]);
}

/**
 * Whether the menu shows this frame: while the button (or the palm) is held,
 * or kept open from its Mode page ("Keep open"). A kept menu closes at the
 * next press of the button, once let go (it stays open while held).
 * `st`: {pinned, armed, wasHeld} (Keep open sets pinned, clears armed).
 */
export function menuOpenState(st, held) {
  let pinned = !!st.pinned, armed = !!st.armed;
  if (pinned && !held) armed = true;
  if (pinned && armed && held && !st.wasHeld) { pinned = false; armed = false; }
  return { pinned, armed, wasHeld: held, open: held || pinned };
}

// ── Drawing ─────────────────────────────────────────────────────────────────

const C = {
  bg: 'rgba(12, 16, 24, 0.94)', head: 'rgba(30, 38, 54, 0.96)', fg: '#e6e9ef', muted: '#9aa3b2',
  btn: '#1d2433', btnHover: '#2b3650', on: '#2f7fd8', onFg: '#ffffff', edge: '#4a5670', warn: '#ffd27a', err: '#ff8a8a',
};

function roundRect(c, x, y, w, h, r) {
  c.beginPath();
  c.moveTo(x + r, y);
  c.arcTo(x + w, y, x + w, y + h, r);
  c.arcTo(x + w, y + h, x, y + h, r);
  c.arcTo(x, y + h, x, y, r);
  c.arcTo(x, y, x + w, y, r);
  c.closePath();
}

function fit(c, text, maxW) {
  if (c.measureText(text).width <= maxW) return text;
  let t = text;
  while (t.length > 1 && c.measureText(`${t}…`).width > maxW) t = t.slice(0, -1);
  return `${t}…`;
}

function button(c, w, { on = false, hover = false, disabled = false, label = w.label } = {}) {
  const [x, y, ww, hh] = w.rect;
  c.fillStyle = on ? C.on : hover && !disabled ? C.btnHover : C.btn;
  roundRect(c, x, y, ww, hh, 10);
  c.fill();
  if (hover) { c.strokeStyle = C.edge; c.lineWidth = 2; c.stroke(); }
  c.fillStyle = disabled ? '#5b6475' : on ? C.onFg : C.fg;
  c.font = '26px -apple-system, system-ui, sans-serif';
  c.textAlign = 'center';
  c.textBaseline = 'middle';
  c.fillText(fit(c, label || '', ww - 14), x + ww / 2, y + hh / 2 + 1);
  c.textAlign = 'left';
}

function toggle(c, w, on, hover) {
  const [x, y, ww, hh] = w.rect;
  c.fillStyle = on ? C.on : hover ? C.btnHover : C.btn;
  roundRect(c, x, y, ww, hh, hh / 2);
  c.fill();
  c.beginPath();
  c.arc(on ? x + ww - hh / 2 : x + hh / 2, y + hh / 2, hh / 2 - 6, 0, 2 * Math.PI);
  c.fillStyle = '#ffffff';
  c.fill();
  c.fillStyle = on ? C.onFg : C.fg;
  c.font = '24px -apple-system, system-ui, sans-serif';
  c.textAlign = 'center';
  c.textBaseline = 'middle';
  c.fillText(on ? 'on' : 'off', on ? x + (ww - hh) / 2 : x + hh + (ww - hh) / 2 - 6, y + hh / 2 + 1);
  c.textAlign = 'left';
}

function lines(c, list, x, y, maxW, lineH = 32, font = '24px -apple-system, system-ui, sans-serif') {
  c.font = font;
  c.textBaseline = 'middle';
  for (const l of list) {
    const [text, color] = Array.isArray(l) ? l : [l, C.muted];
    c.fillStyle = color;
    c.fillText(fit(c, text, maxW), x, y);
    y += lineH;
  }
  return y;
}

/** Words wrapped into lines of at most maxW pixels. */
function wrap(c, text, maxW) {
  const out = [];
  let line = '';
  for (const word of String(text).split(/\s+/).filter(Boolean)) {
    const next = line ? `${line} ${word}` : word;
    if (c.measureText(next).width <= maxW || !line) line = next;
    else { out.push(line); line = word; }
  }
  if (line) out.push(line);
  return out;
}

function drawModePage(c, layout, v, ui) {
  c.font = '24px -apple-system, system-ui, sans-serif';
  c.fillStyle = C.fg;
  c.textBaseline = 'middle';
  c.fillText('Select tool', 20, 160 + WH / 2);
  c.fillText('Clip', 20, 256 + WH / 2);
  for (const w of layout) {
    const on = (w.group === 'mode' && v.mode === w.value) || (w.group === 'sub-select' && v.selectTool === w.value)
      || (w.group === 'sub-clip' && v.clipTool === w.value) || (w.id === 'menu-pin' && v.pinned);
    button(c, w, { on, hover: ui.hover === w.id, label: w.id === 'menu-pin' && v.pinned ? 'Kept open' : w.label,
      disabled: w.id === 'undo' && !v.canUndo });
  }
  c.font = '24px -apple-system, system-ui, sans-serif';
  const y = lines(c, [[v.modeLine || '', '#9fd0ff']], 20, 460, PAGE_PX[0] - 40, 34, 'bold 28px -apple-system, system-ui, sans-serif');
  const out = [];
  for (const l of v.map || []) out.push(...wrap(c, l, PAGE_PX[0] - 40));
  lines(c, out.slice(0, 9), 20, y + 6, PAGE_PX[0] - 40, 32);
  if (v.status) lines(c, [[v.status, v.statusError ? C.err : C.warn]], 20, PAGE_PX[1] - 30, PAGE_PX[0] - 40);
}

function drawLayersPage(c, layout, v, ui) {
  if (!layout.length) {
    lines(c, ['No point table, velocity field or catalogue is shown.',
      'Open a point table or a velocity field, or place a catalogue',
      'over the cube, on the page (or from the desktop).'], 20, 60, PAGE_PX[0] - 40, 36, '26px -apple-system, system-ui, sans-serif');
    return;
  }
  lines(c, [[v.title || '', C.fg]], 20, 30, PAGE_PX[0] - 40, 32, 'bold 26px -apple-system, system-ui, sans-serif');
  for (const w of layout) button(c, w, { on: !!w.on, hover: ui.hover === w.id });
  if (v.note) lines(c, [[v.note.text, v.note.warn ? C.warn : C.muted]], 20, 190, PAGE_PX[0] - 40);
}

function drawSettingsPage(c, layout, v, ui) {
  SETTINGS_ROWS.forEach(([key, label], i) => {
    lines(c, [[label, C.fg]], 20, 20 + i * 84 + WH / 2, 740, 32, '28px -apple-system, system-ui, sans-serif');
  });
  for (const w of layout) {
    if (w.kind === 'toggle') toggle(c, w, !!v[w.key], ui.hover === w.id);
    else button(c, w, { hover: ui.hover === w.id, label: v.refineScaleText || '' });
  }
}

function drawHelpPage(c, layout, v, ui) {
  for (const w of layout) button(c, w, { on: w.group === 'help-mode' && v.helpMode === w.value, hover: ui.hover === w.id });
  c.font = '24px -apple-system, system-ui, sans-serif';
  const out = [];
  for (const l of v.helpLines || []) {
    const [text, color] = Array.isArray(l) ? l : [l, C.muted];
    for (const piece of wrap(c, text, PAGE_PX[0] - 40)) out.push([piece, color]);
  }
  lines(c, out.slice(0, 21), 20, 120, PAGE_PX[0] - 40, 32);
}

/**
 * Draws the menu: tabs, then `page` with its own layout (page pixels).
 * view: {page, look (vr-panel's panelView), analysis (tools-panel's view),
 * mode: {...}, layers: {title, note}, settings: {...}, help: {...}}.
 */
export function drawMenu(c, page, layout, view, ui = {}) {
  const [W, H] = MENU_PX;
  c.clearRect(0, 0, W, H);
  c.fillStyle = C.bg;
  roundRect(c, 0, 0, W, H, 18);
  c.fill();
  c.fillStyle = C.head;
  roundRect(c, 0, 0, W, TAB_H, 18);
  c.fill();
  for (const w of tabLayout()) button(c, w, { on: page === w.value, hover: ui.hover === w.id });
  c.save();
  c.translate(0, TAB_H);
  const pui = { hover: ui.hover, active: ui.active };
  if (page === 'look') P.drawPanel(c, layout, view.look || {}, pui, { embedded: true });
  else if (page === 'analysis') TP.drawTools(c, layout, view.analysis || {}, pui);
  else if (page === 'mode') drawModePage(c, layout, view.mode || {}, pui);
  else if (page === 'layers') drawLayersPage(c, layout, view.layers || {}, pui);
  else if (page === 'settings') drawSettingsPage(c, layout, view.settings || {}, pui);
  else if (page === 'help') drawHelpPage(c, layout, view.help || {}, pui);
  c.restore();
}

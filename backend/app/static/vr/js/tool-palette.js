// The tool palette (issue #257, from the Quest test): A on the primary hand
// opens a small panel of its own, apart from the menu, about 0.8 m in front of
// that hand, with one large icon per tool (tool-icons.js), grouped by task:
// Look (Explore), Select (Kinematic Lasso, Spectrum probe, Box region), Range
// (Velocity range, Spatial crop), Measure (Moment, PV slice). Pointing at an
// icon shows the tool's name and what it does; the trigger picks it and
// closes the palette; A again or × closes it without a change. The tool in
// use is lit; tools that do not apply to the data shown are greyed. The
// secondary stick ←/→ steps through the same list, in the same order.
//
// Layout, placement, the active tool and the picks are pure
// (backend/tests/vr/tool-palette.test.mjs); drawPalette needs a 2-D context.
// The palette is a 2-D canvas uploaded only when it changes (#229).

import * as M from './mat4.js';
import { drawIcon } from './tool-icons.js';
import { drawFit, wrapFit, measureWith } from './text-fit.js';

export const PALETTE_PX = [1024, 380];
/** About 1.1 m from the eyes (0.8 m beyond a hand held 0.3-0.4 m out):
 *  0.62 m wide, a 112-pixel icon 6.8 cm (about 3.5°), the 26-30 px text
 *  0.8-0.9°. */
export const PALETTE_PPM = 1650;
export const PALETTE_SIZE = [PALETTE_PX[0] / PALETTE_PPM, PALETTE_PX[1] / PALETTE_PPM];
export const PALETTE_DIST = 0.8;

export const GROUPS = [['look', 'Look'], ['select', 'Select'], ['range', 'Range'], ['measure', 'Measure']];

/** The tools, in the palette's order (the stick's too): what picking one does
 *  ({mode, sub} for a trigger tool, {measure} for a measurement), the kinds
 *  of data it works on. */
export const TOOLS = [
  { id: 'explore', group: 'look', short: 'Explore', name: 'Explore', mode: 'explore', sub: null, kinds: ['cube', 'points', 'flow'],
    desc: 'Look around: the trigger pins the readout where you point.' },
  { id: 'lasso', group: 'select', short: 'Lasso', name: 'Kinematic Lasso', mode: 'select', sub: 'lasso', kinds: ['cube'],
    desc: 'Select a connected object by pointing at it.' },
  { id: 'probe', group: 'select', short: 'Probe', name: 'Spectrum probe', mode: 'select', sub: 'probe', kinds: ['cube'],
    desc: 'The spectrum of the full cube where you point.' },
  { id: 'box', group: 'select', short: 'Box', name: 'Box region', mode: 'select', sub: 'box', kinds: ['cube'],
    desc: 'Statistics of the voxels between two corners you place.' },
  { id: 'slab', group: 'range', short: 'Velocity', name: 'Velocity range', mode: 'clip', sub: 'slab', kinds: ['cube'],
    desc: 'The trigger centres the channel range on the emission you point at.' },
  { id: 'crop', group: 'range', short: 'Crop', name: 'Spatial crop', mode: 'clip', sub: 'box', kinds: ['cube'],
    desc: 'Hold the trigger near a face of the crop to drag it.' },
  { id: 'moment', group: 'measure', short: 'Moment', name: 'Moment', measure: 'moment', kinds: ['cube'],
    desc: 'The intensity map (M0) of the selection, or of the whole field.' },
  { id: 'pv', group: 'measure', short: 'PV slice', name: 'PV slice', measure: 'pv', kinds: ['cube'],
    desc: 'The trigger adds the points of a position-velocity slice.' },
];
export const toolById = (id) => TOOLS.find((t) => t.id === id) || null;

/** The tool in use, from the mode and its tool (a PV slice being drawn first). */
export function activeTool(mode, sub, pvArmed = false) {
  if (pvArmed) return 'pv';
  if (mode === 'select') return ['lasso', 'probe', 'box'].includes(sub) ? sub : 'lasso';
  if (mode === 'clip') return sub === 'box' ? 'crop' : 'slab';
  return 'explore';
}

/** Why tool `t` is greyed for data `kind` ('' when it applies). */
export function whyNot(t, kind) {
  if (kind === 'none' || !kind) return 'Open a cube, a point table or a velocity field first.';
  return t.kinds.includes(kind) ? '' : 'Cubes only: not on a point table or a velocity field.';
}

const TITLE_H = 64, CELL = 112, LABEL_H = 34, GAP = 6, GROUP_GAP = 22, X0 = 19, Y_GROUP = 72, Y_ICON = 108;
export const TOOLTIP_Y = Y_ICON + CELL + LABEL_H + 18;

/** The palette's widgets for `ctx` {kind, active}: one per tool (its icon
 *  cell and short name), and the close button. */
export function paletteLayout(ctx = { kind: 'none', active: 'explore' }) {
  const out = [];
  let x = X0, prev = null;
  for (const t of TOOLS) {
    if (prev) x += prev === t.group ? GAP : GROUP_GAP;
    out.push({ id: `tool:${t.id}`, kind: 'button', rect: [x, Y_ICON, CELL, CELL + LABEL_H], tool: t.id, group: t.group,
      label: t.short, on: ctx.active === t.id, disabled: whyNot(t, ctx.kind) });
    x += CELL;
    prev = t.group;
  }
  out.push({ id: 'palette-close', kind: 'button', rect: [PALETTE_PX[0] - 84, 8, 72, TITLE_H - 16], label: '×' });
  return out;
}

/** Each group's span on the palette, for its heading: [[id, label, x0, x1]]. */
export function groupSpans(layout = paletteLayout()) {
  return GROUPS.map(([id, label]) => {
    const cells = layout.filter((w) => w.group === id);
    return [id, label, cells[0].rect[0], cells[cells.length - 1].rect[0] + CELL];
  });
}

/** What a pick does: {mode, sub} (a trigger tool) or {measure} (a measurement),
 *  and `disarmPv`: every pick but the PV slice ends a slice being drawn. Null
 *  for no tool. */
export function pickOf(id) {
  const t = toolById(String(id).replace(/^tool:/, ''));
  if (!t) return null;
  const base = { tool: t.id, disarmPv: t.id !== 'pv' };
  return t.measure ? { ...base, measure: t.measure } : { ...base, mode: t.mode, sub: t.sub };
}

/** The tooltip: the tool pointed at (its name and what it does), else the one in use. */
export function tooltip(hoverId, ctx) {
  const t = toolById(String(hoverId || '').replace(/^tool:/, ''));
  if (t) {
    const why = whyNot(t, ctx.kind);
    return { title: t.name, text: why || t.desc, greyed: !!why };
  }
  const a = toolById(ctx.active);
  return { title: a ? `In use: ${a.name}` : 'Tools', text: 'Point at a tool to read what it does; the trigger picks it.', greyed: false };
}

/** The secondary stick's step: the next tool of the palette, in its order,
 *  that applies to `kind` (Moment computes the intensity map when stepped on,
 *  PV slice starts drawing one, as when picked). */
export function stepTool(from, kind, step = 1) {
  const list = TOOLS.filter((t) => !whyNot(t, kind));
  if (!list.length) return null;
  const i = list.findIndex((t) => t.id === from);
  const n = list.length;
  return list[(((i < 0 ? 0 : i + step) % n) + n) % n].id;
}

/** Where the stick's next step starts: the tool in use, or Moment when it was
 *  the last step and the tool in use has not changed since (Moment is an
 *  action, never the tool in use, so without this the stick would land on it
 *  again and again). `last` {id, active} records the last pick. */
export function stepFrom(active, last) {
  return last && last.id === 'moment' && last.active === active ? 'moment' : active;
}

/** Presses from the palette closed to tool `id` picked: A, then the tool (2),
 *  or Infinity when it is greyed for `kind`. */
export function pressesToTool(id, kind) {
  const w = paletteLayout({ kind, active: 'explore' }).find((x) => x.id === `tool:${id}`);
  return w && !w.disabled ? 2 : Infinity;
}

/**
 * Where the palette opens: PALETTE_DIST in front of the primary hand `hand`
 * along the head's horizontal gaze (`head`, column-major), a little above
 * the hand, turned to the eyes. It stays there until it closes.
 */
export function paletteModel(hand, head, { dist = PALETTE_DIST, lift = 0.06 } = {}) {
  const eye = [head[12], head[13], head[14]];
  let f = [-head[8], 0, -head[10]];
  if (Math.hypot(f[0], f[2]) < 0.3) f = [head[4] * (head[9] > 0 ? 1 : -1), 0, head[6] * (head[9] > 0 ? 1 : -1)];
  const n = Math.hypot(f[0], f[2]) || 1;
  const p = [hand[0] + (f[0] / n) * dist, hand[1] + lift, hand[2] + (f[2] / n) * dist];
  let z = M.sub(eye, p);
  if (Math.hypot(z[0], z[2]) < 1e-6) z = [0, 0, 1];
  z = M.normalize(z);
  const x = M.normalize(M.cross([0, 1, 0], z));
  const y = M.cross(z, x);
  return new Float32Array([x[0], x[1], x[2], 0, y[0], y[1], y[2], 0, z[0], z[1], z[2], 0, p[0], p[1], p[2], 1]);
}

// ── Drawing ─────────────────────────────────────────────────────────────────

const C = {
  bg: 'rgba(12, 16, 24, 0.94)', head: 'rgba(30, 38, 54, 0.96)', fg: '#e6e9ef', muted: '#9aa3b2', cell: '#1d2433',
  hover: '#2b3650', on: '#2f7fd8', off: '#4a5262', offBg: '#151a24', why: '#c9a86a', edge: '#9fd0ff',
};
const FONT = '-apple-system, system-ui, sans-serif';

function roundRect(c, x, y, w, h, r) {
  c.beginPath();
  c.moveTo(x + r, y);
  c.arcTo(x + w, y, x + w, y + h, r);
  c.arcTo(x + w, y + h, x, y + h, r);
  c.arcTo(x, y + h, x, y, r);
  c.arcTo(x, y, x + w, y, r);
  c.closePath();
}


/** Draws the palette for `ctx` {kind, active}; ui {hover}. */
export function drawPalette(c, layout, ctx, ui = {}) {
  const [W, H] = PALETTE_PX;
  c.clearRect(0, 0, W, H);
  c.fillStyle = C.bg;
  roundRect(c, 0, 0, W, H, 18);
  c.fill();
  c.fillStyle = C.head;
  roundRect(c, 0, 0, W, TITLE_H, 18);
  c.fill();
  c.textBaseline = 'middle';
  c.fillStyle = C.fg;
  const close = layout.find((w) => w.id === 'palette-close');
  const titleEnd = close ? close.rect[0] - 16 : W - 20;
  const t = drawFit(c, 'Tools', 20, TITLE_H / 2 + 2, 160, `bold 30px ${FONT}`);
  c.fillStyle = C.muted;
  const tx = 20 + c.measureText(t.text).width + 24;
  drawFit(c, 'A or × closes', tx, TITLE_H / 2 + 2, titleEnd - tx, `22px ${FONT}`);
  for (const [, label, x0, x1] of groupSpans(layout)) {
    c.fillStyle = C.muted;
    c.textAlign = 'center';
    drawFit(c, label, (x0 + x1) / 2, Y_GROUP + 14, x1 - x0 - 8, `24px ${FONT}`);
    c.textAlign = 'left';
    c.fillStyle = 'rgba(159, 208, 255, 0.35)';
    c.fillRect(x0 + 4, Y_GROUP + 30, x1 - x0 - 8, 2);
  }
  for (const w of layout) {
    const [x, y, ww, hh] = w.rect;
    const hover = ui.hover === w.id, off = !!w.disabled;
    if (w.id === 'palette-close') {
      c.fillStyle = hover ? C.hover : C.cell;
      roundRect(c, x, y, ww, hh, 10);
      c.fill();
      c.fillStyle = C.fg;
      c.textAlign = 'center';
      drawFit(c, '×', x + ww / 2, y + hh / 2 + 1, ww - 8, `34px ${FONT}`);
      c.textAlign = 'left';
      continue;
    }
    c.fillStyle = w.on && !off ? C.on : off ? C.offBg : hover ? C.hover : C.cell;
    roundRect(c, x, y, ww, hh, 14);
    c.fill();
    if (hover || w.on) { c.strokeStyle = w.on ? '#ffffff' : C.edge; c.lineWidth = w.on ? 4 : 2; c.stroke(); }
    drawIcon(c, w.tool, x + 14, y + 10, CELL - 28, off ? C.off : '#ffffff');
    c.fillStyle = off ? C.off : C.fg;
    c.textAlign = 'center';
    drawFit(c, w.label, x + ww / 2, y + CELL - 2, ww - 10, `22px ${FONT}`);
    c.textAlign = 'left';
  }
  // The tooltip: the name, then what it does on up to two lines (#276).
  const tip = tooltip(ui.hover, ctx);
  c.fillStyle = C.fg;
  drawFit(c, tip.title, 20, TOOLTIP_Y + 20, W - 40, `bold 30px ${FONT}`);
  c.fillStyle = tip.greyed ? C.why : C.muted;
  const body = wrapFit(measureWith(c), `26px ${FONT}`, tip.text, W - 40, 2);
  c.font = body.font;
  body.lines.forEach((l, i) => c.fillText(l, 20, TOOLTIP_Y + 58 + i * 32));
}

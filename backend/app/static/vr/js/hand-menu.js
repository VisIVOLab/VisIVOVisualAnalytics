// The hand menu (issues #201, #257, #276). The secondary hand's lower button
// (X; or its palm turned up) opens it and, pressed again, closes it, as does
// the × on its title bar. It opens just above and beyond that hand, about a
// metre from the eyes and turned to them, and stays there (world-locked), so
// the hand that opened it is free. Its title bar moves it. The primary hand's
// ray and trigger press its items.
//
// Its tabs follow the way an astronomer works and are the same on every
// dataset: Display (look at the data), Range (cut it), Select (an object),
// Measure, More. What a tab holds depends on the data kind shown (a cube, a
// point table or a velocity field: the last two replace the cube). A control
// that does not apply is greyed with a one-line reason, so the layout never
// moves. Choosing a tab sets the mode (Display and Measure: explore; Range:
// clip; Select: select). The footer (Undo, Reset view) is always there. The
// technical settings are a page of their own, reachable only with ?debug.
//
// The layout is a pure function of a context (what is shown and chosen), so
// the hit tests, the greying and the "two presses from opening" rule are
// unit-tested (backend/tests/vr/hand-menu.test.mjs); drawMenu needs only a
// 2-D context. The menu is a 2-D canvas uploaded only when it changed (#229).
// Every text goes through text-fit.js (wrapped, shrunk a little, cut last);
// a button too narrow for its long name carries a short label and the long
// form as its tooltip (`tip`), written under its row while the ray is on it.

import * as M from './mat4.js';
import { COLORMAP_NAMES, colormapTable } from './colormaps.js';
import { drawIcon } from './tool-icons.js';
import { fitLine, wrapFit, measureWith } from './text-fit.js';

export const TITLE_H = 64;
export const TAB_H = 88;
export const PAGE_H = 720;
export const FOOTER_H = 88;
export const PAGE_Y = TITLE_H + TAB_H;
export const FOOTER_Y = PAGE_Y + PAGE_H;
export const PAGE_PX = [1024, PAGE_H];
export const MENU_PX = [1024, TITLE_H + TAB_H + PAGE_H + FOOTER_H];

/** Where the menu stands when it opens: this far in front of the eyes
 *  (horizontally) and this much below them (about 7° at 0.95 m). */
export const MENU_DIST = 0.95;
export const MENU_DROP = 0.12;
/** Before #257 the menu was drawn at 2800 px/m by the wrist, about 0.45 m
 *  from the eyes; at MENU_DIST the same text keeps the same angle: the 22-32
 *  px text is 1.0-1.4° high (about 22 canvas pixels a degree). */
export const MENU_PPM = (2800 * 0.45) / MENU_DIST;
export const MENU_SIZE = [MENU_PX[0] / MENU_PPM, MENU_PX[1] / MENU_PPM];
/** Opened from a hand (#276): its lower edge this far above the hand, its
 *  centre no higher than MENU_RISE above the eyes and no lower than
 *  MENU_SINK below them, and at most MENU_TURN radians to the side of the
 *  gaze (so a hand held far to the side does not put it out of view). */
export const MENU_GAP = 0.05;
export const MENU_RISE = 0.08;
export const MENU_SINK = 0.3;
export const MENU_TURN = (50 * Math.PI) / 180;
/** A hand farther than this from the gaze (behind or beside the user): the menu opens along the gaze. */
export const MENU_BEHIND = (90 * Math.PI) / 180;

export const TABS = [
  ['display', 'Display'], ['range', 'Range'], ['select', 'Select'], ['measure', 'Measure'], ['more', 'More'],
];
/** Every page: the tabs', and the technical settings (?debug only, under More). */
export const PAGES = [...TABS.map((t) => t[0]), 'debug'];
/** The mode a tab sets (null: the mode stays). */
export const TAB_MODE = { display: 'explore', range: 'clip', select: 'select', measure: 'explore', more: null, debug: null };
/** The tab of a mode, for the page shown when the menu opens. */
export const MODE_TAB = { explore: 'display', clip: 'range', select: 'select' };
export const KINDS = ['cube', 'points', 'flow', 'none'];

/** The tab lit for a page (the technical page belongs to More). */
export const tabOf = (page) => (page === 'debug' ? 'more' : page);

/** The page shown when the menu opens: the last one, unless the mode was
 *  changed since (A on the primary hand), then that mode's tab. */
export function pageOnOpen(page, mode) {
  if (!PAGES.includes(page)) return MODE_TAB[mode] || 'display';
  const m = TAB_MODE[page];
  return m && m !== mode ? (MODE_TAB[mode] || 'display') : page;
}

// ── Layout ───────────────────────────────────────────────────────────────────

const WH = 60, X0 = 240, RIGHT = 1004, ROW_STEP = 100, ROW_TOP = 12;
const rowY = (i) => ROW_TOP + i * ROW_STEP;
const span = (i, n, k, x0 = X0, right = RIGHT, gap = 10) => {
  const w = (right - x0 - gap * (n - 1)) / n;
  return [x0 + k * (w + gap), rowY(i), w, WH];
};
/** Wide-label pages (More, the technical settings): the control on the right. */
const RX = 784;

/** The reasons a control is greyed. */
export const WHY = {
  none: 'Open a cube, a point table or a velocity field first.',
  cubeOnly: 'Cubes only: a point table or a velocity field has no channels.',
  noSpectral: 'A point table or a velocity field has no spectral axis.',
  cubeZ: 'On a cube the third axis is the range above.',
  particlesSelect: 'Selections on particles and fields come later (#259).',
  boxApply: 'Isolate and Remove need a Kinematic Lasso selection.',
  applyFirst: 'Isolate or Remove first.',
  measureCube: 'Measure works on a cube.',
  spectrumFirst: 'Take a spectrum with the Probe (Select) first.',
  arrowsOff: 'The arrows are off.',
  splatsOnly: 'Smooth (splats) only.',
  noColour: 'Choose a column to colour by first.',
  noCatalogue: 'Place a catalogue over the cube on the page (or from the desktop).',
};

/** The default context: nothing shown, nothing chosen. main.js fills it. */
export function defaultContext() {
  return {
    kind: 'none', debug: false, colormap: COLORMAP_NAMES[0], render: 0, spectralName: 'Velocity range',
    cat: { loaded: false, show: true, labels: true, pins: true }, clipTool: 'slab',
    sel: { tool: 'lasso', seedMode: 'new', lasso: false, box: false, applied: false },
    meas: { lasso: false, moment: null, pvArmed: false, spectrum: false, pinned: false },
    pts: { fields: [], field: null, log: false, style: 'points', budget: '', box: true, fieldPage: 0 },
    flow: { stream: true, arrows: true, box: true },
    more: { leftHanded: false, labels: true, haptics: true, hud: true },
    dbg: { governor: true, refine: true, refineDepth: true, refineScale: '1×', record: false, run: 'Run protocol', thrSource: 'cube' },
    canUndo: false,
  };
}

const btn = (id, rect, label, extra = {}) => ({ id, kind: 'button', rect, label, ...extra });

function swatches(i, ctx) {
  const n = COLORMAP_NAMES.length;
  return COLORMAP_NAMES.map((name, k) => ({ id: `cmap:${name}`, kind: 'swatch', rect: span(i, n, k), label: name,
    group: 'cmap', value: name, on: ctx.colormap === name }));
}

function displayCube(ctx) {
  const rows = [['cmap', 'Colour map'], ['thr', 'Threshold'], ['cmax', 'Colour max'], ['opacity', 'Opacity'],
    ['render', 'Rendering'], ['cat', 'Catalogue']];
  const c = ctx.cat;
  const noCat = c.loaded ? '' : WHY.noCatalogue;
  const w = [
    ...swatches(0, ctx),
    { id: 'thr', kind: 'slider', rect: [X0, rowY(1), 560, WH] },
    btn('thr-reset', [X0 + 570, rowY(1), RIGHT - X0 - 570, WH], 'Reset'),
    { id: 'cmax', kind: 'slider', rect: [X0, rowY(2), RIGHT - X0, WH] },
    { id: 'opacity', kind: 'slider', rect: [X0, rowY(3), RIGHT - X0, WH] },
    btn('render:0', span(4, 2, 0), 'Emission', { group: 'render', value: 0, on: ctx.render === 0, tip: 'Emission: the voxels add up along each ray' }),
    btn('render:1', span(4, 2, 1), 'MIP', { group: 'render', value: 1, on: ctx.render === 1, tip: 'MIP: the brightest value along each ray' }),
    btn('ct-show', span(5, 3, 0), 'Overlay', { on: c.loaded && c.show, disabled: noCat, tip: 'The catalogue\'s sources over the cube' }),
    btn('ct-labels', span(5, 3, 1), 'Labels', { on: c.loaded && c.labels, disabled: noCat, tip: 'The sources\' names' }),
    btn('ct-pins', span(5, 3, 2), 'Through velocity', { on: c.loaded && c.pins, disabled: noCat, tip: 'Through velocity: pins along the velocity axis' }),
  ];
  return { rows, widgets: w, status: 6 };
}

/** A velocity column (colour by velocity is a main task of #257): VX, v_y, vel, velocity_z, speed, |v|, … */
export const isVelocityField = (f) => /^(v|vel|velocity|speed|\|v\|)([_ ]?[xyz])?$/i.test(String(f).trim());

/** The columns a point table can be coloured by, as chips: none first, then
 *  the velocity columns, then the others in the table's order; at most five
 *  chips a page (the last one pages on when there are more). The menu opens
 *  on the first page, so a velocity column is always two presses away. */
export function fieldChips(fields, page = 0, perPage = 5) {
  const all = [null, ...fields.filter(isVelocityField), ...fields.filter((f) => !isVelocityField(f))];
  if (all.length <= perPage) return { chips: all, more: false, pages: 1 };
  const per = perPage - 1, pages = Math.ceil(all.length / per);
  const p = ((page % pages) + pages) % pages;
  return { chips: all.slice(p * per, p * per + per), more: true, pages };
}

function displayPoints(ctx) {
  const p = ctx.pts;
  const rows = [['field', 'Colour by'], ['scale', 'Scale, range'], ['cmap', 'Colour map'], ['style', 'Style'],
    ['intensity', 'Intensity'], ['shown', 'Particles shown']];
  const { chips, more } = fieldChips(p.fields, p.fieldPage);
  const n = chips.length + (more ? 1 : 0);
  // A column's name is the data's: cut when it is long (`free`), whole in the tooltip.
  const w = chips.map((f, k) => btn(`pt-field:${f ?? ''}`, span(0, n, k), f ?? 'none',
    { group: 'pt-field', value: f, on: (p.field ?? null) === f, free: true, tip: f ? `Colour by ${f}` : 'One colour' }));
  if (more) w.push(btn('pt-field-more', span(0, n, n - 1), 'more ▸', { tip: 'The next columns' }));
  const noCol = p.field ? '' : WHY.noColour;
  w.push(btn('pt-log', [X0, rowY(1), 150, WH], 'Log', { on: !!p.log, disabled: noCol }));
  w.push({ id: 'pt-range', kind: 'range', rect: [X0 + 160, rowY(1), RIGHT - X0 - 160, WH], disabled: noCol });
  w.push(...swatches(2, ctx));
  w.push(btn('pt-style:points', span(3, 4, 0), 'Points', { group: 'pt-style', value: 'points', on: p.style !== 'splats', tip: 'Points: one dot a particle' }));
  w.push(btn('pt-style:splats', span(3, 4, 1), 'Splats', { group: 'pt-style', value: 'splats', on: p.style === 'splats', tip: 'Smooth (splats): a soft blob a particle' }));
  w.push(btn('pt-smaller', span(3, 4, 2), 'Size −', { tip: 'Smaller points or splats' }));
  w.push(btn('pt-larger', span(3, 4, 3), 'Size +', { tip: 'Larger points or splats' }));
  w.push({ id: 'pt-intensity', kind: 'slider', rect: [X0, rowY(4), RIGHT - X0, WH], disabled: p.style === 'splats' ? '' : WHY.splatsOnly });
  w.push(btn('pt-budget', span(5, 3, 0), `${p.budget || '?'} ▸`, { tip: 'Particles sampled: the next budget' }));
  w.push(btn('pt-reload', span(5, 3, 1), 'Sample again', { tip: 'Sample the table again' }));
  w.push(btn('pt-box', span(5, 3, 2), 'Box and ticks', { on: !!p.box, tip: 'The box and its ticks' }));
  return { rows, widgets: w, status: 6 };
}

function displayFlow(ctx) {
  const f = ctx.flow;
  const rows = [['lines', 'Show'], ['arrows', 'Arrow length'], ['seeds', 'Seed density'], ['speed', 'Colour by speed'],
    ['cmap', 'Colour map'], ['box', 'Box']];
  const off = f.arrows ? '' : WHY.arrowsOff;
  const w = [
    btn('fl-stream', span(0, 2, 0), 'Streamlines', { on: !!f.stream }),
    btn('fl-arrows', span(0, 2, 1), 'Arrows', { on: !!f.arrows }),
    btn('fl-shorter', span(1, 2, 0), 'Shorter', { disabled: off }),
    btn('fl-longer', span(1, 2, 1), 'Longer', { disabled: off }),
    btn('fl-fewer', span(2, 3, 0), 'Fewer'),
    btn('fl-more', span(2, 3, 1), 'More'),
    btn('fl-retrace', span(2, 3, 2), 'Reseed streamlines', { tip: 'Reseed streamlines: new lines from new seeds' }),
    { id: 'fl-range', kind: 'range', rect: [X0, rowY(3), RIGHT - X0, WH] },
    ...swatches(4, ctx),
    btn('fl-box', [X0, rowY(5), 300, WH], 'Box and ticks', { on: !!f.box, tip: 'The box and its ticks' }),
  ];
  return { rows, widgets: w, status: 6 };
}

function rangePage(ctx) {
  const cube = ctx.kind === 'cube';
  const rows = [['slab', ctx.spectralName], ['trigger', 'Trigger'], ['crop0', 'Crop x'], ['crop1', 'Crop y'], ['crop2', 'Crop z'],
    ['crop', 'Spatial crop']];
  const notCube = cube ? '' : WHY.noSpectral;
  const w = [
    { id: 'slab', kind: 'range', rect: [X0, rowY(0), 560, WH], disabled: notCube },
    btn('slab-all', [X0 + 570, rowY(0), RIGHT - X0 - 570, WH], 'Full band', { disabled: notCube }),
    btn('sub:clip:slab', span(1, 2, 0), ctx.spectralName, { group: 'sub-clip', value: 'slab', icon: 'slab', on: cube && ctx.clipTool === 'slab', disabled: notCube,
      tip: 'The trigger centres the range on the emission' }),
    // The crop is drawn and grabbed on every kind of data (#277).
    btn('sub:clip:box', span(1, 2, 1), 'Spatial crop', { group: 'sub-clip', value: 'box', icon: 'crop', on: ctx.kind !== 'none' && ctx.clipTool === 'box',
      tip: 'Drag a dot, the inside, or elsewhere for a new box' }),
  ];
  for (const a of [0, 1, 2]) {
    w.push({ id: `crop:${a}`, kind: 'range', rect: [X0, rowY(2 + a), RIGHT - X0, WH], axis: a, disabled: cube && a === 2 ? WHY.cubeZ : '' });
  }
  w.push(btn('crop-reset', [X0, rowY(5), 300, WH], 'Reset crop'));
  return { rows, widgets: w, status: 6 };
}

function selectPage(ctx) {
  const cube = ctx.kind === 'cube';
  const s = ctx.sel;
  const rows = [['tool', 'Tool'], ['lasso', 'Lasso'], ['selection', 'Selection'], ['result', 'Result']];
  const notCube = cube ? '' : WHY.particlesSelect;
  const w = [
    // The tool palette's order and icons (tool-palette.js), so both places look alike.
    btn('tool:lasso', span(0, 3, 0), 'Kinematic Lasso', { group: 'tool', value: 'lasso', icon: 'lasso', on: cube && s.tool === 'lasso', disabled: notCube, tip: 'Kinematic Lasso: a connected object' }),
    btn('tool:probe', span(0, 3, 1), cube ? 'Spectrum probe' : 'Probe', { group: 'tool', value: 'probe', icon: 'probe', on: cube && s.tool === 'probe', disabled: notCube, tip: 'Spectrum probe: the spectrum where you point' }),
    btn('tool:box', span(0, 3, 2), 'Box region', { group: 'tool', value: 'box', icon: 'box', on: cube && s.tool === 'box', disabled: notCube, tip: 'Box region: statistics between two corners' }),
  ];
  [['new', 'New'], ['add', 'Add'], ['carve', 'Subtract']].forEach(([v, label], k) => {
    w.push(btn(`seed:${v}`, span(1, 3, k), label, { group: 'seed', value: v, on: cube && s.tool === 'lasso' && s.seedMode === v, disabled: notCube }));
  });
  w.push(btn('sel-clear', [X0, rowY(2), 240, WH], 'Clear', { disabled: notCube }));
  // The selection's actions, only while there is one (#257).
  if (cube && (s.lasso || s.box)) {
    const why = s.lasso ? '' : WHY.boxApply;
    w.push(btn('isolate', span(3, 3, 0), 'Isolate', { disabled: why }));
    w.push(btn('remove', span(3, 3, 1), 'Remove', { disabled: why }));
    w.push(btn('show-applied', span(3, 3, 2), 'Show result cube', { disabled: why || (s.applied ? '' : WHY.applyFirst), tip: 'Show result cube: the isolated or remaining cube' }));
  }
  return { rows, widgets: w, status: 4, placeholder: { result: 'Isolate, Remove and Show result cube appear once something is selected.' } };
}

function measurePage(ctx) {
  const cube = ctx.kind === 'cube';
  const m = ctx.meas;
  const rows = [['moment', 'Moment', 'moment'], ['pv', 'PV slice', 'pv'], ['pvw', 'PV width'], ['spectrum', 'Spectrum']];
  const notCube = cube ? '' : WHY.measureCube;
  const w = [
    // Short labels, the long form as the tooltip (#276).
    btn('moment:0', span(0, 4, 0), 'M0', { group: 'moment', value: 0, on: m.moment === 0, disabled: notCube, tip: 'Intensity (M0)' }),
    btn('moment:1', span(0, 4, 1), 'M1', { group: 'moment', value: 1, on: m.moment === 1, disabled: notCube, tip: 'Velocity (M1)' }),
    btn('moment:2', span(0, 4, 2), 'M2', { group: 'moment', value: 2, on: m.moment === 2, disabled: notCube, tip: 'Dispersion (M2)' }),
    btn('moment-hide', span(0, 4, 3), 'Hide', { disabled: notCube, tip: 'Hide the moment window' }),
    btn('pv-draw', span(1, 4, 0), m.pvArmed ? 'Drawing' : 'Draw', { armed: !!m.pvArmed, disabled: notCube, tip: 'Draw a path: the trigger adds its points' }),
    btn('pv-undo', span(1, 4, 1), 'Undo', { disabled: notCube, tip: 'Remove the path\'s last point' }),
    btn('pv-new', span(1, 4, 2), 'New', { disabled: notCube, tip: 'Start a new path' }),
    btn('pv-hide', span(1, 4, 3), 'Hide', { disabled: notCube, tip: 'Hide the PV window' }),
    btn('pv-narrow', span(2, 2, 0), 'PV width −', { disabled: notCube, tip: 'PV width −: a narrower slice' }),
    btn('pv-wide', span(2, 2, 1), 'PV width +', { disabled: notCube, tip: 'PV width +: a wider slice' }),
    btn('spec-pin', [X0, rowY(3), 360, WH], m.pinned ? 'Unpin spectrum' : 'Pin spectrum',
      { on: !!m.pinned, disabled: notCube || (m.spectrum || m.pinned ? '' : WHY.spectrumFirst) }),
  ];
  return { rows, widgets: w, status: 4 };
}

export const MORE_ROWS = [['hand', 'Handedness'], ['labels', 'Button labels'], ['haptics', 'Haptics'], ['hud', 'Wrist readout'], ['guide', 'Guide']];

function morePage(ctx) {
  const o = ctx.more;
  const rows = [...MORE_ROWS];
  const w = [
    btn('hand:right', [RX - 260, rowY(0), 250, WH], 'Right-handed', { group: 'hand', value: false, on: !o.leftHanded }),
    btn('hand:left', [RX, rowY(0), RIGHT - RX, WH], 'Left-handed', { group: 'hand', value: true, on: !!o.leftHanded }),
    { id: 'set:labels', kind: 'toggle', rect: [RX, rowY(1), RIGHT - RX, WH], key: 'labels', on: !!o.labels },
    { id: 'set:haptics', kind: 'toggle', rect: [RX, rowY(2), RIGHT - RX, WH], key: 'haptics', on: !!o.haptics },
    { id: 'set:hud', kind: 'toggle', rect: [RX, rowY(3), RIGHT - RX, WH], key: 'hud', on: !!o.hud },
    btn('guide', [RX - 260, rowY(4), RIGHT - RX + 260, WH], 'Show the guide'),
  ];
  if (ctx.debug) {
    rows.push(['debug', 'Technical (?debug)']);
    w.push(btn('debug-page', [RX - 260, rowY(5), RIGHT - RX + 260, WH], 'Technical settings ▸'));
  }
  return { rows, widgets: w, wide: true, status: 6 };
}

export const DEBUG_ROWS = [['governor', 'Hold the frame rate'], ['refine', 'Refine with the full cube'],
  ['refineDepth', 'Reproject refined frames with depth'], ['refineScale', 'Refined frame size'],
  ['record', 'Record measurements'], ['thrsrc', 'Threshold from']];

function debugPage(ctx) {
  const d = ctx.dbg;
  const tog = (key, i) => ({ id: `set:${key}`, kind: 'toggle', rect: [RX, rowY(i), RIGHT - RX, WH], key, on: !!d[key] });
  const w = [
    tog('governor', 0), tog('refine', 1), tog('refineDepth', 2),
    btn('set:refineScale', [RX, rowY(3), RIGHT - RX, WH], d.refineScale || '', { key: 'refineScale' }),
    tog('record', 4),
    btn('set:measureRun', [RX - 300, rowY(4), 290, WH], d.run || 'Run protocol', { key: 'measureRun', on: d.run === 'Cancel run' }),
    btn('thrsrc:reduced', [RX - 300, rowY(5), 290, WH], 'This binning', { group: 'thrsrc', value: 'reduced', on: d.thrSource === 'reduced' }),
    btn('thrsrc:cube', [RX, rowY(5), RIGHT - RX, WH], 'Full cube', { group: 'thrsrc', value: 'cube', on: d.thrSource === 'cube' }),
    btn('debug-back', [20, rowY(6), 300, WH], '◂ More'),
  ];
  return { rows: [...DEBUG_ROWS], widgets: w, wide: true, status: 7 };
}

/**
 * A page for the context: {rows: [[key, label]] (row i at rowY(i)), widgets
 * (page pixels, y from the top of the page; each {id, kind, rect, label, on,
 * disabled: '' or the one-line reason}), status: the first row of the status
 * lines, placeholder: {rowKey: text} where a row's controls are not shown,
 * wide: the labels take the left half}.
 */
export function pageSpec(page, ctx = defaultContext()) {
  let s;
  if (page === 'display') s = ctx.kind === 'points' ? displayPoints(ctx) : ctx.kind === 'flow' ? displayFlow(ctx) : displayCube(ctx);
  else if (page === 'range') s = rangePage(ctx);
  else if (page === 'select') s = selectPage(ctx);
  else if (page === 'measure') s = measurePage(ctx);
  else if (page === 'more') s = morePage(ctx);
  else if (page === 'debug') s = ctx.debug ? debugPage(ctx) : morePage(ctx);
  else s = { rows: [], widgets: [], status: 0 };
  // Nothing loaded: the data pages are greyed whole, the layout as for a cube.
  if (ctx.kind === 'none' && page !== 'more' && page !== 'debug') {
    s.widgets = s.widgets.map((w) => ({ ...w, disabled: WHY.none, on: false }));
  }
  for (const w of s.widgets) {
    w.disabled = w.disabled || '';
    w.row = Math.max(0, Math.round((w.rect[1] - ROW_TOP) / ROW_STEP));
  }
  return s;
}

/** The widgets of a page, in page pixels. */
export function pageLayout(page, ctx) { return pageSpec(page, ctx).widgets; }

/** The title bar: dragged, it moves the menu; its × closes it (#276). */
export const CLOSE_W = 72;
function titleLayout() {
  return [
    { id: 'menu-drag', kind: 'drag', rect: [0, 0, MENU_PX[0] - CLOSE_W - 20, TITLE_H] },
    btn('menu-close', [MENU_PX[0] - CLOSE_W - 12, 8, CLOSE_W, TITLE_H - 16], '×'),
  ];
}

function tabLayout(page) {
  const gap = 8, x0 = 12, n = TABS.length;
  const w = (MENU_PX[0] - 2 * x0 - gap * (n - 1)) / n;
  return TABS.map(([id, label], i) => btn(`tab:${id}`, [x0 + i * (w + gap), TITLE_H + 8, w, TAB_H - 16], label,
    { group: 'tab', value: id, on: tabOf(page) === id }));
}

/** The footer: Undo and Reset view. Keep open went with the toggle of #276:
 *  the menu stays open until X or its × closes it. */
function footerLayout(ctx) {
  const n = 2, gap = 12, x0 = 12;
  const w = (MENU_PX[0] - 2 * x0 - gap * (n - 1)) / n;
  const r = (k) => [x0 + k * (w + gap), FOOTER_Y + 12, w, FOOTER_H - 24];
  return [
    btn('undo', r(0), 'Undo', { disabled: ctx.canUndo ? '' : 'Nothing to undo.', footer: true }),
    btn('reset-view', r(1), 'Reset view', { footer: true }),
  ];
}

/** The whole menu's widgets in menu pixels: the title bar, the tabs, the
 *  page (shifted under them) and the footer. */
export function menuLayout(page, ctx = defaultContext()) {
  const shift = (w) => ({ ...w, rect: [w.rect[0], w.rect[1] + PAGE_Y, w.rect[2], w.rect[3]], page: true });
  return [...titleLayout(), ...tabLayout(page), ...pageLayout(page, ctx).map(shift), ...footerLayout(ctx)];
}

/** The widget `id` of a layout, or null. */
export const widgetById = (layout, id) => layout.find((w) => w.id === id) || null;

/**
 * The presses that reach widget `id` from a menu just opened on `start`:
 * 1 when it is on that page, 2 when it is one tab away (the tabs are always
 * shown), Infinity when it is on no page or greyed there. `ctx` the context
 * at that moment. The pages a tab leads to are the tabs' own (the technical
 * page is one press further, under More).
 */
export function pressesTo(id, start, ctx) {
  const usable = (page) => {
    const w = widgetById(menuLayout(page, ctx), id);
    return !!w && !w.disabled && w.kind !== 'drag';
  };
  if (usable(start)) return 1;
  if (TABS.some(([t]) => usable(t))) return 2;
  if (ctx.debug && usable('debug')) return 3;
  return Infinity;
}

// ── Placement ───────────────────────────────────────────────────────────────

/** The menu centred on `p`, upright, its face (+z) turned to the eye. */
export function menuFacing(p, eye) {
  let z = M.sub(eye, p);
  if (Math.hypot(z[0], z[2]) < 1e-6) z = [0, 0, 1];
  z = M.normalize(z);
  const x = M.normalize(M.cross([0, 1, 0], z));
  const y = M.cross(z, x);
  return new Float32Array([x[0], x[1], x[2], 0, y[0], y[1], y[2], 0, z[0], z[1], z[2], 0, p[0], p[1], p[2], 1]);
}

/**
 * Where the menu opens, from the head's pose (`head`, column-major 4x4):
 * MENU_DIST in front of the eyes along the gaze's horizontal direction,
 * MENU_DROP below them, turned to them. Looking straight down or up, the
 * top of the head says which way is forward.
 */
export function menuFront(head, { dist = MENU_DIST, drop = MENU_DROP } = {}) {
  const eye = [head[12], head[13], head[14]];
  let f = [-head[8], -head[9], -head[10]];
  if (Math.hypot(f[0], f[2]) < 0.3) {
    const s = f[1] < 0 ? 1 : -1;               // down: the top of the head points forward
    f = [s * head[4], s * head[5], s * head[6]];
  }
  const n = Math.hypot(f[0], f[2]) || 1;
  const p = [eye[0] + (f[0] / n) * dist, eye[1] - drop, eye[2] + (f[2] / n) * dist];
  return menuFacing(p, eye);
}

/**
 * Where the menu opens from the hand that opened it (#276): just above and
 * beyond `hand` (its position), MENU_DIST from the eyes along the horizontal
 * direction from the eyes to the hand (at most MENU_TURN from the gaze; the
 * gaze's own when the hand is under the head, or behind or beside the user,
 * more than MENU_BEHIND from the gaze), its lower edge MENU_GAP above
 * the hand (its centre within MENU_RISE above and MENU_SINK below the eyes),
 * turned to the eyes. `head` the head's pose (column-major 4x4); without a
 * hand, menuFront.
 */
export function menuAbove(hand, head, { dist = MENU_DIST, size = MENU_SIZE } = {}) {
  if (!hand) return menuFront(head, { dist });
  const eye = [head[12], head[13], head[14]];
  let f = [-head[8], -head[9], -head[10]];
  if (Math.hypot(f[0], f[2]) < 0.3) {
    const s = f[1] < 0 ? 1 : -1;                // down: the top of the head points forward
    f = [s * head[4], s * head[5], s * head[6]];
  }
  const g = Math.atan2(f[0], -f[2]);            // the gaze's heading (0: towards -z)
  const dx = hand[0] - eye[0], dz = hand[2] - eye[2];
  let a = g;
  if (Math.hypot(dx, dz) >= 0.12) {
    let d = Math.atan2(dx, -dz) - g;
    d = Math.atan2(Math.sin(d), Math.cos(d));   // -π..π
    // A hand behind or beside the user says nothing of where to look: the gaze.
    if (Math.abs(d) <= MENU_BEHIND) a = g + Math.max(-MENU_TURN, Math.min(MENU_TURN, d));
  }
  const y = Math.max(eye[1] - MENU_SINK, Math.min(eye[1] + MENU_RISE, hand[1] + MENU_GAP + size[1] / 2));
  return menuFacing([eye[0] + Math.sin(a) * dist, y, eye[2] - Math.cos(a) * dist], eye);
}

/**
 * The menu moved by its title bar: `drag` {t, rel} recorded at the press (t
 * along the ray, rel from the hit point to the centre), the ray now and the
 * eye: the centre follows the ray at the same distance, turned to the eye.
 */
export function dragMenu(drag, ray, eye) {
  const p = M.add(M.add(ray.o, M.scale3(ray.d, drag.t)), drag.rel);
  return menuFacing(p, eye);
}

/**
 * The menu's button as a toggle (#276): a press (held now, not the frame
 * before) opens a closed menu and closes an open one; holding it changes
 * nothing. `st`: {open, wasHeld}. Returns {open, wasHeld, opened (the frame
 * it opens: its pose is taken then), closed}.
 */
export function menuToggle(st, held) {
  const press = !!held && !st.wasHeld;
  const open = press ? !st.open : !!st.open;
  return { open, wasHeld: !!held, opened: open && !st.open, closed: !open && !!st.open };
}

/**
 * A held signal steadied in time (#276): the palm turned up counts as held
 * once it has been up for `onMs`, and as let go once it has been down for
 * `offMs`, so tracking noise at the edge of the gesture neither opens and
 * closes the menu nor fires twice. A controller's button passes through
 * (onMs = offMs = 0). update(raw, tMs) -> held.
 */
export class Steady {
  constructor({ onMs = 0, offMs = 0 } = {}) { this.onMs = onMs; this.offMs = offMs; this.reset(); }
  /** The hand gone this frame (tracking lost): the gap counts as let go,
   *  so a blink shorter than offMs neither ends a held palm nor fires again. */
  missing(t) { return this.update(false, t); }
  update(raw, t) {
    if (raw) {
      // An up run goes on across dropouts shorter than offMs.
      if (this.upSince === null || t - this.lastUp > this.offMs) this.upSince = t;
      this.lastUp = t;
      if (!this.held && t - this.upSince >= this.onMs) this.held = true;
    } else if (this.upSince !== null && t - this.lastUp >= this.offMs) {
      this.held = false;
      this.upSince = null;
    }
    return this.held;
  }
  /** Taken over from a controller's button (`held` at time t): a later
   *  let-go of the palm still waits offMs from now. */
  reset(held = false, t = -Infinity) {
    this.held = !!held;
    this.upSince = held ? t : null;
    this.lastUp = held ? t : -Infinity;
  }
}

/**
 * The menu's held signal this frame (#276): a tracked hand's palm up through
 * `steady`; a controller's X as it is (steady follows it, so a switch to
 * hands carries no stale state); no secondary hand this frame (tracking
 * lost for a moment) counts as let go through `steady`, so a one-frame blink
 * neither toggles the menu nor wipes the palm's history.
 */
export function menuHeld(steady, hand, raw, t) {
  if (!hand) return steady.missing(t);
  if (hand.isHand) return steady.update(raw, t);
  steady.reset(raw, t);
  return !!raw;
}
/** The palm-up gesture's timing (a controller's X has none). */
export const PALM_STEADY = { onMs: 150, offMs: 250 };

// ── Drawing ─────────────────────────────────────────────────────────────────

const C = {
  bg: 'rgba(12, 16, 24, 0.94)', head: 'rgba(30, 38, 54, 0.96)', headHover: '#34415e', fg: '#e6e9ef', muted: '#9aa3b2',
  btn: '#1d2433', btnHover: '#2b3650', on: '#2f7fd8', onFg: '#ffffff', edge: '#4a5670', warn: '#ffd27a', err: '#ff8a8a',
  off: '#5b6475', offBg: '#161b26', why: '#c9a86a', track: '#3a4357', fill: '#4fa3ff', armed: '#d2731a', note: '#b9c2d0', tip: '#9fd0ff',
};
const FONT = '-apple-system, system-ui, sans-serif';
const TRACK_PAD = 18;
const clamp01 = (v) => Math.max(0, Math.min(1, Number.isFinite(v) ? v : 0));

function roundRect(c, x, y, w, h, r) {
  c.beginPath();
  c.moveTo(x + r, y);
  c.arcTo(x + w, y, x + w, y + h, r);
  c.arcTo(x + w, y + h, x, y + h, r);
  c.arcTo(x, y + h, x, y, r);
  c.arcTo(x, y, x + w, y, r);
  c.closePath();
}


function drawButton(c, w, hover) {
  const [x, y, ww, hh] = w.rect;
  const off = !!w.disabled;
  c.fillStyle = w.armed ? C.armed : w.on && !off ? C.on : off ? C.offBg : hover ? C.btnHover : C.btn;
  roundRect(c, x, y, ww, hh, 10);
  c.fill();
  if (hover && !off) { c.strokeStyle = C.edge; c.lineWidth = 2; c.stroke(); }
  // A tool's button carries the palette's icon on its left (#257).
  const is = w.icon ? hh - 16 : 0;
  if (is) drawIcon(c, w.icon, x + 8, y + 8, is, off ? C.off : w.on || w.armed ? C.onFg : C.fg);
  c.fillStyle = off ? C.off : w.on || w.armed ? C.onFg : C.fg;
  c.textAlign = 'center';
  c.textBaseline = 'middle';
  const close = w.label === '×';
  const r = fitLine(measureWith(c), close ? `bold 34px ${FONT}` : `26px ${FONT}`, w.label || '', ww - 16 - (is ? is + 6 : 0));
  c.font = r.font;
  c.fillText(r.text, x + (is ? is + 8 : 0) + (ww - (is ? is + 8 : 0)) / 2, y + hh / 2 + 1);
  c.textAlign = 'left';
}

function drawToggle(c, w, hover) {
  const [x, y, ww, hh] = w.rect;
  const on = !!w.on, off = !!w.disabled;
  c.fillStyle = off ? C.offBg : on ? C.on : hover ? C.btnHover : C.btn;
  roundRect(c, x, y, ww, hh, hh / 2);
  c.fill();
  c.beginPath();
  c.arc(on ? x + ww - hh / 2 : x + hh / 2, y + hh / 2, hh / 2 - 6, 0, 2 * Math.PI);
  c.fillStyle = off ? C.off : '#ffffff';
  c.fill();
  c.fillStyle = off ? C.off : on ? C.onFg : C.fg;
  c.font = `24px ${FONT}`;
  c.textAlign = 'center';
  c.textBaseline = 'middle';
  c.fillText(on ? 'on' : 'off', on ? x + (ww - hh) / 2 : x + hh + (ww - hh) / 2 - 6, y + hh / 2 + 1);
  c.textAlign = 'left';
}

function drawSwatch(c, w, hover) {
  const [x, y, ww, hh] = w.rect;
  const table = colormapTable(w.value);
  const g = typeof c.createLinearGradient === 'function' ? c.createLinearGradient(x, 0, x + ww, 0) : null;
  if (g && typeof g.addColorStop === 'function') {
    for (let k = 0; k <= 8; k++) {
      const i = Math.round((k / 8) * 255) * 4;
      g.addColorStop(k / 8, `rgb(${table[i]}, ${table[i + 1]}, ${table[i + 2]})`);
    }
    c.fillStyle = g;
  } else {
    c.fillStyle = `rgb(${table[512]}, ${table[513]}, ${table[514]})`;
  }
  roundRect(c, x + 4, y + 4, ww - 8, hh - 8, 8);
  c.fill();
  if (w.on || hover) {
    roundRect(c, x, y, ww, hh, 10);
    c.strokeStyle = w.on ? '#ffffff' : C.edge;
    c.lineWidth = w.on ? 5 : 2;
    c.stroke();
  }
  if (w.disabled) { c.fillStyle = 'rgba(12, 16, 24, 0.7)'; roundRect(c, x, y, ww, hh, 10); c.fill(); }
}

function drawTrack(c, w, value, hover, active) {
  const [x, y, ww, hh] = w.rect;
  const x0 = x + TRACK_PAD, len = ww - 2 * TRACK_PAD, cy = y + hh / 2;
  const off = !!w.disabled;
  c.fillStyle = (hover || active) && !off ? '#202838' : 'rgba(0,0,0,0)';
  roundRect(c, x, y, ww, hh, 10);
  c.fill();
  c.fillStyle = off ? C.offBg : C.track;
  roundRect(c, x0, cy - 5, len, 10, 5);
  c.fill();
  if (off) return;
  let a = 0, b = 0;
  if (w.kind === 'slider') b = clamp01(value);
  else { a = clamp01(value ? value[0] : 0); b = clamp01(value ? value[1] : 1); }
  c.fillStyle = C.fill;
  roundRect(c, x0 + a * len, cy - 5, Math.max(2, (b - a) * len), 10, 5);
  c.fill();
  const knob = (f) => { c.beginPath(); c.arc(x0 + f * len, cy, 16, 0, 2 * Math.PI); c.fillStyle = '#e6e9ef'; c.fill(); };
  if (w.kind === 'range') knob(a);
  knob(b);
}

/** One line in at most maxW: shrunk a little, cut last (text-fit.js). */
function text(c, s, x, y, maxW, { color = C.muted, font = `22px ${FONT}`, align = 'left' } = {}) {
  const r = fitLine(measureWith(c), font, String(s || ''), maxW);
  c.font = r.font;
  c.fillStyle = color;
  c.textBaseline = 'middle';
  c.textAlign = align;
  c.fillText(r.text, x, y);
  c.textAlign = 'left';
}

function drawPage(c, spec, view, ui) {
  const labelW = spec.wide ? RX - 300 - 30 : X0 - 30;
  const notes = view.notes || {};
  // The widget under the ray: its tooltip (or why it is greyed) is its row's line (#276).
  const pointed = spec.widgets.find((w) => w.id === ui.hover) || null;
  spec.rows.forEach(([key, label, icon], i) => {
    const y = rowY(i);
    // A measurement's row carries the palette's icon before its name (#257).
    const ix = icon && drawIcon(c, icon, 16, y + 10, WH - 20, C.fg) ? WH - 12 : 0;
    text(c, label, 20 + ix, y + WH / 2, labelW - ix, { color: C.fg, font: `28px ${FONT}` });
    const inRow = spec.widgets.filter((w) => w.row === i);
    const here = pointed && pointed.row === i ? pointed : null;
    const tip = here ? here.disabled || here.tip || '' : '';
    const why = tip && here.disabled ? tip : tip ? '' : (inRow.find((w) => w.disabled) || {}).disabled;
    const ph = spec.placeholder && spec.placeholder[key] && !inRow.length ? spec.placeholder[key] : '';
    // The widget pointed at's tooltip, else the reason a control is greyed,
    // else the row's value or note.
    const line = tip || why || ph || notes[key] || '';
    if (line) {
      const x = spec.wide ? 20 : X0;
      text(c, line, x, y + (ph ? WH / 2 : WH + 18), RIGHT - x, { color: why ? C.why : tip ? C.tip : ph ? C.muted : C.note });
    }
  });
  for (const w of spec.widgets) {
    const hover = ui.hover === w.id;
    if (w.kind === 'slider' || w.kind === 'range') drawTrack(c, w, (view.values || {})[w.id], hover, ui.active === w.id);
    else if (w.kind === 'toggle') drawToggle(c, w, hover);
    else if (w.kind === 'swatch') drawSwatch(c, w, hover);
    else drawButton(c, w, hover);
  }
  // The status lines (work under way, results, errors) under the rows.
  const top = rowY(spec.status);
  const room = Math.max(0, Math.floor((PAGE_H - top - 8) / 30));
  const font = `22px ${FONT}`;
  const measureIn = measureWith(c);
  const out = [];
  for (const l of view.status || []) {
    const [s, color] = Array.isArray(l) ? l : [l, C.warn];
    for (const piece of wrapFit(measureIn, font, s, RIGHT - 40, Infinity, [1]).lines) out.push([piece, color]);
  }
  if (out.length > room && room > 0) out[room - 1] = [`${out[room - 1][0]}…`, out[room - 1][1]];
  out.slice(0, room).forEach(([s, color], k) => text(c, s, 20, top + 16 + k * 30, RIGHT - 40, { color, font }));
}

/**
 * Draws the menu: title bar, tabs, `page` (its spec from pageSpec) and the
 * footer. view: {title, notes: {rowKey: text}, values: {sliderId: 0..1,
 * rangeId: [lo, hi]}, status: [text | [text, colour]]}; ui: {hover, active}.
 */
export function drawMenu(c, page, spec, view = {}, ui = {}, ctx = defaultContext()) {
  const [W, H] = MENU_PX;
  c.clearRect(0, 0, W, H);
  c.fillStyle = C.bg;
  roundRect(c, 0, 0, W, H, 18);
  c.fill();
  c.fillStyle = ui.hover === 'menu-drag' || ui.active === 'menu-drag' ? C.headHover : C.head;
  roundRect(c, 0, 0, W, TITLE_H + TAB_H, 18);
  c.fill();
  // The title, the drag hint right-aligned before the × (the title keeps its room first).
  const hint = '⠿ drag to move';
  const hintRight = W - CLOSE_W - 32;
  c.font = `22px ${FONT}`;
  const hintW = c.measureText(hint).width;
  const titleW = hintRight - hintW - 24 - 20;
  text(c, view.title || 'VisIVO VR', 20, TITLE_H / 2 + 2, titleW, { color: C.fg, font: `bold 28px ${FONT}` });
  text(c, hint, hintRight, TITLE_H / 2 + 2, hintW + 1, { align: 'right' });
  for (const w of titleLayout()) if (w.kind === 'button') drawButton(c, w, ui.hover === w.id);
  for (const w of tabLayout(page)) drawButton(c, w, ui.hover === w.id);
  c.save();
  c.translate(0, PAGE_Y);
  drawPage(c, spec, view, ui);
  c.restore();
  c.fillStyle = C.head;
  roundRect(c, 0, FOOTER_Y, W, FOOTER_H, 18);
  c.fill();
  for (const w of footerLayout(ctx)) drawButton(c, w, ui.hover === w.id);
}

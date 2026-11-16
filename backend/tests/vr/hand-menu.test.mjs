// The hand menu (issues #201, #257) and the result windows: the layout of
// every tab for every data kind, the greying with its reason, the hit tests,
// each main task at most two presses from opening, the menu standing about a
// metre away and moved by its title bar, X toggling it (#276); the windows'
// bars and which surface takes the ray (node --test).
import test from 'node:test';
import assert from 'node:assert/strict';
import * as HM from '../../app/static/vr/js/hand-menu.js';
import * as W from '../../app/static/vr/js/windows.js';
import * as P from '../../app/static/vr/js/vr-panel.js';
import * as M from '../../app/static/vr/js/mat4.js';
import { COLORMAP_NAMES } from '../../app/static/vr/js/colormaps.js';

const close = (a, b, eps = 1e-9) => assert.ok(Math.abs(a - b) <= eps, `${a} != ${b}`);

/** Contexts as main.js builds them: an HI cube (HCG 44) with or without a
 *  lasso selection, a simulation snapshot (point table), a velocity field,
 *  nothing loaded; with and without ?debug. */
function ctxOf(kind, extra = {}) {
  const c = HM.defaultContext();
  c.kind = kind;
  if (kind === 'points') {
    c.pts = { ...c.pts, fields: ['VX', 'VY', 'VZ', 'Mass', 'Rho', 'Temperature', 'Metals'], field: 'Rho', budget: '1.0 M' };
  }
  if (extra.sel) c.sel = { ...c.sel, ...extra.sel };
  if (extra.meas) c.meas = { ...c.meas, ...extra.meas };
  for (const k of ['debug', 'cat', 'canUndo', 'clipTool', 'simulation']) if (k in extra) c[k] = extra[k];
  if (extra.lv) c.lv = { ...c.lv, ...extra.lv };
  return c;
}
const CONTEXTS = {
  cube: ctxOf('cube'),
  cubeLasso: ctxOf('cube', { sel: { lasso: true, tool: 'lasso' }, meas: { lasso: true } }),
  cubeBox: ctxOf('cube', { sel: { box: true, tool: 'box' } }),
  cubeApplied: ctxOf('cube', { sel: { lasso: true, applied: true }, meas: { lasso: true, spectrum: true }, cat: { loaded: true, show: true, labels: true, pins: false } }),
  points: ctxOf('points'),
  flow: ctxOf('flow'),
  none: ctxOf('none'),
  debugCube: ctxOf('cube', { debug: true, canUndo: true, keep: true }),
  // A simulation volume (issue #317): a cube without a spectral axis.
  simulation: ctxOf('cube', { simulation: true, clipTool: 'box', sel: { tool: 'box' }, lv: { hist: true } }),
};

test('a simulation volume: no spectral tools, percentiles for σ, z cropped as x and y (#317)', () => {
  const sim = CONTEXTS.simulation;
  const why = (page, id) => HM.widgetById(HM.menuLayout(page, sim), id).disabled;
  for (const id of ['tool:lasso', 'tool:probe', 'seed:new', 'seed:add', 'seed:carve']) assert.equal(why('select', id), HM.WHY.simulation, id);
  assert.equal(why('select', 'tool:box'), '');
  for (const id of ['moment:0', 'moment:1', 'moment:2', 'pv-draw', 'pv-wide', 'spec-pin']) assert.equal(why('measure', id), HM.WHY.simulation, id);
  assert.equal(why('range', 'slab'), HM.WHY.simulation);
  assert.equal(why('range', 'sub:clip:slab'), HM.WHY.simulation);
  assert.equal(why('range', 'crop:2'), '', 'z is cropped as x and y');
  assert.ok(HM.widgetById(HM.menuLayout('range', sim), 'sub:clip:box').on);
  // Display: the 90 % and 99 % percentiles where a cube has 3σ and 5σ, at the same places.
  const d = HM.menuLayout('display', sim), c = HM.menuLayout('display', CONTEXTS.cube);
  assert.ok(!d.some((w) => w.group === 'sig'));
  assert.deepEqual(HM.widgetById(d, 'pct:90').rect, HM.widgetById(c, 'sig:3').rect);
  assert.deepEqual(HM.widgetById(d, 'pct:99').rect, HM.widgetById(c, 'sig:5').rect);
  assert.equal(HM.widgetById(d, 'pct:90').disabled, '');
  assert.equal(HM.widgetById(d, 'ct-pins').disabled, HM.WHY.simulation);
  // Levels: σ greyed with its reason, the percentiles there.
  assert.equal(HM.widgetById(HM.menuLayout('levels', sim), 'sig:3').disabled, HM.WHY.simSigma);
  assert.equal(HM.widgetById(HM.menuLayout('levels', sim), 'pct:99').disabled, '');
  // Nothing on any page speaks of channels, velocities or σ as an action left open.
  for (const page of HM.PAGES) {
    for (const w of HM.menuLayout(page, sim)) {
      if (w.disabled) continue;
      assert.ok(!/σ|channel|velocity|spectrum|Kinematic/i.test(`${w.label || ''} ${w.tip || ''}`), `${page} ${w.id}: ${w.label} / ${w.tip}`);
    }
  }
});

test('the tabs follow the work, the same on every dataset; the technical page only with ?debug', () => {
  assert.deepEqual(HM.TABS.map((t) => t[1]), ['Display', 'Range', 'Select', 'Measure', 'More']);
  for (const ctx of Object.values(CONTEXTS)) {
    const tabs = HM.menuLayout('display', ctx).filter((w) => w.group === 'tab').map((w) => w.id);
    assert.deepEqual(tabs, ['tab:display', 'tab:range', 'tab:select', 'tab:measure', 'tab:more']);
  }
  // No technical setting is met without ?debug: neither on More nor on any page.
  const technical = ['set:governor', 'set:refine', 'set:refineDepth', 'set:refineScale', 'set:record', 'set:measureRun',
    'thrsrc:reduced', 'thrsrc:cube', 'debug-page'];
  for (const [name, ctx] of Object.entries(CONTEXTS)) {
    if (ctx.debug) continue;
    for (const page of HM.PAGES) {
      const ids = HM.menuLayout(page, ctx).map((w) => w.id);
      for (const id of technical) assert.ok(!ids.includes(id), `${name} ${page} shows ${id}`);
    }
  }
  const dbg = CONTEXTS.debugCube;
  assert.ok(HM.menuLayout('more', dbg).some((w) => w.id === 'debug-page'));
  const page = HM.menuLayout('debug', dbg).map((w) => w.id);
  for (const id of technical.filter((x) => x !== 'debug-page')) assert.ok(page.includes(id), id);
  // The tab lit for the technical page is More; the Help tab and the Mode page are gone.
  assert.ok(HM.menuLayout('debug', dbg).find((w) => w.id === 'tab:more').on);
  assert.ok(!HM.PAGES.includes('help') && !HM.PAGES.includes('mode'));
});

test('every page of every kind fits, its widgets do not overlap and each is hit at its centre', () => {
  for (const [name, ctx] of Object.entries(CONTEXTS)) {
    for (const page of HM.PAGES) {
      const L = HM.menuLayout(page, ctx);
      for (const w of L) {
        const [x, y, ww, hh] = w.rect;
        assert.ok(x >= 0 && y >= 0 && x + ww <= HM.MENU_PX[0] && y + hh <= HM.MENU_PX[1], `${name} ${page} ${w.id} inside`);
        if (w.page) assert.ok(y >= HM.PAGE_Y && y + hh <= HM.FOOTER_Y, `${name} ${page} ${w.id} within the page`);
        // At least 2.5 cm high and 4 cm wide at the menu's scale (about 1.5° by 2.4° at 0.95 m).
        if (w.kind !== 'drag') {
          assert.ok(hh / HM.MENU_PPM >= 0.025, `${name} ${page} ${w.id} high enough`);
          assert.ok(ww / HM.MENU_PPM >= 0.04, `${name} ${page} ${w.id} wide enough`);
        }
        assert.equal(P.hitWidget(L, x + ww / 2, y + hh / 2).id, w.id, `${name} ${page} ${w.id} hit`);
        // A press at its centre is the widget's own action, through the pointer the menu uses.
        const acts = new P.PanelPointer(L).update({ x: x + ww / 2, y: y + hh / 2 }, true);
        const want = { button: 'press', toggle: 'press', swatch: 'press', slider: 'slide', range: 'range', drag: 'drag-start' }[w.kind];
        assert.equal(acts.length === 1 && acts[0].type, want, `${name} ${page} ${w.id} (${w.kind}) pressed`);
      }
      const ids = L.map((w) => w.id);
      assert.equal(new Set(ids).size, ids.length, `${name} ${page}: unique ids`);
      // The footer and the title bar (its × closes the menu, #276): always there; Keep open is gone.
      for (const id of ['undo', 'reset-view', 'menu-close', 'menu-drag']) assert.ok(ids.includes(id), `${name} ${page} ${id}`);
      assert.ok(!ids.includes('menu-pin'), `${name} ${page}: no Keep open`);
    }
  }
});

test('a control that does not apply is greyed with a one-line reason, and the layout does not move', () => {
  // The same Range, Select (without a selection) and Measure layout for every kind: greyed, not hidden.
  for (const page of ['range', 'select', 'measure']) {
    const rects = (ctx) => HM.menuLayout(page, ctx).map((w) => `${w.id}@${w.rect.join(',')}`);
    for (const k of ['points', 'flow', 'none']) assert.deepEqual(rects(CONTEXTS[k]), rects(CONTEXTS.cube), `${page} ${k}`);
  }
  const why = (page, ctx, id) => HM.widgetById(HM.menuLayout(page, ctx), id).disabled;
  assert.equal(why('range', CONTEXTS.points, 'slab'), HM.WHY.noSpectral);
  assert.equal(why('range', CONTEXTS.points, 'crop:2'), '', 'particles: crop z');
  assert.equal(why('range', CONTEXTS.cube, 'crop:2'), HM.WHY.cubeZ, 'a cube: z is the velocity range');
  assert.equal(why('range', CONTEXTS.cube, 'crop:0'), '');
  assert.equal(why('select', CONTEXTS.flow, 'tool:lasso'), HM.WHY.particlesSelect);
  assert.equal(why('measure', CONTEXTS.points, 'moment:0'), HM.WHY.measureCube);
  assert.equal(why('measure', CONTEXTS.cube, 'moment:0'), '', 'a moment needs no selection (whole field)');
  assert.equal(why('measure', CONTEXTS.cube, 'spec-pin'), HM.WHY.spectrumFirst);
  assert.equal(why('display', CONTEXTS.cube, 'ct-show'), HM.WHY.noCatalogue);
  assert.equal(why('display', CONTEXTS.cubeApplied, 'ct-show'), '');
  assert.equal(why('display', CONTEXTS.none, 'thr'), HM.WHY.none);
  assert.equal(why('display', CONTEXTS.points, 'pt-intensity'), HM.WHY.splatsOnly);
  for (const style of ['splats', 'raytraced']) {
    const ctx = { ...CONTEXTS.points, pts: { ...CONTEXTS.points.pts, style } };
    assert.equal(why('display', ctx, 'pt-intensity'), '', style);
  }
  // Every reason is one line: short enough for the row (about 760 px of 22 px text).
  for (const r of Object.values(HM.WHY)) assert.ok(r.length <= 66, r);
});

test('the selection actions show only while there is a selection', () => {
  const ids = (ctx) => HM.menuLayout('select', ctx).map((w) => w.id);
  for (const id of ['isolate', 'remove', 'show-applied']) {
    assert.ok(!ids(CONTEXTS.cube).includes(id), `${id} hidden without a selection`);
    assert.ok(ids(CONTEXTS.cubeLasso).includes(id), `${id} with a lasso`);
  }
  const L = HM.menuLayout('select', CONTEXTS.cubeLasso);
  assert.equal(HM.widgetById(L, 'isolate').disabled, '');
  assert.equal(HM.widgetById(L, 'show-applied').disabled, HM.WHY.applyFirst);
  assert.equal(HM.widgetById(HM.menuLayout('select', CONTEXTS.cubeApplied), 'show-applied').disabled, '');
  assert.equal(HM.widgetById(HM.menuLayout('select', CONTEXTS.cubeBox), 'isolate').disabled, HM.WHY.boxApply);
  // Hidden actions leave their row with a line saying when they come.
  assert.ok(HM.pageSpec('select', CONTEXTS.cube).placeholder.result);
});

test('each main task is at most two presses from opening the menu (#257 Done list)', () => {
  // [task, context, widget]: the press that does it (a slider or range is
  // one press: the drag starts with it). The menu may open on any page.
  const cube = CONTEXTS.cube, lasso = CONTEXTS.cubeLasso, pts = CONTEXTS.points;
  const tasks = [
    ['show faint emission: the colour map', cube, `cmap:${COLORMAP_NAMES[1]}`],
    ['show faint emission: the threshold', cube, 'thr'],
    ['lasso a galaxy: the Kinematic Lasso', cube, 'tool:lasso'],
    ['lasso a galaxy: a new lasso', cube, 'seed:new'],
    ['... and Isolate it', lasso, 'isolate'],
    ['Moment of the selection', lasso, 'moment:0'],
    ['PV slice', cube, 'pv-draw'],
    ['clip to a velocity range', cube, 'slab'],
    ['simulation: colour by velocity', pts, 'pt-field:VX'],
    ['simulation: crop', pts, 'crop:0'],
  ];
  for (const [task, ctx, id] of tasks) {
    for (const start of HM.PAGES.filter((p) => p !== 'debug')) {
      const n = HM.pressesTo(id, start, ctx);
      assert.ok(n <= 2, `${task}: ${n} presses from ${start}`);
    }
  }
  // The rule itself: on the page shown, one press; a tab away, two; greyed or nowhere, never.
  assert.equal(HM.pressesTo('thr', 'display', cube), 1);
  assert.equal(HM.pressesTo('thr', 'more', cube), 2);
  assert.equal(HM.pressesTo('isolate', 'select', cube), Infinity, 'no selection, no Isolate');
  assert.equal(HM.pressesTo('moment:0', 'display', pts), Infinity, 'greyed on particles');
  assert.equal(HM.pressesTo('set:refine', 'more', CONTEXTS.debugCube), 3, 'technical: one press further, ?debug only');
  assert.equal(HM.pressesTo('set:refine', 'more', cube), Infinity);
});

test('lasso a galaxy and Isolate it: two presses in all, the trigger on the galaxy between them', () => {
  // A fresh start: the menu opens on Display; the Kinematic Lasso is the default Select tool.
  let ctx = ctxOf('cube');
  assert.equal(ctx.sel.tool, 'lasso');
  let page = HM.pageOnOpen('display', 'explore');
  assert.equal(page, 'display');
  let presses = 0;
  // Press 1: the Select tab, which sets Select mode with the lasso as the trigger's tool.
  assert.ok(HM.widgetById(HM.menuLayout(page, ctx), 'tab:select'));
  presses++;
  page = 'select';
  const mode = HM.TAB_MODE[page];
  assert.equal(mode, 'select');
  assert.equal(HM.widgetById(HM.menuLayout(page, ctx), 'tool:lasso').on, true, 'the trigger seeds a lasso');
  assert.ok(!HM.widgetById(HM.menuLayout(page, ctx), 'isolate'), 'nothing selected yet');
  // The trigger on the galaxy grows the selection (the menu kept open, or opened
  // again: it then opens on the Select tab, the mode's).
  ctx = ctxOf('cube', { sel: { lasso: true, tool: 'lasso' }, meas: { lasso: true } });
  assert.equal(HM.pageOnOpen(page, mode), 'select');
  // Press 2: Isolate, there now.
  const iso = HM.widgetById(HM.menuLayout(page, ctx), 'isolate');
  assert.ok(iso && !iso.disabled);
  presses++;
  assert.equal(presses, 2);
  // The Moment of it next: Measure → Intensity (M0), of the lasso object.
  assert.equal(HM.pressesTo('moment:0', 'select', ctx), 2);
  // With another tool kept from an earlier session the lasso is one press more (Select → Kinematic Lasso).
  const probe = ctxOf('cube', { sel: { tool: 'probe' } });
  assert.equal(HM.pressesTo('tool:lasso', 'display', probe), 2);
});

test('choosing a tab sets the mode; the menu opens on the tab of the mode', () => {
  assert.deepEqual(HM.TAB_MODE, { display: 'explore', range: 'clip', select: 'select', measure: 'explore', more: null, levels: 'explore', data: 'explore', overlay: 'explore', iso: 'explore', debug: null });
  assert.equal(HM.pageOnOpen('display', 'select'), 'select', 'A changed the mode: its tab');
  assert.equal(HM.pageOnOpen('measure', 'explore'), 'measure', 'Measure keeps the explore mode');
  assert.equal(HM.pageOnOpen('more', 'clip'), 'more', 'More sets no mode');
  assert.equal(HM.pageOnOpen('debug', 'select'), 'debug');
  assert.equal(HM.pageOnOpen('help', 'clip'), 'range', 'a page that is gone (saved before #257)');
});

test('a point table: column chips (paged), and a velocity field\'s controls', () => {
  const ch = HM.fieldChips(['VX', 'VY', 'VZ', 'Mass', 'Rho', 'Temperature', 'Metals'], 0);
  assert.deepEqual(ch.chips, [null, 'VX', 'VY', 'VZ']);
  assert.ok(ch.more);
  assert.deepEqual(HM.fieldChips(['VX', 'VY', 'VZ', 'Mass', 'Rho', 'Temperature', 'Metals'], 1).chips, ['Mass', 'Rho', 'Temperature', 'Metals']);
  assert.deepEqual(HM.fieldChips(['VX', 'VY', 'VZ', 'Mass', 'Rho', 'Temperature', 'Metals'], 2).chips, [null, 'VX', 'VY', 'VZ'], 'round');
  assert.deepEqual(HM.fieldChips(['Mass', 'Rho'], 0), { chips: [null, 'Mass', 'Rho'], more: false, pages: 1 });
  // The velocity columns come first whatever the table's order, so colour by
  // velocity stays two presses away (the menu opens on the first page).
  const late = ['Mass', 'Rho', 'Temperature', 'Metals', 'VX', 'VY', 'VZ'];
  assert.deepEqual(HM.fieldChips(late, 0).chips, [null, 'VX', 'VY', 'VZ']);
  assert.deepEqual(HM.fieldChips(late, 1).chips, ['Mass', 'Rho', 'Temperature', 'Metals']);
  const ctx = { ...CONTEXTS.points, pts: { ...CONTEXTS.points.pts, fields: late, fieldPage: 0 } };
  for (const start of ['range', 'more', 'select']) assert.equal(HM.pressesTo('pt-field:VX', start, ctx), 2);
  for (const f of ['VX', 'vy', 'v_z', 'vel', 'Velocity', 'speed', '|v|', 'V']) assert.ok(HM.isVelocityField(f), f);
  for (const f of ['Vmax', 'Mvir', 'Rho', 'vol', 'X']) assert.ok(!HM.isVelocityField(f), f);
  const p = HM.menuLayout('display', CONTEXTS.points).map((w) => w.id);
  for (const id of ['pt-field:', 'pt-field:VX', 'pt-field-more', 'pt-log', 'pt-range', 'pt-style:points', 'pt-style:splats', 'pt-style:raytraced',
    'pt-smaller', 'pt-larger', 'pt-intensity', 'pt-budget', 'pt-reload', 'pt-box', `cmap:${COLORMAP_NAMES[0]}`]) assert.ok(p.includes(id), id);
  const f = HM.menuLayout('display', CONTEXTS.flow).map((w) => w.id);
  for (const id of ['fl-stream', 'fl-arrows', 'fl-shorter', 'fl-longer', 'fl-fewer', 'fl-more', 'fl-retrace', 'fl-range', 'fl-box']) assert.ok(f.includes(id), id);
  const c = HM.menuLayout('display', CONTEXTS.cube).map((w) => w.id);
  for (const id of ['thr', 'thr-reset', 'cmax', 'opacity', 'render:0', 'render:1', 'ct-show', 'ct-labels', 'ct-pins']) assert.ok(c.includes(id), id);
  assert.equal(c.filter((id) => id.startsWith('cmap:')).length, COLORMAP_NAMES.length, 'the colour maps as swatches');
});

test('the renames: the desktop\'s words on the menu', () => {
  // The desktop's words, on the button or, where a short label fits better
  // (#276), at the start of its tooltip.
  const label = (page, ctx, id) => HM.widgetById(HM.menuLayout(page, ctx), id).label;
  const tip = (page, ctx, id) => String(HM.widgetById(HM.menuLayout(page, ctx), id).tip).split(':')[0];
  const cube = CONTEXTS.cubeLasso;
  assert.equal(label('range', cube, 'slab-all'), 'Full band');
  assert.equal(label('range', cube, 'sub:clip:box'), 'Spatial crop');
  assert.equal(label('range', cube, 'sub:clip:slab'), 'Velocity range');
  assert.equal(label('select', cube, 'tool:probe'), 'Spectrum probe');
  assert.equal(label('select', CONTEXTS.points, 'tool:probe'), 'Probe');
  assert.equal(label('select', cube, 'show-applied'), 'Show result cube');
  assert.deepEqual(['seed:new', 'seed:add', 'seed:carve'].map((id) => label('select', cube, id)), ['New', 'Add', 'Subtract']);
  assert.deepEqual(['render:0', 'render:1'].map((id) => label('display', cube, id)), ['Emission', 'MIP']);
  assert.deepEqual(['pv-narrow', 'pv-wide'].map((id) => label('measure', cube, id)), ['PV width −', 'PV width +']);
  assert.deepEqual([0, 1, 2].map((o) => tip('measure', cube, `moment:${o}`)), ['Intensity (M0)', 'Velocity (M1)', 'Dispersion (M2)']);
  assert.deepEqual([0, 1, 2].map((o) => label('measure', cube, `moment:${o}`)), ['M0', 'M1', 'M2'], 'short labels');
  assert.equal(tip('display', CONTEXTS.points, 'pt-style:splats'), 'Smooth (splats)');
  assert.equal(label('display', CONTEXTS.points, 'pt-style:splats'), 'Splats');
  assert.equal(label('display', CONTEXTS.points, 'pt-style:raytraced'), 'Ray-traced');
  assert.equal(label('display', CONTEXTS.flow, 'fl-retrace'), 'Reseed streamlines');
  const rows = (page, ctx) => HM.pageSpec(page, ctx).rows.map((r) => r[1]);
  assert.ok(rows('measure', cube).includes('PV slice'));
  assert.ok(rows('display', CONTEXTS.points).includes('Particles shown'));
  assert.deepEqual(rows('more', cube), ['Handedness', 'Button labels', 'Haptics', 'Wrist readout', 'Guide']);
  const spectral = { ...CONTEXTS.cube, spectralName: 'Spectral range' };
  assert.equal(HM.pageSpec('range', spectral).rows[0][1], 'Spectral range', 'without a velocity axis');
});

test('the menu opens about a metre in front, a little low, turned to the eyes, at today\'s angular size', () => {
  const eye = [0.2, 1.6, 0.1];
  for (const yaw of [0, 0.7, -2.4]) {
    // A head turned by yaw, pitched down 20° (looking at the cube).
    const head = M.multiply(M.translation(...eye), M.multiply(M.rotation([0, 1, 0], yaw), M.rotation([1, 0, 0], -0.35)));
    const m = HM.menuFront(head);
    const p = M.position(m);
    const d = M.sub(p, eye);
    close(Math.hypot(d[0], d[2]), HM.MENU_DIST, 1e-5);
    close(d[1], -HM.MENU_DROP, 1e-5);
    const fwd = [-Math.sin(yaw), 0, -Math.cos(yaw)];
    assert.ok(M.dot(M.normalize([d[0], 0, d[2]]), fwd) > 0.9999, 'along the gaze\'s horizontal direction');
    const z = M.normalize(M.sub(M.transformPoint(m, [0, 0, 1]), p));
    assert.ok(M.dot(z, M.normalize(M.sub(eye, p))) > 0.9999, 'faces the eye');
    close(M.sub(M.transformPoint(m, [1, 0, 0]), p)[1], 0, 1e-6);   // upright: no roll
  }
  // Looking straight down: the top of the head says where forward is.
  const down = M.multiply(M.translation(...eye), M.rotation([1, 0, 0], -Math.PI / 2));
  const pd = M.position(HM.menuFront(down));
  assert.ok(pd[2] < eye[2] - 0.9, JSON.stringify(pd));
  // The same angle as the menu by the wrist before #257 (2800 px/m at 0.45 m): 22 px text about 1°.
  close(HM.MENU_PPM * HM.MENU_DIST, 2800 * 0.45, 1e-9);
  const deg = (22 / HM.MENU_PPM / HM.MENU_DIST) * 180 / Math.PI;
  assert.ok(deg > 0.9 && deg < 1.2, deg);
});

test('the title bar moves the menu: it follows the ray at the same distance, still facing the eye', () => {
  const eye = [0, 1.6, 0];
  const m0 = HM.menuFacing([0, 1.48, -0.95], eye);
  const L = HM.menuLayout('display', CONTEXTS.cube);
  const bar = HM.widgetById(L, 'menu-drag');
  const at = (x, y) => M.transformPoint(m0, [(x / HM.MENU_PX[0] - 0.5) * HM.MENU_SIZE[0], (0.5 - y / HM.MENU_PX[1]) * HM.MENU_SIZE[1], 0]);
  const target = at(300, bar.rect[1] + bar.rect[3] / 2);
  const ray = { o: [0.1, 1.2, -0.2], d: M.sub(target, [0.1, 1.2, -0.2]) };
  const hit = P.rayPanel(m0, HM.MENU_SIZE, ray.o, ray.d, HM.MENU_PX);
  const ptr = new P.PanelPointer(L);
  const [start] = ptr.update(hit, true);
  assert.equal(start.type, 'drag-start');
  const drag = { t: hit.t, rel: M.sub(M.position(m0), hit.point) };
  // The same ray: the menu stays put.
  const same = HM.dragMenu(drag, ray, eye);
  for (let i = 0; i < 16; i++) close(same[i], m0[i], 1e-5);
  // The ray turned to the right: the menu goes right and turns to the eye.
  const turned = { o: ray.o, d: M.transformPoint(M.rotation([0, 1, 0], -0.3), ray.d) };
  const m1 = HM.dragMenu(drag, turned, eye);
  const p1 = M.position(m1);
  assert.ok(p1[0] > 0.2, JSON.stringify(p1));
  const z = M.normalize(M.sub(M.transformPoint(m1, [0, 0, 1]), p1));
  assert.ok(M.dot(z, M.normalize(M.sub(eye, p1))) > 0.9999);
  assert.deepEqual(ptr.update(null, true).map((a) => a.type), ['drag']);
  assert.deepEqual(ptr.update(null, false).map((a) => a.type), ['drag-end']);
});

test('a ray from the eye to a tab presses it, through the same ray code as the windows', () => {
  const eye = [0, 1.6, 0];
  const head = M.translation(...eye);
  const m = HM.menuFront(head);
  const L = HM.menuLayout('display', CONTEXTS.cube);
  for (const id of ['tab:measure', 'undo', 'thr-reset']) {
    const w = HM.widgetById(L, id);
    const [cx, cy] = [w.rect[0] + w.rect[2] / 2, w.rect[1] + w.rect[3] / 2];
    const target = M.transformPoint(m, [(cx / HM.MENU_PX[0] - 0.5) * HM.MENU_SIZE[0], (0.5 - cy / HM.MENU_PX[1]) * HM.MENU_SIZE[1], 0]);
    const hit = P.rayPanel(m, HM.MENU_SIZE, eye, M.sub(target, eye), HM.MENU_PX);
    assert.deepEqual(new P.PanelPointer(L).update(hit, true).map((a) => a.id), [id]);
  }
});

test('the menu: X toggles it (#276): a press opens, the next closes; holding changes nothing; the frames it opens and closes are told', () => {
  let st = { wasHeld: false, open: false };
  const log = [];
  const step = (held) => { st = HM.menuToggle(st, held); log.push([st.opened, st.closed]); return st.open; };
  assert.deepEqual([false, true, true, true, false, false].map(step), [false, true, true, true, true, true], 'open from the press on, held or let go');
  assert.deepEqual(log[1], [true, false], 'opened on the press\'s frame');
  assert.ok(log.slice(2).every(([o, c]) => !o && !c), 'and only then');
  assert.deepEqual([true, true, false].map(step), [false, false, false], 'the next press closes it, holding does not open it again');
  assert.deepEqual(log[6], [false, true], 'closed on that press\'s frame');
  assert.equal(step(true), true, 'and the one after opens it again');
  // Closed by its × (main.js sets open = false): the next press opens it.
  st = { ...st, open: false };
  assert.equal(step(false), false);
  assert.equal(step(true), true);
});

test('the palm up toggles the same way, steadied: noise at the edge of the gesture neither flickers nor fires twice', () => {
  const s = new HM.Steady(HM.PALM_STEADY);
  let st = { wasHeld: false, open: false };
  let toggles = 0;
  const feed = (raw, t) => { const r = HM.menuToggle(st, s.update(raw, t)); if (r.opened || r.closed) toggles++; st = r; };
  // Up for 100 ms only, then down: not a press.
  [0, 30, 60, 90].forEach((t) => feed(true, t));
  for (let t = 100; t <= 450; t += 50) feed(false, t);
  assert.equal(toggles, 0, 'a palm up shorter than onMs is not a press');
  // Up and held, with tracking dropouts of 1-2 frames (under offMs): one press.
  let t = 500;
  for (let i = 0; i < 60; i++, t += 11) feed(!(i % 9 === 4 || i % 9 === 5), t);
  assert.equal(toggles, 1, 'one press for a palm held up, dropouts and all');
  assert.equal(st.open, true);
  // Down long enough, then up again: the second press closes it.
  for (let i = 0; i < 30; i++, t += 11) feed(false, t);
  for (let i = 0; i < 30; i++, t += 11) feed(true, t);
  assert.equal(toggles, 2);
  assert.equal(st.open, false);
  // A controller's X passes through.
  const x = new HM.Steady();
  assert.equal(x.update(true, 0), true);
  assert.equal(x.update(false, 0), false);
});

test('the menu\'s held signal: a lost hand for a frame neither toggles nor wipes the palm\'s history (#276 review)', () => {
  const steady = new HM.Steady(HM.PALM_STEADY);
  const hand = { isHand: true }, ctl = { isHand: false };
  let st = { wasHeld: false, open: false }, toggles = 0;
  const feed = (h, raw, t) => { const r = HM.menuToggle(st, HM.menuHeld(steady, h, raw, t)); if (r.opened || r.closed) toggles++; st = r; };
  let t = 0;
  for (let i = 0; i < 30; i++, t += 11) feed(hand, true, t);           // palm up: opens
  assert.equal(toggles, 1);
  // Tracking lost for one and for three frames (Sh undefined), the palm still up.
  feed(undefined, false, t); t += 11;
  for (let i = 0; i < 10; i++, t += 11) feed(hand, true, t);
  for (let i = 0; i < 3; i++, t += 11) feed(undefined, false, t);
  for (let i = 0; i < 10; i++, t += 11) feed(hand, true, t);
  assert.equal(toggles, 1, 'blinks shorter than offMs: no toggle');
  assert.equal(st.open, true);
  // Lost for longer than offMs: let go; the palm up again then closes it.
  for (let i = 0; i < 30; i++, t += 11) feed(undefined, false, t);
  for (let i = 0; i < 30; i++, t += 11) feed(hand, true, t);
  assert.equal(toggles, 2);
  // A controller: X passes through, and a held X taken over by hands is let go offMs later, not never.
  const s2 = new HM.Steady(HM.PALM_STEADY);
  assert.equal(HM.menuHeld(s2, ctl, true, 0), true);
  assert.equal(HM.menuHeld(s2, ctl, false, 10), false);
  assert.equal(HM.menuHeld(s2, ctl, true, 20), true);
  assert.equal(HM.menuHeld(s2, undefined, false, 100), true, 'within offMs of the last press');
  assert.equal(HM.menuHeld(s2, undefined, false, 300), false, 'then let go');
});

test('the menu opens just above and beyond the hand that opened it, at the same distance, turned to the eyes (#276)', () => {
  const head = M.identity();
  head[13] = 1.6;                                         // eyes 1.6 m up, looking along -z
  const hand = [-0.25, 1.15, -0.35];                      // the left controller, held out low
  const m = HM.menuAbove(hand, head);
  const p = M.position(m), eye = [0, 1.6, 0];
  close(Math.hypot(p[0] - eye[0], p[2] - eye[2]), HM.MENU_DIST, 1e-5);
  // Beyond the hand: on the line from the eyes through it (in the floor plane).
  close(Math.atan2(p[0], -p[2]), Math.atan2(hand[0], -hand[2]), 1e-5);
  // Its lower edge just above the hand.
  close(p[1] - HM.MENU_SIZE[1] / 2, hand[1] + HM.MENU_GAP, 1e-5);
  // Facing the eyes, upright.
  // Facing the eyes, with no roll (its x axis level).
  const z = [m[8], m[9], m[10]], toEye = M.normalize(M.sub(eye, p));
  close(M.dot(z, toEye), 1, 1e-5);
  close(m[1], 0, 1e-6);
  // A hand raised high: the menu's centre no higher than MENU_RISE above the eyes.
  close(M.position(HM.menuAbove([-0.2, 1.9, -0.4], head))[1], 1.6 + HM.MENU_RISE, 1e-5);
  // A hand far to the side (63° from the gaze): at most MENU_TURN from it.
  const side = M.position(HM.menuAbove([-0.6, 1.2, -0.3], head));
  close(Math.atan2(side[0], -side[2]), -HM.MENU_TURN, 1e-5);
  // Behind or beside the user (over 90° from the gaze): along the gaze.
  for (const h of [[-0.6, 1.2, 0.1], [0.5, 1.2, 0.3], [0, 1.2, 0.4]]) {
    const b = M.position(HM.menuAbove(h, head));
    close(b[0], 0, 1e-6);
    assert.ok(b[2] < -0.9, `hand ${h}: in front`);
  }
  // A hand under the head: along the gaze; no hand: in front, as before.
  const under = M.position(HM.menuAbove([0.02, 1.0, -0.05], head));
  close(under[0], 0, 1e-6);
  assert.ok(under[2] < -0.9);
  assert.deepEqual([...HM.menuAbove(null, head)], [...HM.menuFront(head)]);
});

test('the value scale (#258): 3σ and 5σ on Display, Levels one press from it, greyed without σ or values', () => {
  const sig = { ...CONTEXTS.cube, lv: { sigma: true, hist: true, stretch: 'sqrt' } };
  const noSig = { ...CONTEXTS.cube, lv: { sigma: false, hist: false, stretch: 'sqrt' } };
  const why = (page, ctx, id) => HM.widgetById(HM.menuLayout(page, ctx), id).disabled;
  assert.equal(why('display', sig, 'sig:3'), '');
  assert.equal(why('display', sig, 'sig:5'), '');
  assert.equal(why('display', noSig, 'sig:3'), HM.WHY.noSigma);
  assert.equal(why('levels', noSig, 'pct:99'), HM.WHY.noHist);
  assert.equal(why('levels', noSig, 'hist'), HM.WHY.noHist);
  assert.equal(why('levels', noSig, 'stretch:log'), '', 'a stretch needs neither');
  assert.equal(why('levels', sig, 'gamma-up'), HM.WHY.powerOnly);
  assert.equal(why('levels', { ...sig, lv: { ...sig.lv, stretch: 'power' } }, 'gamma-up'), '');
  // The stretch lit is the one chosen; every stretch, σ and percentile preset is there.
  const L = HM.menuLayout('levels', sig);
  assert.deepEqual(L.filter((w) => w.group === 'stretch' && w.on).map((w) => w.value), ['sqrt']);
  for (const id of ['stretch:linear', 'stretch:log', 'stretch:sqrt', 'stretch:square', 'stretch:power',
    'sig:1', 'sig:3', 'sig:5', 'pct:99.9', 'levels-back']) assert.ok(HM.widgetById(L, id), id);
  // The histogram is the threshold slider's track, two rows high.
  const h = HM.widgetById(L, 'hist');
  assert.equal(h.kind, 'slider');
  assert.ok(h.rect[3] > 150);
  // Levels lights Display's tab and keeps the explore mode.
  assert.equal(HM.tabOf('levels'), 'display');
  assert.ok(HM.widgetById(L, 'tab:display').on);
  assert.equal(HM.pageOnOpen('levels', 'explore'), 'levels');
  // Presses: 3σ like the threshold; a stretch two from Display, three elsewhere.
  assert.equal(HM.pressesTo('sig:3', 'display', sig), 1);
  assert.equal(HM.pressesTo('sig:3', 'more', sig), 2);
  assert.equal(HM.pressesTo('stretch:log', 'display', sig), 2);
  assert.equal(HM.pressesTo('stretch:log', 'range', sig), 3);
  assert.equal(HM.pressesTo('stretch:log', 'levels', sig), 1);
  assert.equal(HM.pressesTo('stretch:log', 'display', CONTEXTS.points), Infinity, 'a cube only');
  // Display reaches Levels.
  assert.equal(why('display', sig, 'levels-page'), '');
});

test('simulation data (#259): the Data page one press from Display, the Box select on particles', () => {
  const sim = { axes: ['x', 'y', 'z'], series: 5, seriesIndex: 0, sizeOn: false, sizeField: 'hsml', style: 'splats', sel: { box: false, mode: 'all' } };
  const pts = { ...CONTEXTS.points, sim };
  const why = (page, ctx, id) => HM.widgetById(HM.menuLayout(page, ctx), id).disabled;
  assert.equal(why('display', pts, 'pt-data'), '');
  assert.equal(HM.tabOf('data'), 'display');
  assert.equal(HM.pressesTo('axis:0', 'display', pts), 2);
  assert.equal(HM.pressesTo('axis:0', 'select', pts), 3);
  assert.equal(HM.pressesTo('axis:0', 'data', pts), 1);
  assert.equal(HM.widgetById(HM.menuLayout('data', pts), 'axis:1').label, 'y');
  assert.equal(why('data', pts, 'snap-prev'), 'The first snapshot.');
  assert.equal(why('data', pts, 'snap-next'), '');
  assert.equal(why('data', { ...pts, sim: { ...sim, series: 1 } }, 'snap-next'), HM.WHY.noSeries);
  // While the series is still asked for, the press is taken (it waits for it).
  assert.equal(why('data', { ...pts, sim: { ...sim, series: 1, seriesPending: true } }, 'snap-next'), '');
  assert.equal(why('data', { ...pts, sim: { ...sim, series: 1, seriesPending: true } }, 'snap-prev'), '');
  assert.equal(why('data', pts, 'size:field'), '');
  assert.equal(why('data', { ...pts, sim: { ...sim, sizeField: null } }, 'size:field'), HM.WHY.noSizeField);
  assert.equal(why('data', { ...pts, sim: { ...sim, style: 'points' } }, 'size:field'), HM.WHY.splatsOnly);
  assert.equal(why('data', CONTEXTS.cube, 'axis:0'), HM.WHY.pointsOnly);
  // Select on particles: the Box select, the cube's tools greyed; the box's actions once drawn.
  assert.equal(why('select', pts, 'tool:box'), '');
  assert.equal(why('select', pts, 'tool:lasso'), HM.WHY.cubeTool);
  assert.equal(why('select', pts, 'sel-clear'), '');
  assert.equal(why('select', CONTEXTS.flow, 'tool:box'), HM.WHY.particlesSelect);
  assert.ok(!HM.menuLayout('select', pts).some((w) => w.id === 'psel-inside'));
  const drawn = { ...pts, sim: { ...sim, sel: { box: true, mode: 'inside' } } };
  const L = HM.menuLayout('select', drawn);
  for (const id of ['psel-inside', 'psel-outside', 'psel-all']) assert.equal(HM.widgetById(L, id).disabled, '', id);
  assert.ok(HM.widgetById(L, 'psel-inside').on);
  assert.equal(HM.pressesTo('tool:box', 'display', pts), 2, 'the box select: a tab away');
});

test('drawMenu draws every page of every kind on a 2-D context stand-in', () => {
  const calls = [];
  const target = { save() {}, restore() {}, translate(...a) { calls.push(['translate', a]); }, measureText: (t) => ({ width: String(t).length * 12 }),
    createLinearGradient: () => ({ addColorStop() {} }) };
  const c = new Proxy(target, { get: (t, k) => (k in t ? t[k] : () => {}), set: (t, k, v) => { t[k] = v; return true; } });
  const view = { title: 'VisIVO VR · HCG44.fits', notes: { thr: '0.0021 Jy/beam', slab: '4000 to 4800 km/s (radio) · ch 1–100 of 120' },
    values: { thr: 0.2, cmax: 0.8, opacity: 0.2, slab: [0.1, 0.9], 'crop:0': [0, 1], 'pt-range': [0.1, 0.7] },
    status: ['busy', ['an error', '#ff8a8a'], 'a long line '.repeat(30)],
    hist: { density: Array.from({ length: 160 }, (_, k) => (k * 37) % 50), thr: 0.3, cmax: 0.8, marks: [{ at: 0.25, label: '3σ' }, { at: 0.33, label: '5σ' }] } };
  let n = 0;
  const sim = { axes: ['x', 'y', 'z'], series: 3, seriesIndex: 1, sizeOn: true, sizeField: 'hsml', style: 'splats', sel: { box: true, mode: 'outside' } };
  for (const ctx of [...Object.values(CONTEXTS), { ...CONTEXTS.cube, lv: { sigma: true, hist: true, stretch: 'power' } }, { ...CONTEXTS.points, sim }]) {
    for (const page of HM.PAGES) { HM.drawMenu(c, page, HM.pageSpec(page, ctx), view, { hover: 'tab:range', active: 'thr' }, ctx); n++; }
  }
  assert.equal(calls.filter(([k]) => k === 'translate').length, n);
});

test('windows: the bar drags or closes, the content takes the ray, the nearest surface wins', () => {
  const L = W.barLayout();
  assert.deepEqual(L.map((w) => w.id), ['drag', 'close']);
  for (const w of L) assert.equal(P.hitWidget(L, w.rect[0] + w.rect[2] / 2, w.rect[1] + w.rect[3] / 2).id, w.id);
  const anchor = { p: [0, 1.3, -0.75], yaw: 0 };
  const model = W.windowModel(anchor, W.WINDOWS.result.place);
  const size = W.WINDOWS.result.size;
  const bar = W.barModel(model, size);
  // The bar sits just above the content, in its plane.
  const local = M.transformPoint(M.invert(model), M.position(bar));
  close(local[0], 0, 1e-6); close(local[2], 0, 1e-6);
  assert.ok(local[1] > size[1] / 2);
  // A ray at the close button closes; one at the content takes the ray and presses nothing.
  const eye = M.transformPoint(model, [0, 0, 0.8]);
  const at = (m, s, px, x, y) => M.transformPoint(m, [(x / px[0] - 0.5) * s[0], (0.5 - y / px[1]) * s[1], 0]);
  const cb = L[1].rect;
  const toClose = { o: eye, d: M.sub(at(bar, W.barSize(size[0]), W.BAR_PX, cb[0] + cb[2] / 2, cb[1] + cb[3] / 2), eye) };
  const ptr = new W.WindowPointer();
  assert.deepEqual(ptr.update(W.windowHits(model, size, toClose), true).map((a) => a.id), ['close']);
  ptr.update(W.windowHits(model, size, toClose), false);
  const toContent = { o: eye, d: M.sub(M.position(model), eye) };
  const h = W.windowHits(model, size, toContent);
  assert.ok(h.content && !h.bar);
  assert.deepEqual(ptr.update(h, true), []);
  assert.ok(ptr.takesRay() && ptr.captured, 'a press on the content belongs to the window');
  ptr.update(h, false);
  ptr.update({ bar: null, content: null }, false);
  assert.ok(!ptr.takesRay());
  // A drag on the bar: start, follow, end.
  const d0 = L[0].rect;
  const toBar = { o: eye, d: M.sub(at(bar, W.barSize(size[0]), W.BAR_PX, d0[0] + 40, d0[1] + d0[3] / 2), eye) };
  const types = [ptr.update(W.windowHits(model, size, toBar), true), ptr.update({ bar: null, content: null }, true),
    ptr.update({ bar: null, content: null }, false)].flat().map((a) => a.type);
  assert.deepEqual(types, ['drag-start', 'drag', 'drag-end']);
  // The nearest hit takes the ray; a surface holding a press keeps it.
  assert.equal(W.pickSurface([{ t: 2 }, null, { t: 1 }]), 2);
  assert.equal(W.pickSurface([{ t: 2 }, null, { t: 1 }], 0), 0);
  assert.equal(W.pickSurface([null, null]), -1);
});

test('the windows start apart and away from the cube (0.75 m in front)', () => {
  const anchor = { p: [0, 1.3, -0.75], yaw: 0.4 };
  const centres = Object.keys(W.WINDOWS).map((id) => M.position(W.windowModel(anchor, W.WINDOWS[id].place)));
  for (let i = 0; i < centres.length; i++) {
    for (let j = i + 1; j < centres.length; j++) assert.ok(M.length(M.sub(centres[i], centres[j])) > 0.3);
    assert.ok(M.length(M.sub(centres[i], anchor.p)) > 0.4, 'clear of a cube of 0.6 m');
  }
});

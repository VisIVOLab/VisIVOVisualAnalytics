// The tool palette (issue #257): its layout and hit tests, the tools greyed
// by data kind, the tooltip, the picks, the stick's step, the two presses
// from A to a tool, the placement in front of the primary hand, and the
// icons drawn on a 2-D context stand-in (node --test).
import test from 'node:test';
import assert from 'node:assert/strict';
import * as TP from '../../app/static/vr/js/tool-palette.js';
import * as TI from '../../app/static/vr/js/tool-icons.js';
import * as P from '../../app/static/vr/js/vr-panel.js';
import * as M from '../../app/static/vr/js/mat4.js';

const close = (a, b, eps = 1e-9) => assert.ok(Math.abs(a - b) <= eps, `${a} != ${b}`);

test('the tools, grouped by task in the issue\'s order', () => {
  assert.deepEqual(TP.TOOLS.map((t) => t.name), ['Explore', 'Kinematic Lasso', 'Spectrum probe', 'Box region',
    'Velocity range', 'Spatial crop', 'Moment', 'PV slice']);
  assert.deepEqual(TP.groupSpans().map(([, label]) => label), ['Look', 'Select', 'Range', 'Measure']);
  const groups = TP.TOOLS.map((t) => t.group);
  assert.deepEqual(groups, ['look', 'select', 'select', 'select', 'range', 'range', 'measure', 'measure']);
  // Every tool has its own icon and a one-line description.
  for (const t of TP.TOOLS) {
    assert.ok(TI.ICONS[t.id], t.id);
    assert.ok(t.desc.length > 10 && t.desc.length <= 70, t.desc);
  }
});

test('the layout fits, its cells do not overlap, each is hit and pressed at its centre', () => {
  for (const kind of ['cube', 'points', 'flow', 'none']) {
    const L = TP.paletteLayout({ kind, active: 'lasso' });
    for (const w of L) {
      const [x, y, ww, hh] = w.rect;
      assert.ok(x >= 0 && y >= 0 && x + ww <= TP.PALETTE_PX[0] && y + hh <= TP.TOOLTIP_Y, `${kind} ${w.id}`);
      assert.equal(P.hitWidget(L, x + ww / 2, y + hh / 2).id, w.id);
      const acts = new P.PanelPointer(L).update({ x: x + ww / 2, y: y + hh / 2 }, true);
      assert.deepEqual(acts.map((a) => [a.type, a.id]), [['press', w.id]]);
    }
    // The icons are large: at least 6 cm across at the palette's scale.
    assert.ok(L[0].rect[2] / TP.PALETTE_PPM >= 0.06);
  }
});

test('tools that do not apply are greyed, with the reason in the tooltip; the one in use is lit', () => {
  const pts = TP.paletteLayout({ kind: 'points', active: 'explore' });
  assert.deepEqual(pts.filter((w) => w.tool && !w.disabled).map((w) => w.tool), ['explore']);
  assert.ok(TP.paletteLayout({ kind: 'none', active: 'explore' }).filter((w) => w.tool).every((w) => w.disabled));
  const cube = TP.paletteLayout({ kind: 'cube', active: 'probe' });
  assert.ok(cube.filter((w) => w.tool).every((w) => !w.disabled));
  assert.deepEqual(cube.filter((w) => w.on).map((w) => w.tool), ['probe']);
  const tip = TP.tooltip('tool:lasso', { kind: 'cube', active: 'probe' });
  assert.deepEqual([tip.title, tip.greyed], ['Kinematic Lasso', false]);
  assert.match(tip.text, /connected object by pointing/);
  const grey = TP.tooltip('tool:moment', { kind: 'flow', active: 'explore' });
  assert.ok(grey.greyed && /Cubes only/.test(grey.text));
  assert.match(TP.tooltip(null, { kind: 'cube', active: 'crop' }).title, /Spatial crop/);
});

test('the active tool follows the mode; a pick says what to do', () => {
  assert.equal(TP.activeTool('explore', null), 'explore');
  assert.equal(TP.activeTool('select', 'probe'), 'probe');
  assert.equal(TP.activeTool('clip', 'box'), 'crop');
  assert.equal(TP.activeTool('clip', 'slab'), 'slab');
  assert.equal(TP.activeTool('select', 'lasso', true), 'pv', 'a PV slice being drawn');
  assert.deepEqual(TP.pickOf('tool:lasso'), { tool: 'lasso', disarmPv: true, mode: 'select', sub: 'lasso' });
  assert.deepEqual(TP.pickOf('tool:crop'), { tool: 'crop', disarmPv: true, mode: 'clip', sub: 'box' });
  assert.deepEqual(TP.pickOf('tool:moment'), { tool: 'moment', disarmPv: true, measure: 'moment' }, 'Moment ends a slice being drawn');
  assert.deepEqual(TP.pickOf('tool:pv'), { tool: 'pv', disarmPv: false, measure: 'pv' });
  assert.equal(TP.pickOf('palette-close'), null);
});

test('the secondary stick steps through the same list, in the same order, skipping what does not apply', () => {
  // As main.js does it: the step starts from stepFrom(the tool in use, the last pick).
  const walk = (step) => {
    const seq = [];
    let active = 'explore', last = null;
    for (let i = 0; i < 9; i++) {
      const id = TP.stepTool(TP.stepFrom(active, last), 'cube', step);
      seq.push(id);
      last = { id, active };
      const p = TP.pickOf(id);
      if (p.measure === 'pv') active = 'pv';
      else if (!p.measure) active = TP.activeTool(p.mode, p.sub);   // Moment leaves the tool in use as it was
    }
    return seq;
  };
  assert.deepEqual(walk(1), ['lasso', 'probe', 'box', 'slab', 'crop', 'moment', 'pv', 'explore', 'lasso']);
  assert.deepEqual(walk(-1), ['pv', 'moment', 'crop', 'slab', 'box', 'probe', 'lasso', 'explore', 'pv']);
  assert.equal(TP.stepTool('explore', 'points'), 'explore', 'a point table: only Explore');
  assert.equal(TP.stepTool('explore', 'none'), null);
  assert.equal(TP.stepFrom('crop', { id: 'moment', active: 'crop' }), 'moment');
  assert.equal(TP.stepFrom('lasso', { id: 'moment', active: 'crop' }), 'lasso', 'the tool changed since: from it');
  assert.equal(TP.stepFrom('crop', null), 'crop');
});

test('the palette, drawn over the menu, takes the ray first; a captured surface keeps it', async () => {
  const W = await import('../../app/static/vr/js/windows.js');
  // [menu, palette, window]: the menu nearer along the ray than the palette.
  const hits = [{ t: 0.9 }, { t: 1.1 }, null];
  assert.equal(W.pickSurface(hits), 0, 'by distance alone: the menu');
  assert.equal(W.pickSurface(hits, -1, 1), 1, 'the palette on top');
  assert.equal(W.pickSurface([{ t: 0.9 }, null, null], -1, 1), 0, 'the palette missed: the nearest');
  assert.equal(W.pickSurface(hits, 0, 1), 0, 'a press held on the menu keeps it');
});

test('every tool is two presses from the palette closed: A, then the tool', () => {
  for (const t of TP.TOOLS) assert.equal(TP.pressesToTool(t.id, 'cube'), 2, t.id);
  assert.equal(TP.pressesToTool('explore', 'points'), 2);
  assert.equal(TP.pressesToTool('lasso', 'points'), Infinity);
});

test('the palette opens about 0.8 m in front of the primary hand, turned to the eyes', () => {
  const eye = [0, 1.6, 0];
  for (const yaw of [0, 1.1]) {
    const head = M.multiply(M.translation(...eye), M.rotation([0, 1, 0], yaw));
    const hand = [0.25, 1.2, -0.3];
    const m = TP.paletteModel(hand, head);
    const p = M.position(m);
    const d = M.sub(p, hand);
    close(Math.hypot(d[0], d[2]), TP.PALETTE_DIST, 1e-5);
    assert.ok(d[1] > 0 && d[1] < 0.1);
    const fwd = [-Math.sin(yaw), 0, -Math.cos(yaw)];
    assert.ok(M.dot(M.normalize([d[0], 0, d[2]]), fwd) > 0.9999);
    const z = M.normalize(M.sub(M.transformPoint(m, [0, 0, 1]), p));
    assert.ok(M.dot(z, M.normalize(M.sub(eye, p))) > 0.9999);
    // A ray from the hand to a tool's icon presses it.
    const L = TP.paletteLayout({ kind: 'cube', active: 'explore' });
    const w = L.find((x) => x.id === 'tool:pv');
    const [cx, cy] = [w.rect[0] + w.rect[2] / 2, w.rect[1] + w.rect[3] / 2];
    const target = M.transformPoint(m, [(cx / TP.PALETTE_PX[0] - 0.5) * TP.PALETTE_SIZE[0], (0.5 - cy / TP.PALETTE_PX[1]) * TP.PALETTE_SIZE[1], 0]);
    const hit = P.rayPanel(m, TP.PALETTE_SIZE, hand, M.sub(target, hand), TP.PALETTE_PX);
    assert.deepEqual(new P.PanelPointer(L).update(hit, true).map((a) => a.id), ['tool:pv']);
  }
});

test('the palette and the icons draw on a 2-D context stand-in', () => {
  const calls = [];
  const target = { save() {}, restore() {}, measureText: (t) => ({ width: String(t).length * 12 }), setLineDash() {},
    fillText(...a) { calls.push(a[0]); } };
  const c = new Proxy(target, { get: (t, k) => (k in t ? t[k] : () => {}), set: (t, k, v) => { t[k] = v; return true; } });
  for (const kind of ['cube', 'points', 'none']) {
    TP.drawPalette(c, TP.paletteLayout({ kind, active: 'lasso' }), { kind, active: 'lasso' }, { hover: 'tool:box' });
  }
  assert.ok(calls.includes('Box region'), 'the tooltip names the tool pointed at');
  for (const id of Object.keys(TI.ICONS)) assert.equal(TI.drawIcon(c, id, 0, 0, 64), true, id);
  assert.equal(TI.drawIcon(c, 'nothing', 0, 0, 64), false);
});

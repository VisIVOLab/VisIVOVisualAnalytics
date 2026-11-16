// The exploded view of a simulation model (issue #371): the animation, the
// value ranges, the layouts and the pieces' scale, the pieces' matrices and
// the hand's part, picking the piece a hand holds, the drawing order, the
// per-piece uniforms, the Explode page, and the page's side (explode-ui.js)
// against a stand-in renderer: no allocation on the GPU, one draw a shown
// piece from the same texture (node --test).
import test from 'node:test';
import assert from 'node:assert/strict';
import * as EX from '../../app/static/vr/js/explode.js';
import * as HM from '../../app/static/vr/js/hand-menu.js';
import * as P from '../../app/static/vr/js/vr-panel.js';
import * as M from '../../app/static/vr/js/mat4.js';
import { createExplode } from '../../app/static/vr/js/explode-ui.js';

const close = (a, b, eps = 1e-6, what = '') => assert.ok(Math.abs(a - b) <= eps, `${what} ${a} != ${b}`);
const closeArr = (a, b, eps = 1e-6, what = '') => { assert.equal(a.length, b.length, what); a.forEach((v, i) => close(v, b[i], eps, `${what}[${i}]`)); };

test('the explode amount eases from 0 to 1 in ANIM_MS, and a reversal halfway takes the distance left', () => {
  close(EX.ease(0), 0); close(EX.ease(1), 1); close(EX.ease(0.5), 0.5);
  assert.ok(EX.ease(0.1) < 0.1 && EX.ease(0.9) > 0.9, 'flat at both ends');
  const a = EX.newAnim(0);
  assert.equal(EX.amountAt(a, 123), 0);
  EX.retarget(a, 1, 1000);
  assert.equal(a.ms, EX.ANIM_MS);
  close(EX.amountAt(a, 1000), 0);
  close(EX.amountAt(a, 1000 + EX.ANIM_MS / 2), 0.5);
  close(EX.amountAt(a, 1000 + EX.ANIM_MS), 1);
  close(EX.amountAt(a, 99999), 1);
  assert.ok(EX.animating(a, 1100) && !EX.animating(a, 1000 + EX.ANIM_MS));
  // Back halfway: from 0.5 to 0, in half the time.
  EX.retarget(a, 1, 0);
  EX.retarget(a, 0, EX.ANIM_MS / 2);
  close(a.from, 0.5);
  close(a.ms, EX.ANIM_MS / 2);
  close(EX.amountAt(a, EX.ANIM_MS), 0);
  // At once (tests, the measurement run).
  EX.retarget(a, 1, 0, 0);
  assert.equal(EX.amountAt(a, 0), 1);
});

test('value ranges: equal widths from the threshold to the colour max, the top one open, each its own colour window', () => {
  const b = EX.bands(0.2, 0.8, 3);
  assert.equal(b.length, 3);
  closeArr(b.map((x) => x.lo), [0.2, 0.4, 0.6]);
  closeArr(b.map((x) => x.cmax), [0.4, 0.6, 0.8]);
  closeArr(b.map((x) => x.hi), [0.4, 0.6, 2], 1e-9, 'the top range keeps every value above the colour max');
  for (const x of b) assert.equal(x.cmin, x.lo);
  // Contiguous: each value in exactly one range.
  for (const v of [0.2, 0.3, 0.4, 0.55, 0.6, 0.79, 0.8, 1]) assert.equal(b.filter((x) => v >= x.lo && v < x.hi).length, 1, `v=${v}`);
  assert.equal(b.filter((x) => 0.1 >= x.lo && 0.1 < x.hi).length, 0, 'below the threshold: none');
  // On a log-scaled field the codes are log10 of the values: equal widths are equal decades.
  const lo = Math.log10(1e-12), hi = Math.log10(1e-4), norm = (v) => (Math.log10(v) - lo) / (hi - lo);
  const dec = EX.bands(norm(1e-10), norm(1e-6), 4).map((x) => 10 ** (lo + x.lo * (hi - lo)));
  closeArr(dec.map(Math.log10), [-10, -9, -8, -7], 1e-9);
  // Counts are clamped to the pieces there can be; a colour max at the threshold still makes ranges.
  assert.equal(EX.bands(0, 1, 9).length, EX.MAX_PIECES);
  assert.ok(EX.bands(0.5, 0.5, 2).every((x) => x.cmax > x.cmin));
  assert.deepEqual(EX.bandMaps(2), ['blue', 'red']);
  assert.deepEqual(EX.bandMaps(4), ['blue', 'green', 'orange', 'red']);
  assert.equal(new Set(EX.bandMaps(4)).size, 4, 'each range its own colour');
});

test('layouts: a row along x and a column along y, centred; a ring with its neighbours apart; the pieces scaled to keep the span', () => {
  const ext = [0.6, 0.4, 0.3];
  for (const n of [2, 3, 4]) {
    const s = EX.pieceScale(n);
    const span = s * (n + (n - 1) * EX.DEFAULT_SPREAD);
    assert.ok(span <= EX.MAX_SPAN + 1e-9, `n=${n}: the row spans ${span} cube sizes`);
    // About the pixels of the cube alone: n pieces of s² its area.
    assert.ok(n * s * s <= 1 + 1e-9, `n=${n}: ${n * s * s} of the cube's pixels`);
    const row = EX.layout(n, 'row', ext);
    assert.equal(row.scale, s);
    const xs = row.offsets.map((o) => o[0]);
    close(xs.reduce((a, b) => a + b, 0), 0, 1e-9, 'centred');
    for (let i = 1; i < n; i++) close(xs[i] - xs[i - 1], ext[0] * s * (1 + EX.DEFAULT_SPREAD), 1e-9, 'pitch: a piece and the gap');
    assert.ok(row.offsets.every((o) => o[1] === 0 && o[2] === 0));
    const col = EX.layout(n, 'column', ext);
    assert.ok(col.offsets[0][1] > col.offsets[n - 1][1], 'the first piece on top');
    close(col.offsets[0][1] - col.offsets[1][1], ext[1] * s * (1 + EX.DEFAULT_SPREAD), 1e-9);
  }
  assert.equal(EX.pieceScale(1), 1);
  close(EX.pieceScale(2), Math.SQRT1_2, 1e-9, 'two pieces: half the area each');
  // The spread moves the pieces apart, it does not shrink them.
  const wide = EX.layout(3, 'row', ext, 1);
  assert.equal(wide.scale, EX.layout(3, 'row', ext).scale);
  close(wide.offsets[1][0] - wide.offsets[0][0], ext[0] * wide.scale * 2, 1e-9);
  // A ring: equal radius, neighbours a piece and the gap apart, the first on top, in the x-y plane.
  for (const n of [3, 4]) {
    const r = EX.layout(n, 'ring', ext);
    const size = Math.max(ext[0], ext[1]) * r.scale;
    const rad = r.offsets.map((o) => Math.hypot(o[0], o[1]));
    rad.forEach((v) => close(v, rad[0], 1e-9));
    assert.ok(r.offsets.every((o) => o[2] === 0));
    close(r.offsets[0][0], 0, 1e-9); assert.ok(r.offsets[0][1] > 0);
    for (let i = 0; i < n; i++) {
      const a = r.offsets[i], b = r.offsets[(i + 1) % n];
      close(Math.hypot(a[0] - b[0], a[1] - b[1]), size * (1 + EX.DEFAULT_SPREAD), 1e-9, `ring of ${n}`);
    }
  }
  // A ring of two is a row.
  assert.deepEqual(EX.layout(2, 'ring', ext), EX.layout(2, 'row', ext));
});

test('a piece\'s matrix: the cube itself when assembled, its layout place, the hand\'s part and its scale when exploded', () => {
  const scratch = { t: [0, 0, 0], q: [0, 0, 0, 1], s: [1, 1, 1] };
  const out = new Float32Array(16);
  const off = [0.5, -0.2, 0.1];
  const user = { t: [0.05, 0.1, -0.3], q: EX.quatFromMatrix(M.rotation([0, 1, 0], 0.7)) };
  EX.pieceMatrix(out, off, user, 0.6, 0, scratch);
  closeArr(Array.from(out), Array.from(M.identity()), 1e-6, 'e = 0');
  EX.pieceMatrix(out, off, user, 0.6, 1, scratch);
  closeArr([out[12], out[13], out[14]], [0.55, -0.1, -0.2], 1e-6, 'translation');
  close(M.uniformScale(out), 0.6, 1e-6, 'scale');
  const want = M.multiply(M.translation(0.55, -0.1, -0.2), M.multiply(M.rotation([0, 1, 0], 0.7), M.scaling(0.6)));
  closeArr(Array.from(out), Array.from(want), 1e-6, 'T R S');
  // Shrink first, then part: halfway through the shrink, the piece is still in place at a scale between.
  EX.pieceMatrix(out, off, user, 0.6, EX.SHRINK / 2, scratch);
  closeArr([out[12], out[13], out[14]], [0, 0, 0], 1e-9, 'not moved while shrinking');
  close(M.uniformScale(out), 0.8, 1e-6);
  // Halfway through the parting: at its scale, half the way there, half the turn.
  EX.pieceMatrix(out, off, user, 0.6, EX.SHRINK + (1 - EX.SHRINK) / 2, scratch);
  closeArr([out[12], out[13], out[14]], [0.275, -0.05, -0.1], 1e-6);
  close(M.uniformScale(out), 0.6, 1e-6);
  closeArr(Array.from(EX.quatFromMatrix(out)), Array.from(EX.quatFromMatrix(M.rotation([0, 1, 0], 0.35))), 1e-6, 'half the turn');
  // The hand's part back from a piece put somewhere at e = 1 (the round trip a grab makes).
  const P = M.multiply(M.translation(0.9, 0.3, -0.4), M.multiply(M.rotation(M.normalize([1, 2, 0.5]), 2.1), M.scaling(0.6)));
  const u = EX.newUser();
  EX.userFromPiece(u, P, off);
  EX.pieceMatrix(out, off, u, 0.6, 1, scratch);
  closeArr(Array.from(out), Array.from(P), 1e-5, 'round trip');
});

test('the animation never draws more than the cube alone: the volume shrinks, then pieces at their scale part', () => {
  for (const n of [2, 3, 4]) {
    const s = EX.pieceScale(n);
    let worst = 0;
    for (let e = 0; e <= 1.0001; e += 0.01) worst = Math.max(worst, EX.coverageAt(n, e, s));
    assert.ok(worst <= 1 + 1e-9, `n=${n}: at most ${worst} of the cube's pixels`);
    assert.equal(EX.piecesDrawn(EX.SHRINK - 1e-6), false);
    assert.equal(EX.piecesDrawn(EX.SHRINK), true);
    close(EX.wholeScale(EX.SHRINK, s), s, 1e-12, 'the volume ends the shrink at the pieces\' scale');
    close(EX.wholeScale(0, s), 1, 1e-12);
  }
  const p = EX.phases(0.2);
  close(p.shrink, 0.2 / EX.SHRINK); assert.equal(p.spread, 0);
  assert.deepEqual(EX.phases(1), { shrink: 1, spread: 1 });
});

test('invertInto: the inverse with no allocation, false when singular', () => {
  const m = M.multiply(M.translation(0.3, -1, 2), M.multiply(M.rotation(M.normalize([1, 2, 3]), 0.9), M.scaling(0.4)));
  const out = new Float32Array(16);
  assert.equal(EX.invertInto(out, m), true);
  closeArr(Array.from(out), Array.from(M.invert(m)), 1e-5);
  assert.equal(EX.invertInto(out, new Float32Array(16)), false);
});

test('quaternions from rotation matrices: every branch, half turns included, scale taken out', () => {
  const axes = [[1, 0, 0], [0, 1, 0], [0, 0, 1], M.normalize([1, 1, 1]), M.normalize([-1, 0.3, 2])];
  for (const ax of axes) {
    for (const ang of [0, 0.3, 1.5, Math.PI - 1e-3, Math.PI]) {
      const R = M.multiply(M.rotation(ax, ang), M.scaling(2.5));
      const q = EX.quatFromMatrix(R);
      close(Math.hypot(...q), 1, 1e-9);
      const back = new Float32Array(16);
      EX.pieceMatrix(back, [0, 0, 0], { t: [0, 0, 0], q }, 1, 1, { t: [0, 0, 0], q: [0, 0, 0, 1], s: [1, 1, 1] });
      const Rn = M.rotation(ax, ang);
      for (const k of [0, 1, 2, 4, 5, 6, 8, 9, 10]) close(back[k], Rn[k], 1e-5, `axis ${ax} angle ${ang} [${k}]`);
    }
  }
});

test('the piece a hand holds: inside a shown piece\'s box (with a margin), the nearest centre, hidden pieces never', () => {
  const ext = [0.4, 0.4, 0.4];
  const box = { min: [0, 0, 0], max: [1, 1, 1] };
  const worlds = [M.translation(-0.5, 0, 0), M.translation(0, 0, 0), M.multiply(M.translation(0.5, 0, 0), M.scaling(0.5))];
  const invs = worlds.map((w) => M.invert(w));
  assert.equal(EX.pieceAt([-0.5, 0.1, 0], invs, ext, box), 0);
  assert.equal(EX.pieceAt([0.05, -0.1, 0.1], invs, ext, box), 1);
  assert.equal(EX.pieceAt([0.5, 0, 0], invs, ext, box), 2);
  assert.equal(EX.pieceAt([0.5 + 0.12, 0, 0], invs, ext, box), -1, 'outside the half-size piece (0.1 half-width) and its margin (0.016)');
  assert.equal(EX.pieceAt([0.5 + 0.11, 0, 0], invs, ext, box), 2, 'within the margin');
  assert.equal(EX.pieceAt([0, 2, 0], invs, ext, box), -1);
  assert.equal(EX.pieceAt([-0.5, 0, 0], invs, ext, box, [true, false, false]), -1, 'hidden: not grabbed');
  // Two boxes holding the point: the nearer centre.
  const overlap = [M.translation(0, 0, 0), M.translation(0.1, 0, 0)].map((w) => M.invert(w));
  assert.equal(EX.pieceAt([0.09, 0, 0], overlap, ext, box), 1);
  // A crop: only what is drawn is held.
  assert.equal(EX.pieceAt([-0.5 - 0.15, 0, 0], invs, ext, { min: [0.4, 0, 0], max: [1, 1, 1] }), -1);
});

test('the drawing order: the farthest piece first, without allocating', () => {
  const order = new Int32Array(4);
  const centres = new Float32Array([0, 0, -1, 0, 0, -3, 0, 0, -2, 0, 0, 0.5]);
  EX.farthestFirst(order, centres, 4, [0, 0, 1]);
  assert.deepEqual(Array.from(order), [1, 2, 0, 3]);
  EX.farthestFirst(order, centres, 4, [0, 0, -5]);
  assert.deepEqual(Array.from(order), [3, 0, 2, 1]);
  EX.farthestFirst(order, centres, 2, [0, 0, 1]);
  assert.deepEqual(Array.from(order.subarray(0, 2)), [1, 0]);
});

test('per-piece uniforms: one channel between its bounds, in its own atlas row; the other channels never drawn', () => {
  const u = EX.pieceUniforms();
  EX.setPieceUniforms(u, 1, { lo: 0.3, hi: 0.6, cmin: 0.3, cmax: 0.6, stretch: 2, stretchA: 5, opacity: 0.4, row: EX.pieceRow(2) });
  assert.deepEqual(Array.from(u.thr), [2, 0.30000001192092896, 2]);
  closeArr(Array.from(u.thrHi), [2, 0.6, 2]);
  closeArr(Array.from(u.cmax), [1, 0.6, 1]);
  assert.deepEqual(Array.from(u.stretch), [0, 2, 0]);
  closeArr(Array.from(u.opacity), [0, 0.4, 0]);
  closeArr(Array.from(u.rows), [0.625, 0.625, 0.625]);
  // Reused: the next piece's values replace the last's.
  EX.setPieceUniforms(u, 0, { lo: -1, hi: 2, cmin: 0.5, cmax: 0.5, stretch: 0, stretchA: NaN, opacity: 1, row: EX.pieceRow(0) });
  assert.deepEqual(Array.from(u.thr), [0, 2, 2], 'clamped at 0; channel 1 off again');
  assert.ok(u.cmax[0] > u.cmin[0]);
  assert.equal(u.stretchA[0], 1);
  closeArr([0, 1, 2, 3].map(EX.pieceRow), [0.125, 0.375, 0.625, 0.875]);
});

const simCtx = (ex, extra = {}) => ({ ...HM.defaultContext(), kind: 'cube', simulation: true, vars: { names: ['rho', 'T', 'vx'], current: 'rho' },
  ov: { fields: [], edit: 0, composite: 'sum', shown: false, loading: false, room: '', fieldPage: 0 },
  ex: ex === null ? null : { source: 'bands', bands: 3, layout: 'row', exploded: false, animating: false, pieces: [], fieldsWhy: 'overlay first', why: '', ...ex }, ...extra });

test('the hand menu: Display leads to the Explode page on a simulation volume; the page fits and greys with reasons', () => {
  const ids = (ctx) => HM.pageSpec('display', ctx).widgets.map((w) => w.id);
  assert.ok(ids(simCtx({})).includes('explode-page'));
  assert.ok(!ids(simCtx(null)).includes('explode-page'));
  assert.ok(!ids({ ...HM.defaultContext(), kind: 'cube' }).includes('explode-page'), 'a radio cube');
  // A simulation of one variable (no overlay): Emission, MIP, Explode.
  const one = HM.pageSpec('display', simCtx({}, { vars: null, ov: null })).widgets;
  assert.ok(one.some((w) => w.id === 'explode-page') && !one.some((w) => w.id === 'overlay-page'));
  // With the overlay drawn the single look is greyed, the Explode page is not.
  const ovOn = HM.pageSpec('display', simCtx({}, { ov: { fields: [{ name: 'rho' }, { name: 'T' }], edit: 0, composite: 'sum', shown: true, loading: false, room: '', fieldPage: 0 } })).widgets;
  assert.equal(ovOn.find((w) => w.id === 'thr').disabled, HM.WHY.ovOn);
  assert.equal(ovOn.find((w) => w.id === 'explode-page').disabled, '');
  assert.equal(HM.tabOf('explode'), 'display');
  assert.equal(HM.TAB_MODE.explode, 'explore');
  const pieces = [{ label: '≥ 1e-3', hidden: false }, { label: '1e-5–1e-4', hidden: true }, { label: 'rho', hidden: false }, { label: '4', hidden: false }];
  for (const ex of [{}, { pieces: pieces.slice(0, 2) }, { pieces, exploded: true, animating: true }, { source: 'fields', fieldsWhy: '', pieces: pieces.slice(0, 3) }]) {
    const L = HM.menuLayout('explode', simCtx(ex));
    for (const w of L) {
      const [x, y, ww, hh] = w.rect;
      assert.ok(x >= 0 && y >= 0 && x + ww <= HM.MENU_PX[0] && y + hh <= HM.MENU_PX[1], `${w.id} inside`);
      if (w.page) assert.ok(y >= HM.PAGE_Y && y + hh <= HM.FOOTER_Y, `${w.id} within the page`);
      if (w.kind !== 'drag') assert.ok(ww / HM.MENU_PPM >= 0.04 && hh / HM.MENU_PPM >= 0.025, `${w.id} large enough`);
      assert.equal(P.hitWidget(L, x + ww / 2, y + hh / 2).id, w.id, `${w.id} hit at its centre`);
    }
    assert.equal(new Set(L.map((w) => w.id)).size, L.length, 'unique ids');
  }
  const w = (ctx, id) => HM.widgetById(HM.menuLayout('explode', ctx), id);
  assert.equal(w(simCtx({}), 'ex-src:fields').disabled, 'overlay first', 'Fields: the overlay first');
  assert.equal(w(simCtx({}), 'ex-bands:3').on, true);
  assert.equal(w(simCtx({ source: 'fields', fieldsWhy: '' }), 'ex-bands:3').disabled, HM.WHY.bandsOnly);
  assert.equal(w(simCtx({}), 'ex-go').disabled !== '', true, 'nothing to take apart yet');
  assert.equal(w(simCtx({ pieces }), 'ex-go').label, 'Explode');
  assert.equal(w(simCtx({ pieces, exploded: true }), 'ex-go').label, 'Assemble');
  assert.equal(w(simCtx({ pieces }), 'ex-piece:1').on, false, 'a hidden piece');
  // Nothing loaded: greyed whole.
  assert.ok(HM.menuLayout('explode', { ...HM.defaultContext() }).filter((x) => x.page).every((x) => x.disabled));
  // Two presses from opening: Display, then Explode ▸ (a sub-page, as Overlay).
  assert.equal(HM.pageOnOpen('explode', 'explore'), 'explode');
});

/** A stand-in renderer that records the pieces' draws (and would count any 3-D allocation). */
function fakeRenderer() {
  return {
    calls: [], maps: null, lines: 0, alloc3D: 0, volTex: {}, dims: [32, 32, 32], whole: [],
    setPieceColormaps(m) { this.maps = m.slice(); },
    drawPiece(view, model, p) {
      this.calls.push({ model: Float32Array.from(model), src: p.src, thr: Array.from(p.ov.thr), thrHi: Array.from(p.ov.thrHi), rows: Array.from(p.ov.rows), mode: p.mode });
      return true;
    },
    drawLines() { this.lines++; },
    allocOverlay() { this.alloc3D++; }, allocCapacity() { this.alloc3D++; },
  };
}

function setup({ overlayLoaded = null } = {}) {
  let now = 0;
  const R = fakeRenderer();
  const state = { cube: { dims: [32, 32, 32], lo: -12, hi: -4, scale: 'log', bunit: 'g/cm3' }, stretch: 'linear', info: {}, extent: [0.4, 0.4, 0.4], clip: { min: [0, 0, 0], max: [1, 1, 1] }, thrNorm: 0.2, cmaxNorm: 0.8 };
  const overlay = {
    ov: { loaded: overlayLoaded, fields: overlayLoaded ? overlayLoaded.fields.map((f) => f.name) : [], edit: 0 },
    drawn: () => !!overlayLoaded,
    params: (base, o) => ({ ...base, mode: o.mode ?? 0, ov: { thr: [0.1, 0.2, 0.3], cmin: [0.1, 0.2, 0.3], cmax: [0.9, 0.8, 0.7], stretch: [0, 2, 0], stretchA: [1, 1, 1], opacity: [0.4, 0.4, 0.4] } }),
  };
  const volParams = (o = {}) => ({ extent: state.extent, boxMin: state.clip.min, boxMax: state.clip.max, thr: state.thrNorm, cmin: state.thrNorm, cmax: state.cmaxNorm,
    stretch: 2, stretchA: 1, opacity: 0.5, spv: 1.5, mode: 0, skip: true, maxSteps: 4096, texScale: [1, 1, 1], ...o });
  const logs = [], drops = [];
  const ex = createExplode({ R, state, overlay, volParams, dataValue: (n) => 10 ** (-12 + 8 * n), isSimulation: () => true,
    log: (...a) => logs.push(a), changed: () => {}, dropDetail: (why) => drops.push(why), now: () => now,
    drawWhole: (v, model) => R.whole.push(M.uniformScale(model)) });
  const view = { view: M.invert(M.translation(0, 0, 2)), proj: M.perspective(1, 1, 0.01, 10) };
  return { R, state, overlay, ex, view, tick: (ms) => { now += ms; }, logs, drops };
}

test('the page\'s side: value ranges of the single cube drawn as pieces of the same texture, apart, the farthest first; hidden ones skipped', () => {
  const { R, ex, view, tick, drops } = setup();
  assert.equal(ex.active(), false, 'assembled: the volume is drawn as usual');
  ex.set(true, { source: 'bands', bands: 3, layout: 'row' });
  drops.length = 0;
  assert.equal(ex.active(), true);
  assert.equal(drops.length, 1, 'turning active drops the crop\'s detail');
  ex.active();
  assert.equal(drops.length, 1, 'once');
  assert.deepEqual(R.maps, ['blue', 'green', 'red']);
  // Early on: the volume itself, once, shrinking (no pieces).
  tick(EX.ANIM_MS * 0.2);
  R.calls.length = 0;
  assert.equal(ex.draw(view, M.identity()), 0);
  assert.equal(R.calls.length, 0);
  assert.equal(R.whole.length, 1);
  assert.ok(R.whole[0] < 1 && R.whole[0] > EX.pieceScale(3), `shrinking: ${R.whole[0]}`);
  tick(EX.ANIM_MS * 0.3);
  close(ex.amount(), 0.5);
  tick(EX.ANIM_MS);
  close(ex.amount(), 1);
  R.calls.length = 0;
  assert.equal(ex.draw(view, M.identity()), 3);
  assert.equal(R.calls.length, 3);
  assert.ok(R.calls.every((c) => c.src === 'cube'), 'the single cube\'s texture, three times');
  // Each piece its own range of the cube's threshold to colour max, its own row, in its own place.
  const byLo = R.calls.map((c) => c.thr[0]).sort();
  closeArr(byLo, [0.2, 0.4, 0.6], 1e-6);
  assert.ok(R.calls.every((c) => c.thr[1] === 2 && c.thr[2] === 2));
  assert.equal(new Set(R.calls.map((c) => c.rows[0])).size, 3);
  const xs = R.calls.map((c) => c.model[12]);
  assert.equal(new Set(xs.map((x) => x.toFixed(4))).size, 3, 'three places');
  assert.equal(R.lines, 3, 'each piece\'s box outline');
  // Hidden: two drawn.
  // One colour bar a piece, in its colour map over its own range (decades of the log-scaled cube).
  const bars = ex.bars();
  assert.deepEqual(bars.map((b) => b.colormap), ['blue', 'green', 'red']);
  closeArr(bars.map((b) => Math.log10(b.lo)), [-12 + 8 * 0.2, -12 + 8 * 0.4, -12 + 8 * 0.6], 1e-6);
  closeArr(bars.map((b) => Math.log10(b.hi)), [-12 + 8 * 0.4, -12 + 8 * 0.6, -12 + 8 * 0.8], 1e-6);
  assert.equal(ex.bars(), bars, 'kept while nothing changed');
  ex.press('ex-piece:1', { group: 'ex-piece', value: 1 });
  assert.deepEqual(ex.bars().map((b) => b.colormap), ['blue', 'red'], 'a hidden piece has no bar');
  R.calls.length = 0;
  assert.equal(ex.draw(view, M.identity()), 2);
  assert.ok(ex.menuContext().pieces[1].hidden);
  assert.equal(R.alloc3D, 0, 'nothing allocated');
  // Labels: the ranges in data values, the top one open.
  const labels = ex.menuContext().pieces.map((p) => p.label);
  assert.ok(labels[2].startsWith('≥'), labels.join(' | '));
  assert.ok(/–/.test(labels[0]), labels.join(' | '));
  // Assemble: back to the volume once the animation ends; Reset shows every piece.
  ex.press('ex-go', { id: 'ex-go' });
  assert.equal(ex.active(), true, 'on its way back');
  tick(EX.ANIM_MS + 1);
  assert.equal(ex.active(), false);
  ex.press('ex-reset', {});
  assert.ok(ex.menuContext().pieces.every((p) => !p.hidden));
});

test('the page\'s side: the overlaid fields as pieces, each its own channel and colour map; ranges of the edited field', () => {
  const loaded = { fields: [{ name: 'rho', lo: -12, hi: -4, scale: 'log' }, { name: 'T', lo: 3, hi: 7, scale: 'log' }, { name: 'vx', lo: -100, hi: 100, scale: 'linear' }],
    looks: [{ colormap: 'red', thrNorm: 0.1, cmaxNorm: 0.9, opacity: 0.4 }, { colormap: 'green', thrNorm: 0.2, cmaxNorm: 0.8, opacity: 0.3 }, { colormap: 'blue', thrNorm: 0.3, cmaxNorm: 0.7, opacity: 0.2 }] };
  const { R, ex, overlay, view } = setup({ overlayLoaded: loaded });
  ex.set(true, { source: 'fields', layout: 'ring', now: true });
  assert.deepEqual(R.maps, ['red', 'green', 'blue']);
  assert.deepEqual(ex.menuContext().pieces.map((p) => p.label), ['rho', 'T', 'vx']);
  R.calls.length = 0;
  ex.draw(view, M.identity());
  assert.equal(R.calls.length, 3);
  assert.ok(R.calls.every((c) => c.src === 'overlay'));
  // Each draw shows one channel at its field's own threshold.
  for (const c of R.calls) {
    const ch = c.thr.findIndex((v) => v < 2);
    assert.ok(ch >= 0 && c.thr.filter((v) => v < 2).length === 1);
    close(c.thr[ch], loaded.looks[ch].thrNorm, 1e-6);
    close(c.rows[ch], EX.pieceRow(ch), 1e-9);
  }
  // A colour map changed on the Overlay page reaches the atlas.
  loaded.looks[1].colormap = 'inferno';
  ex.draw(view, M.identity());
  assert.deepEqual(R.maps, ['red', 'inferno', 'blue']);
  // Value ranges while the overlay is drawn: of the field edited there (T), from its own threshold.
  overlay.ov.edit = 1;
  ex.set(true, { source: 'bands', bands: 2, now: true });
  R.calls.length = 0;
  ex.draw(view, M.identity());
  assert.equal(R.calls.length, 2);
  assert.ok(R.calls.every((c) => c.src === 'overlay' && c.thr[0] === 2 && c.thr[2] === 2));
  closeArr(R.calls.map((c) => c.thr[1]).sort(), [0.2, 0.5], 1e-6);
  // A field hidden on the Overlay page is not drawn and has no bar; its chip is the same switch.
  ex.set(true, { source: 'fields', now: true });
  loaded.looks[2].on = false;
  R.calls.length = 0;
  ex.draw(view, M.identity());
  assert.equal(R.calls.length, 2);
  assert.equal(ex.bars().length, 2);
  assert.equal(ex.menuContext().pieces[2].hidden, true);
  ex.press('ex-piece:2', { group: 'ex-piece', value: 2 });
  assert.equal(loaded.looks[2].on, true);
  loaded.looks[0].on = false;
  ex.press('ex-reset', {});
  assert.equal(loaded.looks[0].on, true, 'Reset shows every field');
  // The overlay off: Fields have nothing to show (the reason said) and the view is assembled,
  // so the overlay coming back does not explode it by itself.
  overlay.drawn = () => false;
  assert.equal(ex.active(), false);
  assert.match(ex.menuContext().why, /overlay on first/);
  assert.equal(ex.menuContext().exploded, false);
  overlay.drawn = () => true;
  assert.equal(ex.active(), false, 'not exploded again by itself');
});

test('the page\'s side: a grip inside a piece moves and turns that piece only; elsewhere it is the model\'s', () => {
  const { ex, view, R } = setup();
  ex.set(true, { source: 'bands', bands: 3, layout: 'row', now: true });
  const model = M.translation(0, 1, -1);
  ex.draw(view, model);
  const before = [0, 1, 2].map((i) => Float32Array.from(ex.world(i)));
  // The hand at the first piece's centre.
  const hand0 = M.translation(before[0][12], before[0][13], before[0][14]);
  assert.equal(ex.grab('right', hand0, model, true), true);
  // The hand moves 10 cm up and turns 90° about y: the piece follows.
  const hand1 = M.multiply(M.translation(before[0][12], before[0][13] + 0.1, before[0][14]), M.rotation([0, 1, 0], Math.PI / 2));
  assert.equal(ex.grab('right', hand1, model, false), true);
  R.calls.length = 0;
  ex.draw(view, model);
  const after = [0, 1, 2].map((i) => ex.world(i));
  close(after[0][13], before[0][13] + 0.1, 1e-5, 'moved up');
  closeArr(Array.from(EX.quatFromMatrix(after[0])), Array.from(EX.quatFromMatrix(M.rotation([0, 1, 0], Math.PI / 2))), 1e-5, 'turned');
  closeArr(Array.from(after[1]), Array.from(before[1]), 1e-6, 'the others stay');
  // Another hand's continuing grip is not this piece's.
  assert.equal(ex.grab('left', hand1, model, false), false);
  ex.release();
  // A grip away from every piece: the model's.
  assert.equal(ex.grab('right', M.translation(5, 5, 5), model, true), false);
  // While animating nothing is grabbed.
  ex.press('ex-go', { id: 'ex-go' });
  assert.equal(ex.grab('right', hand0, model, true), false);
  // Reset: back to the layout.
  ex.set(true, { now: true });
  ex.press('ex-reset', {});
  ex.draw(view, model);
  closeArr(Array.from(ex.world(0)), Array.from(before[0]), 1e-6, 'reset');
});

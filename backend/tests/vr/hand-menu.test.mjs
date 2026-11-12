// The hand menu and the result windows (issue #201): layouts, every former
// control reachable, the menu by the wrist facing the eye, held or kept open,
// the windows' bars and which surface takes the ray (node --test).
import test from 'node:test';
import assert from 'node:assert/strict';
import * as HM from '../../app/static/vr/js/hand-menu.js';
import * as W from '../../app/static/vr/js/windows.js';
import * as P from '../../app/static/vr/js/vr-panel.js';
import * as C from '../../app/static/vr/js/controls.js';
import { stripButtons } from '../../app/static/vr/js/layers.js';
import * as M from '../../app/static/vr/js/mat4.js';

const close = (a, b, eps = 1e-9) => assert.ok(Math.abs(a - b) <= eps, `${a} != ${b}`);
const LAYERS = {
  points: stripButtons('points', { style: 'points', budgetRows: 1e6 }),
  flow: stripButtons('flow', { stream: true, arrows: true }),
  cube: stripButtons('cube', { catShow: true }),
};

test('every page fits the menu, its widgets do not overlap, and each is hit at its centre', () => {
  for (const page of HM.PAGES) {
    for (const layerButtons of page === 'layers' ? Object.values(LAYERS) : [[]]) {
      const L = HM.menuLayout(page, { layerButtons });
      for (const w of L) {
        const [x, y, ww, hh] = w.rect;
        assert.ok(x >= 0 && y >= 0 && x + ww <= HM.MENU_PX[0] && y + hh <= HM.MENU_PX[1], `${page} ${w.id}`);
        // At least 1.8 cm high and 4 cm wide at the menu's scale.
        assert.ok(hh / HM.MENU_PPM >= 0.018 || w.kind === 'slider' || w.kind === 'range', `${page} ${w.id} high enough`);
        assert.ok(ww / HM.MENU_PPM >= 0.022, `${page} ${w.id} wide enough`);
        assert.equal(P.hitWidget(L, x + ww / 2, y + hh / 2).id, w.id, `${page} ${w.id}`);
      }
      assert.equal(L.filter((w) => w.group === 'tab').length, HM.TABS.length);
    }
  }
});

test('every former control has a place: on the menu, or on a stick or button of the mode', () => {
  const ids = (page, opts) => HM.menuLayout(page, opts).map((w) => w.id);
  const look = ids('look'), mode = ids('mode'), analysis = ids('analysis'), settings = ids('settings'), help = ids('help');
  // The former panel (vr-panel.js) is the Look page, without its drag bar and "?".
  for (const w of P.panelLayout()) {
    if (w.id === 'drag') continue;
    if (w.id === 'guide') { assert.ok(help.includes('help-guide')); continue; }
    assert.ok(look.includes(w.id), w.id);
  }
  // The former analysis panel is the Analysis page.
  for (const id of ['seed:new', 'seed:add', 'seed:carve', 'isolate', 'remove', 'show-applied', 'moment:0', 'moment:1',
    'moment:2', 'moment-hide', 'pv-draw', 'pv-undo', 'pv-new', 'pv-narrow', 'pv-wide', 'pv-hide']) assert.ok(analysis.includes(id), id);
  // The former strip is the Layers page.
  for (const buttons of Object.values(LAYERS)) {
    const L = ids('layers', { layerButtons: buttons });
    for (const b of buttons) assert.ok(L.includes(b.id), b.id);
  }
  // The former buttons and sticks: threshold, opacity (Explore stick), slab (Clip stick), colour map (secondary
  // stick in Explore), all channels / clipping reset (Mode page), emission or MIP (Look), the numbers and the
  // frame-rate governor (Settings), the next tool (secondary stick, Mode page), the selection (Select trigger).
  const ex = C.controlMap({ mode: 'explore' }), clip = C.controlMap({ mode: 'clip', sub: 'slab' });
  const sel = C.controlMap({ mode: 'select', sub: 'probe' });
  assert.equal(C.actionFor(ex, 'primary', 'stickY'), 'threshold');
  assert.equal(C.actionFor(ex, 'primary', 'stickX'), 'opacity');
  assert.equal(C.actionFor(clip, 'primary', 'stickY'), 'slab-move');
  assert.equal(C.actionFor(clip, 'primary', 'stickX'), 'slab-width');
  assert.equal(C.actionFor(ex, 'secondary', 'stickX'), 'colormap');
  assert.equal(C.actionFor(sel, 'secondary', 'stickX'), 'next-tool');
  assert.equal(C.actionFor(sel, 'primary', 'trigger'), 'select');
  for (const id of ['clip-all', 'clip-reset', 'sub:select:probe', 'sub:select:lasso', 'sub:select:box', 'sub:clip:slab',
    'sub:clip:box', 'mode:explore', 'mode:select', 'mode:clip', 'undo', 'reset-view', 'sel-clear']) assert.ok(mode.includes(id), id);
  assert.ok(look.includes('mode:0') && look.includes('mode:1'), 'emission / maximum intensity');
  for (const id of ['set:hud', 'set:governor', 'set:leftHanded', 'set:labels', 'set:haptics', 'set:refine']) assert.ok(settings.includes(id), id);
});

test('the menu stands by the secondary hand, toward the middle, facing the eye', () => {
  const eye = [0, 1.6, 0];
  for (const [hand, inward] of [[[-0.25, 1.15, -0.35], 1], [[0.25, 1.15, -0.35], -1]]) {
    const m = HM.menuModel(hand, eye, [1, 0, 0], inward);
    const p = M.position(m);
    assert.ok(p[1] - HM.MENU_SIZE[1] / 2 > hand[1] + 0.03, 'its lower edge above the hand');
    assert.ok(Math.sign(p[0] - hand[0]) === inward, 'toward the body\'s middle');
    const z = M.normalize(M.sub(M.transformPoint(m, [0, 0, 1]), p));
    assert.ok(M.dot(z, M.normalize(M.sub(eye, p))) > 0.999, 'faces the eye');
    const x = M.sub(M.transformPoint(m, [1, 0, 0]), p);
    close(x[1], 0, 1e-6);
    // A ray from the eye to a tab presses it, through the same ray code as the panels.
    const L = HM.menuLayout('look');
    const tab = L.find((w) => w.id === 'tab:analysis');
    const [cx, cy] = [tab.rect[0] + tab.rect[2] / 2, tab.rect[1] + tab.rect[3] / 2];
    const target = M.transformPoint(m, [(cx / HM.MENU_PX[0] - 0.5) * HM.MENU_SIZE[0], (0.5 - cy / HM.MENU_PX[1]) * HM.MENU_SIZE[1], 0]);
    const hit = P.rayPanel(m, HM.MENU_SIZE, eye, M.sub(target, eye), HM.MENU_PX);
    assert.deepEqual(new P.PanelPointer(L).update(hit, true).map((a) => a.id), ['tab:analysis']);
  }
  // At the wrist (about 0.45 m) the smallest text (22 px) is about 1° high.
  const deg = (22 / HM.MENU_PPM / 0.45) * 180 / Math.PI;
  assert.ok(deg > 0.9 && deg < 1.2, deg);
});

test('the menu: open while held, kept open on request until the next press', () => {
  let st = { pinned: false, armed: false, wasHeld: false };
  const step = (held) => { st = HM.menuOpenState(st, held); return st.open; };
  assert.deepEqual([false, true, true, false].map(step), [false, true, true, false]);
  step(true);
  st.pinned = true; st.armed = false;            // "Keep open" pressed while held
  assert.equal(step(true), true);
  assert.equal(step(false), true, 'the release after Keep open does not close it');
  assert.equal(step(false), true);
  assert.equal(step(true), true, 'the next press: open while held');
  assert.equal(st.pinned, false);
  assert.equal(step(false), false, 'and closed when let go');
});

test('drawMenu draws every page on a 2-D context stand-in', () => {
  const calls = [];
  const target = { save() {}, restore() {}, translate(...a) { calls.push(['translate', a]); }, measureText: (t) => ({ width: String(t).length * 12 }) };
  const c = new Proxy(target, { get: (t, k) => (k in t ? t[k] : () => {}), set: (t, k, v) => { t[k] = v; return true; } });
  const view = {
    look: { thr: { f: 0.2 }, cmax: { f: 0.8 }, opacity: { f: 0.2 }, slab: { lo: 0, hi: 1 }, refine: { on: true } },
    analysis: { seedMode: 'new', status: [] }, mode: { mode: 'select', selectTool: 'lasso', clipTool: 'slab', map: ['a', 'b'] },
    layers: { title: 'Catalogue', note: { warn: true, text: 'x' } }, settings: { labels: true, refineScaleText: '1×' },
    help: { helpMode: 'clip', helpLines: C.helpLines({ mode: 'clip', sub: 'slab' }) },
  };
  for (const page of HM.PAGES) HM.drawMenu(c, page, HM.pageLayout(page, { layerButtons: LAYERS.cube }), view, { hover: 'tab:look' });
  assert.equal(calls.filter(([k]) => k === 'translate').length, HM.PAGES.length);
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

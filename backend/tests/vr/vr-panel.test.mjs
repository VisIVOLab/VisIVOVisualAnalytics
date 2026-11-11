// The in-VR panel and the page's stored settings (issue #172):
// node --test backend/tests/vr/vr-panel.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';
import * as P from '../../app/static/vr/js/vr-panel.js';
import * as S from '../../app/static/vr/js/settings.js';
import * as M from '../../app/static/vr/js/mat4.js';

const close = (a, b, eps = 1e-6) => assert.ok(Math.abs(a - b) <= eps, `${a} != ${b}`);
const centre = (w) => ({ x: w.rect[0] + w.rect[2] / 2, y: w.rect[1] + w.rect[3] / 2 });
const L = P.panelLayout();
const byId = (id) => L.find((w) => w.id === id);

test('the layout fits the canvas, widgets do not overlap, targets are large', () => {
  const [W, H] = P.PANEL_PX;
  for (const w of L) {
    const [x, y, ww, hh] = w.rect;
    assert.ok(x >= 0 && y >= 0 && x + ww <= W && y + hh <= H, w.id);
    // At least 2.5 cm high on the 0.375 m panel (about 2° at 0.75 m).
    assert.ok(hh / H * P.PANEL_SIZE[1] >= 0.025, w.id);
  }
  for (let i = 0; i < L.length; i++) {
    for (let j = i + 1; j < L.length; j++) {
      const a = L[i].rect, b = L[j].rect;
      const overlap = a[0] < b[0] + b[2] && b[0] < a[0] + a[2] && a[1] < b[1] + b[3] && b[1] < a[1] + a[3];
      assert.ok(!overlap, `${L[i].id} overlaps ${L[j].id}`);
    }
  }
  for (const id of ['cmap-prev', 'cmap-next', 'thr', 'thr-reset', 'thrsrc:reduced', 'thrsrc:cube', 'cmax', 'opacity',
    'slab', 'slab-all', 'refine', 'tool:probe', 'tool:lasso', 'tool:box', 'sel-clear', 'mode:0', 'mode:1', 'drag']) {
    assert.ok(byId(id), id);
  }
});

test('every widget is hit at its centre, and nothing between rows', () => {
  for (const w of L) {
    const c = centre(w);
    assert.equal(P.hitWidget(L, c.x, c.y).id, w.id);
  }
  const thr = byId('thr');
  assert.equal(P.hitWidget(L, thr.rect[0] + 10, thr.rect[1] + thr.rect[3] + 4), null);   // the gap below
  assert.equal(P.hitWidget(L, 10, thr.rect[1] + 10), null);                              // the label column
});

test('a ray through the quad lands on the canvas pixel it points at', () => {
  // A panel 1 m in front of the origin, facing it, turned and moved.
  const anchor = { p: [0.2, 1.4, -0.75], yaw: 0.3 };
  const off = { right: -0.5, up: 0.1, fwd: -0.2, yaw: 0.7 };
  const model = P.panelModel(anchor, off);
  const [W, H] = P.PANEL_PX;
  for (const [cx, cy] of [[W / 2, H / 2], [100, 50], [W - 10, H - 10], [700, 400]]) {
    // The world point of canvas (cx, cy), and a ray to it from a viewer.
    const local = [(cx / W - 0.5) * P.PANEL_SIZE[0], (0.5 - cy / H) * P.PANEL_SIZE[1], 0];
    const target = M.transformPoint(model, local);
    const eye = [0.1, 1.5, 0.3];
    const hit = P.rayPanel(model, P.PANEL_SIZE, eye, M.sub(target, eye));
    assert.ok(hit);
    close(hit.x, cx, 1e-3);   // mat4 is float32
    close(hit.y, cy, 1e-3);
    close(hit.t, 1, 1e-5);
    hit.point.forEach((v, i) => close(v, target[i], 1e-5));
  }
  // Away from it, past its edge, or parallel to it: no hit.
  const c = M.transformPoint(model, [0, 0, 0]);
  const eye = [0.1, 1.5, 0.3];
  assert.equal(P.rayPanel(model, P.PANEL_SIZE, eye, M.sub(eye, c)), null);
  const edge = M.transformPoint(model, [P.PANEL_SIZE[0] * 0.51, 0, 0]);
  assert.equal(P.rayPanel(model, P.PANEL_SIZE, eye, M.sub(edge, eye)), null);
  const inPlane = M.sub(M.transformPoint(model, [1, 0, 0]), c);
  assert.equal(P.rayPanel(model, P.PANEL_SIZE, M.transformPoint(model, [-1, 0, 0]), inPlane), null);
});

test('panelModel and offsetFor are inverse, and the panel faces the eye', () => {
  const anchor = { p: [0, 1.3, -0.75], yaw: -0.8 };
  const eye = [0.3, 1.6, 0.2];
  const p = [-0.4, 1.2, -0.5];
  const off = P.offsetFor(anchor, p, eye);
  const m = P.panelModel(anchor, off);
  M.position(m).forEach((v, i) => close(v, p[i], 1e-6));
  const n = M.normalize(M.sub(M.transformPoint(m, [0, 0, 1]), M.position(m)));
  const toEye = M.normalize([eye[0] - p[0], 0, eye[2] - p[2]]);
  close(M.dot(n, toEye), 1, 1e-6);
  // The default: in front of where the viewer stood, facing them roughly.
  const a0 = { p: [0, 1.4, -0.75], yaw: 0 };
  const m0 = P.panelModel(a0, P.DEFAULT_OFFSET);
  const d = M.length(M.sub(M.position(m0), [0, 1.52, 0]));
  assert.ok(d > 0.6 && d < 1.0, `default distance ${d}`);
  const n0 = M.normalize(M.sub(M.transformPoint(m0, [0, 0, 1]), M.position(m0)));
  const e0 = M.normalize(M.sub([0, M.position(m0)[1], 0], M.position(m0)));
  assert.ok(M.dot(n0, e0) > 0.95);
});

test('pointer: a press on a button fires once; a release is not a selection', () => {
  const ptr = new P.PanelPointer(L);
  const b = centre(byId('thr-reset'));
  assert.deepEqual(ptr.update(b, false), []);
  assert.equal(ptr.hover, 'thr-reset');
  assert.ok(ptr.takesRay());
  const a = ptr.update(b, true);
  assert.equal(a.length, 1);
  assert.equal(a[0].type, 'press');
  assert.equal(a[0].id, 'thr-reset');
  assert.deepEqual(ptr.update(b, true), []);   // held: no repeat
  assert.deepEqual(ptr.update(b, false), []);
  assert.ok(ptr.owns);                         // the release frame still belongs to the panel
  ptr.update(null, false);
  assert.ok(!ptr.owns && !ptr.takesRay());
});

test('pointer: a press off the panel is not the panel\'s, even if the ray then moves onto it', () => {
  const ptr = new P.PanelPointer(L);
  ptr.update(null, true);
  assert.ok(!ptr.owns);
  const a = ptr.update(centre(byId('sel-clear')), true);
  assert.deepEqual(a, []);                     // a held trigger dragged onto a button does not press it
});

test('pointer: a slider follows the ray while held, even off its row, and clamps', () => {
  const ptr = new P.PanelPointer(L);
  const w = byId('opacity');
  const y = w.rect[1] + 20;
  const x0 = w.rect[0] + 18, len = w.rect[2] - 36;
  let a = ptr.update({ x: x0 + 0.25 * len, y }, true);
  assert.equal(a[0].type, 'slide');
  close(a[0].value, 0.25, 1e-9);
  a = ptr.update({ x: x0 + 0.75 * len, y: y + 200 }, true);       // another row: still this slider
  close(a[0].value, 0.75, 1e-9);
  a = ptr.update({ x: 5, y }, true);
  close(a[0].value, 0, 1e-12);
  a = ptr.update(null, true);                                      // the ray left the panel: kept
  assert.deepEqual(a, []);
  assert.ok(ptr.takesRay());
  ptr.update(null, false);
  assert.equal(ptr.active, null);
});

test('pointer: the channel range moves its nearer end and keeps them apart', () => {
  let range = [0.2, 0.6];
  const ptr = new P.PanelPointer(L, { rangeValue: () => range, minGap: 0.05 });
  const w = byId('slab');
  const x0 = w.rect[0] + 18, len = w.rect[2] - 36, y = w.rect[1] + 28;
  let a = ptr.update({ x: x0 + 0.1 * len, y }, true);           // near the low end
  assert.equal(a[0].type, 'range');
  a[0].value.forEach((v, i) => close(v, [0.1, 0.6][i], 1e-9));
  range = a[0].value;
  a = ptr.update({ x: x0 + 0.9 * len, y }, true);               // dragged past the high end
  a[0].value.forEach((v, i) => close(v, [0.55, 0.6][i], 1e-9));
  ptr.update(null, false); ptr.update(null, false);
  range = [0.2, 0.6];
  a = ptr.update({ x: x0 + 0.5 * len, y }, true);               // nearer the high end
  a[0].value.forEach((v, i) => close(v, [0.2, 0.5][i], 1e-9));
});

test('the cube in front of the panel takes the ray; behind or beside it, the panel does', () => {
  // Panel 1 m ahead (z = -1), facing the viewer at the origin.
  const panel = M.translation(0, 0, -1);
  const o = [0, 0, 0], d = [0, 0, -1];
  const hit = P.rayPanel(panel, P.PANEL_SIZE, o, d);
  assert.ok(hit);
  const extent = [1, 1, 0.5];
  // A cube of 0.6 m at 0.5 m: in front of the panel.
  const front = M.multiply(M.translation(0, 0, -0.5), M.scaling(0.6));
  assert.equal(P.panelBehindCube(hit.t, front, extent, o, d), true);
  // The same cube 2 m away: behind the panel.
  const behind = M.multiply(M.translation(0, 0, -2), M.scaling(0.6));
  assert.equal(P.panelBehindCube(hit.t, behind, extent, o, d), false);
  // Off to the side: the ray misses it.
  const side = M.multiply(M.translation(1.5, 0, -0.5), M.scaling(0.6));
  assert.equal(P.panelBehindCube(hit.t, side, extent, o, d), false);
  // Clipped away where the ray passes: the drawn box is the clipping box.
  assert.equal(P.panelBehindCube(hit.t, front, extent, o, d, [0.8, 0, 0], [1, 1, 1]), false);
  // Behind the viewer: not in the way.
  const back = M.multiply(M.translation(0, 0, 1), M.scaling(0.6));
  assert.equal(P.panelBehindCube(hit.t, back, extent, o, d), false);
  // The controller inside the box (a cube scaled up around the user): the panel wins.
  const around = M.multiply(M.translation(0, 0, -0.2), M.scaling(4));
  assert.equal(P.panelBehindCube(hit.t, around, extent, o, d), false);
});

test('pointer: a press released off the panel leaves the next press on it working', () => {
  const ptr = new P.PanelPointer(L);
  ptr.update(null, true);       // mouse down off the panel (the camera drag)
  ptr.update(null, false);      // and up
  const a = ptr.update(centre(byId('sel-clear')), true);
  assert.equal(a.length, 1);
  assert.equal(a[0].id, 'sel-clear');
});

test('pointer: the title bar drags', () => {
  const ptr = new P.PanelPointer(L);
  const h = centre(byId('drag'));
  assert.equal(ptr.update(h, true)[0].type, 'drag-start');
  assert.equal(ptr.update(null, true)[0].type, 'drag');
  assert.equal(ptr.update(null, false)[0].type, 'drag-end');
});

test('drawPanel runs on a 2-D context stand-in', () => {
  const calls = { fillText: 0 };
  const ctx = new Proxy({}, {
    get(t, k) {
      if (k === 'measureText') return (s) => ({ width: String(s).length * 12 });
      if (k in t) return t[k];
      return (...args) => { if (k === 'fillText') { calls.fillText++; assert.ok(Number.isFinite(args[1]) && Number.isFinite(args[2])); } };
    },
    set(t, k, v) { t[k] = v; return true; },
  });
  P.drawPanel(ctx, L, {
    title: 'VisIVO VR · HCG44.fits', info: ['a', 'b'], cmap: 'viridis', mode: 0, thr: { f: 0.3, text: '1 mJy' },
    thrSource: 'reduced', cmax: { f: 0.8, text: '2' }, opacity: { f: 0.2, text: '0.4' },
    slab: { lo: 0.1, hi: 0.9, text: 'ch 3-40' }, refine: { on: true, text: 'refined' }, tool: 'lasso', status: 'x', hint: 'y',
  }, { hover: 'thr' });
  assert.ok(calls.fillText > 15);
});

// ── Settings ────────────────────────────────────────────────────────────────

class MemStore {
  constructor() { this.m = new Map(); }
  getItem(k) { return this.m.has(k) ? this.m.get(k) : null; }
  setItem(k, v) { this.m.set(k, String(v)); }
  removeItem(k) { this.m.delete(k); }
}

test('settings round-trip and fall back to the defaults', () => {
  const st = new MemStore();
  assert.deepEqual(S.loadSettings(st), S.sanitizeSettings(null));
  const s = { colormap: 'magma', thrSource: 'cube', refine: { enabled: false, scale: 0.6, quality: 85, depth: false },
    panel: { right: -0.4, up: 0.1, fwd: -0.2, yaw: 0.5 }, tool: 'box' };
  assert.ok(S.saveSettings(st, s));
  assert.deepEqual(S.loadSettings(st, { colormaps: ['viridis', 'magma'], tools: ['probe', 'lasso', 'box'] }), s);
  // Unknown or broken values: replaced one by one, never thrown.
  st.setItem(S.SETTINGS_KEY, JSON.stringify({ v: 1, s: { colormap: 'nope', thrSource: 'x', refine: { scale: 7, enabled: 'yes' },
    panel: { right: 1e9, up: 0, fwd: 0, yaw: 0 }, tool: 'wand', token: 'secret' } }));
  const got = S.loadSettings(st, { colormaps: ['viridis'], tools: ['probe'] });
  assert.equal(got.colormap, 'viridis');
  assert.equal(got.thrSource, 'reduced');
  assert.deepEqual(got.refine, S.DEFAULT_SETTINGS.refine);
  assert.equal(got.panel, null);
  assert.equal(got.tool, 'probe');
  assert.ok(!('token' in got));
  st.setItem(S.SETTINGS_KEY, '{not json');
  assert.deepEqual(S.loadSettings(st), S.sanitizeSettings(null));
  st.setItem(S.SETTINGS_KEY, JSON.stringify({ v: 99, s }));          // another version: ignored
  assert.equal(S.loadSettings(st).colormap, 'viridis');
});

test('a panel let go far away is clamped and then kept, not dropped', () => {
  const st = new MemStore();
  const far = { right: -7.5, up: 4, fwd: 0.3, yaw: 9 };
  const off = S.clampPanelOffset(far);
  assert.deepEqual(off, { right: -S.PANEL_LIMITS.right, up: S.PANEL_LIMITS.up, fwd: 0.3, yaw: S.PANEL_LIMITS.yaw });
  assert.deepEqual(S.clampPanelOffset({ right: NaN, up: 0.1 }), { right: 0, up: 0.1, fwd: 0, yaw: 0 });
  // Unclamped, the saved place would be lost on the next load; clamped, it is kept.
  S.saveSettings(st, { ...S.DEFAULT_SETTINGS, panel: far });
  assert.equal(S.loadSettings(st).panel, null);
  S.saveSettings(st, { ...S.DEFAULT_SETTINGS, panel: off });
  assert.deepEqual(S.loadSettings(st).panel, off);
});

test('settings: a storage that throws or is missing is not an error', () => {
  const bad = { getItem() { throw new Error('denied'); }, setItem() { throw new Error('quota'); } };
  assert.deepEqual(S.loadSettings(bad), S.sanitizeSettings(null));
  assert.equal(S.saveSettings(bad, S.DEFAULT_SETTINGS), false);
  assert.deepEqual(S.loadSettings(null), S.sanitizeSettings(null));
  assert.equal(S.saveSettings(null, S.DEFAULT_SETTINGS), false);
});

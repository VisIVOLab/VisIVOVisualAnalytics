// Composition layers for the text in the headset (issue #226, point 2).
// node --test backend/tests/vr/xr-layers.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';
import * as XL from '../../app/static/vr/js/xr-layers.js';
import * as M from '../../app/static/vr/js/mat4.js';
import * as P from '../../app/static/vr/js/vr-panel.js';
import * as RO from '../../app/static/vr/js/readout.js';

/** A rotation matrix from a quaternion [x, y, z, w]. */
function quatMatrix([x, y, z, w]) {
  return [
    [1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w)],
    [2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w)],
    [2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)],
  ];
}

test('a panel model splits into the rigid pose and the size a quad layer takes', () => {
  for (const [axis, angle] of [[[0, 1, 0], 0.7], [[1, 0, 0], -2.5], [M.normalize([1, 2, -1]), 3.0], [[0, 0, 1], Math.PI]]) {
    const s = 1.7;
    const m = M.multiply(M.translation(0.3, 1.2, -0.8), M.multiply(M.rotation(axis, angle), M.scaling(s)));
    const r = XL.rigidFromModel(m);
    assert.deepEqual(r.position.map((v) => +v.toFixed(6)), [0.3, 1.2, -0.8]);
    assert.ok(Math.abs(r.scale[0] - s) < 1e-6 && Math.abs(r.scale[1] - s) < 1e-6);
    const R = quatMatrix(r.orientation);
    for (let c = 0; c < 3; c++) for (let k = 0; k < 3; k++) {
      assert.ok(Math.abs(R[c][k] - m[4 * c + k] / s) < 1e-5, `axis ${axis} angle ${angle} column ${c}`);
    }
    assert.ok(Math.abs(Math.hypot(...r.orientation) - 1) < 1e-9);
  }
});

test('the readout card as a quad: same plane and size as the panel the ray hits', () => {
  const eye = [0, 1.6, 0];
  const m = RO.readoutModel([0.2, 1.3, -1.6], eye);
  const r = XL.rigidFromModel(m);
  const w = RO.READOUT_SIZE[0] * r.scale[0], h = RO.READOUT_SIZE[1] * r.scale[1];
  // A ray at the quad's corner region (rigid pose + size) hits the panel (model + size).
  const R = quatMatrix(r.orientation);
  const corner = [0, 1, 2].map((k) => r.position[k] + R[0][k] * (0.45 * w) + R[1][k] * (0.45 * h));
  const hit = P.rayPanel(m, RO.READOUT_SIZE, eye, M.sub(corner, eye), RO.READOUT_PX);
  assert.ok(hit, 'the ray finds the same plane');
  const u = hit.x / RO.READOUT_PX[0], v = hit.y / RO.READOUT_PX[1];
  assert.ok(Math.abs(u - 0.95) < 1e-3 && Math.abs(v - 0.05) < 1e-3, `hit at ${u}, ${v}`);
});

/** A fake binding, session and GL that record what the layers do. */
function fakes({ failCreate = false } = {}) {
  const log = [];
  let n = 0;
  const binding = {
    createQuadLayer(init) {
      if (failCreate) throw new Error('layers not enabled');
      const layer = { id: ++n, init, needsRedraw: false, destroyed: false, destroy() { this.destroyed = true; } };
      log.push(['create', layer.id, init.viewPixelWidth, init.viewPixelHeight]);
      return layer;
    },
    getSubImage(layer) {
      // As the spec: only a layer of the session's active array.
      if (!session.active.includes(layer)) throw new TypeError('the layer is not in the session\'s active layers');
      log.push(['sub', layer.id]);
      return { colorTexture: { layer: layer.id }, viewport: { x: 0, y: 0, width: layer.init.viewPixelWidth, height: layer.init.viewPixelHeight } };
    },
  };
  // A render state set now applies at the next frame (tick()).
  const session = { states: [], active: [], pending: null,
    updateRenderState(s) { this.states.push(s.layers.map((l) => (l.base ? 'base' : l.id))); this.pending = s.layers; },
    tick() { if (this.pending) { this.active = this.pending; this.pending = null; } } };
  const gl = new Proxy({ TEXTURE_2D: 1, UNPACK_FLIP_Y_WEBGL: 2, UNPACK_PREMULTIPLY_ALPHA_WEBGL: 3, UNPACK_ALIGNMENT: 4, RGBA: 5, UNSIGNED_BYTE: 6 }, {
    get: (t, k) => (k in t ? t[k] : (...a) => { log.push([k, ...a]); }),
  });
  const q = new XL.QuadLayers({ binding, session, gl, space: {}, makeTransform: (p, o) => ({ p, o }) });
  return { q, log, session };
}

const handle = (w, h) => ({ canvas: { width: w, height: h }, version: 1 });

test('quads: made once per overlay, copied only when the canvas changed, array set only when the set changes', () => {
  const { q, log, session } = fakes();
  q.setBase({ base: true });
  const menu = handle(1024, 700), hud = handle(640, 600);
  const at = M.translation(0, 1.4, -0.5);
  const items = () => [{ id: 'hud', model: at, size: [0.16, 0.15], handle: hud }, { id: 'menu', model: at, size: [0.4, 0.27], handle: menu }];
  const frame = (list) => { session.tick(); return q.frame({}, list); };
  assert.equal(frame(items()), true);
  assert.deepEqual(session.states, [['base', 1, 2]]);
  const copies = () => log.filter((e) => e[0] === 'texSubImage2D').length;
  assert.equal(copies(), 0, 'not active yet: copied from the next frame');
  frame(items());
  assert.equal(copies(), 2);
  // The same frame again: no copy, no new array.
  frame(items());
  assert.equal(copies(), 2);
  assert.equal(session.states.length, 1);
  // The menu redrawn: one copy of it alone.
  menu.version++;
  frame(items());
  assert.equal(copies(), 3);
  // The compositor lost a quad's content: copied again.
  q.pool.get('hud').layer.needsRedraw = true;
  frame(items());
  assert.equal(copies(), 4);
  q.pool.get('hud').layer.needsRedraw = false;
  // The menu closed: the array without it; opened again: copied again (fresh) once active, same layer.
  frame([items()[0]]);
  assert.deepEqual(session.states.at(-1), ['base', 1]);
  frame(items());
  assert.deepEqual(session.states.at(-1), ['base', 1, 2]);
  assert.equal(copies(), 4);
  frame(items());
  assert.equal(copies(), 5);
  assert.equal(log.filter((e) => e[0] === 'create').length, 2, 'bounded: one layer per overlay id');
  // The pixel store is put back after each copy (the page's uploads assume no flip).
  const flips = log.filter((e) => e[0] === 'pixelStorei' && e[1] === 2).map((e) => e[2]);
  assert.deepEqual(flips.slice(-2), [true, false]);
  // Sizes follow the model's scale.
  frame([{ id: 'hud', model: M.multiply(at, M.scaling(2)), size: [0.16, 0.15], handle: hud }]);
  assert.ok(Math.abs(q.pool.get('hud').layer.width - 0.32) < 1e-9);
  // A browser whose quad spans ±width (the Quest Browser, issue #267): half of it.
  q.extent = 0.5;
  frame([{ id: 'hud', model: M.multiply(at, M.scaling(2)), size: [0.16, 0.15], handle: hud }]);
  assert.ok(Math.abs(q.pool.get('hud').layer.width - 0.16) < 1e-9 && Math.abs(q.pool.get('hud').layer.height - 0.15) < 1e-9);
  assert.match(q.describe(), /±width/);
  q.extent = 1;
  // A smaller eye buffer: the projection layer replaced at once, the quads kept.
  q.setBase({ base: true, second: true });
  assert.deepEqual(session.states.at(-1), ['base', 1]);
  // A canvas resized: its layer made again.
  frame([{ id: 'hud', model: at, size: [0.16, 0.15], handle: handle(320, 300) }]);
  assert.equal(log.filter((e) => e[0] === 'create').length, 3);
  frame([{ id: 'hud', model: at, size: [0.16, 0.15], handle: handle(320, 300) }]);
  assert.equal(copies(), 6, 'the new layer copied once active');
  q.destroy();
  assert.equal(q.pool.size, 0);
});

test('quads: any failure turns them off and the page draws the overlays itself', () => {
  const { q, session } = fakes({ failCreate: true });
  let told = '';
  q.onFail = (w) => { told = w; };
  q.setBase({ base: true });
  assert.equal(q.frame({}, [{ id: 'menu', model: M.identity(), size: [0.4, 0.3], handle: handle(10, 10) }]), false);
  assert.equal(q.active, false);
  assert.match(told, /layers not enabled/);
  assert.deepEqual(session.states.at(-1), ['base'], 'back to the projection layer alone');
  assert.equal(q.frame({}, []), false, 'stays off for the session');
  assert.match(q.describe(), /in the scene/);
});

test('layers are asked for only where the browser has them', () => {
  assert.equal(XL.layersAvailable({ enabledFeatures: ['local-floor'] }, { XRWebGLBinding: function () {} }), false);
  assert.equal(XL.layersAvailable({ enabledFeatures: ['layers'] }, { XRWebGLBinding: function () {} }), true);
  assert.equal(XL.layersAvailable({}, { XRWebGLBinding: function () {} }), true);
  assert.equal(XL.layersAvailable({ enabledFeatures: ['layers'] }, {}), false);
  assert.equal(XL.orderChanged(['a', 'b'], ['a', 'b']), false);
  assert.equal(XL.orderChanged(['a', 'b'], ['b', 'a']), true);
  assert.equal(XL.orderChanged(null, []), true);
});

// The PV path in the headset (issue #191): vertices from the ray, the
// desktop's request, origin and labels, the velocity axis orientation, the
// latest-wins live update and the panel that drives it (node --test).
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { deflateSync } from 'node:zlib';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import * as TP from '../../app/static/vr/js/tools-pv.js';
import * as TPanel from '../../app/static/vr/js/tools-panel.js';
import * as P from '../../app/static/vr/js/vr-panel.js';
import * as M from '../../app/static/vr/js/mat4.js';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const REPO = path.resolve(HERE, '../../..');
const close = (a, b, eps = 1e-9) => assert.ok(Math.abs(a - b) <= eps, `${a} != ${b}`);

test('a box point is the fractional pixel the desktop\'s path takes (pixel centres at integers)', () => {
  const n = [700, 700, 158];
  assert.deepEqual(TP.boxToVertex([0.5 / 700, 0.5 / 700, 0.3], n), [0, 0]);
  const v = TP.boxToVertex([(465.25 + 0.5) / 700, (297.75 + 0.5) / 700, 0.9], n);
  close(v[0], 465.25, 1e-9); close(v[1], 297.75, 1e-9);
  assert.deepEqual(TP.boxToVertex([-0.1, 1.2, 0], n), [0, 699], 'kept inside the cube');
  const b = TP.vertexToBox(v, n);
  close(b[0], (465.25 + 0.5) / 700); close(b[1], (297.75 + 0.5) / 700);
});

test('the PV request uses the route\'s fields; origin and labels are the desktop\'s', () => {
  const src = readFileSync(path.join(REPO, 'backend/app/schemas.py'), 'utf8');
  const start = src.indexOf('class CubePvRequest(BaseModel):');
  const fields = new Set([...src.slice(start, src.indexOf('\nclass ', start + 5)).matchAll(/^ {4}(\w+): /gm)].map((m) => m[1]));
  const req = TP.pvRequest([[443.45, 287.57], [487.45, 307.57]], 1.4);
  assert.deepEqual([...Object.keys(req), 'dataset_id'].sort(), [...fields].sort());
  assert.equal(req.width_pixels, 1);
  assert.equal(TP.pvRequest([[0, 0], [1, 1]], 0).width_pixels, 1);
  const verts = [[443.45, 287.57], [465.45, 297.57], [487.45, 307.57]];
  assert.deepEqual(TP.pvOrigin(verts, 1), { kind: 'pvpath', points: verts, width: 1 });
  assert.equal(TP.pvOrigin([[1, 2]], 1), null);
  assert.deepEqual(TP.pvParams(verts, 3), { Path: '3 points', Width: '3 px', From: '(443.4, 287.6)', To: '(487.4, 307.6)',
    'Measured on': 'a 3-point PV path, 3 px wide' });
});

test('rows go up in spectral coordinate, whatever the sign of CDELT3', () => {
  assert.deepEqual(TP.rowOrder([1, 2, 3, 4]), [0, 1, 2, 3]);
  assert.deepEqual(TP.rowOrder([4, 3, 2, 1]), [3, 2, 1, 0], 'CDELT3 < 0: the last channel at the bottom');
  assert.deepEqual(TP.rowOrder([2, 1, 1, 3]), [1, 2, 0, 3], 'ties keep channel order');
  // Velocities of the rows from the page's per-channel axis.
  assert.deepEqual(TP.rowVelocities([3, 2, 1, 0], [40, 30, 20, 10]), [10, 20, 30, 40]);
  assert.equal(TP.rowVelocities([0, 1], [1]), null);
  assert.equal(TP.rowVelocities([0, 1], [100, null]), null, 'a channel without a velocity: the spectral coordinate instead');
  assert.equal(TP.rowCaption([NaN, 2], 'velocity'), 'velocity');
});

function qt(f32) {
  const raw = Buffer.from(f32.buffer);
  const len = Buffer.alloc(4); len.writeUInt32BE(raw.length);
  return Buffer.concat([len, deflateSync(raw)]).toString('base64');
}

test('the rows\' caption follows the actual direction; ticks are interpolated between rows', async () => {
  const PC = await import('../../app/static/vr/js/product-canvas.js');
  assert.equal(TP.rowCaption([900, 1000, 1100], 'velocity'), 'velocity: increasing upward');
  // A frequency cube: rows in increasing frequency, so the optical velocity falls upward.
  const c = 299792.458, f0 = 1420.405751768;
  const freqs = Array.from({ length: 64 }, (_, i) => 1410 + i * 0.25);
  const vel = freqs.map((f) => c * (f0 / f - 1));             // optical cz: not linear in f
  assert.equal(TP.rowCaption(vel, 'velocity'), 'velocity: decreasing upward');
  const t = PC.rowTicks(vel, 5);
  assert.ok(t.length >= 3);
  for (const k of t) {
    // f is where the value sits among the rows (row centres at (i + 0.5) / n).
    const pos = k.f * vel.length - 0.5, i = Math.floor(pos), frac = pos - i;
    const at = vel[i] + (vel[i + 1] - vel[i]) * frac;
    assert.ok(Math.abs(at - Number(k.text)) < 1e-6 * Math.abs(at) + 1e-9, `${at} vs ${k.text}`);
  }
  // A linear axis gives what a linear map would.
  assert.deepEqual(PC.rowTicks([null, 10, 20]), []);
  for (const k of PC.rowTicks([0, 10, null, 30, 40], 4)) assert.ok(Number.isFinite(k.f));
  const lin = PC.rowTicks([0, 10, 20, 30, 40], 4);
  for (const k of lin) assert.ok(Math.abs((k.f * 5 - 0.5) * 10 - Number(k.text)) < 1e-9);
});

test('the PV answer decodes as the backend encodes it (qt-zlib, channel-major)', async () => {
  const depth = 3, n = 2;
  const pv = new Float32Array([1, 2, 3, 4, 5, 6]);        // [channel][sample]
  const spec = new Float64Array([30, 20, 10]);            // CDELT3 < 0
  const r = { num_samples: n, depth, compression: 'qt-zlib', data_base64: qt(pv),
    positions_base64: qt(new Float32Array([0, 1])), positions_arcsec_base64: qt(new Float32Array([0, 8])),
    spatial_unit: 'arcsec', spectral_coordinates_base64: Buffer.from(spec.buffer).toString('base64'),
    spectral_axis_unit: 'm/s', spectral_axis_type: 'VELO-HEL', bunit: 'JY/BEAM' };
  const d = await TP.decodePv(r);
  assert.deepEqual([...d.values], [...pv]);
  assert.deepEqual([...d.positions], [0, 8]);
  assert.equal(d.positionUnit, 'arcsec');
  assert.deepEqual(TP.rowOrder(d.spectral), [2, 1, 0]);
  await assert.rejects(TP.decodePv({ ...r, depth: 4 }), /expected 8/);
});

function pvHarness() {
  const answers = [];
  const calls = [];
  const recipes = [];
  const spec = new Float64Array([1, 2]);
  const answer = () => ({ valid: true, num_samples: 2, depth: 2, compression: 'qt-zlib',
    data_base64: qt(new Float32Array([1, 2, 3, 4])), positions_base64: qt(new Float32Array([0, 1])),
    positions_arcsec_base64: '', spectral_coordinates_base64: Buffer.from(spec.buffer).toString('base64'), bunit: 'JY/BEAM' });
  const backend = { _json: (m, url, body) => { calls.push(body); return new Promise((res) => answers.push(() => res(answer()))); } };
  const sel = { sel: { tool: 'probe' }, pickBox: () => ({ hit: { p: [0.5, 0.5, 0.5] } }),
    record: (product, params, origin, replaceId) => {
      const id = replaceId || `vr-pv-${recipes.length + 1}`;
      const i = recipes.findIndex((r) => r.id === id);
      const r = { id, product: { ...product, id }, params, origin };
      if (i >= 0) recipes[i] = r; else recipes.push(r);
      return r;
    } };
  const st = { clip: { min: [0, 0, 0], max: [1, 1, 1] }, extent: [1, 1, 1] };
  const tool = TP.createPvTool({ selection: sel, backend: () => backend, datasetId: () => 'ds_1', state: () => st,
    info: () => ({ full_shape: [10, 10, 2], axes: { z: { full_velocity_kms: [100, 200] } } }), onChange: () => {} });
  const flush = async () => {
    // qt-zlib decoding uses asynchronous streams: 5 ms is not enough on a
    // loaded CI runner. Wait for this answer to finish (or the queued path's
    // request to start), retaining a deadline so a stuck update still fails.
    const deadline = Date.now() + 5000;
    while (tool.state.inFlight && !answers.length && Date.now() < deadline) {
      await new Promise((r) => setTimeout(r, 1));
    }
    assert.ok(!tool.state.inFlight || answers.length, 'PV update did not finish within 5 seconds');
  };
  return { tool, calls, answers, recipes, sel, flush };
}

test('the path follows the trigger: latest wins, one product re-recorded as the path grows', async () => {
  const h = pvHarness();
  h.tool.arm(true);
  h.tool.addVertex([1, 1]);
  assert.equal(h.calls.length, 0, 'one vertex: no PV yet');
  h.tool.addVertex([5, 5]);
  h.tool.addVertex([8, 2]);            // while the first is in flight
  h.tool.addVertex([9, 9]);
  assert.equal(h.calls.length, 1);
  h.answers.shift()();
  await h.flush();
  // The first answer is shown until the next arrives (as the desktop's live
  // path does); one more request, for the latest path only (not the 3-point one).
  assert.equal(h.calls.length, 2);
  assert.equal(h.calls[1].vertices.length, 4);
  assert.equal(h.tool.state.result.vertices.length, 2);
  h.answers.shift()();
  await h.flush();
  assert.equal(h.tool.state.result.vertices.length, 4);
  assert.deepEqual(h.tool.state.result.velocities, [100, 200]);
  assert.equal(h.recipes.length, 1);
  assert.equal(h.recipes[0].params.Path, '4 points');
  h.tool.undo();                       // back to 3 points: same product, re-recorded
  h.answers.shift()();
  await h.flush();
  assert.equal(h.recipes.length, 1);
  assert.equal(h.recipes[0].params.Path, '3 points');
  assert.deepEqual(h.recipes[0].product.replay[0].request.vertices, [[1, 1], [5, 5], [8, 2]]);
  h.tool.newPath();
  h.tool.addVertex([0, 0]); h.tool.addVertex([1, 0]);
  h.answers.shift()();
  await h.flush();
  assert.equal(h.recipes.length, 2, 'a new path is a new product');
});

test('a selection tool chosen after arming disarms the path', () => {
  const h = pvHarness();
  h.tool.arm(true);
  assert.ok(h.tool.state.armed);
  h.tool.poll();
  assert.ok(h.tool.state.armed);
  h.sel.sel.tool = 'lasso';
  h.tool.poll();
  assert.ok(!h.tool.state.armed);
});

test('the path is drawn on the slab\'s two faces with a post at each vertex', () => {
  const h = pvHarness();
  h.tool.state.vertices = [[0, 0], [9, 9]];
  const segs = h.tool.segments();
  assert.equal(segs.length, 2 + 2);
  // Pixel 0's centre is at box 0.05 -> model -0.45 (extent 1).
  close(segs[0].a[0], -0.45); close(segs[0].a[2], -0.5); close(segs[0].b[2], 0.5);
});

test('the analysis panel: every button large and hit at its centre, under the control panel', () => {
  const L = TPanel.toolsLayout();
  const [W, H] = TPanel.TOOLS_PX;
  for (const w of L) {
    const [x, y, ww, hh] = w.rect;
    assert.ok(x >= 0 && y >= 0 && x + ww <= W && y + hh <= H, w.id);
    assert.ok(hh / H * TPanel.TOOLS_SIZE[1] >= 0.025, `${w.id} at least 2.5 cm high`);
    assert.ok(ww / W * TPanel.TOOLS_SIZE[0] >= 0.045, `${w.id} at least 4.5 cm wide`);
    assert.equal(P.hitWidget(L, x + ww / 2, y + hh / 2).id, w.id);
  }
  for (const id of ['seed:new', 'seed:add', 'seed:carve', 'isolate', 'remove', 'show-applied', 'moment:0', 'moment:1',
    'moment:2', 'moment-hide', 'pv-draw', 'pv-undo', 'pv-new', 'pv-narrow', 'pv-wide', 'pv-hide']) {
    assert.ok(L.find((w) => w.id === id), id);
  }
  // Hung under the control panel, in its plane, without overlapping it.
  const control = M.multiply(M.translation(0.3, 1.4, -0.6), M.rotation([0, 1, 0], 0.7));
  const tm = TPanel.toolsModel(control);
  const local = M.transformPoint(M.invert(control), M.position(tm));
  close(local[0], 0, 1e-6); close(local[2], 0, 1e-6);
  assert.ok(-local[1] - TPanel.TOOLS_SIZE[1] / 2 >= P.PANEL_SIZE[1] / 2, 'below the control panel');
  // A ray at a button's centre presses it through vr-panel's pointer.
  const w = L.find((x) => x.id === 'isolate');
  const u = (w.rect[0] + w.rect[2] / 2) / W - 0.5, v = 0.5 - (w.rect[1] + w.rect[3] / 2) / H;
  const target = M.transformPoint(tm, [u * TPanel.TOOLS_SIZE[0], v * TPanel.TOOLS_SIZE[1], 0]);
  const eye = M.transformPoint(tm, [0, 0, 0.7]);
  const hit = P.rayPanel(tm, TPanel.TOOLS_SIZE, eye, M.sub(target, eye), TPanel.TOOLS_PX);
  const ptr = new P.PanelPointer(L);
  assert.deepEqual(ptr.update(hit, true).map((a) => a.id), ['isolate']);
});

test('the product planes hang above the two panels, apart, facing the viewer, about 0.85 m away', async () => {
  const PC = await import('../../app/static/vr/js/product-canvas.js');
  const anchor = { p: [0.1, 1.38, -0.75], yaw: 0.3 };       // the cube placed 0.75 m in front
  const viewer = M.add(anchor.p, M.scale3([Math.sin(0.3), 0, Math.cos(0.3)], 0.75));
  const corners = (m, size) => [[-1, -1], [1, -1], [1, 1], [-1, 1]].map(([a, b]) => M.transformPoint(m, [a * size[0] / 2, b * size[1] / 2, 0]));
  const mom = TPanel.productModel(anchor, 'moment'), pv = TPanel.productModel(anchor, 'pv');
  const lowest = (m) => Math.min(...corners(m, PC.PLANE_SIZE).map((c) => c[1]));
  // The results panel's top (main.js: up 0.12, 0.248 m high) and the control panel's (DEFAULT_OFFSET).
  assert.ok(lowest(mom) - anchor.p[1] > 0.12 + 0.248 / 2 + 0.02);
  assert.ok(lowest(pv) - anchor.p[1] > P.DEFAULT_OFFSET.up + P.PANEL_SIZE[1] / 2 + 0.02);
  // Apart: the moment's left edge right of the PV's right edge (in the anchor's frame).
  const toLocal = (q) => M.transformPoint(M.invert(M.multiply(M.translation(...anchor.p), M.rotation([0, 1, 0], anchor.yaw))), q);
  const momLeft = Math.min(...corners(mom, PC.PLANE_SIZE).map((c) => toLocal(c)[0]));
  const pvRight = Math.max(...corners(pv, PC.PLANE_SIZE).map((c) => toLocal(c)[0]));
  assert.ok(momLeft - pvRight > 0.1, `${momLeft} ${pvRight}`);
  for (const m of [mom, pv]) {
    const c = M.position(m);
    const d = M.length(M.sub(c, viewer));
    assert.ok(d > 0.7 && d < 1.0, `distance ${d}`);
    // Its face (+z) turned toward the viewer (within 30 degrees).
    const n = M.normalize(M.sub(M.transformPoint(m, [0, 0, 1]), c));
    const to = M.normalize(M.sub(viewer, c));
    assert.ok(M.dot(M.normalize([n[0], 0, n[2]]), M.normalize([to[0], 0, to[2]])) > Math.cos(Math.PI / 6));
  }
});

// Point tables, velocity fields and catalogues in the VR page (issue #190):
// the pure parts of points.js, flow.js, catalogue.js and layers.js.
import test from 'node:test';
import assert from 'node:assert/strict';
import * as PT from '../../app/static/vr/js/points.js';
import * as FL from '../../app/static/vr/js/flow.js';
import * as CT from '../../app/static/vr/js/catalogue.js';
import * as LY from '../../app/static/vr/js/layers.js';
import { parsePairing } from '../../app/static/vr/js/pairing.js';
import { colormapTable } from '../../app/static/vr/js/colormaps.js';
import { niceTicks } from '../../app/static/vr/js/volume.js';
import * as M from '../../app/static/vr/js/mat4.js';
import * as P from '../../app/static/vr/js/vr-panel.js';
import * as TP from '../../app/static/vr/js/tools-panel.js';

test('catalogue buttons sit below analysis and each receives its own ray', () => {
  const control = M.multiply(M.translation(.3, 1.4, -.6), M.rotation([0, 1, 0], .7));
  const strip = LY.stripModel(control, 'cube');
  const layout = LY.stripLayout(LY.stripButtons('cube', {}));
  for (const button of layout) {
    const [x, y, w, h] = button.rect;
    const target = M.transformPoint(strip, [((x + w / 2) / LY.STRIP_PX[0] - .5) * LY.STRIP_SIZE[0],
      (.5 - (y + h / 2) / LY.STRIP_PX[1]) * LY.STRIP_SIZE[1], 0]);
    const eye = M.transformPoint(strip, [0, 0, 1]);
    const ray = M.sub(target, eye);
    assert.equal(P.rayPanel(TP.toolsModel(control), TP.TOOLS_SIZE, eye, ray, TP.TOOLS_PX), null, button.id);
    const hit = P.rayPanel(strip, LY.STRIP_SIZE, eye, ray, LY.STRIP_PX);
    assert.deepEqual(new P.PanelPointer(layout).update(hit, true).map((a) => a.id), [button.id]);
  }
});

const close = (a, b, eps = 1e-9) => assert.ok(Math.abs(a - b) <= eps, `${a} != ${b}`);
const hdr = (o) => (k) => (k in o ? o[k] : null);

test('the box of a point table is the backend default (longest side 1, centred)', () => {
  const d = PT.dataToModel([0, 10, -2, 2, 5, 6]);
  assert.deepEqual(d.extent.map((v) => +v.toFixed(9)), [1, 0.4, 0.1]);
  const m = d.matrix;   // column-major
  const ap = (p) => [0, 1, 2].map((r) => m[r] * p[0] + m[4 + r] * p[1] + m[8 + r] * p[2] + m[12 + r]);
  assert.deepEqual(ap([0, -2, 5]).map((v) => +v.toFixed(9)), [-0.5, -0.2, -0.05]);
  assert.deepEqual(ap([10, 2, 6]).map((v) => +v.toFixed(9)), [0.5, 0.2, 0.05]);
});

test('a sample is read back from its headers and body', () => {
  const info = PT.sampleInfo(hdr({ 'X-Visivo-Count': '2', 'X-Visivo-Total-Rows': '10', 'X-Visivo-Step': '5.0',
    'X-Visivo-Bounds': '0,10,0,1,0,2', 'X-Visivo-Axes': 'X,Y,Z', 'X-Visivo-Color-Field': 'Mass',
    'X-Visivo-Color-Range': '1,100', 'X-Visivo-Color-Log': '1', 'X-Visivo-Data-Range': '0.5,100' }));
  assert.equal(info.count, 2);
  assert.ok(info.colourLog);
  const buf = new Uint16Array([0, 0, 0, 65535, 65535, 65535, 0, 65535]).buffer;
  const a = PT.sampleArrays(buf, info);
  assert.deepEqual(PT.rowPosition(a.positions, 1, info.bounds), [10, 1, 2]);
  close(PT.codeValue(a.colour[1], info.colourRange, true), 100, 1e-9);
  close(PT.codeValue(32767.5, [1, 100], true), 10, 1e-9);
});

test('the colours sent to the point session are the page map on data values', () => {
  const lut = colormapTable('viridis');
  const c = PT.colourCommand('Mass', [1, 100], true, lut, 0, 1, 4);
  assert.equal(c.type, 'set_color');
  assert.equal(c.log, true);
  assert.equal(c.points.length, 5);
  close(c.points[2][0], 10, 1e-9);
  close(c.points[0][1], lut[0] / 255, 1e-9);
  close(c.points[4][3], lut[255 * 4 + 2] / 255, 1e-9);
  const w = PT.colourCommand('T', [0, 10], false, lut, 0.2, 0.6, 2);
  assert.deepEqual([w.min, w.max], [2, 6]);
  assert.deepEqual(PT.colourCommand(null, null, false, lut), { type: 'set_color', field: null });
});

test('splats are sized by the desktop rule and grow for a sample', () => {
  const s = PT.splatStyle([0, 100, 0, 100, 0, 100], 1e6, 1e6, 8, 35);
  close(s.radius, 1, 1e-9);                      // spacing 1, size 8 -> 1
  close(s.opacity, Math.max(0.05, Math.min(0.9, 20 / 100) * 0.35), 1e-12);
  close(PT.splatStyle([0, 100, 0, 100, 0, 100], 1e6, 1e3, 8, 35).sampleRadius, 10, 1e-9);
});

test('the governor sheds rows when the GPU is late and takes them back slowly', () => {
  assert.equal(PT.governRows(1e6, 2e6, -1, 72), 1e6);
  const down = PT.governRows(2e6, 2e6, 20, 72);          // budget 8.33 ms at 72 Hz
  assert.equal(down, 1.2e6);                              // at most 40 % less per step
  const up = PT.governRows(1e6, 2e6, 2, 72);
  assert.equal(up, 1.1e6);
  assert.equal(PT.governRows(1e5, 2e6, 100, 72), 1e5);   // never below the minimum
  assert.equal(PT.governRows(2e6, 2e6, 1, 72), 2e6);     // never above the sample
});

test('point look commands and box decorations', () => {
  const cmds = PT.pointLook({ axes: ['X', 'Y', 'Z'], colourField: null, colourRange: null, colourLog: false,
    lut: colormapTable('grey'), c0: 0, c1: 1, style: 'splats', splatSize: 2, pointPx: 3, intensity: 40 });
  assert.deepEqual(cmds.map((c) => c.type), ['set_point_fields', 'set_color', 'set_point_style', 'set_show_outline']);
  assert.equal(cmds[2].mode, 'gaussian');
  const { segs, labels } = PT.boxDecorations(PT.dataToModel([0, 10, 0, 5, 0, 1]), ['X', 'Y', 'Z'], niceTicks);
  assert.ok(segs.length >= 12 && labels.some((l) => l.text === '10'));
});

test('flow lines: headers, body, indices and arrows', () => {
  const info = FL.linesInfo(hdr({ 'X-Visivo-Line-Points': '3', 'X-Visivo-Lines': '1', 'X-Visivo-Arrows': '1',
    'X-Visivo-Width': '4', 'X-Visivo-Height': '4', 'X-Visivo-Depth': '4', 'X-Visivo-Spacing': '2,2,2',
    'X-Visivo-Origin': '-3,-3,-3', 'X-Visivo-Full-Speed-Max': '10' }));
  assert.deepEqual(info.bounds, [-3, 3, -3, 3, -3, 3]);
  const n = 3, buf = new ArrayBuffer(n * 16 + 2 * 4 + 24);
  const f = new Float32Array(buf, 0, 12);
  f.set([0, 0, 0, 1, 0, 0, 2, 0, 0, 5, 6, 7]);
  new Uint32Array(buf, 48, 2).set([0, 3]);
  new Float32Array(buf, 56, 6).set([0, 0, 0, 3, 4, 0]);
  const a = FL.linesArrays(buf, info);
  assert.deepEqual(Array.from(FL.lineIndices(a.offsets)), [0, 1, 1, 2]);
  const v = FL.arrowVertices(a.arrowPos, a.arrowVec, 0.2);
  assert.deepEqual(Array.from(v.slice(4, 8)), [0.6000000238418579, 0.800000011920929, 0, 5]);
  assert.throws(() => FL.linesArrays(new ArrayBuffer(8), info));
});

test('flow geometry without the spacing headers is the current grid', () => {
  const info = FL.linesInfo(hdr({ 'X-Visivo-Width': '32', 'X-Visivo-Height': '32', 'X-Visivo-Depth': '32',
    'X-Visivo-Full-Shape': '96,96,96', 'X-Visivo-Downsample-Factor': '3' }), { boxMpc: 190 });
  close(info.spacing[0], (190 / 95) * 3, 1e-12);
  close(info.origin[0], -95, 1e-12);
  const per = FL.linesInfo(hdr({ 'X-Visivo-Width': '2', 'X-Visivo-Height': '2', 'X-Visivo-Depth': '2',
    'X-Visivo-Downsample-Factors': '1,2,4', 'X-Visivo-Spacing': '1,2,4', 'X-Visivo-Origin': '0,0.5,1.5' }));
  assert.deepEqual(per.factors, [1, 2, 4]);
  assert.deepEqual(per.bounds, [0, 1, 0.5, 2.5, 1.5, 5.5]);
});

test('catalogue sources go to the pixel the backend gave', () => {
  close(CT.pixelToModel(-0.5, 10, 1), -0.5);
  close(CT.pixelToModel(9.5, 10, 1), 0.5);
  close(CT.pixelToModel(4.5, 10, 2), 0);
  const ov = { valid: true, sources: [
    { x: 4.5, y: 4.5, z: null, band: 'ft', label: 'a', ellipse: [[3.5, 4.5], [5.5, 4.5]] },
    { x: 0, y: 0, z: 2, band: '', label: 'b', ellipse: null },
    { x: 9, y: 9, z: 9, band: '', label: 'c', ellipse: null }],
  polylines: [{ label: 'f', points: [[0, 0], [9, 9]] }] };
  const clip = { min: [0, 0, 0], max: [1, 1, 0.5] };
  const r = CT.overlaySegments(ov, [10, 10, 10], [1, 1, 1], clip, { labels: true });
  assert.equal(r.drawn, 3);                        // c is beyond the slab
  assert.deepEqual(r.segs[0].a.map((v) => +v.toFixed(12)), [-0.1, 0, -0.5]);  // the ellipse on the back face
  const pin = r.segs[1];
  assert.deepEqual([pin.a[2], pin.b[2]], [-0.5, 0]);
  assert.equal(r.labels.length, 2);
  assert.match(CT.overlaySummary({ valid: true, n_sources: 2, n_outside: 1, spectral: { used: false } }), /on the sky only/);
  assert.deepEqual(CT.overlayRequest('d', { source: 'vlkb_sources' }).bands, ['ft']);
});

test('what a path opens as, and the pairing of other data types', () => {
  assert.equal(LY.kindOfPath('/d/snap.bin.head'), 'points');
  assert.equal(LY.kindOfPath('/d/cube.fits'), 'cube');
  assert.equal(LY.kindOfPath('/d/cf4_velocity.fits'), 'flow');
  assert.equal(LY.kindOfPath('/d/cube.fits', 'flow'), 'flow');
  assert.equal(LY.catalogueFormat('/d/x.xml'), 'votable');
  assert.equal(LY.catalogueFormat('/d/x.bin'), 'vbt');
  assert.equal(LY.pairedMode('velocity'), 'flow');
  assert.equal(LY.pairedMode(''), 'cube');
  const p = parsePairing('#token=t&dataset=vf_1&kind=velocity&box=200&path=%2Fd%2Ff.fits');
  assert.equal(p.kind, 'velocity');
  assert.equal(p.box, '200');
  assert.equal(p.rest, '');
  assert.equal(LY.stripLayout(LY.stripButtons('points', { style: 'points', budgetRows: 1e6 })).length, 5);
});

test('the lines say the box they were traced with', () => {
  const base = { 'X-Visivo-Width': '2', 'X-Visivo-Height': '2', 'X-Visivo-Depth': '2',
    'X-Visivo-Spacing': '1,1,1', 'X-Visivo-Origin': '0,0,0' };
  assert.equal(FL.linesInfo(hdr({ ...base, 'X-Visivo-Box-Mpc': '0.0' }), { boxMpc: 200 }).boxMpc, 0);
  assert.equal(FL.linesInfo(hdr({ ...base, 'X-Visivo-Box-Mpc': '100.0' })).boxMpc, 100);
  assert.equal(FL.linesInfo(hdr(base), { boxMpc: 200 }).boxMpc, 200);    // an older backend
});

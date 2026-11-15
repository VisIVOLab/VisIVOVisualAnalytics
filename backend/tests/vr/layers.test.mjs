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
  // No X-Visivo-Placed-Rows (an older backend): every row counts.
  assert.equal(info.placedRows, 10);
  // A sky catalogue's unplaced rows are not counted for the splat size (#265).
  const sky = PT.sampleInfo(hdr({ 'X-Visivo-Count': '2', 'X-Visivo-Total-Rows': '10', 'X-Visivo-Placed-Rows': '7' }));
  assert.equal(sky.totalRows, 10);
  assert.equal(sky.placedRows, 7);
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
  close(s.opacity, Math.max(0.05, Math.min(0.9, 20 / 100) * 0.35), 1e-12);   // the desktop's
  // In a float32 target over size² (#263): the size changes the size, not the light.
  close(PT.splatStyle([0, 100, 0, 100, 0, 100], 1e6, 1e6, 8, 35, 'v2-f32').opacity, 0.07 / 64, 1e-12);
  close(PT.splatStyle([0, 100, 0, 100, 0, 100], 1e6, 1e6, 1, 35, 'v2-f32').opacity, 0.07, 1e-12);
  close(PT.splatStyle([0, 100, 0, 100, 0, 100], 1e6, 1e3, 8, 35).sampleRadius, 10, 1e-9);
});

test('splat look (#263): tone curve, default colour column, axis units', () => {
  // Linear for faint light, the hue kept, never above 1 (app/point_splat.py tone_map).
  const faint = PT.toneMap([1e-4, 2e-4, 3e-4]);
  faint.forEach((v, i) => close(v, [1e-4, 2e-4, 3e-4][i], 1e-7));
  const y = 0.2126 * 0.5 + 0.7152 * 0.25 + 0.0722 * 0.1;
  PT.toneMap([0.5, 0.25, 0.1]).forEach((v, i) => close(v, [0.5, 0.25, 0.1][i] * (1 - Math.exp(-y)) / y, 1e-12));
  const hot = PT.toneMap([40, 20, 8]);
  close(Math.max(...hot), 1, 1e-12);
  close(hot[1] / hot[0], 0.5, 1e-12);
  assert.deepEqual(PT.toneMap([0, 0, 0]), [0, 0, 0]);
  // A density first; else not a position, velocity, sky coordinate or id.
  const axes = ['x', 'y', 'z'];
  assert.equal(PT.defaultColourField(['x', 'y', 'z', 'vx', 'vy', 'vz', 'LogMass', 'LogD', 'LogT'], axes), 'LogD');
  assert.equal(PT.defaultColourField(['X', 'Y', 'Z', 'vx', 'vy', 'vz', 'Lmass', 'LTemp', 'LDensity'], ['X', 'Y', 'Z']), 'LDensity');
  assert.equal(PT.defaultColourField(['x', 'y', 'z', 'Mass', 'Rho'], axes), 'Rho');
  assert.equal(PT.defaultColourField(['RA', 'Dec', 'z', 'Ktmag', '@x', '@y', '@z'], ['@x', '@y', '@z']), 'Ktmag');
  assert.equal(PT.defaultColourField(['ID', 'RAJ2000', 'DEJ2000', 'cz', '@x', '@y', '@z'], ['@x', '@y', '@z']), 'cz');
  assert.equal(PT.defaultColourField(['x', 'y', 'z', 'vx'], axes), null);
  // A sky catalogue's computed positions say their unit; a table's own keep their names.
  assert.deepEqual(PT.axisTitles(['@x', '@y', '@z']), ['x (Mpc)', 'y (Mpc)', 'z (Mpc)']);
  assert.deepEqual(PT.axisTitles(['x', 'y', 'POS_Z']), ['x', 'y', 'POS_Z']);
  assert.equal(PT.MAX_SPLAT_PX, 256, 'the backend draws up to point_splat.MAX_PX');
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
  assert.deepEqual(cmds.map((c) => c.type), ['set_point_fields', 'set_color', 'set_point_style', 'set_show_outline', 'set_filters']);
  assert.equal(cmds[2].mode, 'gaussian');
  // The box selection (#259) reaches the session as its filters; none: every row.
  assert.deepEqual(cmds[4].filters, []);
  const box = [{ box: { fields: ['X', 'Y', 'Z'], min: [0, 0, 0], max: [1, 1, 1] }, op: 'outside' }];
  assert.deepEqual(PT.pointLook({ axes: ['X', 'Y', 'Z'], lut: colormapTable('grey'), style: 'points', filters: box })[4].filters, box);
  const { segs, labels } = PT.boxDecorations(PT.dataToModel([0, 10, 0, 5, 0, 1]), ['X', 'Y', 'Z'], niceTicks);
  assert.ok(segs.length >= 12 && labels.some((l) => l.text === '10'));
});

test('a sky catalogue\'s placement goes to the point session and the status line (#260, #265)', () => {
  const sky = { frame: 'supergalactic', distance_mode: 'comoving', cosmology: 'Planck18' };
  const cmds = PT.pointLook({ axes: ['@x', '@y', '@z'], colourField: null, colourRange: null, colourLog: false,
    lut: colormapTable('grey'), c0: 0, c1: 1, style: 'points', splatSize: 2, pointPx: 3, intensity: 40, sky });
  assert.deepEqual(cmds[0], { type: 'set_sky_positions', ...sky });
  assert.equal(cmds[1].type, 'set_point_fields');
  assert.equal(PT.skyLabel({ ...sky, unplaced: 26 }), 'supergalactic · comoving · Planck18 · 26 unplaced');
  assert.equal(PT.skyLabel({ frame: 'fk5', distance_mode: 'redshift_space', cosmology: 'WMAP9', unplaced: 0 }),
    'fk5 · redshift space · WMAP9');
  assert.equal(PT.skyLabel(null), '');
  assert.equal(PT.skyLabel({ ...sky, unplaced: 0, sky_sphere: true }), 'supergalactic · celestial sphere (no distances)');
});

test('a refined pose carries the rows the splats are sized for and the eye height (#262)', () => {
  const box = PT.dataToModel([0, 10, 0, 5, 0, 1]);
  const eyes = [{ eye: 'left', width: 1600, height: 1000 }, { eye: 'right', width: 1600, height: 1000 }];
  const splats = PT.refinePoseExtra(box, 'splats', 999999.6, eyes);
  assert.deepEqual(splats.data_to_model, Array.from(box.matrix));
  assert.equal(splats.sample_rows, 1000000);
  assert.deepEqual(splats.viewport_size, [1600, 1000]);
  // An eye larger than the session draws: no size (the frame's own is used).
  assert.ok(!('viewport_size' in PT.refinePoseExtra(box, 'splats', 1e6, [{ width: 5000, height: 5000 }])));
  // Points: the box alone (their size is in pixels).
  assert.deepEqual(Object.keys(PT.refinePoseExtra(box, 'points', 1e6, eyes)), ['data_to_model']);
  assert.ok(!('sample_rows' in PT.refinePoseExtra(box, 'splats', 0, [])));
  // Ray-traced splats (#264) are refined for the same sample and eye size.
  assert.deepEqual(PT.refinePoseExtra(box, 'raytraced', 999999.6, eyes), splats);
});

test('ray-traced style (#264): drawn as splats, refined by the backend ray tracer', () => {
  assert.deepEqual(PT.STYLES, ['points', 'splats', 'raytraced']);
  assert.ok(PT.isSplats('splats') && PT.isSplats('raytraced') && !PT.isSplats('points'));
  // A simulation's particles open ray-traced; a catalogue's objects as points.
  assert.equal(PT.defaultStyle('vbt'), 'raytraced');
  assert.equal(PT.defaultStyle('catalogue'), 'points');
  assert.equal(PT.defaultStyle(undefined), 'points', 'an older backend without format');
  const look = (style) => PT.pointLook({ axes: ['x', 'y', 'z'], colourField: null, style, splatSize: 2,
    pointPx: 3, intensity: 40 }).find((c) => c.type === 'set_point_style');
  assert.deepEqual(look('raytraced'), { type: 'set_point_style', mode: 'gaussian', size: 2, intensity: 40,
    stereo_renderer: 'raytrace', splat_rule: 'v1' });
  assert.deepEqual(look('splats'), { type: 'set_point_style', mode: 'gaussian', size: 2, intensity: 40,
    stereo_renderer: 'splat', splat_rule: 'v1' });
  // The page's splat rule goes with its look (#263).
  const rule = PT.pointLook({ axes: ['x', 'y', 'z'], colourField: null, style: 'splats', splatSize: 2,
    pointPx: 3, intensity: 40, splatRule: 'v2-f32' }).find((c) => c.type === 'set_point_style').splat_rule;
  assert.equal(rule, 'v2-f32');
  assert.equal(look('points').mode, 'points');
  // The look key changes with the renderer: a swap re-renders the still frame.
  assert.notDeepEqual(look('raytraced'), look('splats'));
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
});

test('the menu\'s helpers for point tables and fields (#257): crop, columns, seeds', () => {
  assert.equal(LY.isCropped({ min: [0, 0, 0], max: [1, 1, 1] }), false);
  assert.equal(LY.isCropped({ min: [0, 0.2, 0], max: [1, 1, 1] }), true);
  assert.equal(LY.isCropped(null), false);
  assert.deepEqual(LY.colourFields({ axes: ['X', 'Y', 'Z'], fields: ['X', 'Y', 'Z', 'VX', '@index', 'Rho'] }), ['VX', 'Rho']);
  assert.deepEqual(LY.colourFields(null), []);
  assert.equal(LY.stepSeeds(200, 1), 283);
  assert.equal(LY.stepSeeds(LY.stepSeeds(200, 1), 1), 400, 'two presses double');
  assert.equal(LY.stepSeeds(200, -1), 141);
  assert.equal(LY.stepSeeds(10, -1), 10, 'the backend\'s floor');
  assert.equal(LY.stepSeeds(4900, 1), 5000, 'and ceiling');
  assert.equal(LY.stepSeeds(11, -1), 10);
});

test('the lines say the box they were traced with', () => {
  const base = { 'X-Visivo-Width': '2', 'X-Visivo-Height': '2', 'X-Visivo-Depth': '2',
    'X-Visivo-Spacing': '1,1,1', 'X-Visivo-Origin': '0,0,0' };
  assert.equal(FL.linesInfo(hdr({ ...base, 'X-Visivo-Box-Mpc': '0.0' }), { boxMpc: 200 }).boxMpc, 0);
  assert.equal(FL.linesInfo(hdr({ ...base, 'X-Visivo-Box-Mpc': '100.0' })).boxMpc, 100);
  assert.equal(FL.linesInfo(hdr(base), { boxMpc: 200 }).boxMpc, 200);    // an older backend
});

test('splat rules (#263 review): no vanishing splats without a float target, frames of the page\'s rule only', () => {
  assert.deepEqual(PT.SPLAT_RULES, ['v1', 'v2-8bit', 'v2-f16', 'v2-f32']);
  // A size-12 splat: over size² it would add 0.05/144 < 1/510 to an 8-bit target (nothing).
  close(PT.ruleOpacity(0.05, 12, 'v2-f32'), 0.05 / 144, 1e-15);
  assert.equal(PT.ruleOpacity(0.05, 12, 'v2-8bit'), 0.05);
  assert.equal(PT.ruleOpacity(0.05, 12, 'v1'), 0.05);
  assert.equal(PT.ruleOpacity(0.05, 12, 'v2-f16'), PT.F16_OPACITY_FLOOR);
  assert.equal(PT.ruleOpacity(0.5, 2, 'v2-f16'), 0.125);
  // Refined frames: shown only when drawn by the page's rule (a backend from
  // before #263 says none); points do not depend on it.
  assert.ok(PT.acceptFrame({ splat_rule: 'v2-f32' }, 'raytraced', 'v2-f32'));
  assert.ok(!PT.acceptFrame({}, 'splats', 'v2-f32'), 'an older backend');
  assert.ok(!PT.acceptFrame({ splat_rule: 'v2-8bit' }, 'splats', 'v2-f16'));
  assert.ok(PT.acceptFrame({}, 'points', 'v2-f32'));
});

test('row governor without a GPU timer (#295): frame intervals, ceiling, half-octave steps', () => {
  const hz = 72, P = 1000 / hz;
  // recentFrames: the newest intervals up to the window; pauses over 250 ms are not frames.
  const f = PT.recentFrames([100, P, P, 3 * P, P], hz, 4.5 * P);
  assert.equal(f.n, 3);
  close(f.missed, 1 / 3, 1e-12);
  assert.equal(PT.recentFrames([P, 400, P, P, P, P], hz).n, 5, 'a 400 ms pause skipped');
  const g = new PT.FrameRowGovernor({ min: 20000, holdMs: 1000, ceilingRiseMs: 2000 });
  const late = { n: 10, meanMs: 2 * P, missed: 0.6 }, clean = { n: 10, meanMs: P, missed: 0 };
  // A Quest at 36 fps with 1 M splats: cut by the period over the interval (at most to half).
  let rows = g.update(1_000_000, 1_000_000, late, hz, 0);
  assert.equal(rows, 500_000);
  assert.equal(g.ceiling, 1_000_000);
  assert.equal(g.update(rows, 1_000_000, clean, hz, 500), rows, 'held after a cut');
  rows = g.update(rows, 1_000_000, clean, hz, 1200);
  assert.equal(rows, 560_000, '12 % up when clean');
  // Up to 90 % of where it missed, then a quarter more of ceiling per 2 s clean.
  for (let t = 1450; t <= 2450; t += 250) rows = g.update(rows, 1_000_000, clean, hz, t);
  assert.equal(rows, 900_000);
  for (let t = 2700; t <= 3700; t += 250) rows = g.update(rows, 1_000_000, clean, hz, t);
  assert.equal(rows, 1_000_000, 'the ceiling rose (x1.5 after 2 s clean): the whole sample');
  // One late frame in a window (3 %) keeps the count, and a mean raised by it does not cut.
  assert.equal(g.update(300_000, 1_000_000, { n: 33, meanMs: 1.4 * P, medianMs: P, missed: 0.03, slow: 0.03 }, hz, 9000), 300_000);
  // A Quest pacing frames between one and 1.5 periods (63 fps, under 1 % missed) is late.
  assert.equal(g.update(300_000, 1_000_000, { n: 30, meanMs: 1.14 * P, medianMs: 1.14 * P, missed: 0.005 }, hz, 9500),
    255_000);
  assert.equal(PT.recentFrames([P, 1.2 * P, 1.1 * P, 200, P], hz, 1000).medianMs, 1.1 * P);
  close(PT.recentFrames([P, 1.2 * P, 1.05 * P, P], hz, 1000).slow, 0.25, 1e-12);
  // Sustained sparse misses (5 % to 15 %) or many slow frames: an 8 % cut, then held.
  const q = new PT.FrameRowGovernor({ min: 20000, holdMs: 1000 });
  assert.equal(q.update(700_000, 1_000_000, { n: 36, meanMs: 1.05 * P, medianMs: P, missed: 0.06, slow: 0.06 }, hz, 0), 644_000);
  assert.equal(q.update(644_000, 1_000_000, { n: 36, meanMs: 1.05 * P, medianMs: P, missed: 0.06, slow: 0.06 }, hz, 250), 644_000);
  assert.equal(q.update(644_000, 1_000_000, { n: 36, meanMs: 1.04 * P, medianMs: P, missed: 0.01, slow: 0.2 }, hz, 1300), 592_480);
  // Startup cuts ratchet the ceiling down; clean frames bring it back within seconds.
  const h = new PT.FrameRowGovernor({ min: 20000, holdMs: 1000 });
  let r = 200_000;
  for (let t = 0; t < 5000; t += 1100) r = h.update(r, 1_000_000, late, hz, t);
  assert.equal(r, 20_000, 'hitches at entry: down to the floor');
  let t = 6000;
  for (; r < 150_000 && t < 60_000; t += 250) r = h.update(r, 1_000_000, clean, hz, t);
  assert.ok(t - 6000 <= 12_000, `back to 150 k in ${t - 6000} ms`);
  // Steady near the limit: a cut right after a ceiling rise doubles the wait for the next rise.
  const k = new PT.FrameRowGovernor({ min: 20000, holdMs: 1000, ceilingRiseMs: 1500 });
  k.update(300_000, 1_000_000, late, hz, 0);                 // missed at 300 k
  let u = 255_000;
  for (let tt = 1000; tt <= 3000; tt += 250) u = k.update(u, 1_000_000, clean, hz, tt);
  assert.ok(Number.isFinite(k.lastRiseAt) && k.lastRiseAt <= 3000, 'the ceiling rose');
  k.update(u, 1_000_000, late, hz, k.lastRiseAt + 1000);    // and it missed again at once
  assert.equal(k.riseMs, 3000);
  // Never below the floor nor above the sample; too few frames say nothing.
  assert.equal(g.update(25_000, 1_000_000, late, hz, 10000), 20_000);
  assert.equal(g.update(40_000, 30_000, clean, hz, 20000), 30_000);
  assert.equal(g.update(123_456, 1_000_000, { n: 2, meanMs: 5 * P, missed: 1 }, hz, 30000), 123_456);
  g.reset();
  assert.equal(g.ceiling, Infinity);
  // The rows drawn: half-octave steps, or the whole sample.
  assert.deepEqual([141_421, 200_000, 260_000, 1_000_000].map((n) => PT.quantizeRows(n, 1_000_000)),
    [131_072, 185_364, 262_144, 1_000_000]);
  assert.equal(PT.quantizeRows(700_000, 600_000), 600_000);
  assert.equal(PT.HEADSET_SPLAT_ROWS, 200_000);
});

test('row governor review (#297): which governor, earned clean time, decaying backoff, step hysteresis', () => {
  const hz = 72, P = 1000 / hz;
  // 1, 6: the frame governor only in a VR session whose display rate has
  // settled; a GPU timer wins; else the count is kept (a desktop browser
  // without a timer may run at 30 or 50 Hz against an assumed 60).
  assert.equal(PT.rowGovernorKind({ gpuMs: 3.2, inXR: false, rateSettled: false }), 'gpu');
  assert.equal(PT.rowGovernorKind({ gpuMs: -1, inXR: true, rateSettled: true }), 'frames');
  assert.equal(PT.rowGovernorKind({ gpuMs: -1, inXR: true, rateSettled: false }), 'none', 'settling 90 -> 72 Hz');
  assert.equal(PT.rowGovernorKind({ gpuMs: -1, inXR: false, rateSettled: false }), 'none', 'Safari or Firefox');
  const clean = { n: 18, meanMs: P, medianMs: P, missed: 0, slow: 0 };
  const late = { n: 18, meanMs: 2 * P, medianMs: 2 * P, missed: 0.6, slow: 0.6 };
  // 2: clean time is earned by consecutive updates only: a gap (splats not
  // drawn under a refined frame) or interrupted() restarts it.
  const g = new PT.FrameRowGovernor({ holdMs: 0, ceilingRiseMs: 1500 });
  g.update(400_000, 1_000_000, late, hz, 0);                    // ceiling 400 k
  g.update(340_000, 1_000_000, clean, hz, 250);
  assert.equal(g.cleanSince, 250);
  g.update(340_000, 1_000_000, clean, hz, 5000);               // 4.75 s not drawn
  assert.equal(g.cleanSince, 5000, 'the gap is not clean time');
  assert.equal(g.ceiling, 400_000, 'no unearned rise');
  g.interrupted(5100);
  assert.equal(g.cleanSince, null);
  assert.equal(g.update(340_000, 1_000_000, clean, hz, 5300), 340_000, 'held after the frames come back');
  // 3: the probing backoff decays with time: a cut long after the probes
  // (a zoom into the box) is climbed back from at the fast pace.
  const k = new PT.FrameRowGovernor({ holdMs: 0, ceilingRiseMs: 1500, decayMs: 20000 });
  k.riseMs = 48000; k.lastBackoffAt = 0;
  for (let t = 250; t <= 90_000; t += 250) k.update(300_000, 1_000_000, clean, hz, t);
  assert.equal(k.riseMs, 3000, 'halved every 20 s: 48 -> 24 -> 12 -> 6 -> 3 s');
  // 4: steps with hysteresis: a count hovering at a midpoint keeps its step.
  const prev = PT.quantizeRows(200_000, 1_000_000);              // 185 364
  for (const n of [215_000, 225_000, 240_000, 160_000, 141_000]) assert.equal(PT.quantizeRows(n, 1_000_000, prev), prev, String(n));
  assert.equal(PT.quantizeRows(260_000, 1_000_000, prev), 262_144, 'more than 0.4 octave: the next step');
  assert.equal(PT.quantizeRows(120_000, 1_000_000, prev), 131_072);
  assert.equal(PT.quantizeRows(900_000, 1_000_000, 1_000_000), 1_000_000, 'the whole sample kept near it');
  assert.equal(PT.quantizeRows(240_000, 200_000, prev), prev, 'clamped to the sample before comparing');
});

test('#300: the row gate: cuts always; still under refined splats no ceiling rise, growth only after a frame stays shown whole', () => {
  const g = new LY.RowGrowthGate(5000);
  const st = (now, motion, status, weight, splats = true, refineOn = true) => ({ ...g.step(now, splats, refineOn, motion, status, weight) });
  assert.deepEqual(st(0, 'moving', 'waiting', 0), { grow: true, riseCeiling: true }, 'moving');
  assert.deepEqual(st(0, 'still', 'waiting', 0, false), { grow: true, riseCeiling: true }, 'points');
  assert.deepEqual(st(0, 'still', 'waiting', 0, true, false), { grow: true, riseCeiling: true }, 'refine off');
  assert.deepEqual(st(0, 'still', 'connecting', 0), { grow: true, riseCeiling: true }, 'no frame to come');
  assert.deepEqual(st(0, 'still', 'unavailable', 0), { grow: true, riseCeiling: true });
  assert.deepEqual(st(0, 'still', 'waiting', 0), { grow: false, riseCeiling: false }, 'a frame on its way');
  assert.deepEqual(st(100, 'still', 'refined', 0.5), { grow: false, riseCeiling: false }, 'fading in');
  assert.deepEqual(st(200, 'still', 'refined', 1), { grow: false, riseCeiling: false }, 'shown whole: from now');
  assert.deepEqual(st(5199, 'still', 'refined', 1), { grow: false, riseCeiling: false });
  assert.deepEqual(st(5200, 'still', 'refined', 1), { grow: true, riseCeiling: false }, 'shown whole for 5 s: one step');
  assert.deepEqual(st(5300, 'still', 'refined', 0.9), { grow: false, riseCeiling: false }, 'that step dropped it');
  assert.deepEqual(st(5400, 'still', 'refined', 1), { grow: false, riseCeiling: false }, 'and the time starts again');
  assert.equal(g.step(1, true, true, 'still', 'waiting', 0), g.step(2, true, true, 'moving', 'off', 0), 'one object, reused');
  // The governor: no clean time and no ceiling rise when told so.
  const hz = 72, P = 1000 / hz;
  const clean = { n: 18, meanMs: P, medianMs: P, missed: 0, slow: 0 };
  const late = { n: 18, meanMs: 2 * P, medianMs: 2 * P, missed: 0.6, slow: 0.6 };
  const f = new PT.FrameRowGovernor({ holdMs: 0, ceilingRiseMs: 1500 });
  f.update(400_000, 1_000_000, late, hz, 0);
  let r = 200_000;
  for (let t = 250; t <= 20_000; t += 250) r = f.update(r, 1_000_000, clean, hz, t, { riseCeiling: false });
  assert.equal(f.ceiling, 400_000);
  assert.equal(r, 360_000, 'up to 90 % of the ceiling, not past it');
  assert.equal(f.cleanSince, null);
});

/**
 * drawScene's governor under a still view whose refined frames never cover
 * it (a particle close to the eye), with the refiner modelled: a row step
 * changes the look, so the frame on show is dropped (it fades out in 60 ms),
 * the next one is asked for and arrives `latencyMs` later, then fades in over
 * 120 ms (refine-client.js frame() / tick()). A hitch at t = 0 cuts 400 k
 * rows; later the frames are late while more than `capacity` rows are drawn,
 * clean otherwise. `rule(gateOut, status, weight)` returns {grow, riseCeiling}.
 */
function simulateStillUncovered(rule, { seconds = 120, latencyMs = 200, capacity = 300_000 } = {}) {
  const hz = 72, P = 1000 / hz, count = 1_000_000, dt = 1000 / hz;
  const late = { n: 18, meanMs: 2 * P, medianMs: 2 * P, missed: 0.6, slow: 0.6 };
  const clean = { n: 18, meanMs: P, medianMs: P, missed: 0, slow: 0 };
  const gov = new PT.FrameRowGovernor();
  const gate = new LY.RowGrowthGate();
  let govRows = 400_000, drawRows = PT.quantizeRows(govRows, count);
  let version = 0, shown = 0, weight = 1, pending = null, key = drawRows, governedAt = -Infinity;
  const drops = [];
  for (let now = 0; now <= seconds * 1000; now += dt) {
    if (drawRows !== key) { key = drawRows; version++; drops.push(now); }       // a new look: the frame on show is stale
    if (pending && now >= pending.at) { if (pending.version === version) shown = pending.version; pending = null; }
    const usable = shown === version;
    weight = usable ? Math.min(1, weight + dt / 120) : Math.max(0, weight - dt / 60);
    if (!usable && weight <= 0 && !pending) pending = { version, at: now + latencyMs };
    const status = weight > 0 ? 'refined' : 'waiting';
    const g = rule(gate.step(now, true, true, 'still', status, weight), status, weight);
    if (now - governedAt >= 250) {
      governedAt = now;
      const next = gov.update(govRows, count, now === 0 || drawRows > capacity ? late : clean, hz, now, g);
      govRows = g.grow ? next : Math.min(next, govRows);
      drawRows = PT.quantizeRows(govRows, count, drawRows);
    }
  }
  return { drops, drawRows, ceiling: gov.ceiling };
}

test('#300 review: under an uncovered refined frame the rows recover from a cut in a few steps, without the frame flickering', () => {
  const gated = simulateStillUncovered((g) => g);
  // The cut, then at most one step per frame shown whole for 5 s; a step
  // past what the headset holds is cut again, and below the ceiling that cut
  // left (no rise while still) the rows settle: 5 dropped frames in the
  // first 20 s (cut, step, step, cut, step), none in the 100 s after.
  assert.ok(gated.drawRows >= 262_144, `${gated.drawRows}: back above the cut`);
  assert.ok(gated.ceiling <= 400_000, 'the ceiling did not rise while still');
  assert.ok(gated.drops.length <= 5, `${gated.drops.length} refined frames dropped in 120 s`);
  assert.equal(gated.drops.filter((t) => t > 20_000).length, 0, `drops at ${gated.drops.map((t) => t.toFixed(0))} ms`);
  // The first #301 rule (grow whenever a frame is shown whole, the ceiling
  // free to rise): 26 dropped frames in 120 s, three every 20 s for good as
  // it probes above where it missed.
  const naive = simulateStillUncovered((g, status, weight) => ({
    grow: !(status === 'waiting' || (status === 'refined' && weight < 1)), riseCeiling: true }));
  assert.ok(naive.drops.length > 3 * gated.drops.length, `${naive.drops.length} drops against ${gated.drops.length}`);
  assert.ok(naive.drops.filter((t) => t > 60_000).length >= 3, 'still probing after a minute');
});

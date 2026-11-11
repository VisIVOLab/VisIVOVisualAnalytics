// Unit tests of the VR page's shader-independent logic (node --test).
import test from 'node:test';
import assert from 'node:assert/strict';
import * as V from '../../app/static/vr/js/volume.js';
import * as M from '../../app/static/vr/js/mat4.js';

const close = (a, b, eps = 1e-6) => assert.ok(Math.abs(a - b) <= eps, `${a} != ${b}`);

test('uint8 codes decode as the backend encodes them (0 blank, 1..255 = 0..1)', () => {
  assert.equal(V.codeToNorm(0), -1);
  close(V.codeToNorm(1), 0);
  close(V.codeToNorm(255), 1);
  close(V.codeToNorm(128), 127 / 254);
});

test('float16 bits decode', () => {
  close(V.halfToFloat(0x3c00), 1);
  close(V.halfToFloat(0xbc00), -1);
  close(V.halfToFloat(0x3800), 0.5);
  close(V.halfToFloat(0x0000), 0);
  close(V.halfToFloat(0x3555), 0.333251953125);
  assert.ok(Number.isNaN(V.halfToFloat(0x7e00)));
});

test('the skipping grid holds every cell maximum, with a one-voxel apron', () => {
  const dims = [20, 9, 5];
  const data = new Uint8Array(dims[0] * dims[1] * dims[2]).fill(1);   // all norm 0
  const at = (x, y, z) => (z * dims[1] + y) * dims[0] + x;
  data[at(8, 3, 2)] = 255;      // first voxel of x-cell 1: its left neighbour cell sees it too
  data[at(19, 8, 4)] = 128;
  data[at(0, 0, 0)] = 0;        // blank
  const { data: g, dims: gd } = V.buildMacrocells(data, dims, 'uint8', 8);
  assert.deepEqual(gd, [3, 2, 1]);
  const cell = (x, y, z) => g[(z * gd[1] + y) * gd[0] + x];
  close(cell(1, 0, 0), 1);
  close(cell(0, 0, 0), 1, 1e-9);   // apron: x = 8 is read by cell 0's trilinear samples
  close(cell(2, 1, 0), 127 / 254);
  close(cell(2, 0, 0), 127 / 254);  // y apron (y = 8 is the first voxel of y-cell 1)
  // Brute force: every cell >= every voxel inside it.
  for (let z = 0; z < dims[2]; z++) for (let y = 0; y < dims[1]; y++) for (let x = 0; x < dims[0]; x++) {
    const v = V.codeToNorm(data[at(x, y, z)]);
    assert.ok(cell(Math.floor(x / 8), Math.floor(y / 8), Math.floor(z / 8)) >= v);
  }
});

test('nice ticks and world <-> texture on a decreasing axis (RA, CDELT1 < 0)', () => {
  const n = 10;
  const world = Array.from({ length: n }, (_, i) => 155.5 - 0.2 * i);   // voxel centres
  const t = V.worldToTex(world, 155.5);
  close(t, 0.5 / n);
  close(V.worldToTex(world, 155.4), 1 / n);
  close(V.texToWorld(world, 0.5 / n), 155.5);
  close(V.texToWorld(world, V.worldToTex(world, 154.37)), 154.37, 1e-9);
  assert.equal(V.worldToTex(world, 160), null);
  const ticks = V.niceTicks(153.7, 155.5, 4);
  assert.deepEqual(ticks.map((v) => +v.toFixed(6)), [154, 154.5, 155, 155.5]);
});

test('the z axis is labelled in km/s when the backend gives a velocity', () => {
  const vel = [1310, 1306, 1302, 1298];               // decreasing with channel (FREQ cube)
  const r = V.axisTicks({ name: 'FREQ', unit: 'Hz', world: [1, 2, 3, 4], velocity_kms: vel }, 'z', 3);
  assert.equal(V.axisTitle(r), 'Velocity (km/s)');
  for (const tk of r.ticks) close(V.texToWorld(vel, tk.t), tk.value, 1e-9);
  const native = V.axisTicks({ name: 'FREQ', unit: 'Hz', world: [1, 2, 3, 4], velocity_kms: null }, 'z');
  assert.equal(V.axisTitle(native), 'FREQ (Hz)');
});

test('box extent and the explicit velocity scale', () => {
  // The box spans the cube's 700 pixels, not the 702 of 234 whole blocks.
  assert.deepEqual(V.boxExtent([700, 700, 158], 1).map((v) => +v.toFixed(6)), [1, 1, +(158 / 700).toFixed(6)]);
  close(V.boxExtent([700, 700, 158], 2)[2], 316 / 700);
  const ts = V.texScale([700, 700, 158], [3, 3, 1], [234, 234, 158]);
  close(ts[0], 700 / 702); close(ts[2], 1);
  close(V.texToBox(V.boxToTex(0.37, 700, 3), 700, 3), 0.37, 1e-9);
  const info = { pixel_scale_arcsec: 8, factors: [3, 3, 1], axes: { z: { channel_width_kms: 8.24 } } };
  assert.match(V.velocityScaleText(info, 1), /1 full-resolution channel = 8\.24 km\/s drawn as 8\.0″ of sky \(61\.8 km\/s per arcmin/);
  assert.match(V.velocityScaleText(info, 1), /a drawn voxel holds 1 channel$/);
  assert.match(V.velocityScaleShort(info, 2), /8\.24 km\/s = 16\.0″ \(×2\.00\)/);
  assert.match(V.velocityScaleShort({ ...info, factors: [2, 2, 2] }, 1), /2 per voxel/);
});

test('ticks land on their world value with partial edge blocks', () => {
  // 7 pixels binned by 3: blocks [0-2], [3-5], [6]; centres 1, 4, 6 (backend's
  // block_centres). World = pixel index here, so tick v must sit at box u = (v + 0.5) / 7.
  assert.deepEqual(V.blockCentres(7, 3), [1, 4, 6]);
  const world = [1, 4, 6];
  for (const v of [1, 2, 3, 4, 5, 6]) {
    close(V.texToBox(V.worldToTex(world, v), 7, 3), (v + 0.5) / 7, 1e-9);
  }
  // ...and back: the world at a box coordinate.
  for (const v of [1.5, 4, 5.25]) close(V.texToWorld(world, V.boxToTex((v + 0.5) / 7, 7, 3)), v, 1e-9);
});

test('auto factors match the backend rule', () => {
  assert.deepEqual(V.autoFactors([700, 700, 158], 256), [3, 3, 1]);
  assert.deepEqual(V.autoFactors([196, 280, 109], 256), [2, 2, 1]);
  assert.deepEqual(V.reducedShape([700, 700, 158], [3, 3, 1]), [234, 234, 158]);
});

test('the governor lowers sampling on missed frames and raises it slowly', () => {
  const g = new V.Governor({ start: 1, max: 1, min: 0.25, upAfterMs: 1000 });
  for (let i = 0; i < 30; i++) g.update(22.2, 11.1);     // every frame missed at 90 Hz
  assert.ok(g.value < 0.2 + 0.25 && g.value >= 0.25);
  const low = g.value;
  for (let i = 0; i < 100; i++) g.update(11.1, 11.1);    // 1.1 s on time
  assert.ok(g.value > low && g.value <= 1);
  for (let i = 0; i < 10000; i++) g.update(11.1, 11.1);
  close(g.value, 1);
});

test('frame statistics', () => {
  const s = new V.FrameStats(10);
  for (const v of [11, 11, 11, 11, 22]) s.push(v, 1, 2);
  const r = s.summary(11.1);
  close(r.interval.mean, 13.2);
  close(r.missedFraction, 0.2);
  close(r.gpu.mean, 2);
});

test('synthetic cube has no blank code and emission above the noise', () => {
  const c = V.syntheticCube(32);
  assert.equal(c.length, 32 ** 3);
  assert.ok(c.every((v) => v >= 1));
  const above = c.filter((v) => V.codeToNorm(v) > 0.25).length;
  assert.ok(above > 50 && above < c.length * 0.2);
});

test('matrices: inverse, ray-box, grabs', () => {
  const m = M.multiply(M.translation(1, 2, 3), M.multiply(M.rotation([0, 1, 0], 0.7), M.scaling(0.5)));
  const p = M.transformPoint(M.multiply(M.invert(m), m), [0.3, -0.2, 0.9]);
  [0.3, -0.2, 0.9].forEach((v, i) => close(p[i], v, 1e-6));
  assert.deepEqual(M.rayBox([0.5, 0.5, -1], [0, 0, 1], [0, 0, 0], [1, 1, 1]), [1, 2]);
  assert.equal(M.rayBox([2, 0.5, -1], [0, 0, 1], [0, 0, 0], [1, 1, 1]), null);
  // One hand: the model follows the controller rigidly.
  const c0 = M.translation(0, 1, 0), model = M.translation(0, 1, -1);
  const rel = M.multiply(M.invert(c0), model);
  const c1 = M.multiply(M.translation(0.2, 1, 0), M.rotation([0, 1, 0], Math.PI / 2));
  const moved = M.grabOne(c1, rel);
  close(moved[12], 0.2 - 1, 1e-6); close(moved[14], 0, 1e-6);
  // Two hands: doubling the hand distance doubles the scale, about the midpoint.
  const two = M.grabTwo(M.identity(), [-0.1, 1, 0], [0.1, 1, 0], [-0.2, 1, 0], [0.2, 1, 0]);
  close(M.uniformScale(two), 2, 1e-6);
  const q = M.transformPoint(two, [0, 1, 0]);
  close(q[0], 0, 1e-6); close(q[1], 1, 1e-6);
  // Rotating the hands about the vertical turns the model with them.
  const turned = M.grabTwo(M.identity(), [-0.1, 1, 0], [0.1, 1, 0], [0, 1, 0.1], [0, 1, -0.1]);
  const x = M.transformPoint(turned, [1, 1, 0]);
  close(x[0], 0, 1e-6); close(x[2], -1, 1e-6);
});

// ── Longitudes across 0° ──────────────────────────────────────────────────

test('an RA axis crossing 0° is interpolated and ticked through 0, not 180', () => {
  const world = [359.8, 359.9, 0, 0.1, 0.2];
  const axis = { name: 'RA', ctype: 'RA---SIN', unit: 'deg', world };
  assert.ok(V.isLongitudeAxis(axis));
  assert.deepEqual(V.unwrapDegrees(world).map((v) => +v.toFixed(6)), [359.8, 359.9, 360, 360.1, 360.2]);
  // Halfway between the 2nd and 3rd voxel centres: 359.95, not 179.95.
  const t = (1.5 + 0.5) / 5;
  close(V.axisTexToWorld(axis, world, t), 359.95, 1e-9);
  close(V.axisTexToWorld(axis, world, (2 + 0.5) / 5), 0, 1e-9);
  close(V.axisTexToWorld(axis, world, (3.5 + 0.5) / 5), 0.15, 1e-9);
  const ax = V.axisTicks(axis, 'x', 5);
  const values = ax.ticks.map((k) => +k.value.toFixed(6));
  assert.ok(values.every((v) => v >= 0 && v < 360), values.join());
  assert.ok(values.every((v) => v > 359.7 || v < 0.3), `no tick far from the field: ${values.join()}`);
  assert.ok(values.includes(0), values.join());
  // Ticks in order along the box, and each one where its value is.
  for (let i = 1; i < ax.ticks.length; i++) assert.ok(ax.ticks[i].t > ax.ticks[i - 1].t);
  for (const k of ax.ticks) close(V.axisTexToWorld(axis, world, k.t), k.value, 1e-6);
  assert.equal(ax.ticks.find((k) => k.value === 0).label.replace(/0/g, '').replace('.', ''), '');
});

test('decreasing RA across 0° (CDELT1 < 0), and axes that are not longitudes', () => {
  const world = [0.2, 0.1, 0, 359.9, 359.8];
  const axis = { name: 'RA', unit: 'deg', world };
  close(V.axisTexToWorld(axis, world, (2.5 + 0.5) / 5), 359.95, 1e-9);
  const ticks = V.axisTicks(axis, 'x', 5).ticks;
  assert.ok(ticks.length >= 3);
  assert.ok(ticks.every((k) => k.value > 359.7 || k.value < 0.3));
  // Dec, a velocity axis and an RA away from 0° are unchanged.
  const dec = { name: 'DEC', unit: 'deg', world: [-0.2, -0.1, 0, 0.1, 0.2] };
  assert.ok(!V.isLongitudeAxis(dec));
  close(V.axisTexToWorld(dec, dec.world, 0.5), 0, 1e-12);
  const ra = { name: 'RA', unit: 'deg', world: [150.2, 150.1, 150.0, 149.9] };
  close(V.axisTexToWorld(ra, ra.world, 0.5), V.texToWorld(ra.world, 0.5), 1e-12);
  assert.deepEqual(V.axisTicks(ra, 'x', 4), (() => { const a = { ...ra, name: 'X' }; const r = V.axisTicks(a, 'x', 4); return { ...r, title: 'RA' }; })());
  const vel = { name: 'VRAD', unit: 'km/s', world: [350, 10] };
  assert.ok(!V.isLongitudeAxis(vel));
  assert.ok(!V.isLongitudeAxis({ name: 'RA', unit: 'rad', world: [6.2, 0.1] }));
  assert.equal(V.wrap360(-0.1).toFixed(6), '359.900000');
  assert.equal(V.wrap360(360), 0);
  assert.equal(V.wrap360(720.5), 0.5);
});

test('a pixel picked across 0° gets its RA through 0 (selection.js worldOfPixel)', async () => {
  const S = await import('../../app/static/vr/js/selection.js');
  const info = { axes: { x: { name: 'RA', unit: 'deg', world: [359.8, 359.9, 0, 0.1, 0.2] },
    y: { name: 'DEC', unit: 'deg', world: [1, 2, 3, 4, 5] }, z: { name: 'VRAD', unit: 'm/s', world: [1, 2, 3, 4, 5] } },
  factors: [1, 1, 1], full_shape: [5, 5, 5] };
  const w = S.worldOfPixel(info, [2, 2, 2]);
  close(w.x, 0, 1e-9);
  close(S.worldOfPixel(info, [1, 2, 2]).x, 359.9, 1e-9);
});

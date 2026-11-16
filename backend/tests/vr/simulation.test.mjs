// Simulation volumes on the VR page (issue #317): values on a log scale shown
// as values, the box drawn to the grid spacing, no spectral wording, field
// references opened as volumes, the palette's spectral tools greyed, the
// refined frames' transfer function on the log scale (node --test).
import test from 'node:test';
import assert from 'node:assert/strict';
import * as V from '../../app/static/vr/js/volume.js';
import * as LV from '../../app/static/vr/js/levels.js';
import * as RO from '../../app/static/vr/js/readout.js';
import * as TP from '../../app/static/vr/js/tool-palette.js';
import * as HUB from '../../app/static/vr/js/headset.js';
import { reducedScale } from '../../app/static/vr/js/api.js';
import { kindOfPath } from '../../app/static/vr/js/layers.js';
import { transferFunction } from '../../app/static/vr/js/refine.js';
import { colormapTable } from '../../app/static/vr/js/colormaps.js';

const close = (a, b, eps = 1e-9) => assert.ok(Math.abs(a - b) <= eps * Math.max(1, Math.abs(b)), `${a} != ${b}`);
const lin = (a, b, n) => Array.from({ length: n }, (_, i) => a + (b - a) * i / (n - 1));

/** reduced_info of a 64 x 32 x 16 VBT volume, voxel 0.5 x 0.5 x 1 (kpc). */
function simInfo(spacing = [0.5, 0.5, 1.0], unit = 'kpc') {
  const ax = (name, n, d) => ({ name, unit, world: lin(0, (n - 1) * d, n), spacing: d });
  return {
    simulation: true, object: 'rho', full_shape: [64, 32, 16], factors: [1, 1, 1], reduced_shape: [64, 32, 16],
    spacing, noise_sigma: null, beam: null, rest_frequency_hz: null, rest_line: '', pixel_scale_arcsec: null,
    axes: { x: ax('X', 64, spacing[0]), y: ax('Y', 32, spacing[1]), z: { ...ax('Z', 16, spacing[2]), velocity_kms: null } },
  };
}

test('a log-scaled cube: codes are of log10, every value shown is a value', () => {
  const lo = -12, hi = -5;
  close(V.normToData(0, lo, hi, 'log'), 1e-12, 1e-9);
  close(V.normToData(1, lo, hi, 'log'), 1e-5, 1e-9);
  close(V.normToData(0.5, lo, hi, 'log'), 10 ** -8.5, 1e-9);
  close(V.dataToNorm(1e-8, lo, hi, 'log'), 4 / 7, 1e-12);
  assert.equal(V.dataToNorm(0, lo, hi, 'log'), -Infinity, 'zero is below every code');
  assert.equal(V.dataToNorm(-1, lo, hi, 'log'), -Infinity);
  // Linear is the default and unchanged.
  close(V.normToData(0.25, -1, 3), 0);
  close(V.dataToNorm(0, -1, 3), 0.25);
  assert.equal(V.valueText(1.83467e-12), '1.83e-12');
  assert.equal(V.valueText(2.11593e-5), '2.12e-5');
  assert.equal(V.valueText(12.5), '12.5');
});

test('the reduced cube\'s headers: a log scale gives the log range to the shader and values to the page', () => {
  const H = (o) => (k) => (k in o ? o[k] : null);
  const log = reducedScale(H({ 'X-Visivo-Scale': 'log', 'X-Visivo-Log-Range': '-11.7,-4.67',
    'X-Visivo-Range-Min': '2e-12', 'X-Visivo-Range-Max': '2.1e-5', 'X-Visivo-Nonpositive-Voxels': '3' }));
  assert.deepEqual(log, { scale: 'log', lo: -11.7, hi: -4.67, valueMin: 2e-12, valueMax: 2.1e-5, nonpositive: 3 });
  // A radio cube, and a backend without the header: linear, the range the values'.
  const radio = reducedScale(H({ 'X-Visivo-Scale': 'linear', 'X-Visivo-Log-Range': '', 'X-Visivo-Range-Min': '-0.01', 'X-Visivo-Range-Max': '0.3' }));
  assert.deepEqual([radio.scale, radio.lo, radio.hi], ['linear', -0.01, 0.3]);
  assert.equal(reducedScale(H({ 'X-Visivo-Range-Min': '0', 'X-Visivo-Range-Max': '1' })).scale, 'linear');
  assert.throws(() => reducedScale(H({ 'X-Visivo-Scale': 'log', 'X-Visivo-Range-Min': '1', 'X-Visivo-Range-Max': '2' })), /Log-Range/);
});

test('the box follows the grid spacing; a cube keeps its pixel box', () => {
  const info = simInfo();
  assert.deepEqual(V.voxelAspect(info), [0.5, 0.5, 1]);
  // 64 x 0.5 = 32, 32 x 0.5 = 16, 16 x 1 = 16 kpc: 1 : 0.5 : 0.5.
  assert.deepEqual(V.boxExtent(info.full_shape, 1, V.voxelAspect(info)), [1, 0.5, 0.5]);
  assert.deepEqual(V.boxExtent(info.full_shape, 1), [1, 0.5, 0.25], 'by voxel counts');
  // A uniform grid: the same box either way.
  assert.deepEqual(V.voxelAspect(simInfo([2, 2, 2])), [1, 1, 1]);
  // Units that differ, a missing spacing, a radio cube: no aspect.
  const mixed = simInfo();
  mixed.axes.z.unit = 'Myr';
  assert.equal(V.voxelAspect(mixed), null);
  assert.equal(V.voxelAspect({ ...simInfo(), spacing: [0.5, null, 1] }), null);
  assert.equal(V.voxelAspect({ ...simInfo(), simulation: false }), null);
  assert.equal(V.voxelAspect({ ...simInfo(), spacing: [1, 1, 5000] }), null, 'past the render session\'s ratio');
});

test('no channel, velocity or sky wording for a simulation volume', () => {
  const info = simInfo();
  const words = /channel|velocity|km\/s|beam|rest|σ|″/i;
  for (const text of [V.velocityScaleText(info, 1), V.velocityScaleShort(info, 1), LV.infoLine(info),
    LV.infoLine(info, { scale: 'log', valueMin: 1.8e-12, valueMax: 2.1e-5, bunit: 'g/cm3', nonpositive: 0 })]) {
    assert.ok(!words.test(text), text);
  }
  assert.match(V.velocityScaleText(info, 1), /grid spacing/);
  assert.match(LV.infoLine(info), /^Simulation volume: X, Y, Z in kpc · voxel 0\.5 × 0\.5 × 1$/);
  assert.match(LV.infoLine(info, { scale: 'log', valueMin: 1.8e-12, valueMax: 2.1e-5, bunit: 'g/cm3' }), /log scale 1\.80e-12 to 2\.10e-5 g\/cm3/);
  // The axes are ticked in their own units, large ones in scientific notation.
  const big = { name: 'X', unit: 'cm', world: lin(0, 3e18, 64) };
  const t = V.axisTicks(big, 'x', 4);
  assert.equal(t.title, 'X');
  assert.ok(t.ticks.length >= 3 && t.ticks.every((k) => /e\+18$|^0$/.test(k.label)), t.ticks.map((k) => k.label).join(' '));
  assert.equal(V.axisTitle(V.axisTicks(info.axes.z, 'z', 4)), 'Z (kpc)');
});

test('the readout: X, Y, Z in the axes\' units and the value, never a channel', () => {
  const info = simInfo();
  const n = 64 * 32 * 16;
  const data = new Uint8Array(n).fill(128);           // the middle code: 10^-8.5 on -12..-5
  const cube = { data, dims: [64, 32, 16], encoding: 'uint8', lo: -12, hi: -5, scale: 'log', bunit: 'g/cm3', factors: [1, 1, 1] };
  const p = [(10 + 0.5) / 64, (4 + 0.5) / 32, (3 + 0.5) / 16];
  const lines = RO.readoutLines('EXPLORE', p, { info, cube, texScale: [1, 1, 1] });
  assert.equal(lines.length, 4);
  assert.equal(lines[1], 'X 5.000  Y 2.000 kpc');
  assert.equal(lines[2], 'Z 3.000 kpc  voxel 10, 4, 3');
  const v = 10 ** (-12 + (127 / 254) * 7);
  assert.equal(lines[3], `${v.toExponential(3)} g/cm3`);
  assert.ok(!/ch |km\/s/.test(lines.join(' ')));
});

test('the colour bar of a log-scaled volume: values from threshold to colour max, ticks at the decades', () => {
  const bar = LV.volumeBar({ thrNorm: 4 / 7, cmaxNorm: 1, lo: -12, hi: -5, scale: 'log', stretch: 'linear', stretchA: 0,
    title: 'rho', unit: 'g/cm3' });
  close(bar.lo, 1e-8, 1e-9);
  close(bar.hi, 1e-5, 1e-9);
  assert.equal(bar.title, 'rho [g/cm3], log scale');
  assert.deepEqual(bar.ticks.map((v) => Math.round(Math.log10(v))), [-8, -7, -6, -5]);
  // Linear in log10 on the Linear stretch: the decades evenly spaced.
  bar.ticks.forEach((v, k) => close(bar.at(v), k / 3, 1e-9));
  // Sqrt: placed where the shader puts their colour.
  const sq = LV.volumeBar({ thrNorm: 4 / 7, cmaxNorm: 1, lo: -12, hi: -5, scale: 'log', stretch: 'sqrt', stretchA: 0, title: 'rho' });
  close(sq.at(1e-7), Math.sqrt(1 / 3), 1e-9);
  // A linear volume: nice ticks.
  const l = LV.volumeBar({ thrNorm: 0, cmaxNorm: 1, lo: 0, hi: 10, scale: 'linear', stretch: 'linear', stretchA: 0, title: 'T' });
  assert.equal(l.log, false);
  assert.deepEqual(l.ticks, V.niceTicks(0, 10, 4));
  l.ticks.forEach((v) => close(l.at(v), v / 10, 1e-12));
});

test('σ on a log scale: k σ found on the logarithm, zero below the codes', () => {
  // 3 σ = 3e-7 on a cube spanning 1e-12..1e-5.
  const t = LV.sigmaThreshold(3, 1e-7, -12, -5, 'log');
  close(t.norm, (Math.log10(3e-7) + 12) / 7, 1e-12);
  assert.equal(t.clamped, false);
  assert.ok(LV.sigmaMarks(1e-7, -12, -5, [3], 0.06, 'log').length === 1);
});

test('the refined frames\' transfer function: points in log10 of the values, the session told to draw in log10 (#328)', () => {
  const tf = transferFunction({ thrNorm: 4 / 7, cmaxNorm: 1, stretch: 'linear', stretchA: 0, opacity: 1, mode: 0,
    lo: -12, hi: -5, scale: 'log', factors: [1, 1, 1], lut: colormapTable('viridis') }, 6);
  assert.equal(tf.scale, 'log');
  const xs = tf.color.map((c) => c[0]);
  close(xs[0], -8, 1e-9);
  close(xs[xs.length - 1], -5, 1e-9);
  // Evenly spaced in log10: half a decade between neighbours.
  for (let i = 1; i < xs.length; i++) close(xs[i] - xs[i - 1], 0.5, 1e-9);
  // The transparent point just below the threshold.
  assert.ok(tf.opacity[0][0] < -8 && tf.opacity[0][0] > -8.001);
  assert.equal(tf.opacity[0][1], 0);
  // A linear cube: values, no scale.
  const lin = transferFunction({ thrNorm: 0.5, cmaxNorm: 1, stretch: 'linear', stretchA: 0, opacity: 1, mode: 0,
    lo: 0, hi: 2, scale: 'linear', factors: [1, 1, 1], lut: colormapTable('viridis') }, 4);
  assert.equal(lin.scale, undefined);
  close(lin.color[0][0], 1, 1e-9);
});

test('the desktop is told Log for the page\'s Linear on a log-scaled cube', () => {
  assert.equal(HUB.desktopStretch('linear', 'log'), 'log');
  assert.equal(HUB.desktopStretch('sqrt', 'log'), 'sqrt');
  assert.equal(HUB.desktopStretch('linear', 'linear'), 'linear');
  assert.equal(HUB.desktopStretch('sqrt'), 'sqrt');
});

test('a field reference opens as a volume, whatever its file is called', () => {
  assert.equal(kindOfPath('/d/v.bin::vbt-field=0'), 'cube');
  assert.equal(kindOfPath('/d/v.bin::vbt-field=12::vbt-order=big'), 'cube');
  assert.equal(kindOfPath('/d/data.0004.dbl::pluto-var=rho'), 'cube');
  assert.equal(kindOfPath('/d/rho.0000.flt::pluto-var=rho'), 'cube');
  // A table stays a table.
  assert.equal(kindOfPath('/d/v.bin'), 'points');
  assert.equal(kindOfPath('/d/v.bin.head'), 'points');
  assert.equal(kindOfPath('/d/v.bin.head::vbt-order=little'), 'points');
  assert.equal(kindOfPath('/d/cube.fits'), 'cube');
  assert.equal(kindOfPath('/d/v.bin::vbt-field=0', 'points'), 'points', 'the selector wins');
});

test('the palette greys the spectral tools on a simulation volume (kind volume)', () => {
  const layout = TP.paletteLayout({ kind: 'volume', active: 'explore' });
  const on = layout.filter((w) => w.tool && !w.disabled).map((w) => w.tool);
  assert.deepEqual(on, ['explore', 'box', 'crop']);
  for (const id of ['lasso', 'probe', 'slab', 'moment', 'pv']) {
    assert.match(TP.whyNot(TP.toolById(id), 'volume'), /simulation volume has no spectral axis/);
  }
  // The stick steps only through what applies.
  assert.equal(TP.stepTool('explore', 'volume', 1), 'box');
  assert.equal(TP.stepTool('box', 'volume', 1), 'crop');
  assert.equal(TP.stepTool('crop', 'volume', 1), 'explore');
  // A cube keeps every tool.
  assert.equal(TP.paletteLayout({ kind: 'cube', active: 'explore' }).filter((w) => w.tool && !w.disabled).length, TP.TOOLS.length);
});

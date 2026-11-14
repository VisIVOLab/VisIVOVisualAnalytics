// The cube's value scale (issue #258, levels.js): the stretches and their
// inverses (shader and refined frames), the threshold in σ, the histogram and
// its percentiles, the info line (node --test).
import test from 'node:test';
import assert from 'node:assert/strict';
import * as LV from '../../app/static/vr/js/levels.js';
import { transferFunction } from '../../app/static/vr/js/refine.js';
import { colormapTable } from '../../app/static/vr/js/colormaps.js';

const close = (a, b, eps = 1e-9) => assert.ok(Math.abs(a - b) <= eps, `${a} != ${b}`);

test('every stretch maps 0..1 onto 0..1, rises, and its inverse undoes it', () => {
  for (const s of LV.STRETCHES) {
    const a = LV.stretchParam(s, { gamma: 2.5, thrData: 0.003, cmaxData: 0.3 });
    close(LV.stretchValue(0, s, a), 0);
    close(LV.stretchValue(1, s, a), 1);
    let prev = -1;
    for (let k = 0; k <= 50; k++) {
      const t = k / 50;
      const v = LV.stretchValue(t, s, a);
      assert.ok(v >= prev - 1e-12, `${s} rises at ${t}`);
      prev = v;
      close(LV.stretchInverse(v, s, a), t, 1e-9);
    }
  }
  close(LV.stretchValue(0.25, 'sqrt', 0), 0.5);
  close(LV.stretchValue(0.5, 'square', 0), 0.25);
  close(LV.stretchValue(0.5, 'power', 3), 0.125);
  // Out-of-range input is clamped, as the shader's clamp.
  assert.equal(LV.stretchValue(-1, 'linear', 0), 0);
  assert.equal(LV.stretchValue(2, 'log', 10), 1);
});

test('the log is the desktop\'s log10(v / min) / log10(max / min) over threshold..colour max', () => {
  const thr = 0.004, cmax = 0.4;
  const a = LV.stretchParam('log', { thrData: thr, cmaxData: cmax });
  close(a, 99, 1e-9);
  for (const v of [0.004, 0.01, 0.05, 0.2, 0.4]) {
    const t = (v - thr) / (cmax - thr);
    close(LV.stretchValue(t, 'log', a), Math.log10(v / thr) / Math.log10(cmax / thr), 1e-12);
  }
  // A threshold at or below zero: astropy's a = 1000.
  assert.equal(LV.stretchParam('log', { thrData: -0.01, cmaxData: 0.4 }), LV.LOG_A_DEFAULT);
  assert.equal(LV.stretchParam('power', { gamma: 50 }), LV.GAMMA_MAX);
  assert.equal(LV.stretchParam('sqrt', {}), 0);
  assert.equal(LV.stretchOf('nope'), LV.DEFAULT_STRETCH);
  assert.equal(LV.stepGamma(2, 1), 2.5);
  assert.equal(LV.stepGamma(2, -1), 1.6);
  assert.equal(LV.stepGamma(LV.GAMMA_MAX, 1), LV.GAMMA_MAX);
});

test('the refined frames\' transfer function follows the stretch: the map\'s middle where the stretch reaches 0.5', () => {
  const lut = colormapTable('viridis');
  const base = { thrNorm: 0.1, cmaxNorm: 0.6, opacity: 0.4, mode: 0, lo: -2, hi: 18, factors: [1, 1, 1], lut };
  for (const s of LV.STRETCHES) {
    const a = LV.stretchParam(s, { gamma: 3, thrData: 0, cmaxData: 10 });
    const tf = transferFunction({ ...base, stretch: s, stretchA: a }, 16);
    const xs = tf.color.map((c) => c[0]);
    assert.deepEqual(xs, [...xs].sort((p, q) => p - q), s);
    const t = LV.stretchInverse(0.5, s, a);
    close(tf.color[8][0], -2 + (0.1 + t * 0.5) * 20, 1e-9);
  }
  // Without a stretch: the old power of gamma (the sqrt default before #258).
  const old = transferFunction({ ...base, gamma: 0.5 }, 16);
  close(old.color[8][0], -2 + (0.1 + 0.25 * 0.5) * 20, 1e-9);
});

test('the threshold in σ: n σ is the value n·σ, as the desktop\'s 3σ mask', () => {
  const info = { noise_sigma: 0.002 };
  assert.equal(LV.sigmaOf(info), 0.002);
  assert.equal(LV.sigmaOf({ noise_sigma: null }), null);
  assert.equal(LV.sigmaOf({ noise_sigma: 0 }), null);
  assert.equal(LV.sigmaText(0.0064, 0.002), '= 3.2 σ');
  assert.equal(LV.sigmaText(0.03, 0.002), '= 15 σ');
  assert.equal(LV.sigmaText(0.03, null), '');
  const t = LV.sigmaThreshold(3, 0.002, -0.01, 0.19);
  close(t.norm, (0.006 + 0.01) / 0.2);
  assert.equal(t.clamped, false);
  assert.deepEqual(LV.sigmaThreshold(500, 0.002, -0.01, 0.19), { norm: 1, clamped: true });
  assert.equal(LV.sigmaThreshold(3, null, 0, 1), null);
  // Marks on the slider's scale (sqrt of the normalised value), close ones dropped.
  const marks = LV.sigmaMarks(0.002, -0.01, 0.19);
  assert.ok(marks.find((m) => m.k === 3));
  for (let i = 1; i < marks.length; i++) assert.ok(marks[i].at - marks[i - 1].at >= 0.06);
  close(marks.find((m) => m.k === 3).at, Math.sqrt(0.08));
});

test('the histogram: uint8 codes are bins of their own; percentiles and the fraction above a threshold', () => {
  // uint8: code 0 blank, codes 1..255 = (code - 1) / 254.
  const data = new Uint8Array(10000);
  for (let i = 0; i < data.length; i++) data[i] = i < 1000 ? 0 : 1 + (i % 100);   // 9000 values, codes 1..100
  const h = LV.normHistogram(data, 'uint8', 1e9);
  assert.equal(h.n, 9000);
  assert.equal(h.counts.length, 255);
  assert.equal(h.counts[0], 90);
  assert.equal(h.counts[99], 90);
  assert.equal(h.counts[100], 0);
  // The median of codes 1..100 (values 0..99/254) is about 49.5/254.
  close(LV.histQuantile(h, 0.5), 49.5 / 254, 0.6 / 254);
  close(LV.percentileThreshold(h, 99), 98.5 / 254, 1 / 254);
  close(LV.fractionAbove(h, 49.5 / 254), 0.5, 0.01);
  assert.equal(LV.fractionAbove(h, 1), 0);
  // The 99th percentile of what is above a threshold (the colour max rule).
  const q = LV.histQuantile(h, 0.99, 80 / 254);
  assert.ok(q > 98 / 254 && q <= 100 / 254, String(q));
  // Float16: bins over 0..1; blank (-1) left out.
  const f = new Uint16Array([0x3800, 0x3800, 0xbc00, 0x3c00]);    // 0.5, 0.5, -1, 1
  const hf = LV.normHistogram(f, 'float16');
  assert.equal(hf.n, 3);
  close(LV.histQuantile(hf, 0.5), 0.5, 1 / LV.FLOAT_BINS);
  assert.equal(LV.histQuantile({ n: 0 }, 0.5), null);
});

test('the histogram along the slider: area kept, a bin wider than a column spread over its columns', () => {
  const data = new Uint8Array(5000);
  for (let i = 0; i < data.length; i++) data[i] = 1 + (i % 255);
  const h = LV.normHistogram(data, 'uint8', 1e9);
  const n = 160;
  const d = LV.sliderDensity(h, n);
  const area = d.reduce((s, v) => s + v / n, 0);
  close(area, h.n, 1e-6);
  // No empty column inside the values: uint8's codes leave no gaps on the slider's scale.
  for (let j = 0; j < n; j++) assert.ok(d[j] > 0, `column ${j}`);
});

test('the info line: HI rest frequency, channel width in km/s (drawn binning), beam', () => {
  const info = {
    rest_frequency_hz: 1420405751.768, rest_line: 'HI', factors: [2, 2, 3],
    beam: { major_arcsec: 15, minor_arcsec: 12.04, pa_deg: 33.4, per_channel: false },
    axes: { z: { channel_width_kms: 5.153, velocity_convention: 'optical (cz), from frequency and RESTFRQ', specsys: 'BARYCENT', world: [] } },
  };
  assert.equal(LV.infoLine(info), 'HI rest 1420.406 MHz · channel 5.15 km/s (drawn ×3: 15.5) · beam 15.0″ × 12.0″, PA 33°');
  assert.equal(LV.conventionLine(info), 'velocity optical (cz), from frequency and RESTFRQ, BARYCENT');
  // No rest frequency, no velocity axis, no beam: the native channel width.
  const plain = { rest_frequency_hz: null, factors: [1, 1, 2], beam: null,
    axes: { z: { channel_width_kms: null, unit: 'Hz', world: [1.4e9, 1.4e9 - 48828.125, 1.4e9 - 97656.25] } } };
  assert.equal(LV.infoLine(plain), 'no rest frequency · channel 24.414 kHz');
  assert.equal(LV.beamText({ major_arcsec: 30, minor_arcsec: 30, pa_deg: null, per_channel: true }), 'beam 30.0″ × 30.0″ (central channel)');
  assert.equal(LV.infoLine(null), '');
  // A simulation volume (no spectral keywords): nothing to say.
  assert.equal(LV.infoLine({ factors: [1, 1, 1], axes: { z: { unit: '', world: [0, 1, 2] } } }), '');
  assert.equal(LV.freqText(115.2712018e9), '115.271202 GHz');
});

test('the sliders\' scale is the square root of the normalised value', () => {
  assert.equal(LV.normToSlider(0.25), 500);
  close(LV.sliderToNorm(500), 0.25);
  assert.equal(LV.normToSlider(2), 1000);
});

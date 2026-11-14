// Result windows in the headset: the title bar above the content, the window
// named after what it shows, long lines wrapped (issue #267).
// node --test backend/tests/vr/windows.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';
import * as W from '../../app/static/vr/js/windows.js';
import * as XL from '../../app/static/vr/js/xr-layers.js';
import * as M from '../../app/static/vr/js/mat4.js';
import { wrapText, fitText } from '../../app/static/vr/js/text-fit.js';
import { PLANE_SIZE, PLANE_PX, PRODUCT_ROWS, productLines } from '../../app/static/vr/js/product-canvas.js';
import { createSelection, TOOL_NAMES } from '../../app/static/vr/js/select-ui.js';

const close = (a, b, eps = 1e-6, what = '') => assert.ok(Math.abs(a - b) < eps, `${what} ${a} vs ${b}`);
/** Every result window with the content size main.js gives it. */
const SIZES = { result: W.WINDOWS.result.size, moment: PLANE_SIZE, pv: PLANE_SIZE };
/** Text widths as a fixed advance per character (a monospace font). */
const mono = (px) => (s) => String(s).length * px;

test('every window: the bar sits above its content, as wide, and never covers it', () => {
  for (const [id, size] of Object.entries(SIZES)) {
    const r = W.windowRects(size);
    const [cx0, cy0, cx1, cy1] = r.content, [bx0, by0, bx1, by1] = r.bar;
    close(cx1 - cx0, size[0], 1e-9, `${id} content width`);
    close(cy1 - cy0, size[1], 1e-9, `${id} content height`);
    close(bx0, cx0, 1e-9, `${id} left edges`); close(bx1, cx1, 1e-9, `${id} right edges`);
    assert.ok(by0 > cy1, `${id}: the bar starts above the content's top`);
    close(by0 - cy1, W.BAR_GAP, 1e-9, `${id} gap`);
    close(by1 - by0, W.barSize(size[0])[1], 1e-9, `${id} bar height`);
    // barModel puts the bar quad's centre at the middle of that rectangle.
    const model = M.multiply(M.translation(0.2, 1.4, -0.9), M.rotation([0, 1, 0], 0.6));
    const local = M.transformPoint(M.invert(model), M.position(W.barModel(model, size)));
    close(local[0], 0, 1e-5, `${id} x`); close(local[2], 0, 1e-5, `${id} z`);
    close(local[1], (by0 + by1) / 2, 1e-5, `${id} y`);
  }
});

const QUEST_UA = 'Mozilla/5.0 (X11; Linux x86_64; Quest 3) AppleWebKit/537.36 (KHTML, like Gecko) OculusBrowser/40.1.0.12.62 SamsungBrowser/4.0 Chrome/134.0.6998.193 VR Safari/537.36';
const CHROME_UA = 'Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/142.0.0.0 Safari/537.36';

test('the quad extent factor: half on the Quest Browser (±width), full elsewhere, ?layerhalf overrides', () => {
  assert.equal(XL.quadExtentFactor(QUEST_UA), 0.5);
  assert.equal(XL.quadExtentFactor(CHROME_UA), 1);
  assert.equal(XL.quadExtentFactor(''), 1);
  assert.equal(XL.quadExtentFactor(undefined), 1);
  assert.equal(XL.quadExtentFactor(CHROME_UA, '1'), 0.5);
  assert.equal(XL.quadExtentFactor(QUEST_UA, '0'), 1);
  assert.equal(XL.quadExtentFactor(QUEST_UA, null), 0.5);
  assert.deepEqual(XL.quadLayerSize([0.34, 0.248], [2, 2], 0.5), [0.34, 0.248]);
  assert.deepEqual(XL.quadLayerSize([0.34, 0.248]), [0.34, 0.248]);
});

test('as quad layers, in either reading of width and height, the bar and the content land on their rectangles', () => {
  // How each browser draws a layer: the Quest Browser spans ±width, ±height
  // (factor 0.5); a browser taking them as the full size spans ±width/2.
  for (const [ua, halfOf] of [[QUEST_UA, (w) => w], [CHROME_UA, (w) => w / 2]]) for (const [id, size] of Object.entries(SIZES)) {
    const factor = XL.quadExtentFactor(ua);
    const r = W.windowRects(size);
    const model = M.multiply(M.translation(-0.3, 1.2, -0.8), M.rotation(M.normalize([0.2, 1, 0.1]), -0.4));
    const drawn = (m, s) => {
      // What the compositor draws: centred on the layer's pose.
      const { position, scale } = XL.rigidFromModel(m);
      const [lw, lh] = XL.quadLayerSize(s, scale, factor);
      const hw = halfOf(lw), hh = halfOf(lh);
      const c = M.transformPoint(M.invert(model), position);
      return [c[0] - hw, c[1] - hh, c[0] + hw, c[1] + hh];
    };
    const content = drawn(model, size);
    const bar = drawn(W.barModel(model, size), W.barSize(size[0]));
    r.content.forEach((v, k) => close(content[k], v, 1e-5, `${id} content[${k}]`));
    r.bar.forEach((v, k) => close(bar[k], v, 1e-5, `${id} bar[${k}]`));
    assert.ok(bar[1] > content[3], `${id}: the drawn bar is above the drawn content`);
  }
});

test('the full size given as a quad layer\'s width reproduces the Quest 3 picture of issue #267', () => {
  // Both quads twice as large about their centres: the bar's centre falls
  // about a fifth of the way down the content, as on the headset's screenshot.
  const size = W.WINDOWS.result.size;
  const r = W.windowRects(size);
  const barCentre = (r.bar[1] + r.bar[3]) / 2;
  const top = size[1];                     // the content drawn from -h to +h
  const fromTop = (top - barCentre) / (2 * size[1]);
  assert.ok(fromTop > 0.18 && fromTop < 0.25, `bar at ${fromTop.toFixed(3)} of the content from its top`);
  // On the Quest Browser quadLayerSize now gives half: the drawn content ends at h/2, below the bar.
  assert.ok(XL.quadLayerSize(size, [1, 1], XL.quadExtentFactor(QUEST_UA))[1] < r.bar[1]);
});

test('the bar names the window', () => {
  assert.equal(W.windowTitle('result', { tool: 'probe' }), 'Spectrum probe');
  assert.equal(W.windowTitle('result', { tool: 'box' }), 'Box region');
  assert.equal(W.windowTitle('result', { tool: 'lasso' }), TOOL_NAMES.lasso);
  assert.equal(W.windowTitle('result', {}), W.WINDOWS.result.title);
  assert.equal(W.windowTitle('moment', { momentOrder: 1 }), 'Moment 1');
  assert.equal(W.windowTitle('moment', { momentOrder: null }), 'Moment');
  assert.equal(W.windowTitle('pv', { tool: 'probe' }), 'PV diagram');
});

test('bar text: a short title keeps the drag hint, a long one takes its room and is cut to fit', () => {
  const title = mono(17), hint = mono(11);
  const short = W.barText('Spectrum probe', title, hint);
  assert.equal(short.hint, W.BAR_HINT);
  assert.equal(short.title, 'Spectrum probe');
  assert.ok(18 + title(short.title) <= short.hintRight - hint(W.BAR_HINT), 'title left of the hint');
  const long = W.barText('Moment 1 (Intensity-weighted coordinate) of the lasso object', title, hint);
  assert.equal(long.hint, '');
  assert.ok(title(long.title) <= long.titleMax && long.title.endsWith('…'));
  assert.ok(18 + long.titleMax <= W.barLayout()[1].rect[0], 'never under the close button');
});

test('wrapText: lines break at spaces, never wider than the panel', () => {
  const m = mono(12);
  const v = 'velocity 1015.39 km/s (optical (cz), from frequency and RESTFRQ)';
  assert.ok(m(v) > 736, 'the issue\'s line is wider than the 768 px panel');
  const rows = wrapText(m, v, 736);
  assert.equal(rows.length, 2);
  assert.equal(rows.join(' '), v, 'nothing lost, spaces at the break dropped');
  for (const r of rows) assert.ok(m(r) <= 736);
  // A word wider than the line is broken where it reaches the edge.
  const word = 'x'.repeat(100);
  const parts = wrapText(m, `a ${word} b`, 600);
  for (const r of parts) assert.ok(m(r) <= 600, r);
  assert.equal(parts.join('').replace(/ /g, ''), `a${word}b`);
  // A cap on the rows: the last ends in an ellipsis, inside the width.
  const capped = wrapText(m, 'one two three four five six seven eight nine ten', 100, 2);
  assert.equal(capped.length, 2);
  assert.ok(capped[1].endsWith('…') && m(capped[1]) <= 100);
  assert.deepEqual(wrapText(m, '', 100), ['']);
  assert.deepEqual(wrapText(m, 'abc def', 100, 0), [], 'a cap of no rows');
  assert.deepEqual(wrapText(m, '', 100, 0), []);
  // Narrower than one character: one character a row (a glyph is not split), and it ends.
  assert.deepEqual(wrapText(m, 'ab', 5), ['a', 'b']);
  assert.deepEqual(wrapText(m, 'fits', 100), ['fits']);
});

test('fitText: kept when it fits, cut with an ellipsis when not', () => {
  const m = mono(10);
  assert.equal(fitText(m, 'abc', 30), 'abc');
  const cut = fitText(m, 'abcdefghij', 50);
  assert.equal(cut, 'abcd…');
  assert.equal(fitText(m, 'abcdef', 10), '…', 'only the ellipsis fits');
  assert.equal(fitText(m, 'abcdef', 5), '', 'not even the ellipsis fits');
  assert.equal(fitText(m, '', 0), '');
});

test('product windows: lines wrap into the rows above the frame, the last cut when they do not fit', () => {
  const m = mono(11.4);                        // 19 px monospace
  const maxW = PLANE_PX[0] - 32;
  const pvLines = ['12.5 px of path, 40 samples × 109 channels, 31 ms',
    'velocity (optical (cz), from frequency and RESTFRQ) runs from 885.59 km/s at the bottom to 1310.8 km/s at the top'];
  const rows = productLines(m, pvLines, maxW);
  assert.ok(rows.length <= PRODUCT_ROWS);
  for (const r of rows) assert.ok(m(r) <= maxW, r);
  assert.equal(rows.length, 3);
  // The rows end above the frame (its top at y = 112): baseline 56 + 2 × 22 = 100.
  assert.ok(56 + (PRODUCT_ROWS - 1) * 22 + 6 < 112);
  const many = productLines(m, ['a '.repeat(400), 'b'], maxW);
  assert.equal(many.length, PRODUCT_ROWS);
  assert.ok(many.at(-1).endsWith('…'));
  assert.deepEqual(productLines(m, ['short', 'also short'], maxW), ['short', 'also short']);
});

/** A 2-D canvas for node that records text: monospace 12 px a character, other fonts 14. */
function textCanvas() {
  const texts = [];
  const state = { font: '', textAlign: 'left' };
  const width = (s) => String(s).length * (/monospace/.test(state.font) ? 12 : 14);
  const ctx2 = new Proxy(state, {
    get(t, k) {
      if (k in t) return t[k];
      if (k === 'measureText') return (s) => ({ width: width(s) });
      if (k === 'fillText') return (s, x, y) => texts.push({ s: String(s), x, y, w: width(s), align: t.textAlign });
      return () => {};
    },
    set(t, k, v) { t[k] = v; return true; },
  });
  return { canvas: { width: 768, height: 560, getContext: () => ctx2 }, texts };
}

test('the Spectrum probe panel: the long velocity line wraps inside the panel, nothing drawn past its edge', () => {
  const { canvas, texts } = textCanvas();
  const state = {
    info: { full_shape: [196, 280, 109], factors: [2, 2, 1], reduced_shape: [98, 140, 109], bunit: 'Jy/beam',
      axes: { x: { name: 'RA' }, y: { name: 'DEC' }, z: { name: 'FREQ', velocity_convention: 'optical (cz), from frequency and RESTFRQ' } } },
    cube: null, open: { dataset_id: 'ds_1' }, clip: { min: [0, 0, 0], max: [1, 1, 1] }, extent: [1, 1, 1],
    texScale: [1, 1, 1], thrNorm: 0.1, mode: 0,
  };
  const sel = createSelection({ state, renderer: { setMask() {}, createPanel: () => ({}), updatePanel() {} },
    backend: () => null, onChange() {}, panelCanvas: canvas });
  sel.sel.tool = 'probe';
  sel.sel.pick = { seed: [85, 60, 75], reduced: [42, 30, 75], snapped: 'voxel', snapMs: 3,
    sky: { ra_deg: 152.2647, dec_deg: -29.1047 },
    world: { xName: 'RA', yName: 'DEC', x: 152.2647, y: -29.1047, velocity: 1015.39 } };
  sel.sel.spectrum = { x: 85, y: 60, values: Array.from({ length: 109 }, (_, i) => Math.sin(i)), ms: 15,
    axis: Array.from({ length: 109 }, (_, i) => 885.59 + i * 3.94), channel: 75, unit: 'Jy/beam', axisLabel: 'velocity (km/s)' };
  sel.refreshDom();
  const left = texts.filter((t) => t.x === 16 && t.align === 'left');
  assert.ok(left.length >= 6, `${left.length} lines`);
  for (const t of left) assert.ok(t.x + t.w <= 768 - 16, `"${t.s}" ends at ${t.x + t.w}`);
  const vel = left.filter((t) => /velocity|RESTFRQ/.test(t.s) && !/spectrum/.test(t.s));
  assert.equal(vel.length, 2, 'the velocity line takes two rows');
  assert.ok(vel[1].y > vel[0].y);
  assert.ok(left.some((t) => /RESTFRQ\)$/.test(t.s)), 'its end is drawn, not cut');
  // The rows go down the panel one after the other.
  for (let i = 1; i < left.length; i++) assert.ok(left[i].y > left[i - 1].y);
});

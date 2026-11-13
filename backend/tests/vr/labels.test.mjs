// The cube's labels fixed to the cube (issue #226, point 3).
// node --test backend/tests/vr/labels.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';
import * as L from '../../app/static/vr/js/labels.js';
import { readFileSync } from 'node:fs';

const cell = (w = 120) => ({ u0: 0, u1: 0.2, v0: 0, v1: 0.1, widthPx: w, rowPx: 40 });

test('a label lies in its face of the cube, whatever the viewer: no billboard', () => {
  const labels = [
    { pos: [0.1, -0.5, 0.5], text: '10h', height: 0.03, anchor: 'center' },
    { pos: [0.5, -0.5, 0.2], text: '1200', height: 0.03, anchor: 'left', face: 'side' },
  ];
  const v = L.labelVertices(labels, [cell(), cell(80)]);
  assert.equal(v.length, 2 * 6 * L.LABEL_STRIDE);
  for (const [li, l] of labels.entries()) {
    const f = L.faceOf(l);
    const n = L.faceNormal(f);
    for (let k = 0; k < 6; k++) {
      const p = L.vertexPosition(v, li * 6 + k);
      const d = [p[0] - l.pos[0], p[1] - l.pos[1], p[2] - l.pos[2]];
      // In the face's plane through the label's anchor.
      assert.ok(Math.abs(d[0] * n[0] + d[1] * n[1] + d[2] * n[2]) < 1e-6, `label ${li} vertex ${k} off its face`);
    }
  }
  // The vertices carry no view: the same labels give the same numbers.
  assert.deepEqual(Array.from(L.labelVertices(labels, [cell(), cell(80)])), Array.from(v));
  // The shader's billboard is gone: the label vertex shader has no view-space offset.
  const src = readFileSync(new URL('../../app/static/vr/js/renderer.js', import.meta.url), 'utf8');
  const vs = src.slice(src.indexOf('const LABEL_VS'), src.indexOf('const TEX_FS'));
  assert.ok(!/uMV \*|c \+ vec4\(aOffset/.test(vs), 'no view-space billboard offset');
  assert.match(vs, /aCenter \+ aRight \* aOffset\.x \+ aUp \* aOffset\.y/);
});

test('the faces read from the default view: front towards +z, the velocity labels at 45° between +z and +x', () => {
  const front = L.faceNormal(L.FACES.front), side = L.faceNormal(L.FACES.side);
  assert.deepEqual(front.map((x) => Math.round(x * 1e6) / 1e6 + 0), [0, 0, 1]);
  assert.ok(side[2] > 0.7 && side[0] > 0.7, `side normal ${side}`);
  // The viewer placed in front of the cube (+z, a little above): both faces seen from the front.
  const eye = [0.2, 0.3, 2.0];
  assert.ok(!L.seenFromBehind([0, -0.5, 0.5], L.FACES.front, eye));
  assert.ok(!L.seenFromBehind([0.5, -0.5, 0], L.FACES.side, eye));
  // From behind the cube, the front face is seen from behind (the shader mirrors its texture).
  assert.ok(L.seenFromBehind([0, -0.5, 0.5], L.FACES.front, [0, 0, -2]));
  // Anchors: 'right' ends at the anchor, 'center' straddles it, 'left' starts there.
  const one = (anchor) => {
    const v = L.labelVertices([{ pos: [0, 0, 0], height: 0.04, anchor }], [cell(120)]);
    const xs = [0, 1, 2, 3, 4, 5].map((k) => L.vertexPosition(v, k)[0]);
    return [Math.min(...xs), Math.max(...xs)];
  };
  const w = 0.04 * 120 / 40;
  assert.deepEqual(one('right').map((x) => +x.toFixed(6)), [-w, 0].map((x) => +x.toFixed(6)));
  assert.deepEqual(one('center').map((x) => +x.toFixed(6)), [-w / 2, w / 2].map((x) => +x.toFixed(6)));
  assert.deepEqual(one('left').map((x) => +x.toFixed(6)), [0, w].map((x) => +x.toFixed(6)));
});

test('the page puts the velocity axis labels on the side face, the sky axes on the front', () => {
  const main = readFileSync(new URL('../../app/static/vr/js/main.js', import.meta.url), 'utf8');
  const deco = main.slice(main.indexOf('function rebuildDecorations'), main.indexOf('// ── Drawing'));
  assert.equal((deco.match(/face: 'side'/g) || []).length, 2, 'the z ticks and the z title');
  const pts = readFileSync(new URL('../../app/static/vr/js/points.js', import.meta.url), 'utf8');
  assert.equal((pts.match(/face: 'side'/g) || []).length, 2);
});

// The first Quest 3 session's findings (issue #226): the readout's size and
// contrast (point 4) and the guide's place (point 6).
// node --test backend/tests/vr/quest226.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';
import * as RO from '../../app/static/vr/js/readout.js';
import * as N from '../../app/static/vr/js/notice.js';
import * as G from '../../app/static/vr/js/onboarding.js';
import * as M from '../../app/static/vr/js/mat4.js';
import { readFileSync } from 'node:fs';

test('the readout\'s text keeps at least its minimum angle from 30 cm to 4 m', () => {
  const F = RO.READOUT_FONT;
  for (let d = RO.READOUT_NEAR; d <= RO.READOUT_FAR + 1e-9; d += 0.05) {
    const s = RO.readoutScale(d);
    const body = RO.textAngleDeg(F.body, RO.READOUT_PX[1], RO.READOUT_SIZE[1], s, d);
    assert.ok(body >= RO.READOUT_MIN_DEG, `${body.toFixed(3)}° at ${d.toFixed(2)} m`);
  }
  // What it was (30 px on a 168 px, 4.2 cm card): under half a degree.
  assert.ok(RO.textAngleDeg(30, 168, 0.042, 1, 1) < 0.45);
  // The card's own model carries that scale: its size at distance d is READOUT_SIZE × d.
  const eye = [0, 1.6, 0];
  const m = RO.readoutModel([0, 1.6, -2], eye);
  assert.ok(Math.abs(M.uniformScale(m) - 2) < 1e-6);
  // The lines fit the card.
  assert.ok(F.top + 3 * F.pitch + F.body / 2 <= RO.READOUT_PX[1]);
});

test('the readout is bright on dark: contrast well over 7:1', () => {
  const C = RO.READOUT_COLORS;
  assert.ok(RO.contrastRatio(C.text, C.bg) > 15, `${RO.contrastRatio(C.text, C.bg)}`);
  assert.ok(RO.contrastRatio(C.title, C.bg) > 7);
  assert.ok(RO.contrastRatio(C.text, C.bgPinned) > 7);
  assert.ok(Math.abs(RO.contrastRatio([255, 255, 255], [0, 0, 0]) - 21) < 1e-6);
});

/** The angle (degrees) between two directions from the head. */
function angleBetween(head, a, b) {
  const da = M.normalize(M.sub(a, head)), db = M.normalize(M.sub(b, head));
  return (Math.acos(Math.max(-1, Math.min(1, M.dot(da, db)))) * 180) / Math.PI;
}

test('the guide\'s card never covers the cube: beside it, on the primary hand\'s side', () => {
  const headPos = [0, 1.6, 0];
  for (const yaw of [0, 0.6, -1.2, 2.5]) {
    const head = M.multiply(M.translation(...headPos), M.rotation([0, 1, 0], yaw));
    for (const [dist, radius] of [[0.75, 0.42], [1.2, 0.6], [0.5, 0.3]]) {
      const fwd = [-Math.sin(yaw), 0, -Math.cos(yaw)];
      const centre = M.add(headPos, M.add(M.scale3(fwd, dist), [0, -0.12, 0]));
      for (const side of [1, -1]) {
        const card = N.guideCardModel(head, { cube: { centre, radius }, side });
        const c = M.position(card);
        // The card's nearest edge to the cube, seen from the head, is outside the cube's silhouette.
        const halfW = N.CARD_SIZE[0] / 2;
        const right = M.normalize(M.sub(M.transformPoint(card, [1, 0, 0]), c));
        const edges = [M.add(c, M.scale3(right, -halfW)), M.add(c, M.scale3(right, halfW))];
        const cubeHalf = (Math.asin(radius / Math.hypot(...M.sub(centre, headPos))) * 180) / Math.PI;
        const nearest = Math.min(...edges.map((e) => angleBetween(headPos, e, centre)));
        assert.ok(nearest > cubeHalf, `yaw ${yaw} side ${side}: edge at ${nearest.toFixed(1)}°, cube ${cubeHalf.toFixed(1)}°`);
        // On the asked side: to the right of the cube for side +1 (the right hand points).
        const toCube = M.sub(centre, headPos), toCard = M.sub(c, headPos);
        // The cube direction's right is (-z, 0, x): > 0 means the card is to its right.
        const crossY = toCube[0] * toCard[2] - toCube[2] * toCard[0];
        assert.equal(Math.sign(crossY), side, `yaw ${yaw}: the card on side ${side}`);
        // Facing the viewer.
        const z = M.normalize(M.sub(M.transformPoint(card, [0, 0, 1]), c));
        const toHead = M.normalize(M.sub(headPos, c));
        assert.ok(M.dot([z[0], 0, z[2]], M.normalize([toHead[0], 0, toHead[2]])) > 0.999);
      }
    }
  }
});

test('the guide is on the 2-D page, not shown by itself in the headset; in VR it is drawn lowest', () => {
  assert.ok(G.shouldShowGuide({ guideSeen: false }));
  const main = readFileSync(new URL('../../app/static/vr/js/main.js', import.meta.url), 'utf8');
  const enter = main.slice(main.indexOf('async function enterXR'), main.indexOf('function placeInFront'));
  assert.ok(!/showGuide\(/.test(enter), 'entering VR does not open the guide');
  assert.match(main, /showGuideOnPage\(\);/);
  // The overlays back to front: the guide's card first, the menu after the labels.
  const list = main.slice(main.indexOf('function overlayList'), main.indexOf('function onXRFrame'));
  const iGuide = list.indexOf("if (guide) out.push"), iMenu = list.indexOf("id: 'menu'"), iLabels = list.indexOf('for (const k of cards)');
  assert.ok(iGuide > 0 && iGuide < iLabels && iLabels < iMenu);
  const shell = readFileSync(new URL('../../app/static/vr/js/shell.js', import.meta.url), 'utf8');
  assert.match(shell, /guideCardModel\(head, place\)/);
  const html = readFileSync(new URL('../../app/static/vr/index.html', import.meta.url), 'utf8');
  assert.match(html, /id="guide-ok"/);
});

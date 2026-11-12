// The message card and the first-run guide (epic #166).
// node --test backend/tests/vr/notice.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import * as N from '../../app/static/vr/js/notice.js';
import * as G from '../../app/static/vr/js/onboarding.js';
import * as M from '../../app/static/vr/js/mat4.js';
import { noticeFor } from '../../app/static/vr/js/lifecycle.js';
import { PanelPointer, rayPanel } from '../../app/static/vr/js/vr-panel.js';

const close = (a, b, eps = 1e-4) => assert.ok(Math.abs(a - b) <= eps, `${a} != ${b}`);

test('card buttons fit, do not overlap and are large enough to hit', () => {
  for (const n of [1, 2, 3, 4, 5]) {
    const actions = Array.from({ length: n }, (_, i) => ({ id: `a${i}`, label: `Action ${i}` }));
    const L = N.cardLayout({ actions });
    assert.equal(L.length, Math.min(n, N.MAX_ACTIONS));
    const [W, H] = N.CARD_PX;
    for (const w of L) {
      const [x, y, ww, hh] = w.rect;
      assert.ok(x >= 0 && y >= 0 && x + ww <= W && y + hh <= H, w.id);
      assert.ok(hh / H * N.CARD_SIZE[1] >= 0.03, 'at least 3 cm high');
      assert.ok(ww / W * N.CARD_SIZE[0] >= 0.06, 'at least 6 cm wide');
    }
    for (let i = 1; i < L.length; i++) assert.ok(L[i].rect[0] >= L[i - 1].rect[0] + L[i - 1].rect[2], 'no overlap');
  }
  assert.deepEqual(N.cardLayout(null), []);
});

test('text wraps at word boundaries, cuts only words longer than a line', () => {
  const measure = (t) => t.length * 10;
  assert.deepEqual(N.wrapText('aaa bbb ccc', 75, measure), ['aaa bbb', 'ccc']);
  assert.deepEqual(N.wrapText('abcdefghij', 40, measure), ['abcd', 'efgh', 'ij']);
  assert.deepEqual(N.wrapText('one\ntwo', 1000, measure), ['one', 'two']);
  assert.deepEqual(N.wrapText('', 100, measure), ['']);
  // Every notice's text fits the card at the body font (about 15 px a character).
  const est = (t) => t.length * 15;
  for (const k of ['unreachable', 'auth', 'reload', 'update-app', 'reopen-failed', 'context-failed', 'xr-ended', 'budget']) {
    const n = noticeFor(k, { loopback: true, headset: true, inVR: true, retryInS: 15, attempts: 9, detail: 'x'.repeat(60), why: 'page build aaaaaaaaaaaa, the backend serves bbbbbbbbbbbb' });
    const lines = n.lines.flatMap((l) => N.wrapText(l, N.CARD_PX[0] - 72, est));
    assert.ok(lines.length <= N.bodyCapacity(), `${k}: ${lines.length} lines > ${N.bodyCapacity()}`);
  }
  for (let i = 0; i < G.GUIDE_PAGES.length; i++) {
    const lines = G.guideNotice(i).lines.flatMap((l) => N.wrapText(l, N.CARD_PX[0] - 72, est));
    assert.ok(lines.length <= N.bodyCapacity(), `guide page ${i}: ${lines.length} lines`);
  }
});

test('the card stands in front of the viewer, facing them, and moves when they turn away', () => {
  // Head at (0, 1.6, 0) looking down -z.
  const head = M.translation(0, 1.6, 0);
  const card = N.cardModel(head, 0.8, 0.06);
  const p = M.position(card);
  close(p[0], 0); close(p[1], 1.54); close(p[2], -0.8);
  // The quad faces +z in its own frame: toward the viewer.
  const n = M.sub(M.transformPoint(card, [0, 0, 1]), p);
  close(n[2], 1);
  // A ray from the eye to the card's centre hits it in the middle.
  const hit = rayPanel(card, N.CARD_SIZE, [0, 1.6, 0], M.sub(p, [0, 1.6, 0]), N.CARD_PX);
  close(hit.x, N.CARD_PX[0] / 2, 0.5); close(hit.y, N.CARD_PX[1] / 2, 0.5);
  assert.equal(N.cardNeedsMove(card, head), false);
  const turned = M.multiply(M.translation(0, 1.6, 0), M.rotation([0, 1, 0], 50 * Math.PI / 180));
  assert.equal(N.cardNeedsMove(card, turned), true);
  const slightly = M.multiply(M.translation(0, 1.6, 0), M.rotation([0, 1, 0], 20 * Math.PI / 180));
  assert.equal(N.cardNeedsMove(card, slightly), false);
  assert.equal(N.cardNeedsMove(card, M.translation(0, 1.6, 3)), true);     // walked away
  assert.equal(N.cardNeedsMove(null, head), true);
  // Looking straight down: still a card, upright.
  const down = M.multiply(M.translation(0, 1.6, 0), M.rotation([1, 0, 0], -Math.PI / 2));
  assert.ok(Number.isFinite(M.position(N.cardModel(down))[0]));
});

test('the card is used with the same pointer as the panel', () => {
  const n = noticeFor('unreachable', { inVR: true, loopback: true, headset: true });
  const L = N.cardLayout(n);
  const ptr = new PanelPointer(L);
  const retry = L.find((w) => w.action === 'retry');
  const at = { x: retry.rect[0] + 10, y: retry.rect[1] + 10 };
  const a = ptr.update(at, true);
  assert.deepEqual(a.map((x) => x.widget.action), ['retry']);
  assert.ok(ptr.takesRay());
  assert.deepEqual(ptr.update(at, false), []);
});

test('notice centre: priority, replace by id, dismiss, auto-hide', () => {
  let t = 0;
  const c = new N.NoticeCenter(() => t);
  assert.equal(c.current(), null);
  c.show(G.guideNotice(0));
  assert.equal(c.current().id, 'guide');
  c.show(noticeFor('restarted'));
  assert.equal(c.current().id, 'restarted');            // info over the guide
  c.show(noticeFor('unreachable', { attempts: 2 }));
  assert.equal(c.current().id, 'unreachable');           // an error over everything
  const v = c.version;
  c.show(noticeFor('unreachable', { attempts: 2 }));
  assert.equal(c.version, v, 'the same notice again changes nothing');
  c.show(noticeFor('unreachable', { attempts: 3 }));
  assert.equal(c.version, v + 1);
  c.clear('unreachable');
  assert.equal(c.current().id, 'restarted');
  t += 9000;                                              // restarted auto-hides after 8 s
  assert.equal(c.current().id, 'guide');
  c.show(noticeFor('xr-ended'));
  c.dismiss('xr-ended');
  c.show(noticeFor('xr-ended'));
  assert.equal(c.current().id, 'guide', 'dismissed: not brought back by the same condition');
  c.show(noticeFor('xr-ended'), { force: true });
  assert.equal(c.current().id, 'xr-ended');
  c.dismiss('xr-ended'); c.clear('xr-ended');
  c.show(noticeFor('xr-ended'));
  assert.equal(c.current().id, 'xr-ended', 'cleared: shown again next time');
});

test('the guide: pages, buttons, first time only', () => {
  const n = G.GUIDE_PAGES.length;
  assert.ok(n >= 3);
  assert.deepEqual(G.guideNotice(0).actions.map((a) => a.id), ['guide-next', 'guide-close']);
  assert.deepEqual(G.guideNotice(1).actions.map((a) => a.id), ['guide-back', 'guide-next', 'guide-close']);
  assert.deepEqual(G.guideNotice(n - 1).actions.map((a) => [a.id, a.label]), [['guide-back', 'Back'], ['guide-close', 'Close']]);
  assert.equal(G.guideStep(0, 'guide-next'), 1);
  assert.equal(G.guideStep(n - 1, 'guide-next'), n - 1);
  assert.equal(G.guideStep(0, 'guide-back'), 0);
  assert.equal(G.guideStep(1, 'guide-close'), null);
  assert.ok(G.shouldShowGuide({ guideSeen: false }));
  assert.ok(G.shouldShowGuide(null));
  assert.ok(!G.shouldShowGuide({ guideSeen: true }));
  assert.ok(!G.shouldShowGuide({ guideSeen: false }, { bench: true }));
  assert.match(G.guideText(), /Grip/);
});

test('the guide lists the controls main.js binds', () => {
  const main = readFileSync(new URL('../../app/static/vr/js/main.js', import.meta.url), 'utf8');
  const text = G.GUIDE_PAGES.flatMap((p) => p.lines).join('\n');
  // Each control bound in handleInput, and the words the guide uses for it.
  const pairs = [["pressedEdge('rA'", /\bA: next colour map/], ["pressedEdge('rB'", /\bB: all channels/],
    ["pressedEdge('lX'", /\bX: emission/], ["pressedEdge('lY'", /\bY: the numbers/],
    ["pressedEdge('lStick'", /Left stick click/], ["pressedEdge('rStick'", /Right stick click/],
    ["pressedEdge('rTrig'", /Right trigger/], ["pressedEdge('lTrig'", /Left trigger/],
    ['grabTwo', /Both grips/], ['grabOne', /Grip \(either hand\)/]];
  for (const [code, words] of pairs) {
    assert.ok(main.includes(code), `main.js no longer has ${code}: update the guide`);
    assert.match(text, words);
  }
  assert.match(text, /threshold/i);
  assert.match(text, /opacity/i);
  assert.match(text, /slab/i);
  assert.match(text, /"\?"/);
});

test('a sticky notice can be hidden into a badge until its state changes', () => {
  const c = new N.NoticeCenter(() => 0);
  c.show(noticeFor('unreachable', { attempts: 2 }));
  c.show(noticeFor('auth', {}));
  assert.equal(c.badge(), '');
  c.hide(c.current().id);
  c.hide(c.current().id);
  assert.equal(c.current(), null, 'both off the card');
  assert.match(c.badge(), /Backend not reachable|Token refused/);
  c.show(noticeFor('unreachable', { attempts: 3 }), { force: true });
  assert.equal(c.current(), null, 'still hidden after the next failed check');
  c.clear('unreachable');
  assert.match(c.badge(), /Token refused/);
  c.show(noticeFor('unreachable', { attempts: 1 }), { force: true });
  assert.equal(c.current().id, 'unreachable', 'down again: a new state, shown');
  assert.ok(c.unhideAll());
  assert.equal(c.badge(), '');
  assert.ok(!c.unhideAll());
});

test('a parked card is to the left, lower, out of the way of the cube', () => {
  const head = M.translation(0, 1.6, 0);
  const front = M.position(N.cardModel(head));
  const parked = M.position(N.parkedCardModel(head));
  assert.ok(parked[0] < -0.3, `x ${parked[0]}`);
  assert.ok(parked[1] < front[1] - 0.2, `y ${parked[1]}`);
  const to = M.sub(parked, [0, 1.6, 0]);
  const ang = Math.acos(-to[2] / Math.hypot(...to)) * 180 / Math.PI;
  assert.ok(ang > 35, `${ang.toFixed(1)} deg`);
});

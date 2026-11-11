// The refinement over a far link (issue #187):
// node --test backend/tests/vr/link-policy.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';

import { LinkPolicy, LINK_DEFAULTS } from '../../app/static/vr/js/link-policy.js';

/** A frame's timing_ms as the backend sends it. */
const timing = (render, depth, encode, wait = 0, queue = 0) =>
  ({ render, depth, encode, wait, queue, total: render + depth + encode + wait + queue + 1 });

function feed(p, n, networkMs, t) { for (let i = 0; i < n; i++) p.push(networkMs, t); return p.decide(); }

test('on the Mac (loopback) nothing changes', () => {
  const p = new LinkPolicy();
  const d = feed(p, 20, 75, timing(40, 5, 25));       // 72 ms pose->frame measured locally
  assert.equal(d.remote, false);
  assert.equal(d.window, LINK_DEFAULTS.minWindow);
  assert.equal(d.maxEyePx, Infinity);
});

test('the PLEIADI numbers widen the window; the eye is capped only when asked', () => {
  // 512^2: pose->frame 122 ms, render 28 + depth 2.2 + JPEG 4.5 ms (2026-11-11).
  const p = new LinkPolicy();
  const d = feed(p, 20, 122, timing(28, 2.2, 4.5, 4));
  assert.equal(d.remote, true);
  assert.ok(d.transportMs > 70 && d.transportMs < 90, d.transportMs);
  assert.equal(d.window, Math.ceil((d.transportMs + 34.7) / 34.7));
  assert.equal(d.window, 4);
  assert.equal(d.maxEyePx, Infinity);                       // full size by default
  const capped = feed(new LinkPolicy({ remoteMaxEyePx: 768 }), 20, 122, timing(28, 2.2, 4.5, 4));
  assert.equal(capped.maxEyePx, 768);
  // 1024^2: 175 ms, render 80 + 7.9 + 19.
  const q = feed(new LinkPolicy(), 20, 175, timing(80, 7.9, 19, 6));
  assert.equal(q.window, 2);
  assert.equal(q.remote, true);
});

test('waits are not work: a fuller queue does not shrink the window', () => {
  const p = new LinkPolicy();
  const a = feed(p, 20, 122, timing(28, 2.2, 4.5, 0));
  const b = feed(p, 40, 160, timing(28, 2.2, 4.5, 40));   // same work, frames waiting longer
  assert.ok(b.window >= a.window);
});

test('nothing is decided before enough frames, and the change is reported once', () => {
  const p = new LinkPolicy();
  for (let i = 0; i < LINK_DEFAULTS.minSamples - 1; i++) {
    p.push(300, timing(20, 1, 4));
    assert.equal(p.decide().changed, false);
  }
  p.push(300, timing(20, 1, 4));
  const d = p.decide();
  assert.equal(d.changed, true);
  assert.equal(d.window, LINK_DEFAULTS.maxWindow);       // clamped
  assert.equal(p.decide().changed, false);
});

test('a far link stays far until the round trip is clearly short again', () => {
  const p = new LinkPolicy();
  feed(p, 40, 80 + 30, timing(20, 2, 5));                 // ~ 80 ms transport
  assert.equal(p.decide().remote, true);
  feed(p, 40, 25 + 30, timing(20, 2, 5));                 // 25 ms: between leave and enter
  assert.equal(p.decide().remote, true);
  feed(p, 40, 10 + 30, timing(20, 2, 5));
  assert.equal(p.decide().remote, false);
});

test('bad samples are ignored and the HUD line names the link', () => {
  const p = new LinkPolicy({ remoteMaxEyePx: 768 });
  p.push(NaN, timing(1, 1, 1));
  p.push(100, null);
  p.push(100, { total: 50 });                              // no work timings: an older header
  assert.equal(p.transport.length, 0);
  feed(p, 10, 122, timing(28, 2.2, 4.5));
  assert.match(p.hudLine(), /^ {2}link far rtt \d+ ms {2}window 4 {2}eye <= 768 px$/);
  assert.match(new LinkPolicy().hudLine(feed(new LinkPolicy(), 10, 122, timing(28, 2.2, 4.5))), /window 4$/);
  p.reset();
  assert.match(p.hudLine(), /link near rtt - ms {2}window 2$/);
});

test('the window does not flap around a step', () => {
  // A frames-in-flight ratio wandering around 3 (3.05 / 2.95) keeps the window.
  const p = new LinkPolicy();
  const work = timing(30, 0, 0);                         // work 30 ms, total 31
  const at = (ratio) => 30 * ratio - 30 + 31;            // network for that ratio
  feed(p, 40, at(3.3), work);
  assert.equal(p.decide().window, 4);
  for (const r of [2.95, 3.05, 2.95, 3.05, 2.9]) {
    feed(p, 40, at(r), work);
    assert.equal(p.decide().window, 4, `ratio ${r}`);
  }
  feed(p, 40, at(2.7), work);                            // clearly below: down
  assert.equal(p.decide().window, 3);
  for (const r of [3.05, 3.1]) { feed(p, 40, at(r), work); assert.equal(p.decide().window, 3); }
  feed(p, 40, at(3.3), work);                            // clearly above: up
  assert.equal(p.decide().window, 4);
});

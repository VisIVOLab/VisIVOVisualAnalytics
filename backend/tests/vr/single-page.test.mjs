// One active VR page (issue #226, point 5).
// node --test backend/tests/vr/single-page.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';
import { SinglePage, CHANNEL_NAME, STOOD_DOWN, theyWin, stoodDownText } from '../../app/static/vr/js/single-page.js';
import { readFileSync } from 'node:fs';

/** BroadcastChannel stand-ins on one bus: a message reaches every other member. */
function bus() {
  const members = new Set();
  return () => {
    const ch = {
      onmessage: null,
      postMessage(m) { for (const o of members) if (o !== ch && o.onmessage) o.onmessage({ data: structuredClone(m) }); },
      close() { members.delete(ch); },
    };
    members.add(ch);
    return ch;
  };
}

test('a newer page makes the older ones stand down; Use this window takes over again', () => {
  const channel = bus();
  let clock = 1000;
  const now = () => clock;
  const events = [];
  const mk = (name) => new SinglePage({ channel: channel(), id: name, now, onChange: (a) => events.push([name, a]) });
  const a = mk('a');
  a.claim();
  clock += 5;
  const b = mk('b');
  b.claim();
  assert.equal(a.active, false);
  assert.equal(b.active, true);
  assert.deepEqual(events, [['a', false]]);
  // A third (the paired loader's iframe on the same origin, or another adb open).
  clock += 5;
  const c = mk('c');
  c.claim();
  assert.deepEqual([a.active, b.active, c.active], [false, false, true]);
  // The first one takes over.
  clock += 5;
  a.claim();
  assert.deepEqual([a.active, b.active, c.active], [true, false, false]);
  assert.deepEqual(events.slice(-2), [['c', false], ['a', true]]);
  // A page closed leaves the others as they are.
  c.close();
  clock += 5;
  b.claim();
  assert.deepEqual([a.active, b.active], [false, true]);
});

test('two pages claiming in the same millisecond end with exactly one active', () => {
  const channel = bus();
  const now = () => 42;
  const a = new SinglePage({ channel: channel(), id: 'aaa', now });
  const b = new SinglePage({ channel: channel(), id: 'bbb', now });
  a.claim();
  b.claim();
  assert.equal([a, b].filter((p) => p.active).length, 1);
});

test('an older claim reaching a newer page is answered, so the older page stands down', () => {
  const channel = bus();
  let t = 10;
  const now = () => t;
  const newer = new SinglePage({ channel: channel(), id: 'n', now });
  t = 20; newer.claim();
  const older = new SinglePage({ channel: channel(), id: 'o', now });
  older.since = 5;           // a clock behind (a page restored from the back-forward cache)
  older._post({ type: 'claim', id: 'o', t: 5, content: false });
  assert.equal(newer.active, true);
  assert.equal(older.active, false, 'told by the newer one');
});

test('without BroadcastChannel the page is simply active, and the page wires it up', () => {
  const p = new SinglePage({ channel: null });
  p.claim();
  assert.equal(p.active, true);
  assert.equal(CHANNEL_NAME, 'visivo-vr-page');
  assert.match(STOOD_DOWN.title, /Open in another window/);
  const main = readFileSync(new URL('../../app/static/vr/js/main.js', import.meta.url), 'utf8');
  assert.match(main, /new BroadcastChannel\(CHANNEL_NAME\)/);
  assert.match(main, /single\.claim\(\);/);
  // Enter VR is off on a page that stood down, and entering refuses there.
  assert.match(main, /\$\('enter-vr'\)\.disabled = !xrSupport\.vr \|\| !on/);
  assert.match(main, /if \(!single\.active\) refuse\(/);
  const html = readFileSync(new URL('../../app/static/vr/index.html', import.meta.url), 'utf8');
  assert.match(html, /id="stood-down"[^>]*hidden/);
  assert.match(html, /id="take-over"/);
});

// ── By content (issue #237) ────────────────────────────────────────────────

test('a newer page with nothing on show never makes the page with a cube stand down: it stands down itself', () => {
  const channel = bus();
  let clock = 1000;
  const now = () => clock;
  const events = [];
  const a = new SinglePage({ channel: channel(), id: 'a', now, content: true, cube: 'ngc3137.fits',
    onChange: (on, info) => events.push(['a', on, info]) });
  a.claim();
  clock += 50;
  // A tab the browser restored, or an empty loader: no cube, no link to one.
  const b = new SinglePage({ channel: channel(), id: 'b', now, onChange: (on, info) => events.push(['b', on, info]) });
  b.claim();
  assert.equal(a.active, true, 'the page with the cube stays in use');
  assert.equal(b.active, false, 'the empty page stands down');
  assert.equal(events.length, 1);
  const [who, on, info] = events[0];
  assert.deepEqual([who, on, info.reason, info.cube, info.since], ['b', false, 'content', 'ngc3137.fits', 1000]);
  // It says which window has the cube.
  const text = stoodDownText(info);
  assert.match(text.title, /ngc3137\.fits is open in another window/);
  assert.match(text.lines[0], /shows nothing/);
  assert.match(text.lines[0], /in use since \d\d:\d\d/);
  // A claim of its own again (a page restored from the back-forward cache) is still refused...
  clock += 50;
  b.claim();
  assert.deepEqual([a.active, b.active], [true, false], 'an empty page claiming again is still refused');
  b.setContent(true, 'other.fits');           // nothing posted: b is not active
  assert.equal(a.active, true);
  // ...but "Use this window" is the user's choice: it wins whatever the others show.
  clock += 50;
  b.claim({ force: true });
  assert.deepEqual([a.active, b.active], [false, true]);
  const aDown = events.find((e) => e[0] === 'a');
  assert.deepEqual([aDown[1], aDown[2].reason], [false, 'newer']);
  assert.deepEqual(events.at(-1).slice(0, 2), ['b', true]);
});

test('a newer page with a cube (or a link to load one) makes the older one stand down, as before', () => {
  const channel = bus();
  let clock = 1000;
  const now = () => clock;
  const a = new SinglePage({ channel: channel(), id: 'a', now, content: true, cube: 'm33.fits' });
  a.claim();
  clock += 10;
  const infos = [];
  a.onChange = (on, info) => infos.push(info);
  const b = new SinglePage({ channel: channel(), id: 'b', now, content: true, cube: 'ngc3137.fits' });
  b.claim();
  assert.deepEqual([a.active, b.active], [false, true]);
  assert.equal(infos[0].reason, 'newer');
  assert.equal(infos[0].cube, 'ngc3137.fits');
  assert.match(stoodDownText(infos[0]).lines[0], /the window showing ngc3137\.fits/);
});

test('an active page that gets a cube says so; an empty page claiming later stands down then', () => {
  const channel = bus();
  let clock = 1000;
  const now = () => clock;
  const a = new SinglePage({ channel: channel(), id: 'a', now });
  a.claim();
  clock += 10;
  const b = new SinglePage({ channel: channel(), id: 'b', now });
  b.claim();
  // Both empty: the newer one is in use, as before.
  assert.deepEqual([a.active, b.active], [false, true]);
  // b loads a cube: nothing changes for the paused a.
  b.setContent(true, 'cube.fits');
  assert.deepEqual([a.active, b.active], [false, true]);
  // a takes over (empty): b has a cube, so a's claim is answered and a stands down again.
  clock += 10;
  a.claim();
  assert.deepEqual([a.active, b.active], [false, true]);
  // b's cube goes (a failed load): b keeps its rank while in use (a rank
  // never drops, so no message in flight can leave no page in use)...
  b.setContent(false);
  clock += 10;
  a.claim();
  assert.deepEqual([a.active, b.active], [false, true]);
  // ...and the user's "Use this window" takes a over.
  clock += 10;
  a.claim({ force: true });
  assert.deepEqual([a.active, b.active], [true, false]);
  // A page that held its rank loses it when it stands down: b, empty, claiming again stays down.
  clock += 10;
  b.claim();
  assert.deepEqual([a.active, b.active], [true, false]);
});

test('held rank: a forced or content claim outranks every claim without, the newer between equals', () => {
  assert.equal(theyWin({ t: 5, id: 'a', content: true, held: true }, { t: 9, id: 'b', content: false, held: true }), true);
  assert.equal(theyWin({ t: 9, id: 'a', content: false, held: true }, { t: 5, id: 'b', content: true, held: true }), false);
  assert.equal(theyWin({ t: 9, id: 'a', content: false, held: false }, { t: 5, id: 'b', content: false, held: true }), true);
});

test('the rule: content first, then the newer claim; a claim of an older build counts as content', () => {
  assert.equal(theyWin({ t: 5, id: 'a', content: true }, { t: 9, id: 'b', content: false }), false);
  assert.equal(theyWin({ t: 9, id: 'a', content: false }, { t: 5, id: 'b', content: true }), true);
  assert.equal(theyWin({ t: 5, id: 'a', content: true }, { t: 9, id: 'b', content: true }), true);
  assert.equal(theyWin({ t: 9, id: 'a', content: false }, { t: 5, id: 'b', content: false }), false);
  assert.equal(theyWin({ t: 5, id: 'a', content: false }, { t: 5, id: 'b', content: false }), true, 'a tie goes to the larger id');
  assert.equal(theyWin({ t: 5, id: 'a', content: true }, { t: 9, id: 'b' }), true, 'no content field: an older build, newer wins');
  assert.equal(theyWin({ t: 9, id: 'a', content: false }, { t: 5, id: 'b' }), true);
  // Exactly one of two pages wins, whatever they hold.
  for (const ca of [true, false]) for (const cb of [true, false]) for (const [ta, tb] of [[1, 2], [2, 1], [3, 3]]) {
    const A = { t: ta, id: 'a', content: ca }, B = { t: tb, id: 'b', content: cb };
    assert.notEqual(theyWin(A, B), theyWin(B, A), JSON.stringify([A, B]));
  }
});

test('the page sets its content before its first claim, and stands down with words that name the cube', () => {
  const main = readFileSync(new URL('../../app/static/vr/js/main.js', import.meta.url), 'utf8');
  const claimAt = main.indexOf('single.claim();         // older VR pages');
  assert.ok(claimAt > 0 && main.lastIndexOf('single.content = ', claimAt) > 0, 'content set before the claim');
  assert.match(main, /single\.setContent\(pageHasContent\(\), shownName\(\)\)/);
  assert.match(main, /const text = stoodDownText\(info\)/);
  assert.match(main, /hub\.stop\(\);/);
  assert.equal(stoodDownText({}), STOOD_DOWN);
});

/** BroadcastChannel stand-ins whose messages wait in one queue, delivered in any order. */
function queuedBus() {
  const members = new Set();
  const queue = [];
  const make = () => {
    const ch = {
      onmessage: null,
      postMessage(m) { for (const o of members) if (o !== ch) queue.push(() => o.onmessage && o.onmessage({ data: structuredClone(m) })); },
      close() { members.delete(ch); },
    };
    members.add(ch);
    return ch;
  };
  return { make, queue };
}

test('"Use this window" with an older reply still in flight: exactly one page stays in use', () => {
  const { make, queue } = queuedBus();
  let t = 1;
  const now = () => t;
  const a = new SinglePage({ channel: make(), id: 'a', now, content: true, cube: 'a.fits' });
  a.claim();
  t = 2;
  const b = new SinglePage({ channel: make(), id: 'b', now });
  b.claim();
  queue.shift()();            // a answers b's empty claim; the answer waits in the queue
  t = 3;
  b.claim({ force: true });   // the user presses Use this window on b before it arrives
  while (queue.length) queue.shift()();
  assert.deepEqual([a.active, b.active], [false, true]);
  // And back, with a content update of b's queued before a's forced claim.
  b.setContent(true, 'b.fits');
  t = 4;
  a.claim({ force: true });
  while (queue.length) queue.shift()();
  assert.deepEqual([a.active, b.active], [true, false]);
});

function randomRun(seedIn, { contentChanges }) {
  let seed = seedIn;
  const rnd = (n) => { seed = (seed * 16807) % 2147483647; return seed % n; };
  const { make, queue } = queuedBus();
  let t = 0;
  const now = () => t;
  const pages = ['a', 'b', 'c', 'd'].map((id) => new SinglePage({ channel: make(), id, now, content: rnd(2) === 1 }));
  for (const p of pages) { t += 1; p.claim(); }
  // Each step: a claim (forced or not), a content change, or one message delivered.
  for (let step = 0; step < 16; step++) {
    t += 1 + rnd(2);
    const p = pages[rnd(pages.length)];
    const k = rnd(4);
    if (k === 0) p.claim({ force: rnd(3) === 0 });
    else if (k === 1 && contentChanges) p.setContent(rnd(2) === 1, 'x.fits');
    else if (queue.length) queue.splice(rnd(queue.length), 1)[0]();
  }
  for (let guard = 0; queue.length && guard < 5000; guard++) queue.splice(rnd(queue.length), 1)[0]();
  return pages.filter((p) => p.active).length;
}

test('claims in any delivery order (the three-page cycle included) settle on exactly one page in use', () => {
  for (let run = 1; run <= 500; run++) assert.equal(randomRun(run * 7919, { contentChanges: false }), 1, `run ${run}`);
  // The cycle of the review: a with a cube, b forced (empty), c newer (empty), delivered crosswise.
  const { make, queue } = queuedBus();
  let t = 1;
  const now = () => t;
  const ch = { a: make(), b: make(), c: make() };
  const a = new SinglePage({ channel: ch.a, id: 'a', now, content: true });
  a.claim();
  t = 1.5;
  const b = new SinglePage({ channel: ch.b, id: 'b', now });
  b.claim();
  while (queue.length) queue.shift()();
  t = 2; b.claim({ force: true });
  t = 3;
  const c = new SinglePage({ channel: ch.c, id: 'c', now });
  c.claim();
  for (let guard = 0; queue.length && guard < 100; guard++) queue.splice(queue.length - 1, 1)[0]();
  assert.deepEqual([a.active, b.active, c.active], [false, true, false]);
});

test('with content changing while claims fly, still exactly one page in use', () => {
  for (let run = 1; run <= 500; run++) assert.equal(randomRun(run * 104729, { contentChanges: true }), 1, `run ${run}`);
});

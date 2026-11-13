// One active VR page (issue #226, point 5).
// node --test backend/tests/vr/single-page.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';
import { SinglePage, CHANNEL_NAME, STOOD_DOWN } from '../../app/static/vr/js/single-page.js';
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
  older._post({ type: 'claim', id: 'o', t: 5 });
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
  assert.match(main, /if \(!single\.active\) throw new Error/);
  const html = readFileSync(new URL('../../app/static/vr/index.html', import.meta.url), 'utf8');
  assert.match(html, /id="stood-down"[^>]*hidden/);
  assert.match(html, /id="take-over"/);
});

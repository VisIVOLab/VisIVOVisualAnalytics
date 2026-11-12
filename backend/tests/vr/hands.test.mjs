// Hand tracking (issue #191): pinch and grip from synthetic WebXR joints, the
// hand's frame for grabbing, its ray and the ray's stabilisation, the WebXR
// layer over a fake frame (node --test). The joints are a synthetic hand
// (sizes of an adult hand, 8 mm fingertip radii as the Quest reports them),
// placed by random rigid motions and shaken by 1.5 mm of noise.
import test from 'node:test';
import assert from 'node:assert/strict';
import * as H from '../../app/static/vr/js/hands.js';
import * as M from '../../app/static/vr/js/mat4.js';

const close = (a, b, eps) => assert.ok(Math.abs(a - b) <= eps, `${a} != ${b} (±${eps})`);

/** Seeded random numbers, so a failure is reproducible. */
function rng(seed) {
  let s = seed >>> 0;
  return () => { s = (s * 1664525 + 1013904223) >>> 0; return s / 2 ** 32; };
}

const BASE = {
  wrist: [0, -0.045, 0],
  'thumb-metacarpal': [0.028, -0.03, 0.012], 'thumb-phalanx-proximal': [0.045, -0.006, 0.022],
  'index-finger-metacarpal': [0.02, 0, 0], 'index-finger-phalanx-proximal': [0.025, 0.045, 0],
  'middle-finger-metacarpal': [0.003, 0, 0], 'middle-finger-phalanx-proximal': [0.005, 0.047, 0],
  'ring-finger-phalanx-proximal': [-0.015, 0.044, 0],
  'pinky-finger-metacarpal': [-0.025, 0, 0], 'pinky-finger-phalanx-proximal': [-0.035, 0.038, 0],
};
const OPEN_TIPS = {
  'thumb-tip': [0.075, 0.03, 0.035], 'index-finger-tip': [0.027, 0.12, 0.005], 'middle-finger-tip': [0.006, 0.13, 0.005],
  'ring-finger-tip': [-0.016, 0.122, 0.005], 'pinky-finger-tip': [-0.037, 0.1, 0.005],
};
const PINCH_TIPS = { ...OPEN_TIPS, 'index-finger-tip': [0.036, 0.098, 0.035], 'thumb-tip': [0.04, 0.092, 0.041] };
const FIST_TIPS = {
  'thumb-tip': [0.03, 0.03, 0.04], 'index-finger-tip': [0.026, 0.025, 0.03], 'middle-finger-tip': [0.005, 0.025, 0.032],
  'ring-finger-tip': [-0.015, 0.024, 0.03], 'pinky-finger-tip': [-0.034, 0.02, 0.026],
};
const lerp3 = (a, b, f) => a.map((v, i) => v + (b[i] - v) * f);

/** Joints {name: {p, r}} of the hand with tips `tips`, moved by `pose` (a
 *  rigid 4x4), with `noise` metres of jitter. */
function hand(tips, pose = M.identity(), noise = 0, rand = Math.random) {
  const out = {};
  for (const [name, p] of Object.entries({ ...BASE, ...tips })) {
    const q = M.transformPoint(pose, p);
    out[name] = { p: q.map((v) => v + (rand() - 0.5) * 2 * noise), r: name.endsWith('tip') ? 0.008 : 0.01 };
  }
  return out;
}
const pose = (rand) => M.multiply(M.translation(rand() - 0.5, 1 + rand() * 0.5, -rand()),
  M.multiply(M.rotation(M.normalize([rand() - 0.5, rand() - 0.5, rand() - 0.5]), rand() * 6), M.identity()));

test('an open hand neither pinches nor grips, in any pose, with noise', () => {
  const rand = rng(1);
  for (let i = 0; i < 200; i++) {
    const j = hand(OPEN_TIPS, pose(rand), 0.0015, rand);
    const g = new H.Gesture().update(j);
    assert.ok(!g.pinch && !g.grip, JSON.stringify(g));
    assert.ok(g.curl > 1.2 && g.pinchDist > 0.03, JSON.stringify(g));
  }
});

test('a pinch presses once when the tips meet and releases only when they part', () => {
  const rand = rng(2);
  const g = new H.Gesture();
  const P = pose(rand);
  const states = [];
  // Close over 20 frames, hold 20 frames with 1.5 mm noise, open over 20 frames.
  const fs = [...Array(20).keys()].map((i) => i / 19).concat(Array(20).fill(1)).concat([...Array(20).keys()].map((i) => 1 - i / 19));
  for (const f of fs) {
    const tips = Object.fromEntries(Object.keys(OPEN_TIPS).map((k) => [k, lerp3(OPEN_TIPS[k], PINCH_TIPS[k], f)]));
    states.push(g.update(hand(tips, P, 0.0015, rand)).pinch);
  }
  const edges = states.reduce((n, s, i) => n + (s && !states[i - 1] ? 1 : 0), 0);
  assert.equal(edges, 1, `one press, states ${states.map(Number).join('')}`);
  assert.ok(states[30], 'held while pinched');
  assert.ok(!states.at(-1), 'released when open');
  // Hysteresis: between ON and OFF the state does not change.
  const h = new H.Gesture();
  const at = (d) => {
    const j = hand(OPEN_TIPS);
    j['thumb-tip'].p = M.add(j['index-finger-tip'].p, [d + 0.016, 0, 0]);
    return h.update(j).pinch;
  };
  assert.ok(!at(0.03), 'a new hand: the tips apart once first');
  assert.ok(!at(0.02));
  assert.ok(at(0.005));
  assert.ok(at(0.02), 'still pressed between the thresholds');
  assert.ok(!at(0.03));
});

test('a closed hand grips and never pinches (the thumb rests on the index in a fist)', () => {
  const rand = rng(3);
  for (let i = 0; i < 100; i++) {
    const g = new H.Gesture().update(hand(FIST_TIPS, pose(rand), 0.0015, rand));
    assert.ok(g.grip && !g.pinch, JSON.stringify(g));
  }
  // A pinch held while the hand closes is released; opening releases the grip.
  const g = new H.Gesture();
  g.update(hand(OPEN_TIPS));
  assert.ok(g.update(hand(PINCH_TIPS)).pinch);
  const s = g.update(hand(FIST_TIPS));
  assert.ok(s.grip && !s.pinch);
  assert.ok(!g.update(hand(OPEN_TIPS)).grip);
});

test('opening a fist with the thumb still on the index never fires a pinch', () => {
  const rand = rng(7);
  const g = new H.Gesture();
  const P = pose(rand);
  assert.ok(g.update(hand(FIST_TIPS, P)).grip);
  const states = [];
  // 30 frames: the fingers open gradually while the thumb tip rides on the index
  // tip (touching, then 4 mm apart) for the first 20, then the thumb leaves.
  for (let i = 0; i <= 30; i++) {
    const f = Math.min(1, i / 20);
    const tips = Object.fromEntries(Object.keys(OPEN_TIPS).map((k) => [k, lerp3(FIST_TIPS[k], OPEN_TIPS[k], f)]));
    if (i <= 20) tips['thumb-tip'] = M.add(tips['index-finger-tip'], [0.004 + 0.016 * (i / 20) * 0.1, 0, 0.012]);
    const s = g.update(hand(tips, P, 0.001, rand));
    states.push(s.pinch);
  }
  assert.ok(states.every((s) => !s), `states ${states.map(Number).join('')}`);
  assert.ok(!g.update(hand(OPEN_TIPS, P)).grip);
  // Once the tips have parted, a deliberate pinch works again.
  assert.ok(g.update(hand(PINCH_TIPS, P)).pinch);
});

test('a hand first seen already pinched does not press until its tips part', () => {
  const g = new H.Gesture();
  assert.ok(!g.update(hand(PINCH_TIPS)).pinch);
  assert.ok(!g.update(hand(PINCH_TIPS)).pinch);
  assert.ok(!g.update(hand(OPEN_TIPS)).pinch);
  assert.ok(g.update(hand(PINCH_TIPS)).pinch);
});

test('the hand\'s frame is rigid and moves with the hand, so a grabbed cube follows it exactly', () => {
  const rand = rng(4);
  for (let i = 0; i < 50; i++) {
    const A = pose(rand), B = pose(rand);
    const fa = H.handFrame(hand(FIST_TIPS, A));
    const fb = H.handFrame(hand(FIST_TIPS, M.multiply(B, A)));
    const x = [fa[0], fa[1], fa[2]], y = [fa[4], fa[5], fa[6]], z = [fa[8], fa[9], fa[10]];
    close(M.length(x), 1, 1e-5); close(M.length(y), 1, 1e-5); close(M.dot(x, y), 0, 1e-5);
    close(M.dot(M.cross(x, y), z), 1, 1e-5);
    // Frame after the motion = motion · frame before.
    const want = M.multiply(B, fa);
    for (let k = 0; k < 16; k++) close(fb[k], want[k], 2e-5);
    // grabOne: the cube's model relative to the hand stays.
    const cube = M.multiply(M.translation(0.2, 1.3, -0.7), M.scaling(0.6));
    const rel = M.multiply(M.invert(fa), cube);
    const moved = M.grabOne(fb, rel);
    const expect = M.multiply(B, cube);
    for (let k = 0; k < 16; k++) close(moved[k], expect[k], 5e-5);
  }
});

test('the fallback ray runs from the shoulder through the knuckles', () => {
  const j = hand(OPEN_TIPS, M.translation(0.25, 1.2, -0.4));
  const head = [0, 1.6, 0];
  const r = H.handRay(j, head, [1, 0, 0], 'right');
  close(M.length(r.d), 1, 1e-9);
  const shoulder = [0.17, 1.44, 0];
  const at = r.o;
  const want = M.normalize(M.sub(at, shoulder));
  for (let k = 0; k < 3; k++) close(r.d[k], want[k], 1e-9);
  assert.ok(r.d[2] < 0, 'points forward');
  const l = H.handRay(j, head, [1, 0, 0], 'left');
  assert.ok(l.d[0] > r.d[0], 'the left shoulder is on the other side');
});

test('the ray is still while a pinch closes, then follows again', () => {
  const st = new H.RayStabilizer({ tau: 1e-6, hold: 90, settle: 140 });
  let t = 0;
  const dirAt = (a) => [Math.sin(a), 0, -Math.cos(a)];
  for (; t < 300; t += 11) st.update({ o: [0, 1, 0], d: dirAt(0) }, t);
  // The fingers close: the hand turns the ray by 3 degrees over 60 ms, then the pinch registers.
  for (let k = 1; k <= 6; k++, t += 11) st.update({ o: [0, 1, 0], d: dirAt(0.0524 * k / 6) }, t);
  const atPress = st.update({ o: [0, 1, 0], d: dirAt(0.0524) }, t, true);
  close(atPress.d[0], 0, 1e-6);           // the direction from before the fingers moved
  t += 11;
  const held = st.update({ o: [0.01, 1, 0], d: dirAt(0.0524) }, t);
  close(held.d[0], 0, 1e-6);
  close(held.o[0], 0.01, 1e-6);           // the origin is still carried
  for (t += 11; t < 700; t += 11) st.update({ o: [0, 1, 0], d: dirAt(0.0524) }, t);
  close(st.update({ o: [0, 1, 0], d: dirAt(0.0524) }, t).d[0], Math.sin(0.0524), 1e-6);
  assert.equal(st.update(null, t), null);
});

test('smoothing takes out the tremor but keeps the aim', () => {
  const rand = rng(5);
  const st = new H.RayStabilizer();
  let errRaw = 0, errSmooth = 0, n = 0;
  for (let t = 0; t < 2000; t += 11) {
    const noisy = M.normalize([(rand() - 0.5) * 0.02, (rand() - 0.5) * 0.02, -1]);
    const s = st.update({ o: [0, 0, 0], d: noisy }, t);
    if (t > 200) { errRaw += Math.hypot(noisy[0], noisy[1]); errSmooth += Math.hypot(s.d[0], s.d[1]); n++; }
  }
  assert.ok(errSmooth / n < 0.6 * (errRaw / n), `${errSmooth / n} vs ${errRaw / n}`);
});

/** A fake XRFrame/XRHand/XRInputSource over the synthetic joints. */
function fakeSource(joints, { ray = true, missing = '' } = {}) {
  const spaces = new Map(H.JOINTS.map((n) => [n, { joint: n }]));
  const handObj = { get: (n) => spaces.get(n) };
  const frame = {
    getJointPose: (space) => (space.joint === missing || !joints[space.joint] ? null
      : { transform: { position: { x: joints[space.joint].p[0], y: joints[space.joint].p[1], z: joints[space.joint].p[2] } }, radius: joints[space.joint].r }),
    getPose: () => ({ transform: { matrix: new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.1, 1.2, -0.3, 1]) } }),
  };
  return { frame, src: { handedness: 'right', hand: handObj, targetRaySpace: ray ? {} : null } };
}

test('the WebXR layer: a tracked hand is the page\'s hand object, button 0 the pinch, button 1 the grip', () => {
  const tr = new H.HandTracker();
  const open = fakeSource(hand(OPEN_TIPS, M.translation(0.2, 1.2, -0.4)));
  assert.equal(tr.update(open.frame, open.src, {}, 0, [0, 1.6, 0], [1, 0, 0]).g.buttons[0].pressed, false);
  const { frame, src } = fakeSource(hand(PINCH_TIPS, M.translation(0.2, 1.2, -0.4)));
  const h = tr.update(frame, src, {}, 11, [0, 1.6, 0], [1, 0, 0]);
  assert.ok(h.isHand);
  assert.equal(h.g.buttons[0].pressed, true);
  assert.equal(h.g.buttons[1].pressed, false);
  assert.deepEqual(h.g.axes, []);
  assert.equal(h.g.buttons[4].pressed, h.palmUp, 'button 4 is the palm up (the menu on the secondary hand)');
  assert.equal(h.g.buttons[5].pressed, false, 'no upper face button on a hand');
  // The system's ray (targetRaySpace): -z of its pose.
  assert.deepEqual(h.ray.d.map((v) => Math.round(v * 1e6) / 1e6 + 0), [0, 0, -1]);
  close(h.pos[1], 1.2, 0.05);
  // No ray space: the shoulder ray.
  const f2 = fakeSource(hand(FIST_TIPS, M.translation(0.2, 1.2, -0.4)), { ray: false });
  const h2 = new H.HandTracker().update(f2.frame, f2.src, {}, 0, [0, 1.6, 0], [1, 0, 0]);
  assert.ok(h2.g.buttons[1].pressed && !h2.g.buttons[0].pressed);
  assert.ok(h2.ray.d[2] < 0);
  // A joint not tracked this frame: no hand (the page then has none for that side).
  const f3 = fakeSource(hand(OPEN_TIPS), { missing: 'index-finger-tip' });
  assert.equal(new H.HandTracker().update(f3.frame, f3.src, {}, 0, [0, 1.6, 0]), null);
  // Segments to draw it: fingertips, the pinch line, the palm.
  assert.ok(H.handSegments(h, [1, 1, 1, 1]).length >= 5 * 3 + 1 + 3);
  tr.keep(new Set());
  assert.equal(tr.hands.size, 0);
});

test('palm up (issue #201): either hand, any heading, with hysteresis; never in a fist', () => {
  const rand = rng(7);
  const mirror = (j) => Object.fromEntries(Object.entries(j).map(([k, v]) => [k, { ...v, p: [-v.p[0], v.p[1], v.p[2]] }]));
  // The synthetic hand is a right hand, palm toward +z; mirrored in x, a left hand.
  const palmUp = M.rotation([1, 0, 0], -Math.PI / 2);       // +z (the palm) to +y
  const palmDown = M.rotation([1, 0, 0], Math.PI / 2);
  for (const handed of ['right', 'left']) {
    const shape = (tips, pose) => {
      const j = hand(tips, M.identity());
      const jj = handed === 'left' ? mirror(j) : j;
      return Object.fromEntries(Object.entries(jj).map(([k, v]) => [k, { ...v, p: M.transformPoint(pose, v.p) }]));
    };
    for (let i = 0; i < 50; i++) {
      const turn = M.multiply(M.translation(0, 1.1, -0.3), M.rotation([0, 1, 0], rand() * 6.28));
      assert.ok(H.palmUpness(shape(OPEN_TIPS, M.multiply(turn, palmUp)), handed) > 0.95, `${handed} up`);
      assert.ok(H.palmUpness(shape(OPEN_TIPS, M.multiply(turn, palmDown)), handed) < -0.95, `${handed} down`);
    }
    const p = new H.PalmUp();
    const tilt = (deg) => shape(OPEN_TIPS, M.multiply(M.rotation([1, 0, 0], (deg * Math.PI) / 180), palmUp));
    // Up at 30° off vertical; still up at 55°; off at 70°; not again until within about 45°.
    assert.deepEqual([30, 55, 70, 55, 40].map((d) => p.update(tilt(d), handed)), [true, true, false, false, true]);
    assert.equal(p.update(tilt(10), handed, true), false, 'a closed hand is not the menu');
  }
  // On the page's hand object, button 4 (the menu's place on a controller).
  const o = H.handObject(hand(OPEN_TIPS), { pinch: false, grip: false }, null, true);
  assert.equal(o.g.buttons[4].pressed, true);
  assert.equal(o.palmUp, true);
});

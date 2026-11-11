// Unit tests of the refinement logic (issue #170): node --test backend/tests/vr/refine.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';
import { deflateSync } from 'node:zlib';
import * as F from '../../app/static/vr/js/refine.js';
import * as M from '../../app/static/vr/js/mat4.js';
import { colormapTable } from '../../app/static/vr/js/colormaps.js';

const close = (a, b, eps = 1e-6) => assert.ok(Math.abs(a - b) <= eps, `${a} != ${b} (eps ${eps})`);

/** A VXR1 message as the backend builds it (_stereo.XRFrame.to_bytes). */
function vxr1(header, color, depth) {
  const h = structuredClone(header);
  h.color = { ...h.color, offset: 0, length: color.length };
  if (depth) h.depth = { ...h.depth, offset: color.length, length: depth.length };
  const blob = new TextEncoder().encode(JSON.stringify(h));
  const out = new Uint8Array(8 + blob.length + color.length + (depth ? depth.length : 0));
  out.set([0x56, 0x58, 0x52, 0x31]);
  new DataView(out.buffer).setUint32(4, blob.length, true);
  out.set(blob, 8);
  out.set(color, 8 + blob.length);
  if (depth) out.set(depth, 8 + blob.length + color.length);
  return out.buffer;
}

test('a VXR1 message gives back its header and payloads', () => {
  const color = new Uint8Array([0xff, 0xd8, 1, 2, 3]);
  const depth = new Uint8Array([9, 8, 7]);
  const buf = vxr1({ v: 1, pose_id: 42, color: { format: 'jpeg', width: 4, height: 2 },
    depth: { format: 'u16-zlib', width: 2, height: 1, z_near: 0.5, z_far: 1 } }, color, depth);
  assert.ok(F.isVXR1(buf));
  const p = F.parseVXR1(buf);
  assert.equal(p.header.pose_id, 42);
  assert.deepEqual([...p.color], [...color]);
  assert.deepEqual([...p.depth], [...depth]);
  const none = F.parseVXR1(vxr1({ v: 1, pose_id: 1, color: { format: 'jpeg' }, depth: null }, color, null));
  assert.equal(none.depth, null);
});

test('a mono JPEG is not a stereo frame, and a truncated frame is refused', () => {
  assert.equal(F.isVXR1(new Uint8Array([0xff, 0xd8, 0xff, 0xe0, 0, 0, 0, 0]).buffer), false);
  assert.throws(() => F.parseVXR1(new Uint8Array([0xff, 0xd8, 0xff, 0xe0, 0, 0, 0, 0]).buffer));
  const buf = vxr1({ v: 1, pose_id: 1, color: { format: 'jpeg' }, depth: null }, new Uint8Array(10), null);
  assert.throws(() => F.parseVXR1(buf.slice(0, buf.byteLength - 3)), /past the message/);
});

test('the depth payload inflates and decodes as the backend encodes it', async () => {
  // _stereo.encode_depth: 0 = no sample, else 1 + round((d - z0) / (z1 - z0) * 65534)
  const z0 = 0.42, z1 = 1.37;
  const want = [0, z0, z1, 0.9, 0];
  const q = new Uint16Array(want.map((d) => (d === 0 ? 0 : 1 + Math.round((d - z0) / (z1 - z0) * 65534))));
  const z = deflateSync(Buffer.from(q.buffer));   // little-endian on every platform node runs on
  const raw = await F.inflate(new Uint8Array(z));
  const d = F.decodeDepth(raw, 5, 1, z0, z1);
  want.forEach((w, i) => close(d[i], w, (z1 - z0) / 65534));
  assert.equal(d[0], 0);
  assert.throws(() => F.decodeDepth(raw, 6, 1, z0, z1));
});

const view = (yawDeg, x = 0, y = 0, z = 0) =>
  M.invert(M.multiply(M.translation(x, y, z), M.rotation([0, 1, 0], yawDeg * Math.PI / 180)));

test('the pose delta measures the eye relative to the cube, in degrees and metres', () => {
  const model = M.multiply(M.translation(0, 1.5, -0.8), M.scaling(0.5));
  const a = M.multiply(view(0), model), b = M.multiply(view(3, 0.02, 0, 0), model);
  const d = F.poseDelta(a, b);
  close(d.angleDeg, 3, 1e-3);
  close(d.metres, 0.02, 1e-5);
  // Moving the cube with the hand is the same motion as moving the head.
  const moved = M.multiply(M.translation(-0.02, 0, 0), model);
  close(F.poseDelta(a, M.multiply(view(0), moved)).metres, 0.02, 1e-5);
  // A uniform scale of the cube, the eye fixed, moves the eye in the cube.
  const big = M.multiply(M.translation(0, 1.5, -0.8), M.scaling(1.0));
  assert.ok(F.poseDelta(a, M.multiply(view(0), big)).metres > 0.1);
});

test('the motion detector: moving, then slow, then still, and back to moving at once', () => {
  const det = new F.MotionDetector();
  const model = M.translation(0, 0, -1);
  let t = 0, yaw = 0;
  const run = (degS, ms) => {
    let s;
    for (let k = 0; k < ms / 11; k++) { t += 11; yaw += degS * 0.011; s = det.push(t, M.multiply(view(yaw), model)); }
    return s;
  };
  assert.equal(run(120, 300), 'moving');
  assert.equal(run(8, 400), 'slow');
  assert.equal(det.stoppedAt !== null, true);
  const stop = det.stoppedAt;
  assert.equal(run(0.5, 400), 'still');
  assert.equal(run(2.5, 100), 'still');           // hysteresis: jitter does not leave still
  assert.equal(run(200, 60), 'moving');           // a head turn: at once (within the smoothing)
  assert.equal(det.stoppedAt, stop);
  run(0, 400);
  assert.ok(det.stoppedAt > stop);
});

test('a pause in the frames starts the detector over, as moving', () => {
  const det = new F.MotionDetector();
  const a = M.translation(0, 0, -1);
  for (let t = 0; t < 500; t += 11) det.push(t, a);
  assert.equal(det.state, 'still');
  det.push(2000, a);
  assert.equal(det.state, 'moving');
});

test('the pose book: a frame answers its pose and every older one', () => {
  const b = new F.PoseBook();
  const ids = [1, 2, 3, 4].map((k) => b.add({ tSent: k * 10 }));
  assert.deepEqual(ids, [1, 2, 3, 4]);
  assert.equal(b.outstanding(45, 500), 4);
  assert.equal(b.take(3).tSent, 30);
  assert.equal(b.skipped, 2);
  assert.equal(b.take(2), null);                  // merged into 3 by the backend
  assert.equal(b.outstanding(45, 500), 1);
  assert.equal(b.outstanding(1000, 500), 0);      // timed out
});

test('reprojection: a still pose maps every pixel onto itself', () => {
  const proj = M.perspective(1.6, 1.2, 0.05, 50);
  const v = view(10, 0.1, 1.6, 0.3), model = M.multiply(M.translation(0, 1.5, -0.7), M.scaling(0.6));
  const E = F.reprojectionMatrix(v, model, v, model);
  const ndc = F.reprojectPixel(M.invert(proj), E, proj, 0.3, 0.8, 0.9);
  close(ndc[0], 0.3 * 2 - 1, 1e-4);
  close(ndc[1], 1 - 0.8 * 2, 1e-4);
});

test('reprojection with depth puts a world point where the current eye sees it', () => {
  const proj = M.perspective(1.6, 1, 0.05, 50);
  const model = M.multiply(M.translation(0, 0, -1), M.scaling(0.5));
  const vf = view(0), vn = view(4, 0.03, 0.01, 0);      // head turned 4 deg and moved 3 cm
  const pModel = [0.1, -0.05, 0.2];                       // a point of the cube
  const eyeF = M.transformPoint(M.multiply(vf, model), pModel);
  const ndcF = M.transformPoint(proj, eyeF);
  const u = (ndcF[0] + 1) / 2, v = (1 - ndcF[1]) / 2;
  const E = F.reprojectionMatrix(vf, model, vn, model);
  const got = F.reprojectPixel(M.invert(proj), E, proj, u, v, -eyeF[2]);
  const want = M.transformPoint(M.multiply(proj, M.multiply(vn, model)), pModel);
  close(got[0], want[0], 1e-4);
  close(got[1], want[1], 1e-4);
  // Without the depth (a plane through the cube's centre) the point lands
  // elsewhere: the parallax that depth reprojection removes.
  const plane = F.reprojectPixel(M.invert(proj), E, proj, u, v, F.centreDistance(vf, model));
  // 3 cm * (1/0.9 - 1/1.0) m^-1 = 3.3 mrad, about 0.003 in NDC here.
  assert.ok(Math.hypot(plane[0] - want[0], plane[1] - want[1]) > 2e-3);
  // A pure head turn needs no depth: any distance lands on the same pixel.
  const turn = F.reprojectionMatrix(vf, model, view(4), model);
  const a = F.reprojectPixel(M.invert(proj), turn, proj, u, v, 0.3);
  const b = F.reprojectPixel(M.invert(proj), turn, proj, u, v, 3);
  close(a[0], b[0], 1e-4);
  close(a[1], b[1], 1e-4);
});

test('reprojection follows the cube when it is grabbed', () => {
  const proj = M.perspective(1.6, 1, 0.05, 50);
  const mf = M.multiply(M.translation(0, 0, -1), M.scaling(0.5));
  const mn = M.multiply(M.translation(0.05, 0, -1), M.multiply(M.rotation([0, 1, 0], 0.05), M.scaling(0.55)));
  const v = view(0);
  const pModel = [-0.2, 0.1, 0.1];
  const eyeF = M.transformPoint(M.multiply(v, mf), pModel);
  const ndcF = M.transformPoint(proj, eyeF);
  const got = F.reprojectPixel(M.invert(proj), F.reprojectionMatrix(v, mf, v, mn), proj,
    (ndcF[0] + 1) / 2, (1 - ndcF[1]) / 2, -eyeF[2]);
  const want = M.transformPoint(M.multiply(proj, M.multiply(v, mn)), pModel);
  close(got[0], want[0], 1e-4);
  close(got[1], want[1], 1e-4);
});

test('a frame is shown only while the view is not moving and its pose is near', () => {
  const model = M.translation(0, 0, -1);
  const a = M.multiply(view(0), model);
  assert.ok(F.frameUsable(a, M.multiply(view(2, 0.01), model), 'still'));
  assert.ok(!F.frameUsable(a, M.multiply(view(2, 0.01), model), 'moving'));
  assert.ok(!F.frameUsable(a, M.multiply(view(8), model), 'slow'));
  assert.ok(!F.frameUsable(a, M.multiply(view(0, 0.05), model), 'slow'));
  assert.ok(F.poseChanged(null, a));
  assert.ok(!F.poseChanged(a, M.multiply(view(0.1), model), 0.2, 0.0015));
  assert.ok(F.poseChanged(a, M.multiply(view(0.5), model), 0.2, 0.0015));
});

test('the fade moves toward the frame while it may be shown, faster out', () => {
  let w = 0;
  for (let i = 0; i < 6; i++) w = F.fadeStep(w, true, 11);
  close(w, 66 / 120, 1e-9);
  w = F.fadeStep(w, true, 1000);
  assert.equal(w, 1);
  w = F.fadeStep(w, false, 30);
  close(w, 0.5, 1e-9);
  assert.equal(F.fadeStep(w, false, 100), 0);
});

test('the transfer function sent is the page\'s mapping on the full cube\'s values', () => {
  const lut = colormapTable('viridis');
  const p = { thrNorm: 0.1, cmaxNorm: 0.6, gamma: 0.5, opacity: 0.4, mode: 0, lo: -2, hi: 18, factors: [2, 2, 2], lut };
  const tf = F.transferFunction(p, 16);
  assert.equal(tf.type, 'set_transfer_functions');
  assert.equal(tf.blend_mode, 0);
  const xs = tf.opacity.map((q) => q[0]);
  assert.deepEqual(xs, [...xs].sort((a, b) => a - b));
  // Transparent below the threshold (data -2 + 0.1 * 20 = 0), drawn from it on.
  assert.equal(tf.opacity[0][1], 0);
  close(tf.opacity[0][0], 0, 1e-4);
  close(tf.opacity[1][0], 0, 1e-9);
  // At the threshold: per full voxel, the page's 0.04 term over a block of 2.
  close(tf.opacity[1][1], 1 - Math.exp(-0.4 * 0.04 / 2), 1e-9);
  // The colour maximum: the top of the map, full opacity term.
  const last = tf.color[tf.color.length - 1];
  close(last[0], -2 + 0.6 * 20, 1e-9);
  close(last[1], lut[255 * 4] / 255, 1e-9);
  close(tf.opacity[tf.opacity.length - 1][1], 1 - Math.exp(-0.4 * 1.04 / 2), 1e-9);
  // sqrt stretch: the map's middle at a quarter of the range above the threshold.
  const mid = tf.color[8];
  close(mid[0], -2 + (0.1 + 0.25 * 0.5) * 20, 1e-9);
  const mip = F.transferFunction({ ...p, mode: 1 }, 16);
  assert.equal(mip.blend_mode, 1);
  close(mip.opacity[1][1], 0.25, 1e-9);
});

test('the crop is the clipping box, or none for the whole cube', () => {
  assert.deepEqual(F.cropBox({ min: [0, 0, 0], max: [1, 1, 1] }), { type: 'set_crop', box: null });
  assert.deepEqual(F.cropBox({ min: [0, 0, 0.4], max: [1, 1, 0.6] }).box, { min: [0, 0, 0.4], max: [1, 1, 0.6] });
});

test('rolling statistics: quantiles and a rate', () => {
  const r = new F.Rolling(10);
  for (let i = 0; i < 20; i++) r.push(i * 25);
  assert.equal(r.length, 10);
  close(r.rate(475, 2000), 40, 1e-9);
  assert.equal(r.quantile(0.5), 375);
});

test('a page drawn under 4 frames a second still reaches still; a real pause starts over', () => {
  const det = new F.MotionDetector();
  const a = M.translation(0, 0, -1);
  let t = 0;
  for (let k = 0; k < 6; k++) { t += 300; det.push(t, a); }
  assert.equal(det.state, 'still');
  det.push(t + 3000, a);                          // ten intervals: a pause
  assert.equal(det.state, 'moving');
});

test('a clipping box the backend would refuse is named before it is sent', () => {
  assert.equal(F.cropError({ min: [0, 0, 0.4], max: [1, 1, 0.6] }), '');
  assert.match(F.cropError({ min: [0, 0, 0.6], max: [1, 1, 0.4] }), /axis z/);
  assert.match(F.cropError({ min: [0, -0.1, 0], max: [1, 1, 1] }), /axis y/);
  assert.match(F.cropError({ min: [0, NaN, 0], max: [1, 1, 1] }), /finite/);
});

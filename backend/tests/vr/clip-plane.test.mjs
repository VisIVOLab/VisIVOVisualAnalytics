// The free clipping plane of a simulation volume (issue #369): its state in
// the cube's voxel coordinates, the conversions to the box and the page's
// model space (an anisotropic grid included), the shader's numbers against
// the plane's own signed distance, the ray clipped as the shader clips it
// (half-space and slab, the section's face), flip / snap / reset / nudge,
// the grab with a controller, the gizmo, and the session's command (node --test).
import test from 'node:test';
import assert from 'node:assert/strict';
import * as CP from '../../app/static/vr/js/clip-plane.js';
import * as M from '../../app/static/vr/js/mat4.js';
import * as C from '../../app/static/vr/js/controls.js';

const close = (a, b, eps = 1e-6) => assert.ok(Math.abs(a - b) <= eps, `${a} != ${b}`);
const closeArr = (a, b, eps = 1e-6) => a.forEach((v, i) => close(v, b[i], eps));
const on = (p, extra = {}) => ({ ...p, on: true, ...extra });
const PLUTO = [2048, 2048, 2048];
/** A simulation grid whose z step is twice its x and y steps (as the VBT of the smoke). */
const SIM = { dims: [64, 48, 24], extent: [1, 0.75, 0.75] };

test('a new plane: through the centre, along x, off; a slab a tenth of the longest side', () => {
  const p = CP.defaultPlane([64, 48, 24]);
  assert.equal(p.on, false);
  assert.deepEqual(p.origin, [31.5, 23.5, 11.5]);
  assert.deepEqual(p.normal, [1, 0, 0]);
  assert.equal(p.thickness, 6);
  assert.equal(CP.defaultPlane(PLUTO).thickness, 205);
  assert.ok(CP.fitsCube(p, [64, 48, 24]));
  assert.ok(!CP.fitsCube(p, [64, 48, 25]), 'a cube of another shape gets its own plane');
});

test('voxels, box and model space: the plane is the same set of points in all three (anisotropic grid)', () => {
  const p = on(CP.defaultPlane(SIM.dims), { origin: [20, 30, 5], normal: M.normalize([1, -2, 3]) });
  const nM = CP.modelNormal(p, SIM.extent), cM = CP.modelOrigin(p, SIM.extent);
  close(M.length(nM), 1);
  // Voxel points on the plane are on the model plane; off it, on the same side.
  const u = M.normalize(M.cross(p.normal, [0, 0, 1])), w = M.cross(p.normal, u);
  for (const [a, b, h] of [[3, -2, 0], [-7, 5, 0], [1, 1, 4], [2, -6, -3]]) {
    const v = M.add(p.origin, M.add(M.scale3(u, a), M.add(M.scale3(w, b), M.scale3(p.normal, h))));
    const x = CP.boxToModel(CP.voxelToBox(v, p.dims), SIM.extent);
    const s = M.dot(nM, M.sub(x, cM));
    close(s / CP.modelPerVoxel(p, SIM.extent), h, 1e-9);
    if (h) assert.equal(CP.keeps(p, v), h > 0);
  }
  // And back: a plane given in model space is the one it came from.
  const q = CP.fromModel(CP.defaultPlane(SIM.dims), cM, nM, SIM.extent);
  closeArr(q.origin, p.origin, 1e-9);
  closeArr(q.normal, p.normal, 1e-9);
  // Box coordinates: the faces at the outer voxel edges.
  closeArr(CP.voxelToBox([-0.5, 47.5, 11.5], SIM.dims), [0, 1, 0.5]);
});

test('the shader\'s numbers give the plane\'s signed distance, in units of the longest side', () => {
  const p = on(CP.defaultPlane(SIM.dims), { origin: [20, 30, 5], normal: M.normalize([1, -2, 3]) });
  const c = CP.shaderParams(p);
  assert.equal(c.mode, 1);
  for (const v of [[0, 0, 0], [63, 47, 23], [20, 30, 5], [10.5, 3, 17]]) {
    close(CP.shaderS(c, CP.voxelToBox(v, p.dims)), CP.signedDistance(p, v) / 64, 1e-12);
  }
  // On the 2048³ density: values of order 1 (safe at mediump), a voxel a 1/2048 step.
  const big = CP.shaderParams(on(CP.defaultPlane(PLUTO), { normal: M.normalize([1, 0, 1]) }));
  assert.ok(big.plane.every((v) => Math.abs(v) <= 2));
  close(CP.shaderS(big, CP.voxelToBox([1024.5, 7, 1023.5], PLUTO)), (1 / Math.SQRT2) * (1 + 0) / 2048, 1e-9);
  const slab = CP.shaderParams(on(CP.defaultPlane(SIM.dims), { slab: true, thickness: 8 }));
  assert.equal(slab.mode, 2);
  close(slab.half, 4 / 64);
  assert.equal(CP.shaderParams(CP.defaultPlane(SIM.dims)), null, 'off: no plane in the shader');
});

test('the ray is clipped as the shader clips it: the kept half, the section on the face it enters through', () => {
  // The plane x = 0.5 of the box (box coordinates), keeping x >= 0.5.
  const p = on(CP.defaultPlane([10, 10, 10]), { origin: [4.5, 4.5, 4.5] });
  const c = CP.shaderParams(p);
  // From the cut side, looking +x: the ray starts on the plane, and the section is drawn there.
  const a = CP.clipRayJs(c, [-1, 0.5, 0.5], [1, 0, 0], 1, 2);
  closeArr(a, [1.5, 2, 1]);
  // From the kept side, looking -x: the ray stops at the plane, no section.
  const b = CP.clipRayJs(c, [2, 0.5, 0.5], [-1, 0, 0], 1, 2);
  closeArr(b, [1, 1.5, 0]);
  // Wholly in the cut half: nothing is drawn.
  assert.equal(CP.clipRayJs(c, [0.2, -1, 0.5], [0, 1, 0], 1, 2), null);
  // Flipped: the other half.
  const f = CP.shaderParams(CP.flip(p));
  closeArr(CP.clipRayJs(f, [-1, 0.5, 0.5], [1, 0, 0], 1, 2), [1, 1.5, 0]);
  // A slab 2 voxels thick about x = 4.5 (box 0.4..0.6), entered through its near face.
  const s = CP.shaderParams(on(p, { slab: true, thickness: 2 }));
  closeArr(CP.clipRayJs(s, [-1, 0.5, 0.5], [1, 0, 0], 1, 2), [1.4, 1.6, 1]);
  closeArr(CP.clipRayJs(s, [2, 0.5, 0.5], [-1, 0, 0], 1, 2), [1.4, 1.6, 1]);
  // Along the slab, inside it: kept, no face; outside it: nothing.
  closeArr(CP.clipRayJs(s, [0.5, -1, 0.5], [0, 1, 0], 1, 2), [1, 2, 0]);
  assert.equal(CP.clipRayJs(s, [0.3, -1, 0.5], [0, 1, 0], 1, 2), null);
});

test('flip, snap, reset, nudge; the plane always cuts the box', () => {
  const p = on(CP.defaultPlane([100, 50, 20]), { origin: [10, 10, 10], normal: M.normalize([0.2, -0.9, 0.3]), slab: true, thickness: 30 });
  assert.deepEqual(CP.flip(p).normal.map(Math.sign), [-1, 1, -1]);
  assert.deepEqual(CP.flip(CP.flip(p)).normal, p.normal);
  assert.deepEqual(CP.snap(p).normal, [0, -1, 0]);
  const r = CP.reset(p);
  assert.deepEqual([r.on, r.slab, r.origin, r.normal, r.thickness], [true, true, [49.5, 24.5, 9.5], [1, 0, 0], 10]);
  const n = CP.nudge(p, 5);
  close(CP.signedDistance(p, n.origin), 5, 1e-9);
  // Pushed far past the box: brought back to its last corner, the point inside the box.
  const far = CP.nudge(p, 1e6);
  const s = CP.corners(p.dims).map((q) => CP.signedDistance(far, q));
  close(Math.max(...s), 0, 1e-9);
  assert.ok(far.origin.every((v, i) => v >= -0.5 - 1e-9 && v <= p.dims[i] - 0.5 + 1e-9), JSON.stringify(far.origin));
  // The slab: thinner and thicker, within its limits.
  assert.equal(CP.scaleThickness(p, 1e-3).thickness, CP.MIN_THICKNESS);
  close(CP.scaleThickness(p, 1e3).thickness, Math.hypot(100, 50, 20));
});

test('the section of the box: a square across an axis, a hexagon across the diagonal', () => {
  const cs = CP.corners([10, 10, 10]);
  const sq = CP.sectionPolygon([4.5, 4.5, 4.5], [1, 0, 0], cs);
  assert.equal(sq.length, 4);
  assert.ok(sq.every((q) => Math.abs(q[0] - 4.5) < 1e-9));
  const hex = CP.sectionPolygon([4.5, 4.5, 4.5], M.normalize([1, 1, 1]), cs);
  assert.equal(hex.length, 6);
  // In order around the polygon: consecutive edges all turn the same way.
  const n = M.normalize([1, 1, 1]);
  for (let i = 0; i < 6; i++) {
    const a = hex[i], b = hex[(i + 1) % 6], c = hex[(i + 2) % 6];
    assert.ok(M.dot(M.cross(M.sub(b, a), M.sub(c, b)), n) > 0);
  }
  assert.deepEqual(CP.sectionPolygon([40, 4.5, 4.5], [1, 0, 0], cs), [], 'past the box: none');
});

test('the grab: the plane on a stick, moved with the ray and turned with the wrist', () => {
  const ext = [1, 1, 1];
  const p = on(CP.defaultPlane([64, 64, 64]));          // x = 31.5 (model x = 0), keeps +x
  const hand0 = M.translation(0, 0, 1.5);
  const ray0 = { o: [-0.6, 0, 1.5], d: M.normalize([0.6, 0, -1.5]) };   // meets the plane at the centre
  const g = CP.startGrab(p, ext, ray0, hand0);
  closeArr(g.g0, [0, 0, 0], 1e-9);
  // The hand moved up 0.1 (the ray with it): the plane follows; its normal as it was.
  const up = CP.dragGrab(g, ext, { o: [-0.6, 0.1, 1.5], d: ray0.d }, M.translation(0, 0.1, 1.5));
  closeArr(CP.modelNormal(up, ext), [1, 0, 0], 1e-9);
  // (Moving within the plane leaves it where it was: same plane, the point slid along it.)
  close(CP.signedDistance(p, up.origin), 0, 1e-9);
  // Moved along x: the plane moves along x.
  const right = CP.dragGrab(g, ext, { o: [-0.5, 0, 1.5], d: ray0.d }, M.translation(0.1, 0, 1.5));
  close(CP.signedDistance(p, right.origin), 0.1 * 64, 1e-6);
  // The wrist turned 45° about y: the normal turns 45° about y, about the grab point.
  const turn = M.multiply(M.translation(0, 0, 1.5), M.rotation([0, 1, 0], Math.PI / 4));
  const tilted = CP.dragGrab(g, ext, ray0, turn);
  closeArr(CP.modelNormal(tilted, ext), [Math.SQRT1_2, 0, -Math.SQRT1_2], 1e-9);
  close(CP.signedDistance(tilted, [31.5, 31.5, 31.5]), 0, 1e-6);
  // Through a scaled hand matrix (the model's scale): only its rotation counts.
  const scaled = M.multiply(turn, M.scaling(3));
  closeArr(CP.modelNormal(CP.dragGrab(g, ext, ray0, scaled), ext), [Math.SQRT1_2, 0, -Math.SQRT1_2], 1e-9);
});

test('the 45° plane through the centre of the 2048³ density, as the issue asks', () => {
  const ext = [1, 1, 1];
  const p = on(CP.defaultPlane(PLUTO));
  const g = CP.startGrab(p, ext, { o: [0, 0, 2], d: [0, 0, -1] }, M.identity());
  const q = CP.dragGrab(g, ext, { o: [0, 0, 2], d: [0, 0, -1] }, M.rotation([0, 0, 1], Math.PI / 4));
  closeArr(q.normal, [Math.SQRT1_2, Math.SQRT1_2, 0], 1e-9);
  closeArr(q.origin, [1023.5, 1023.5, 1023.5], 1e-6);
  const cmd = CP.command(q);
  assert.equal(cmd.type, 'set_clip_plane');
  closeArr(cmd.plane.normal, [Math.SQRT1_2, Math.SQRT1_2, 0], 1e-9);
  assert.equal(cmd.plane.thickness, null);
  assert.equal(cmd.plane.section_opacity, CP.SECTION_ALPHA);
  assert.match(CP.describe(q), /^Plane at \(1024, 1024, 1024\) px, normal \(0\.71, 0\.71, 0\.00\); keeps/);
});

test('the 2-D page: a drag along the normal, a tilt by the pointer\'s turn', () => {
  const ext = [1, 0.5, 0.5];
  const p = on(CP.defaultPlane([64, 32, 32]));
  const u = CP.alongNormal(p, ext, { o: [0.3, 0, 2], d: [0, 0, -1] });
  close(u, 0.3, 1e-9);
  assert.equal(CP.alongNormal(p, ext, { o: [2, 0, 0], d: [-1, 0, 0] }), null, 'along the normal: no answer');
  close(CP.signedDistance(p, CP.moveModel(p, ext, 0.25).origin), 0.25 * 64, 1e-9);
  const t = CP.tiltByRays(p, ext, [0, 0, -1], M.normalize([Math.tan(0.1), 0, -1]), 3);
  const n = CP.modelNormal(t, ext);
  close(Math.acos(Math.max(-1, Math.min(1, n[0]))), 0.3, 1e-6);
});

test('the gizmo: the section\'s outline and the arrow; a slab\'s two faces; nothing past the crop', () => {
  const ext = [1, 1, 1], whole = { min: [0, 0, 0], max: [1, 1, 1] };
  const p = on(CP.defaultPlane([10, 10, 10]));
  const g = CP.gizmo(p, ext, whole);
  assert.equal(g.polygon.length, 4);
  assert.equal(g.edges.length, 4 + 3, 'four sides and an arrow of three strokes');
  closeArr(g.handle, [0, 0, 0]);
  const s = CP.gizmo({ ...p, slab: true, thickness: 4 }, ext, whole);
  assert.equal(s.edges.length, 12, 'two faces and the plane faintly between');
  assert.ok(s.edges.slice(0, 4).every((e) => Math.abs(e.a[0] + 0.2) < 1e-9));
  const cut = CP.gizmo(p, ext, { min: [0.7, 0, 0], max: [1, 1, 1] });
  assert.ok(cut.empty);
});

test('the command and the checks the backend makes', () => {
  const p = CP.defaultPlane([8, 8, 8]);
  assert.deepEqual(CP.command(p), { type: 'set_clip_plane', plane: null });
  const s = on(p, { slab: true, thickness: 3 });
  assert.deepEqual(CP.command(s).plane, { origin: [3.5, 3.5, 3.5], normal: [1, 0, 0], thickness: 3, section_opacity: CP.SECTION_ALPHA });
  assert.equal(CP.planeError(s), '');
  assert.match(CP.planeError(on(p, { normal: [0, 0, 0] })), /normal/);
  assert.match(CP.planeError(on(p, { origin: [0, NaN, 0] })), /point/);
  assert.match(CP.planeError(on(p, { slab: true, thickness: 0 })), /thickness/);
});

test('the GLSL declares what the uniform setter sets', () => {
  for (const name of ['uClipMode', 'uClipPlane', 'uClipHalf', 'uSectionA']) assert.match(CP.CLIP_GLSL, new RegExp(name));
  assert.match(CP.CLIP_GLSL, /float clipRay\(vec3 o, vec3 d, inout float tn, inout float tf\)/);
  assert.match(CP.CLIP_GLSL, /bool clipKept\(vec3 t\)/);
  const calls = [];
  const gl = new Proxy({}, { get: (t, k) => (...a) => calls.push([k, ...a.slice(1)]) });
  const u = { uClipMode: 'm', uClipPlane: 'p', uClipHalf: 'h', uSectionA: 'a' };
  CP.setClipUniforms(gl, u, null);
  assert.deepEqual(calls, [['uniform1i', 0]]);
  calls.length = 0;
  CP.setClipUniforms(gl, u, { mode: 2, plane: [1, 0, 0, -0.5], half: 0.1, sectionA: 0.85 });
  assert.deepEqual(calls, [['uniform1i', 2], ['uniform4f', 1, 0, 0, -0.5], ['uniform1f', 0.1], ['uniform1f', 0.85]]);
});

test('the Clipping plane tool\'s controls: the trigger holds the plane, the stick moves it and sizes the slab', () => {
  const map = C.controlMap({ mode: 'clip', sub: 'plane' });
  const act = (role, control) => C.actionFor(map, role, control);
  assert.equal(act('primary', 'trigger'), 'plane-drag');
  assert.equal(act('primary', 'stickY'), 'plane-move');
  assert.equal(act('primary', 'stickX'), 'plane-width');
  assert.equal(C.modeLine('clip', 'plane'), 'RANGE · Clipping plane');
  assert.ok(C.SUBTOOLS.clip.includes('plane'));
});

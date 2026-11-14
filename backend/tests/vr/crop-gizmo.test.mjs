// The spatial crop seen and grabbed (issue #277): the frames of a cube and
// of a point table or velocity field, the hit test (handles, inside, new),
// and the drag maths (a face, the whole crop, a new crop from two corners),
// in model space and through a moved, turned and scaled cube (node --test).
import test from 'node:test';
import assert from 'node:assert/strict';
import * as CG from '../../app/static/vr/js/crop-gizmo.js';
import * as M from '../../app/static/vr/js/mat4.js';

const close = (a, b, eps = 1e-5) => assert.ok(Math.abs(a - b) <= eps, `${a} != ${b}`);
const closeArr = (a, b, eps = 1e-5) => a.forEach((v, i) => close(v, b[i], eps));
const WHOLE = { min: [0, 0, 0], max: [1, 1, 1] };
/** A ray from `o` through `q` (model space), its direction a unit vector (as a controller's). */
const ray = (o, q) => ({ o, d: M.normalize(M.sub(q, o)) });

test('frames: box fractions to model space and back, for a cube and for a point table', () => {
  const f = CG.cubeFrame([1, 0.8, 0.5]);
  closeArr(CG.toModel(f, [0, 0, 0]), [-0.5, -0.4, -0.25]);
  closeArr(CG.toModel(f, [1, 1, 1]), [0.5, 0.4, 0.25]);
  closeArr(CG.toFrac(f, CG.toModel(f, [0.2, 0.7, 0.9])), [0.2, 0.7, 0.9]);
  // A point table drawn as (p - centre) / side, its crop fractions of lo..hi (points.js).
  const box = { lo: [-10, 0, 5], hi: [30, 20, 15], centre: [10, 10, 10], side: 40 };
  const s = CG.sceneFrame(box);
  closeArr(CG.toModel(s, [0, 0, 0]), [-0.5, -0.25, -0.125]);
  closeArr(CG.toModel(s, [1, 1, 1]), [0.5, 0.25, 0.125]);
  // The same mapping as the shader's uLo + aPos * uSpan.
  const t = [0.3, 0.6, 0.1];
  closeArr(CG.toModel(s, t), [0, 1, 2].map((i) => (box.lo[i] - box.centre[i]) / box.side + t[i] * (box.hi[i] - box.lo[i]) / box.side));
});

test('the six handles sit at the centres of the crop\'s faces', () => {
  const clip = { min: [0.2, 0.1, 0.3], max: [0.6, 0.9, 0.5] };
  const h = Object.fromEntries(CG.handlePoints(clip).map((x) => [x.id, x.p]));
  assert.deepEqual(Object.keys(h), ['x-min', 'x-max', 'y-min', 'y-max', 'z-min', 'z-max']);
  closeArr(h['x-min'], [0.2, 0.5, 0.4]);
  closeArr(h['y-max'], [0.4, 0.9, 0.4]);
  closeArr(h['z-min'], [0.4, 0.5, 0.3]);
});

test('hit test: a handle under the ray (the nearest along it), the inside of a crop, else a new crop', () => {
  const f = CG.cubeFrame([1, 1, 1]);
  const clip = { min: [0.2, 0.2, 0.2], max: [0.8, 0.8, 0.8] };
  // From the front (+z), straight at the z-max handle (model (0, 0, 0.3)).
  let h = CG.hitCrop(f, clip, ray([0, 0, 2], [0, 0, 0]));
  assert.equal(h.kind, 'face');
  assert.equal(h.id, 'z-max', 'the nearest handle along the ray, not the one behind it');
  // Just off the handle, within HIT_M (scale 1: model = metres).
  h = CG.hitCrop(f, clip, ray([0.03, 0, 2], [0.03, 0, 0]));
  assert.equal(h.id, 'z-max');
  // At the x-max handle's side: (0.3, 0, 0).
  h = CG.hitCrop(f, clip, ray([0.3, 0.02, 2], [0.3, 0.02, 0]));
  assert.equal(h.id, 'x-max');
  // A scaled-down cube (0.25 m per model unit): the same 4.5 cm is 0.18 model units.
  assert.equal(CG.hitCrop(f, clip, ray([0.1, 0, 2], [0.1, 0, 0]), { scale: 0.25 }).id, 'z-max');
  assert.equal(CG.hitCrop(f, clip, ray([0.1, 0, 2], [0.1, 0, 0]), { scale: 1 }).kind, 'inside');
  // Inside the crop, away from the handles.
  h = CG.hitCrop(f, clip, ray([0.15, 0.15, 2], [0.15, 0.15, 0]));
  assert.equal(h.kind, 'inside');
  close(h.t, 2 - 0.3);
  closeArr(h.point, [0.15, 0.15, 0.3]);
  // Outside the crop (but inside the box): a new crop.
  assert.equal(CG.hitCrop(f, clip, ray([0.45, 0.45, 2], [0.45, 0.45, 0])).kind, 'new');
  // Pointing away from everything.
  assert.equal(CG.hitCrop(f, clip, ray([0, 0, 2], [0, 0, 3])).kind, 'new');
  // Nothing cropped yet: the whole box is where a new crop is drawn (moving it is meaningless).
  assert.equal(CG.hitCrop(f, WHOLE, ray([0.2, 0.2, 2], [0.2, 0.2, 0])).kind, 'new');
  // Cropped only along the ray (a velocity range seen along it): a new crop too; from the side it moves.
  const slab = { min: [0, 0, 0.3], max: [1, 1, 0.7] };
  assert.equal(CG.hitCrop(f, slab, ray([0.2, 0.2, 2], [0.2, 0.2, 0])).kind, 'new');
  assert.equal(CG.hitCrop(f, slab, ray([2, 0.2, 0.1], [0, 0.2, 0.1])).kind, 'inside');
  // The controller's tip near a handle lights it up even when the ray points elsewhere.
  h = CG.hitCrop(f, clip, ray([0.32, 0, 0.02], [0.32, 5, 0.02]), { tip: [0.32, 0, 0.02] });
  assert.equal(h.id, 'x-max');
});

test('hit test through the cube\'s pose: a world ray into model space', () => {
  const f = CG.cubeFrame([1, 1, 1]);
  const clip = { min: [0.2, 0.2, 0.2], max: [0.8, 0.8, 0.8] };
  // The cube 0.5 m across, turned 90° about y, 1 m in front of the eyes.
  const model = M.multiply(M.translation(0, 1.5, -1), M.multiply(M.rotation([0, 1, 0], -Math.PI / 2), M.scaling(0.5)));
  // The model's +x face now looks at +z in the world: the x-max handle is at world (0, 1.5, -1 + 0.15).
  const hp = M.transformPoint(model, CG.toModel(f, [0.8, 0.5, 0.5]));
  closeArr(hp, [0, 1.5, -0.85], 1e-5);
  const r = CG.modelRay(model, { o: [0, 1.5, 0], d: [0, 0, -1] });
  const h = CG.hitCrop(f, clip, r, { scale: M.uniformScale(model) });
  assert.equal(h.id, 'x-max');
  close(h.t, 0.85, 1e-5);   // the same parameter as along the world ray
});

test('dragging a handle moves its face along its axis with the ray; the other faces stay', () => {
  const f = CG.cubeFrame([2, 1, 1]);             // x twice as long: a fraction is 2 model units
  const clip = { min: [0.2, 0.2, 0.2], max: [0.8, 0.8, 0.8] };
  const r0 = ray([0.6, 0, 3], [0.6, 0, 0]);       // at the x-max handle, model (0.6, 0, 0)
  const hit = CG.hitCrop(f, clip, r0);
  assert.equal(hit.id, 'x-max');
  const d = CG.startDrag(hit, clip, r0, f);
  // The ray swings to model x = 0.8: the face moves 0.2 model units = 0.1 of the box.
  let c = CG.dragCrop(d, ray([0.8, 0, 3], [0.8, 0, 0]), f);
  close(c.max[0], 0.9);
  assert.deepEqual([c.min[0], c.min[1], c.max[1], c.min[2], c.max[2]], [0.2, 0.2, 0.8, 0.2, 0.8]);
  // Up and down does not move an x face.
  c = CG.dragCrop(d, ray([0.8, 0.3, 3], [0.8, 0.3, 0]), f);
  close(c.max[0], 0.9);
  // Not past the box, nor past the opposite face (GAP apart).
  close(CG.dragCrop(d, ray([5, 0, 3], [5, 0, 0]), f).max[0], 1);
  close(CG.dragCrop(d, ray([-5, 0, 3], [-5, 0, 0]), f).max[0], 0.2 + CG.GAP);
  // A ray along the face's own axis cannot say where: the last place stays.
  CG.dragCrop(d, ray([0.8, 0, 3], [0.8, 0, 0]), f);
  close(CG.dragCrop(d, ray([3, 0, 0], [-3, 0, 0]), f).max[0], 0.9);
  // The start's crop is not changed in place.
  assert.deepEqual(clip, { min: [0.2, 0.2, 0.2], max: [0.8, 0.8, 0.8] });
});

test('dragging the inside moves the whole crop, its size kept, inside the box', () => {
  const f = CG.cubeFrame([1, 1, 1]);
  const clip = { min: [0.2, 0.2, 0.2], max: [0.6, 0.6, 0.6] };
  const r0 = ray([-0.2, -0.2, 2], [-0.2, -0.2, 0]);
  const hit = CG.hitCrop(f, clip, r0);
  assert.equal(hit.kind, 'inside');
  const d = CG.startDrag(hit, clip, r0, f);
  // The controller carried sideways, the ray parallel: the grab point moves with it.
  const o = [-0.2, -0.2, 2], grab = hit.point;
  const to = (dx, dy) => ray([o[0] + dx, o[1] + dy, o[2]], [grab[0] + dx, grab[1] + dy, grab[2]]);
  let c = CG.dragCrop(d, to(0.1, 0), f);
  closeArr(c.min, [0.3, 0.2, 0.2]);
  closeArr(c.max, [0.7, 0.6, 0.6]);
  // Far to the right: stopped at the box's edge, the size kept.
  c = CG.dragCrop(d, to(5, 0), f);
  closeArr(c.min, [0.6, 0.2, 0.2]);
  closeArr(c.max, [1, 0.6, 0.6]);
  c = CG.dragCrop(d, to(-5, -5), f);
  closeArr(c.min, [0, 0, 0.2]);
  closeArr(c.max, [0.4, 0.4, 0.6]);
});

test('a drag elsewhere draws a new crop from two corners across the axes facing the ray; the depth is kept', () => {
  const f = CG.cubeFrame([1, 1, 1]);
  const clip = { min: [0, 0, 0.3], max: [1, 1, 0.7] };   // a cube's velocity range, nothing cropped in x, y
  const o = [0, 0, 3];
  const r0 = ray(o, [-0.3, -0.3, 0]);
  const hit = CG.hitCrop(f, clip, r0);
  assert.equal(hit.kind, 'new');
  const d = CG.startDrag(hit, clip, r0, f);
  assert.equal(d.k, 2, 'looking along z: the depth axis is z');
  // The corners where the rays meet the plane across z at the crop's middle depth (model z 0).
  let c = CG.dragCrop(d, ray(o, [0.1, 0.2, 0]), f);
  closeArr(c.min, [0.2, 0.2, 0.3]);
  closeArr(c.max, [0.6, 0.7, 0.7], 1e-5);
  // Dragged the other way past the first corner: still min < max.
  c = CG.dragCrop(d, ray(o, [-0.45, -0.4, 0]), f);
  closeArr(c.min, [0.05, 0.1, 0.3]);
  closeArr(c.max, [0.2, 0.2, 0.7]);
  assert.deepEqual(CG.endDrag(d, c), c);
  // Past the box: clamped to it.
  c = CG.dragCrop(d, ray(o, [3, 3, 0]), f);
  closeArr(c.max, [1, 1, 0.7]);
  // A click (no drag): the crop stays as it was.
  const d2 = CG.startDrag(hit, clip, r0, f);
  const c2 = CG.dragCrop(d2, r0, f);
  assert.deepEqual(CG.endDrag(d2, c2), clip);
  // Nothing cropped, seen from the side (along x): the new crop spans y and z, keeps x.
  const side = ray([3, 0.1, 0.1], [0, 0.1, 0.1]);
  const d3 = CG.startDrag(CG.hitCrop(f, WHOLE, side), WHOLE, side, f);
  assert.equal(d3.k, 0);
  const c3 = CG.dragCrop(d3, ray([3, 0.1, 0.1], [0, 0.3, 0.2]), f);
  closeArr(c3.min, [0, 0.6, 0.6]);
  closeArr(c3.max, [1, 0.8, 0.7]);
});

test('the drawing: twelve edges on the crop, six faces sized to it, six dots, the pointed one lit', () => {
  const f = CG.cubeFrame([1, 1, 1]);
  const clip = { min: [0.2, 0.2, 0.2], max: [0.6, 0.8, 0.5] };
  const g = CG.gizmo(f, clip, { kind: 'face', id: 'y-max' });
  assert.equal(g.edges.length, 12);
  for (const e of g.edges) {
    // Each edge runs along one axis, at the crop's corners.
    const diff = M.sub(e.b, e.a).map((v) => Math.abs(v) > 1e-9);
    assert.equal(diff.filter(Boolean).length, 1);
  }
  assert.equal(g.faces.length, 6);
  const zmax = g.faces.find((x) => x.id === 'z-max');
  closeArr(zmax.size, [0.4, 0.6]);
  closeArr([zmax.m[12], zmax.m[13], zmax.m[14]], [-0.1, 0, 0]);
  assert.deepEqual(g.handles.filter((h) => h.hot).map((h) => h.id), ['y-max']);
  assert.equal(g.fillHot, false);
  assert.equal(CG.gizmo(f, clip, { kind: 'inside' }).fillHot, true);
  assert.equal(CG.gizmo(f, clip, { kind: 'move' }).fillHot, true, 'while the crop is moved');
});

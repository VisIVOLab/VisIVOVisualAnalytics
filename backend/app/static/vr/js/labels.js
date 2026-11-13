// The cube's tick labels and axis titles as quads fixed to the cube (issue
// #226, point 3). Until #226 they turned to face the head (billboards), which
// read as unstable in the headset. Now each label lies in a plane of the
// cube's own model space and moves only when the cube is turned.
//
// The faces are chosen for the default view (the cube placed in front of the
// viewer with its +z face towards them, renderer.js and main.js placeInFront):
// - 'front': in the plane of the +z face (RA and Dec ticks and titles, the
//   binning note), text along +x;
// - 'side': the velocity axis runs away from the viewer along the right
//   bottom edge; its labels stand at 45° between the +z and +x faces, so they
//   read from the front and from the right.
// A label seen from behind its face would read mirrored: the renderer flips
// its texture horizontally then (the quad itself does not move).
//
// Pure: unit-tested in backend/tests/vr/labels.test.mjs.

const S = Math.SQRT1_2;

/** The label faces: the text's right and up directions in model space. */
export const FACES = {
  front: { right: [1, 0, 0], up: [0, 1, 0] },
  side: { right: [S, 0, -S], up: [0, 1, 0] },
};

/** The face of label `l` (front unless it names another). */
export function faceOf(l) {
  return FACES[l && l.face] || FACES.front;
}

/** The normal a face is read from: cross(right, up), towards the reader. */
export function faceNormal(f) {
  const [a, b] = [f.right, f.up];
  return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
}

/** Floats per vertex of labelVertices: centre 3, offset 2, uv 2, right 3, up 3, u range 2. */
export const LABEL_STRIDE = 15;

/**
 * The vertices of the label quads: for each label `l` ({pos, height, anchor,
 * face}) and its atlas cell `cell` ({u0, u1, v0, v1, widthPx, rowPx}), two
 * triangles in the label's face, anchored at l.pos. The positions are model
 * space and do not depend on the viewer.
 */
export function labelVertices(labels, cells) {
  const out = [];
  labels.forEach((l, i) => {
    const c = cells[i];
    if (!c) return;
    const f = faceOf(l);
    const hgt = l.height || 0.025, wid = hgt * c.widthPx / c.rowPx;
    const ax = l.anchor === 'right' ? -wid : l.anchor === 'center' ? -wid / 2 : 0;
    const corners = [[ax, -hgt / 2, c.u0, c.v1], [ax + wid, -hgt / 2, c.u1, c.v1], [ax + wid, hgt / 2, c.u1, c.v0],
      [ax, -hgt / 2, c.u0, c.v1], [ax + wid, hgt / 2, c.u1, c.v0], [ax, hgt / 2, c.u0, c.v0]];
    for (const [ox, oy, u, v] of corners) out.push(...l.pos, ox, oy, u, v, ...f.right, ...f.up, c.u0, c.u1);
  });
  return new Float32Array(out);
}

/** A vertex's model-space position (what the vertex shader computes). */
export function vertexPosition(v, k) {
  const b = k * LABEL_STRIDE;
  const [cx, cy, cz, ox, oy] = [v[b], v[b + 1], v[b + 2], v[b + 3], v[b + 4]];
  const r = [v[b + 7], v[b + 8], v[b + 9]], u = [v[b + 10], v[b + 11], v[b + 12]];
  return [cx + r[0] * ox + u[0] * oy, cy + r[1] * ox + u[1] * oy, cz + r[2] * ox + u[2] * oy];
}

/** Is a label at `pos` on face `f` seen from behind by an eye at `eye` (model space)? */
export function seenFromBehind(pos, f, eye) {
  const n = faceNormal(f);
  return (eye[0] - pos[0]) * n[0] + (eye[1] - pos[1]) * n[1] + (eye[2] - pos[2]) * n[2] < 0;
}

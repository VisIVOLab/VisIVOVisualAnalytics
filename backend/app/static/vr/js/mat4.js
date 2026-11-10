// Column-major 4x4 matrices (the WebGL / WebXR layout), as Float32Array(16).
// Pure functions: no DOM, no GL, so node runs the tests:
// node --test backend/tests/vr/*.test.mjs (Node 26 takes files, not a directory).

export function identity() {
  const m = new Float32Array(16);
  m[0] = m[5] = m[10] = m[15] = 1;
  return m;
}

export function multiply(a, b) {
  const o = new Float32Array(16);
  for (let c = 0; c < 4; c++) {
    for (let r = 0; r < 4; r++) {
      o[c * 4 + r] = a[r] * b[c * 4] + a[4 + r] * b[c * 4 + 1]
        + a[8 + r] * b[c * 4 + 2] + a[12 + r] * b[c * 4 + 3];
    }
  }
  return o;
}

export function invert(m) {
  const a00 = m[0], a01 = m[1], a02 = m[2], a03 = m[3];
  const a10 = m[4], a11 = m[5], a12 = m[6], a13 = m[7];
  const a20 = m[8], a21 = m[9], a22 = m[10], a23 = m[11];
  const a30 = m[12], a31 = m[13], a32 = m[14], a33 = m[15];
  const b00 = a00 * a11 - a01 * a10, b01 = a00 * a12 - a02 * a10;
  const b02 = a00 * a13 - a03 * a10, b03 = a01 * a12 - a02 * a11;
  const b04 = a01 * a13 - a03 * a11, b05 = a02 * a13 - a03 * a12;
  const b06 = a20 * a31 - a21 * a30, b07 = a20 * a32 - a22 * a30;
  const b08 = a20 * a33 - a23 * a30, b09 = a21 * a32 - a22 * a31;
  const b10 = a21 * a33 - a23 * a31, b11 = a22 * a33 - a23 * a32;
  let det = b00 * b11 - b01 * b10 + b02 * b09 + b03 * b08 - b04 * b07 + b05 * b06;
  if (!det) return null;
  det = 1.0 / det;
  const o = new Float32Array(16);
  o[0] = (a11 * b11 - a12 * b10 + a13 * b09) * det;
  o[1] = (a02 * b10 - a01 * b11 - a03 * b09) * det;
  o[2] = (a31 * b05 - a32 * b04 + a33 * b03) * det;
  o[3] = (a22 * b04 - a21 * b05 - a23 * b03) * det;
  o[4] = (a12 * b08 - a10 * b11 - a13 * b07) * det;
  o[5] = (a00 * b11 - a02 * b08 + a03 * b07) * det;
  o[6] = (a32 * b02 - a30 * b05 - a33 * b01) * det;
  o[7] = (a20 * b05 - a22 * b02 + a23 * b01) * det;
  o[8] = (a10 * b10 - a11 * b08 + a13 * b06) * det;
  o[9] = (a01 * b08 - a00 * b10 - a03 * b06) * det;
  o[10] = (a30 * b04 - a31 * b02 + a33 * b00) * det;
  o[11] = (a21 * b02 - a20 * b04 - a23 * b00) * det;
  o[12] = (a11 * b07 - a10 * b09 - a12 * b06) * det;
  o[13] = (a00 * b09 - a01 * b07 + a02 * b06) * det;
  o[14] = (a31 * b01 - a30 * b03 - a32 * b00) * det;
  o[15] = (a20 * b03 - a21 * b01 + a22 * b00) * det;
  return o;
}

export function translation(x, y, z) {
  const m = identity();
  m[12] = x; m[13] = y; m[14] = z;
  return m;
}

export function scaling(x, y = x, z = x) {
  const m = identity();
  m[0] = x; m[5] = y; m[10] = z;
  return m;
}

/** Rotation by `angle` radians about the unit vector `axis`. */
export function rotation(axis, angle) {
  const [x, y, z] = axis;
  const c = Math.cos(angle), s = Math.sin(angle), t = 1 - c;
  const m = identity();
  m[0] = t * x * x + c;     m[4] = t * x * y - s * z; m[8] = t * x * z + s * y;
  m[1] = t * x * y + s * z; m[5] = t * y * y + c;     m[9] = t * y * z - s * x;
  m[2] = t * x * z - s * y; m[6] = t * y * z + s * x; m[10] = t * z * z + c;
  return m;
}

export function perspective(fovyRad, aspect, near, far) {
  const f = 1.0 / Math.tan(fovyRad / 2), nf = 1 / (near - far);
  const m = new Float32Array(16);
  m[0] = f / aspect; m[5] = f;
  m[10] = (far + near) * nf; m[11] = -1;
  m[14] = 2 * far * near * nf;
  return m;
}

export function transformPoint(m, p) {
  const [x, y, z] = p;
  const w = m[3] * x + m[7] * y + m[11] * z + m[15];
  return [
    (m[0] * x + m[4] * y + m[8] * z + m[12]) / w,
    (m[1] * x + m[5] * y + m[9] * z + m[13]) / w,
    (m[2] * x + m[6] * y + m[10] * z + m[14]) / w,
  ];
}

export function sub(a, b) { return [a[0] - b[0], a[1] - b[1], a[2] - b[2]]; }
export function add(a, b) { return [a[0] + b[0], a[1] + b[1], a[2] + b[2]]; }
export function scale3(a, s) { return [a[0] * s, a[1] * s, a[2] * s]; }
export function dot(a, b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
export function length(a) { return Math.hypot(a[0], a[1], a[2]); }
export function cross(a, b) {
  return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
}
export function normalize(a) {
  const l = length(a);
  return l > 0 ? [a[0] / l, a[1] / l, a[2] / l] : [0, 0, 0];
}

/** The rotation that turns direction `a` into direction `b` (shortest arc). */
export function rotationBetween(a, b) {
  const u = normalize(a), v = normalize(b);
  const c = Math.max(-1, Math.min(1, dot(u, v)));
  let axis = cross(u, v);
  if (length(axis) < 1e-9) {
    if (c > 0) return identity();
    // Opposite: any axis perpendicular to u.
    axis = Math.abs(u[0]) < 0.9 ? cross(u, [1, 0, 0]) : cross(u, [0, 1, 0]);
  }
  return rotation(normalize(axis), Math.acos(c));
}

/** Uniform scale factor of an affine matrix (length of its first column). */
export function uniformScale(m) {
  return Math.hypot(m[0], m[1], m[2]);
}

export function position(m) { return [m[12], m[13], m[14]]; }

/**
 * Slab test of the ray o + t d against the box [lo, hi]: [tNear, tFar], or
 * null when the ray misses. The same test as the fragment shader's.
 */
export function rayBox(o, d, lo, hi) {
  let tn = -Infinity, tf = Infinity;
  for (let i = 0; i < 3; i++) {
    if (Math.abs(d[i]) < 1e-12) {
      if (o[i] < lo[i] || o[i] > hi[i]) return null;
      continue;
    }
    let t1 = (lo[i] - o[i]) / d[i], t2 = (hi[i] - o[i]) / d[i];
    if (t1 > t2) [t1, t2] = [t2, t1];
    tn = Math.max(tn, t1); tf = Math.min(tf, t2);
  }
  return tn <= tf ? [tn, tf] : null;
}

/**
 * Grab with one hand: the model keeps its pose relative to the controller.
 * `rel` = inverse(controllerAtGrab) * modelAtGrab; returns the new model matrix.
 */
export function grabOne(controllerNow, rel) {
  return multiply(controllerNow, rel);
}

/**
 * Grab with two hands: translate with the midpoint, rotate with the line
 * between the hands, scale with their distance. Positions are [x, y, z].
 */
export function grabTwo(modelAtGrab, leftAt, rightAt, leftNow, rightNow,
  minScale = 0.02, maxScale = 50) {
  const midAt = scale3(add(leftAt, rightAt), 0.5);
  const midNow = scale3(add(leftNow, rightNow), 0.5);
  const vAt = sub(rightAt, leftAt), vNow = sub(rightNow, leftNow);
  const dAt = length(vAt), dNow = length(vNow);
  let s = dAt > 1e-4 ? dNow / dAt : 1;
  const s0 = uniformScale(modelAtGrab);
  s = Math.min(maxScale / s0, Math.max(minScale / s0, s));
  const r = rotationBetween(vAt, vNow);
  let m = translation(-midAt[0], -midAt[1], -midAt[2]);
  m = multiply(scaling(s), m);
  m = multiply(r, m);
  m = multiply(translation(midNow[0], midNow[1], midNow[2]), m);
  return multiply(m, modelAtGrab);
}

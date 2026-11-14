// Velocity fields in the headset (issue #190): the streamlines and arrows the
// backend traces on the desktop's display grid (/v1/velocity/lines_bin, app/
// velocity_lines.py: the same VTK filters and settings as the desktop's
// velocity viewer), drawn as lines coloured by the speed through the page's
// colour maps. The headset integrates nothing.
//
// Model space: the field's box centred on the origin, its longest side 1,
// over the display grid's bounds as the route reports them (X-Visivo-Bounds,
// or its spacing and origin; without either, the grid the velocity routes
// describe: spacing box/(n-1), origin -box/2, or unit spacing, origin
// -(n-1)/2). Pure functions unit-tested in backend/tests/vr/layers.test.mjs.

import * as M from './mat4.js';

/** Geometry and counts of a lines_bin answer, from its headers (`get`). */
export function linesInfo(get, fallback = {}) {
  const list = (k) => { const v = get(k); return v ? v.split(',').map(Number) : null; };
  const num = (k, d = 0) => { const v = get(k); return v === null || v === undefined || v === '' ? d : Number(v); };
  const shape = [num('X-Visivo-Width', fallback.shape ? fallback.shape[0] : 0),
    num('X-Visivo-Height', fallback.shape ? fallback.shape[1] : 0), num('X-Visivo-Depth', fallback.shape ? fallback.shape[2] : 0)];
  const factors = list('X-Visivo-Downsample-Factors')
    || Array(3).fill(num('X-Visivo-Downsample-Factor', 1));
  let spacing = list('X-Visivo-Spacing');
  let origin = list('X-Visivo-Origin');
  if (!spacing || !origin) {
    // The velocity routes' grid (docs/backend-api/velocity.md): spacing per
    // axis box/(n-1) (unit without a box), times the block factor; origin at
    // the full grid's first point.
    const full = (list('X-Visivo-Full-Shape') || []).slice().reverse();   // nz,ny,nx -> nx,ny,nz
    const n = full.length === 3 ? full : shape.map((s, i) => s * factors[i]);
    const box = fallback.boxMpc;
    const cell = n.map((v) => (box ? box / Math.max(1, v - 1) : 1));
    spacing = spacing || cell.map((c, i) => c * factors[i]);
    origin = origin || n.map((v) => (box ? -box / 2 : -(v - 1) / 2));
  }
  let bounds = list('X-Visivo-Bounds');
  if (!bounds) {
    bounds = [];
    for (let i = 0; i < 3; i++) bounds.push(origin[i], origin[i] + spacing[i] * Math.max(0, shape[i] - 1));
  }
  return {
    points: num('X-Visivo-Line-Points'), lines: num('X-Visivo-Lines'), arrows: num('X-Visivo-Arrows'),
    shape, factors, spacing, origin, bounds,
    cell: num('X-Visivo-Glyph-Cell', Math.min(...spacing)),
    speedMin: num('X-Visivo-Full-Speed-Min'), speedMax: num('X-Visivo-Full-Speed-Max', 1),
    seeds: num('X-Visivo-Seeds'), seed: num('X-Visivo-Seed'),
    unit: get('X-Visivo-Unit') || '', frame: get('X-Visivo-Frame') || '',
    // The box the lines were traced with (0: grid units); without the header,
    // the one asked for.
    boxMpc: get('X-Visivo-Box-Mpc') !== null && get('X-Visivo-Box-Mpc') !== undefined
      ? num('X-Visivo-Box-Mpc') : (fallback.boxMpc || 0),
  };
}

/** The body's arrays (app/velocity_lines.py FlowLines.encode). */
export function linesArrays(buf, info) {
  let o = 0;
  const take = (Ctor, n) => { const a = new Ctor(buf, o, n); o += n * Ctor.BYTES_PER_ELEMENT; return a; };
  const points = take(Float32Array, 3 * info.points);
  const speed = take(Float32Array, info.points);
  const offsets = take(Uint32Array, info.lines + 1);
  const arrowPos = take(Float32Array, 3 * info.arrows);
  const arrowVec = take(Float32Array, 3 * info.arrows);
  if (o !== buf.byteLength) throw new Error(`lines_bin: ${buf.byteLength} bytes, ${o} expected`);
  return { points, speed, offsets, arrowPos, arrowVec };
}

/** Index pairs (GL LINES) joining consecutive points of each streamline. */
export function lineIndices(offsets) {
  let n = 0;
  for (let l = 0; l + 1 < offsets.length; l++) n += Math.max(0, offsets[l + 1] - offsets[l] - 1);
  const out = new Uint32Array(2 * n);
  let k = 0;
  for (let l = 0; l + 1 < offsets.length; l++) {
    for (let i = offsets[l]; i + 1 < offsets[l + 1]; i++) { out[k++] = i; out[k++] = i + 1; }
  }
  return out;
}

/**
 * Arrows as line vertices (x, y, z, speed per vertex): a shaft along the
 * vector, of length |v| x scale (the desktop's glyph: scale = user x 3 x cell
 * / full speed max), and a head of two strokes, 0.35 of the length, in the
 * plane of the vector and a fixed perpendicular.
 */
export function arrowVertices(pos, vec, scale) {
  const n = pos.length / 3;
  const out = new Float32Array(n * 6 * 4);
  let k = 0;
  const put = (x, y, z, s) => { out[k++] = x; out[k++] = y; out[k++] = z; out[k++] = s; };
  for (let i = 0; i < n; i++) {
    const px = pos[3 * i], py = pos[3 * i + 1], pz = pos[3 * i + 2];
    const vx = vec[3 * i], vy = vec[3 * i + 1], vz = vec[3 * i + 2];
    const sp = Math.hypot(vx, vy, vz);
    const tx = px + vx * scale, ty = py + vy * scale, tz = pz + vz * scale;
    // A perpendicular: v x (the axis least aligned with v).
    const ax = Math.abs(vx) <= Math.abs(vy) && Math.abs(vx) <= Math.abs(vz) ? [1, 0, 0]
      : Math.abs(vy) <= Math.abs(vz) ? [0, 1, 0] : [0, 0, 1];
    let qx = vy * ax[2] - vz * ax[1], qy = vz * ax[0] - vx * ax[2], qz = vx * ax[1] - vy * ax[0];
    const ql = Math.hypot(qx, qy, qz) || 1;
    const len = sp * scale;
    qx *= (0.12 * len) / ql; qy *= (0.12 * len) / ql; qz *= (0.12 * len) / ql;
    const bx = tx - vx * scale * 0.35, by = ty - vy * scale * 0.35, bz = tz - vz * scale * 0.35;
    put(px, py, pz, sp); put(tx, ty, tz, sp);
    put(tx, ty, tz, sp); put(bx + qx, by + qy, bz + qz, sp);
    put(tx, ty, tz, sp); put(bx - qx, by - qy, bz - qz, sp);
  }
  return out;
}

/** Interleaves points (3 per vertex) and speeds into (x, y, z, speed) vertices. */
export function streamVertices(points, speed) {
  const n = speed.length;
  const out = new Float32Array(4 * n);
  for (let i = 0; i < n; i++) {
    out[4 * i] = points[3 * i]; out[4 * i + 1] = points[3 * i + 1]; out[4 * i + 2] = points[3 * i + 2];
    out[4 * i + 3] = speed[i];
  }
  return out;
}

// ── GL ─────────────────────────────────────────────────────────────────────

const VS = `#version 300 es
precision highp float;
in vec4 aV;            // x, y, z (field units), speed
uniform mat4 uMVP;
uniform vec3 uCentre;
uniform float uInvSide;
uniform vec3 uLo;      // the box's lower corner and sides (field units): box fractions for the crop
uniform vec3 uSpan;
out float vS;          // the speed, interpolated unclamped: the colour window is applied per fragment
out vec3 vB;
void main() {
  gl_Position = uMVP * vec4((aV.xyz - uCentre) * uInvSide, 1.0);
  vS = aV.w;
  vB = (aV.xyz - uLo) / max(uSpan, vec3(1e-30));
}`;

const FS = `#version 300 es
precision mediump float;
in highp float vS;
in highp vec3 vB;
uniform highp float uS0;  // the colour window over the speed (#257: narrower than the speed range)
uniform highp float uS1;
uniform sampler2D uLut;
uniform float uAlpha;
uniform int uClipOn;
uniform highp vec3 uClipMin;  // the spatial crop (#257), box fractions
uniform highp vec3 uClipMax;
out vec4 outColor;
void main() {
  if (uClipOn == 1 && (any(lessThan(vB, uClipMin)) || any(greaterThan(vB, uClipMax)))) discard;
  float t = clamp((vS - uS0) / max(1e-12, uS1 - uS0), 0.0, 1.0);
  vec3 c = texture(uLut, vec2(t, 0.5)).rgb;
  outColor = vec4(c * uAlpha, uAlpha);
}`;

function compile(gl) {
  const p = gl.createProgram();
  for (const [type, src] of [[gl.VERTEX_SHADER, VS], [gl.FRAGMENT_SHADER, FS]]) {
    const s = gl.createShader(type);
    gl.shaderSource(s, src);
    gl.compileShader(s);
    if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) throw new Error(`flow shader: ${gl.getShaderInfoLog(s)}`);
    gl.attachShader(p, s);
  }
  gl.bindAttribLocation(p, 0, 'aV');
  gl.linkProgram(p);
  if (!gl.getProgramParameter(p, gl.LINK_STATUS)) throw new Error(`flow link: ${gl.getProgramInfoLog(p)}`);
  const u = {};
  const n = gl.getProgramParameter(p, gl.ACTIVE_UNIFORMS);
  for (let i = 0; i < n; i++) { const info = gl.getActiveUniform(p, i); u[info.name] = gl.getUniformLocation(p, info.name); }
  return { p, u };
}

/** Streamlines (indexed lines) and arrows (line pairs) on the GPU. */
export class FlowLines {
  constructor(gl, lutTexture) {
    this.gl = gl;
    this.lut = lutTexture;
    this.prog = compile(gl);
    const make = () => ({ vao: gl.createVertexArray(), buf: gl.createBuffer(), idx: gl.createBuffer(), count: 0 });
    this.stream = make();
    this.arrows = make();
    this.bytes = 0;
  }

  _set(target, vertices, indices) {
    const gl = this.gl;
    gl.bindVertexArray(target.vao);
    gl.bindBuffer(gl.ARRAY_BUFFER, target.buf);
    gl.bufferData(gl.ARRAY_BUFFER, vertices, gl.STATIC_DRAW);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 4, gl.FLOAT, false, 16, 0);
    if (indices) {
      gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, target.idx);
      gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, indices, gl.STATIC_DRAW);
      target.count = indices.length;
      target.indexed = true;
    } else {
      target.count = vertices.length / 4;
      target.indexed = false;
    }
    gl.bindVertexArray(null);
    return vertices.byteLength + (indices ? indices.byteLength : 0);
  }

  setStreamlines(points, speed, offsets) {
    this.streamBytes = this._set(this.stream, streamVertices(points, speed), lineIndices(offsets));
  }

  setArrows(pos, vec, scale) {
    this.arrowBytes = this._set(this.arrows, arrowVertices(pos, vec, scale), null);
  }

  get bytesOnGpu() { return (this.streamBytes || 0) + (this.arrowBytes || 0); }

  /** One eye: `p` {box (dataToModel: centre, side, lo, hi), speedMin, speedMax (the
   *  colour range), stream, arrows (booleans), clip ({min, max} box fractions, or none)}. */
  draw(view, model, p) {
    const gl = this.gl;
    const { u } = this.prog;
    gl.useProgram(this.prog.p);
    gl.uniformMatrix4fv(u.uMVP, false, M.multiply(view.proj, M.multiply(view.view, model)));
    gl.uniform3fv(u.uCentre, p.box.centre);
    gl.uniform1f(u.uInvSide, 1 / p.box.side);
    gl.uniform1f(u.uS0, p.speedMin);
    gl.uniform1f(u.uS1, p.speedMax);
    const lo = p.box.lo || [0, 0, 0], hi = p.box.hi || [1, 1, 1];
    gl.uniform3fv(u.uLo, lo);
    gl.uniform3fv(u.uSpan, [0, 1, 2].map((i) => hi[i] - lo[i]));
    const cl = p.clip;
    const cropped = !!cl && [0, 1, 2].some((i) => cl.min[i] > 0 || cl.max[i] < 1);
    gl.uniform1i(u.uClipOn, cropped ? 1 : 0);
    gl.uniform3fv(u.uClipMin, cropped ? cl.min : [0, 0, 0]);
    gl.uniform3fv(u.uClipMax, cropped ? cl.max : [1, 1, 1]);
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, this.lut);
    gl.uniform1i(u.uLut, 0);
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA);
    gl.enable(gl.DEPTH_TEST);
    gl.depthFunc(gl.LEQUAL);
    for (const [on, t, a] of [[p.stream, this.stream, 1], [p.arrows, this.arrows, 0.9]]) {
      if (!on || !t.count) continue;
      gl.uniform1f(u.uAlpha, a);
      gl.bindVertexArray(t.vao);
      if (t.indexed) gl.drawElements(gl.LINES, t.count, gl.UNSIGNED_INT, 0);
      else gl.drawArrays(gl.LINES, 0, t.count);
    }
    gl.bindVertexArray(null);
    gl.disable(gl.DEPTH_TEST);
  }
}

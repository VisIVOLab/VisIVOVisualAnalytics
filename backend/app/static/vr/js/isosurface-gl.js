// Isosurfaces on the GPU (issue #370): one vertex, one normal and one index
// buffer, each sized once for MAX_SURFACES surfaces of SURFACE_TRIANGLES
// (isosurface.js), filled with bufferSubData as surfaces arrive (no buffer is
// allocated in VR; no 3-D texture is used at all), and drawn in the cube's
// frame after the volume: opaque surfaces with depth, translucent ones over
// them without depth writes, two-sided, lit from the eye. The crop box clips
// them as it clips the volume, and the free clipping plane (issue #369,
// clip-plane.js CLIP_GLSL's clipKept) cuts them open. Nothing is allocated
// per frame.

import * as M from './mat4.js';
import { MAX_VERTICES, MAX_TRIANGLES, POS_BYTES, NRM_BYTES } from './isosurface.js';
import { CLIP_GLSL, setClipUniforms } from './clip-plane.js';

// All in highp: on the Quest's GPU mediump is fp16 (normals divided by the
// cube's extent would flush to zero, as the controller models' did, #232).
const VS = `#version 300 es
precision highp float;
in vec3 aPos;          // 0..1 over the surface's bounding box (uint16)
in vec3 aNormal;       // -1..1, box coordinates (int8)
uniform mat4 uMVP;
uniform mat4 uMV;
uniform mat3 uNormalMat;   // box normal -> view normal (cofactors, unnormalised)
uniform vec3 uLo, uSpan, uExtent;
out vec3 vBox;
out vec3 vNormal;
out vec3 vView;
void main() {
  vec3 b = uLo + aPos * uSpan;
  vBox = b;
  vec4 p = vec4((b - 0.5) * uExtent, 1.0);
  vView = (uMV * p).xyz;
  vNormal = uNormalMat * aNormal;
  gl_Position = uMVP * p;
}`;

// The clipping plane's half-space or slab (#369) on box coordinates.
const FS = `#version 300 es
precision highp float;
in vec3 vBox;
in vec3 vNormal;
in vec3 vView;
uniform vec3 uBoxMin, uBoxMax;
uniform vec4 uColor;   // rgb, opacity
out vec4 outColor;
${CLIP_GLSL}
void main() {
  if (any(lessThan(vBox, uBoxMin)) || any(greaterThan(vBox, uBoxMax))) discard;
  if (!clipKept(vBox)) discard;
  float l2 = dot(vNormal, vNormal);
  vec3 n = l2 > 0.0 ? vNormal * inversesqrt(l2) : vec3(0.0, 0.0, 1.0);
  vec3 v = normalize(-vView);
  float facing = abs(dot(n, v));                       // both sides lit
  vec3 key = normalize(vec3(0.35, 0.8, 0.45));         // above the eye, in view space
  float diffuse = 0.3 + 0.5 * facing + 0.2 * abs(dot(n, key));
  vec3 h = normalize(key + v);
  float spec = 0.25 * pow(abs(dot(n, h)), 32.0);
  vec3 c = min(uColor.rgb * diffuse + spec, vec3(1.0));
  outColor = vec4(c * uColor.a, uColor.a);
}`;

function compile(gl, vs, fs) {
  const sh = (type, src) => {
    const s = gl.createShader(type);
    gl.shaderSource(s, src);
    gl.compileShader(s);
    if (!gl.getShaderParameter(s, gl.COMPILE_STATUS) && !gl.isContextLost()) throw new Error(`isosurface shader: ${gl.getShaderInfoLog(s)}`);
    return s;
  };
  const p = gl.createProgram();
  gl.attachShader(p, sh(gl.VERTEX_SHADER, vs));
  gl.attachShader(p, sh(gl.FRAGMENT_SHADER, fs));
  gl.bindAttribLocation(p, 0, 'aPos');
  gl.bindAttribLocation(p, 1, 'aNormal');
  gl.linkProgram(p);
  if (!gl.getProgramParameter(p, gl.LINK_STATUS) && !gl.isContextLost()) throw new Error(`isosurface program: ${gl.getProgramInfoLog(p)}`);
  const u = {};
  for (const name of ['uMVP', 'uMV', 'uNormalMat', 'uLo', 'uSpan', 'uExtent', 'uBoxMin', 'uBoxMax', 'uColor',
    'uClipMode', 'uClipPlane', 'uClipHalf', 'uSectionA']) u[name] = gl.getUniformLocation(p, name);
  return { p, u };
}

/** The cofactor matrix of the 3×3 linear part of `mv` × diag(extent),
 *  column-major into `out`: its inverse transpose up to a scale (normals
 *  are normalised in the shader; its sign does not matter, both sides lit). */
export function normalMatrix(out, mv, extent) {
  const a0x = mv[0] * extent[0], a0y = mv[1] * extent[0], a0z = mv[2] * extent[0];
  const a1x = mv[4] * extent[1], a1y = mv[5] * extent[1], a1z = mv[6] * extent[1];
  const a2x = mv[8] * extent[2], a2y = mv[9] * extent[2], a2z = mv[10] * extent[2];
  // Columns: a1 × a2, a2 × a0, a0 × a1.
  out[0] = a1y * a2z - a1z * a2y; out[1] = a1z * a2x - a1x * a2z; out[2] = a1x * a2y - a1y * a2x;
  out[3] = a2y * a0z - a2z * a0y; out[4] = a2z * a0x - a2x * a0z; out[5] = a2x * a0y - a2y * a0x;
  out[6] = a0y * a1z - a0z * a1y; out[7] = a0z * a1x - a0x * a1z; out[8] = a0x * a1y - a0y * a1x;
  return out;
}

export class IsoSurfaceGL {
  constructor(gl) {
    this.gl = gl;
    this.prog = null;
    this.vao = null;
    this.buf = null;            // {pos, nrm, idx}
    this.slots = [];            // [{at: {vOff, tOff}, mesh, rgb, opacity, shown}]
    this.draws = 0;             // surfaces drawn in the last call (both passes)
    this.triangles = 0;         // triangles drawn in the last call
    this._mv = new Float32Array(16);
    this._mvp = new Float32Array(16);
    this._vp = new Float32Array(16);
    this._nm = new Float32Array(9);
    this._span = new Float32Array(3);
    this._color = new Float32Array(4);
  }

  /** The GPU bytes of the buffers (0 before the first surface). */
  get bytes() { return this.buf ? MAX_VERTICES * (POS_BYTES + NRM_BYTES) + MAX_TRIANGLES * 12 : 0; }

  /** The buffers, allocated once at their full size (the page calls this
   *  outside VR when a cube is shown, so a session never allocates). */
  ensure() {
    if (this.buf) return;
    const gl = this.gl;
    if (!this.prog) this.prog = compile(gl, VS, FS);
    const make = (target, bytes) => {
      const b = gl.createBuffer();
      gl.bindBuffer(target, b);
      gl.bufferData(target, bytes, gl.STATIC_DRAW);
      return b;
    };
    this.vao = gl.createVertexArray();
    gl.bindVertexArray(this.vao);
    this.buf = {
      pos: make(gl.ARRAY_BUFFER, MAX_VERTICES * POS_BYTES),
      nrm: make(gl.ARRAY_BUFFER, MAX_VERTICES * NRM_BYTES),
      idx: make(gl.ELEMENT_ARRAY_BUFFER, MAX_TRIANGLES * 12),
    };
    gl.enableVertexAttribArray(0);
    gl.enableVertexAttribArray(1);
    gl.bindVertexArray(null);
    gl.bindBuffer(gl.ARRAY_BUFFER, null);
  }

  /** A decoded mesh (isosurface.js decodeMesh) written at `at` {vOff, tOff}. */
  upload(mesh, at) {
    this.ensure();
    const gl = this.gl;
    if (!mesh.triangles) return;
    gl.bindBuffer(gl.ARRAY_BUFFER, this.buf.pos);
    gl.bufferSubData(gl.ARRAY_BUFFER, at.vOff * POS_BYTES, mesh.positions);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.buf.nrm);
    gl.bufferSubData(gl.ARRAY_BUFFER, at.vOff * NRM_BYTES, mesh.normals);
    gl.bindBuffer(gl.ARRAY_BUFFER, null);
    // The element buffer is bound through the VAO (WebGL2 keeps it there).
    gl.bindVertexArray(this.vao);
    gl.bufferSubData(gl.ELEMENT_ARRAY_BUFFER, at.tOff * 12, mesh.indices);
    gl.bindVertexArray(null);
  }

  /** What is drawn: [{at, mesh, rgb, opacity, shown}] (already uploaded). */
  setSlots(slots) { this.slots = slots; }

  /** After a lost context: the program and the buffers again (the page then
   *  uploads its surfaces again). */
  restore() {
    this.prog = null;
    this.vao = null;
    this.buf = null;
  }

  _drawSlot(s) {
    const gl = this.gl, { u } = this.prog, m = s.mesh;
    gl.vertexAttribPointer(0, 3, gl.UNSIGNED_SHORT, true, POS_BYTES, s.at.vOff * POS_BYTES);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.buf.nrm);
    gl.vertexAttribPointer(1, 3, gl.BYTE, true, NRM_BYTES, s.at.vOff * NRM_BYTES);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.buf.pos);
    gl.uniform3f(u.uLo, m.lo[0], m.lo[1], m.lo[2]);
    gl.uniform3f(u.uSpan, m.hi[0] - m.lo[0], m.hi[1] - m.lo[1], m.hi[2] - m.lo[2]);
    gl.uniform4f(u.uColor, s.rgb[0], s.rgb[1], s.rgb[2], s.opacity);
    gl.drawElements(gl.TRIANGLES, 3 * m.triangles, gl.UNSIGNED_INT, s.at.tOff * 12);
    this.draws++;
    this.triangles += m.triangles;
  }

  /**
   * The surfaces for one eye: `view` {proj, view}, `model` the cube's,
   * `p` {extent, boxMin, boxMax} (the crop, box coordinates), `p.clip` the
   * clipping plane's shader numbers (clip-plane.js shaderParams; null or
   * absent: none). Leaves depth
   * testing off and depth writes on, as the page's other draws do; blending
   * premultiplied (ONE, ONE_MINUS_SRC_ALPHA).
   */
  draw(view, model, p) {
    this.draws = 0;
    this.triangles = 0;
    const slots = this.slots;
    let any = false;
    for (let i = 0; i < slots.length; i++) if (slots[i].shown && slots[i].mesh.triangles) { any = true; break; }
    if (!any || !this.buf) return;
    const gl = this.gl, { u } = this.prog;
    gl.useProgram(this.prog.p);
    M.multiplyInto(this._mv, view.view, model);
    M.multiplyInto(this._mvp, view.proj, this._mv);
    normalMatrix(this._nm, this._mv, p.extent);
    gl.uniformMatrix4fv(u.uMVP, false, this._mvp);
    gl.uniformMatrix4fv(u.uMV, false, this._mv);
    gl.uniformMatrix3fv(u.uNormalMat, false, this._nm);
    gl.uniform3f(u.uExtent, p.extent[0], p.extent[1], p.extent[2]);
    gl.uniform3f(u.uBoxMin, p.boxMin[0], p.boxMin[1], p.boxMin[2]);
    gl.uniform3f(u.uBoxMax, p.boxMax[0], p.boxMax[1], p.boxMax[2]);
    setClipUniforms(gl, u, p.clip || null);
    gl.bindVertexArray(this.vao);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.buf.pos);
    gl.disable(gl.CULL_FACE);
    gl.enable(gl.DEPTH_TEST);
    gl.depthFunc(gl.LEQUAL);
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA);
    // Opaque surfaces first, with depth: they hide what is behind them.
    gl.depthMask(true);
    for (let i = 0; i < slots.length; i++) {
      const s = slots[i];
      if (s.shown && s.mesh.triangles && s.opacity >= 0.995) this._drawSlot(s);
    }
    // Translucent ones over them, without depth writes: every layer of a
    // surface shows (one colour: its layers blend alike in any order).
    gl.depthMask(false);
    for (let i = 0; i < slots.length; i++) {
      const s = slots[i];
      if (s.shown && s.mesh.triangles && s.opacity < 0.995 && s.opacity > 0.004) this._drawSlot(s);
    }
    gl.depthMask(true);
    gl.disable(gl.DEPTH_TEST);
    gl.bindVertexArray(null);
    gl.bindBuffer(gl.ARRAY_BUFFER, null);
  }
}

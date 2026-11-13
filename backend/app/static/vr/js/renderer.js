// WebGL2 ray marcher of the VR page: one 3-D texture (R8 or R16F), an
// empty-space-skipping grid, a colour map, front-to-back compositing with
// early termination, a clipping box, and the lines, labels and panels drawn
// around it. One draw per eye; the same code serves the desktop (no XR) mode.

import * as M from './mat4.js';
import { colormapTable } from './colormaps.js';
import { buildMacrocells } from './volume.js';
import { labelVertices, LABEL_STRIDE } from './labels.js';
import { MESH_STRIDE } from './models.js';

const MAX_LOOP = 4096;

const VOL_VS = `#version 300 es
precision highp float;
in vec3 aPos;
uniform mat4 uMVP;
uniform vec3 uExtent, uBoxMin, uBoxMax;
out vec3 vTex;
void main() {
  vec3 t = mix(uBoxMin, uBoxMax, aPos);
  vTex = t;
  gl_Position = uMVP * vec4((t - 0.5) * uExtent, 1.0);
}`;

const VOL_FS = `#version 300 es
precision highp float;
precision highp int;
precision highp sampler3D;
in vec3 vTex;
out vec4 outColor;
uniform sampler3D uVol;
uniform sampler3D uMacro;
uniform sampler2D uLut;
uniform vec3 uCamTex, uBoxMin, uBoxMax, uVoxDims, uCellDims, uTexScale;
uniform float uCell, uThr, uCmin, uCmax, uGamma, uOpacity, uSpv;
uniform int uEnc, uMaxSteps, uMode, uSkip;
// A selection on the drawn grid (issue #171): 255 where a reduced voxel holds a
// selected full-resolution voxel, over the box [uMaskOrigin, uMaskOrigin + uMaskDims).
uniform highp sampler3D uMask;
uniform int uMaskOn;
// Only the selection (issue #170): drawn over a refined frame from the backend,
// which has no selection in it.
uniform int uMaskOnly;
uniform ivec3 uMaskOrigin, uMaskDims;
const vec3 MASK_COLOR = vec3(0.25, 1.0, 0.45);

float maskAt(vec3 p, vec3 vox) {
  ivec3 iv = ivec3(floor(p * vox)) - uMaskOrigin;
  if (any(lessThan(iv, ivec3(0))) || any(greaterThanEqual(iv, uMaskDims))) return 0.0;
  return texelFetch(uMask, iv, 0).r;
}

bool cellMeetsMask(vec3 cf) {
  ivec3 lo = ivec3(cf * uCell), hi = ivec3((cf + 1.0) * uCell);
  return all(lessThan(lo, uMaskOrigin + uMaskDims)) && all(greaterThan(hi, uMaskOrigin));
}

// p is a box coordinate (0..1 over the cube's full-resolution extent); the
// texture covers whole blocks, so a partial last block makes the texture a
// little longer than the box: uTexScale = full pixels / (blocks * factor).
float normAt(vec3 p) {
  float s = texture(uVol, p * uTexScale).r;
  return uEnc == 0 ? (s * 255.0 - 1.0) / 254.0 : s;
}

void main() {
  vec3 o = uCamTex;
  vec3 d = vTex - uCamTex;
  d = mix(d, vec3(1e-7), lessThan(abs(d), vec3(1e-7)));
  vec3 inv = 1.0 / d;
  vec3 t1 = (uBoxMin - o) * inv, t2 = (uBoxMax - o) * inv;
  vec3 tlo = min(t1, t2), thi = max(t1, t2);
  float tn = max(max(tlo.x, tlo.y), max(tlo.z, 0.0));
  float tf = min(min(thi.x, thi.y), thi.z);
  if (tf <= tn) discard;
  vec3 vox = uVoxDims * uTexScale;                  // reduced voxels per box unit
  float dt = 1.0 / (length(d * vox) * uSpv);         // one step = 1/uSpv voxels
  float stepVox = 1.0 / uSpv;
  float jit = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);
  float t = tn + dt * jit;
  vec4 acc = vec4(0.0);
  float best = -1.0;
  float bestMask = 0.0;
  int i = 0;
  for (; i < ${MAX_LOOP}; i++) {
    if (i >= uMaxSteps || t > tf) break;
    vec3 p = o + d * t;
    if (uSkip == 1) {
      vec3 cf = clamp(floor(p * vox / uCell), vec3(0.0), uCellDims - 1.0);
      float m = texelFetch(uMacro, ivec3(cf), 0).r;
      float need = uMaskOnly == 1 ? 2.0 : (uMode == 1 ? max(uThr, best) : uThr);
      // A selected voxel is drawn below the threshold too (the lasso grows into
      // the faint margin), so the cells it meets are never skipped.
      if (m < need && !(uMaskOn == 1 && cellMeetsMask(cf))) {
        vec3 a = (cf * uCell / vox - o) * inv;
        vec3 b = ((cf + 1.0) * uCell / vox - o) * inv;
        float texit = min(min(max(a.x, b.x), max(a.y, b.y)), max(a.z, b.z));
        t = max(t, texit) + 0.01 * dt;
        continue;
      }
    }
    float v = normAt(p);
    float sel = uMaskOn == 1 ? maskAt(p, vox) : 0.0;
    if (uMode == 1) {
      if (v > best) { best = v; bestMask = sel; }
    } else if ((uMaskOnly == 0 && v >= uThr) || sel > 0.5) {
      float x = clamp((v - uCmin) / max(uCmax - uCmin, 1e-6), 0.0, 1.0);
      x = pow(x, uGamma);
      vec3 col = texture(uLut, vec2(x, 0.5)).rgb;
      float a = 1.0 - exp(-uOpacity * (0.04 + x) * stepVox);
      if (sel > 0.5) {
        col = mix(col, MASK_COLOR, 0.7);
        a = max(a, 1.0 - exp(-uOpacity * 0.25 * stepVox));
      }
      acc.rgb += (1.0 - acc.a) * a * col;
      acc.a += (1.0 - acc.a) * a;
      if (acc.a > 0.98) break;
    }
    t += dt;
  }
  if (uMode == 2) {            // steps taken by this ray, for measurements
    outColor = vec4(vec3(float(i) / float(${MAX_LOOP})), 1.0);
    return;
  }
  if (uMode == 1) {
    if (best < uThr) discard;
    float x = pow(clamp((best - uCmin) / max(uCmax - uCmin, 1e-6), 0.0, 1.0), uGamma);
    float a = clamp(0.25 + 0.75 * x, 0.0, 1.0);
    vec3 col = texture(uLut, vec2(x, 0.5)).rgb;
    if (bestMask > 0.5) col = mix(col, MASK_COLOR, 0.7);
    outColor = vec4(col * a, a);
    return;
  }
  if (acc.a < 0.002) discard;
  outColor = acc;
}`;

const LINE_VS = `#version 300 es
precision highp float;
in vec3 aPos;
in vec4 aColor;
uniform mat4 uMVP;
out vec4 vColor;
void main() { vColor = aColor; gl_Position = uMVP * vec4(aPos, 1.0); }`;

const LINE_FS = `#version 300 es
precision mediump float;
in vec4 vColor;
out vec4 outColor;
void main() { outColor = vec4(vColor.rgb * vColor.a, vColor.a); }`;

// Text fixed to the cube (issue #226): each label is a quad in its own face
// of the cube's model space (labels.js), not a billboard turned to the eye.
// Seen from behind its face, its texture is mirrored so it still reads.
const LABEL_VS = `#version 300 es
precision highp float;
in vec3 aCenter;
in vec2 aOffset;
in vec2 aUV;
in vec3 aRight;
in vec3 aUp;
in vec2 aURange;
uniform mat4 uMVP;
uniform vec3 uEye;       // the eye in model space
out vec2 vUV;
void main() {
  vec3 n = cross(aRight, aUp);
  bool back = dot(uEye - aCenter, n) < 0.0;
  vUV = vec2(back ? aURange.x + aURange.y - aUV.x : aUV.x, aUV.y);
  gl_Position = uMVP * vec4(aCenter + aRight * aOffset.x + aUp * aOffset.y, 1.0);
}`;

const TEX_FS = `#version 300 es
precision mediump float;
in vec2 vUV;
uniform sampler2D uTex;
out vec4 outColor;
void main() { vec4 c = texture(uTex, vUV); outColor = vec4(c.rgb * c.a, c.a); }`;

const QUAD_VS = `#version 300 es
precision highp float;
in vec2 aPos;
in vec2 aUV;
uniform mat4 uMVP;
uniform vec2 uSize;
out vec2 vUV;
void main() { vUV = aUV; gl_Position = uMVP * vec4(aPos * uSize, 0.0, 1.0); }`;

// Controllers and tracked hands (issue #226): solid triangles in world space,
// lit from above and from the eye, with depth among themselves.
const MESH_VS = `#version 300 es
precision highp float;
in vec3 aPos;
in vec3 aNormal;
in vec4 aColor;
uniform mat4 uVP;
uniform vec3 uEye;
out vec4 vColor;
void main() {
  vec3 n = normalize(aNormal);
  vec3 toEye = normalize(uEye - aPos);
  float l = 0.35 + 0.4 * abs(dot(n, toEye)) + 0.25 * max(dot(n, vec3(0.0, 1.0, 0.0)), 0.0);
  vColor = vec4(aColor.rgb * l, aColor.a);
  gl_Position = uVP * vec4(aPos, 1.0);
}`;

const MESH_FS = `#version 300 es
precision mediump float;
in vec4 vColor;
out vec4 outColor;
void main() { outColor = vec4(vColor.rgb * vColor.a, vColor.a); }`;

function compile(gl, vs, fs) {
  const p = gl.createProgram();
  for (const [type, src] of [[gl.VERTEX_SHADER, vs], [gl.FRAGMENT_SHADER, fs]]) {
    const s = gl.createShader(type);
    gl.shaderSource(s, src);
    gl.compileShader(s);
    if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) {
      throw new Error(`shader: ${gl.getShaderInfoLog(s)}`);
    }
    gl.attachShader(p, s);
  }
  gl.linkProgram(p);
  if (!gl.getProgramParameter(p, gl.LINK_STATUS)) throw new Error(`link: ${gl.getProgramInfoLog(p)}`);
  const u = {};
  const n = gl.getProgramParameter(p, gl.ACTIVE_UNIFORMS);
  for (let i = 0; i < n; i++) {
    const info = gl.getActiveUniform(p, i);
    u[info.name] = gl.getUniformLocation(p, info.name);
  }
  return { p, u };
}

// Unit cube, 12 triangles, outward winding (counter-clockwise from outside).
const CUBE = new Float32Array([
  0, 0, 1, 1, 0, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 0, 1, 1, // +z
  1, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0, // -z
  1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 1, 1, 1, 0, 1, 1, 1, // +x
  0, 0, 0, 0, 0, 1, 0, 1, 1, 0, 0, 0, 0, 1, 1, 0, 1, 0, // -x
  0, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 0, // +y
  0, 0, 0, 1, 0, 0, 1, 0, 1, 0, 0, 0, 1, 0, 1, 0, 0, 1, // -y
]);

/**
 * The mask texture's storage for a selection of `dims` reduced voxels: the
 * current capacity grown to hold it, each side rounded up to a multiple of
 * 32 and never past the cube's own grid `grid` (a selection cannot be larger
 * than the cube). Pure: tested in backend/tests/vr/alloc.test.mjs.
 */
export function maskCapacityFor(dims, cap, grid) {
  return [0, 1, 2].map((i) => {
    const need = Math.max(dims[i], cap ? cap[i] : 0, 1);
    const round = Math.ceil(need / 32) * 32;
    const limit = grid && grid[i] ? Math.max(grid[i], dims[i]) : round;
    return Math.min(round, limit);
  });
}

export class Renderer {
  constructor(gl) {
    this.gl = gl;
    this._panels = new Set();      // createPanel() handles, re-made by restore()
    this._cmap = 'viridis';
    this._volume = null;           // the last setVolume() input, for restore()
    this._maskInput = null;
    this._lineSegs = {};           // target name -> the last segments
    this._labels = null;
    this._labelCanvas = null;      // one canvas for the label atlas, reused (issue #226)
    this._init();
  }

  /** The GL objects: made at construction and again after a context loss. */
  _init() {
    const gl = this.gl;
    this.vol = compile(gl, VOL_VS, VOL_FS);
    this.line = compile(gl, LINE_VS, LINE_FS);
    this.label = compile(gl, LABEL_VS, TEX_FS);
    this.quad = compile(gl, QUAD_VS, TEX_FS);
    this.meshProg = compile(gl, MESH_VS, MESH_FS);
    this.maxTex3D = gl.getParameter(gl.MAX_3D_TEXTURE_SIZE);

    this.cubeVao = gl.createVertexArray();
    gl.bindVertexArray(this.cubeVao);
    const cb = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, cb);
    gl.bufferData(gl.ARRAY_BUFFER, CUBE, gl.STATIC_DRAW);
    const loc = gl.getAttribLocation(this.vol.p, 'aPos');
    gl.enableVertexAttribArray(loc);
    gl.vertexAttribPointer(loc, 3, gl.FLOAT, false, 0, 0);

    const lineVao = () => {
      const vao = gl.createVertexArray();
      const buf = gl.createBuffer();
      gl.bindVertexArray(vao);
      gl.bindBuffer(gl.ARRAY_BUFFER, buf);
      const lp = gl.getAttribLocation(this.line.p, 'aPos');
      const lc = gl.getAttribLocation(this.line.p, 'aColor');
      gl.enableVertexAttribArray(lp);
      gl.vertexAttribPointer(lp, 3, gl.FLOAT, false, 28, 0);
      gl.enableVertexAttribArray(lc);
      gl.vertexAttribPointer(lc, 4, gl.FLOAT, false, 28, 12);
      return { vao, buf, count: 0, capacity: 0 };
    };
    this.boxLines = lineVao();     // box, ticks: rebuilt when the clip box changes
    this.tmpLines = lineVao();     // controllers: rebuilt every frame
    this.selLines = lineVao();     // selections (model space): rebuilt when they change

    this.labelVao = gl.createVertexArray();
    this.labelBuf = gl.createBuffer();
    gl.bindVertexArray(this.labelVao);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.labelBuf);
    const LS = LABEL_STRIDE * 4;
    for (const [name, n, off] of [['aCenter', 3, 0], ['aOffset', 2, 3], ['aUV', 2, 5], ['aRight', 3, 7], ['aUp', 3, 10], ['aURange', 2, 13]]) {
      const loc2 = gl.getAttribLocation(this.label.p, name);
      if (loc2 < 0) continue;
      gl.enableVertexAttribArray(loc2);
      gl.vertexAttribPointer(loc2, n, gl.FLOAT, false, LS, off * 4);
    }
    this.labelCount = 0;
    this.labelTex = gl.createTexture();
    this.labelTexSize = null;      // [w, h] of its storage: re-specified only when it changes

    // Controllers and hands (issue #226): one world-space triangle list a frame.
    this.meshVao = gl.createVertexArray();
    this.meshBuf = gl.createBuffer();
    gl.bindVertexArray(this.meshVao);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.meshBuf);
    const MS = MESH_STRIDE * 4;
    for (const [name, n, off] of [['aPos', 3, 0], ['aNormal', 3, 3], ['aColor', 4, 6]]) {
      const loc3 = gl.getAttribLocation(this.meshProg.p, name);
      if (loc3 < 0) continue;
      gl.enableVertexAttribArray(loc3);
      gl.vertexAttribPointer(loc3, n, gl.FLOAT, false, MS, off * 4);
    }
    this.meshCount = 0;
    this.meshCapacity = 0;

    this.quadVao = gl.createVertexArray();
    gl.bindVertexArray(this.quadVao);
    const qb = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, qb);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([
      -0.5, -0.5, 0, 1, 0.5, -0.5, 1, 1, 0.5, 0.5, 1, 0, -0.5, -0.5, 0, 1, 0.5, 0.5, 1, 0, -0.5, 0.5, 0, 0,
    ]), gl.STATIC_DRAW);
    const q0 = gl.getAttribLocation(this.quad.p, 'aPos');
    const q1 = gl.getAttribLocation(this.quad.p, 'aUV');
    gl.enableVertexAttribArray(q0); gl.vertexAttribPointer(q0, 2, gl.FLOAT, false, 16, 0);
    gl.enableVertexAttribArray(q1); gl.vertexAttribPointer(q1, 2, gl.FLOAT, false, 16, 8);
    gl.bindVertexArray(null);

    this.lut = gl.createTexture();
    this.setColormap(this._cmap);
    this.volTex = null;
    this.macroTex = null;
    // The selection overlay: a 1-voxel placeholder until a mask is set, so the
    // sampler is never unbound.
    this.maskTex = gl.createTexture();
    this.mask = null;
    this.maskCapacity = null;      // the dims of the mask texture's storage (issue #226)
    this._uploadMask(new Uint8Array(1), [1, 1, 1]);
    this.timer = gl.getExtension('EXT_disjoint_timer_query_webgl2');
    this.queries = [];
    this.activeQuery = null;
    this.gpuMs = -1;
  }

  /**
   * After a lost WebGL context is restored (epic #166): every GL object is
   * made again and what was on show uploaded again from what the page still
   * holds (the cube's voxels, the mask, the lines, the labels, the panels'
   * canvases). Throws when the context cannot make them (still lost).
   */
  restore() {
    this.textureBytes = 0;
    this._init();
    if (this._volume) this.setVolume(this._volume, this._volume.cell);
    if (this._maskInput) this.setMask(this._maskInput);
    for (const [name, segs] of Object.entries(this._lineSegs)) if (this[name]) this.setLines(segs, this[name]);
    if (this._labels) this.setLabels(this._labels);
    for (const h of this._panels) {
      h.tex = this._panelTexture();
      h.size = null;
      if (h.canvas) this.updatePanel(h, h.canvas);
    }
  }

  /**
   * What the renderer holds on the GPU now (issue #226): textures and their
   * bytes by kind, as allocated (not as the driver may pad them).
   */
  gpuStats() {
    const panelBytes = [...this._panels].reduce((a, h) => a + (h.size ? h.size[0] * h.size[1] * 4 : 0), 0);
    const mc = this.maskCapacity;
    const maskBytes = mc ? mc[0] * mc[1] * mc[2] : 0;
    const labelBytes = this.labelTexSize ? this.labelTexSize[0] * this.labelTexSize[1] * 4 : 0;
    const vol = this.textureBytes || 0;
    const macro = this.macroDims ? this.macroDims[0] * this.macroDims[1] * this.macroDims[2] * 4 : 0;
    return { panels: this._panels.size, panelBytes, maskBytes, labelBytes, volumeBytes: vol, macroBytes: macro,
      totalBytes: panelBytes + maskBytes + labelBytes + vol + macro + 256 * 4 };
  }

  setColormap(name) {
    const gl = this.gl;
    this._cmap = name;
    gl.bindTexture(gl.TEXTURE_2D, this.lut);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, 256, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE, colormapTable(name));
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
  }

  /** Upload the reduced cube; returns the timings and the texture size. */
  setVolume({ data, dims, encoding }, cell = 8) {
    const gl = this.gl;
    const [w, h, d] = dims;
    if (Math.max(w, h, d) > this.maxTex3D) {
      throw new Error(`${w} x ${h} x ${d} exceeds this GPU's 3-D texture limit (${this.maxTex3D}): use a larger binning factor.`);
    }
    this._volume = { data, dims, encoding, cell };
    const t0 = performance.now();
    if (this.volTex) gl.deleteTexture(this.volTex);
    this.volTex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_3D, this.volTex);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
    if (encoding === 'uint8') {
      gl.texImage3D(gl.TEXTURE_3D, 0, gl.R8, w, h, d, 0, gl.RED, gl.UNSIGNED_BYTE, data);
    } else {
      gl.texImage3D(gl.TEXTURE_3D, 0, gl.R16F, w, h, d, 0, gl.RED, gl.HALF_FLOAT, data);
    }
    for (const [k, v] of [[gl.TEXTURE_MIN_FILTER, gl.LINEAR], [gl.TEXTURE_MAG_FILTER, gl.LINEAR],
      [gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE], [gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE], [gl.TEXTURE_WRAP_R, gl.CLAMP_TO_EDGE]]) {
      gl.texParameteri(gl.TEXTURE_3D, k, v);
    }
    const t1 = performance.now();
    const macro = buildMacrocells(data, dims, encoding, cell);
    const t2 = performance.now();
    if (this.macroTex) gl.deleteTexture(this.macroTex);
    this.macroTex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_3D, this.macroTex);
    gl.texImage3D(gl.TEXTURE_3D, 0, gl.R32F, macro.dims[0], macro.dims[1], macro.dims[2], 0,
      gl.RED, gl.FLOAT, macro.data);
    gl.texParameteri(gl.TEXTURE_3D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_3D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    this.dims = dims;
    // A mask texture grown for a larger cube is given back (the next
    // selection grows it again, to this cube's grid at most).
    const mc = this.maskCapacity;
    if (mc && (mc[0] > w || mc[1] > h || mc[2] > d) && !this.mask) {
      this.maskCapacity = null;
      this._uploadMask(new Uint8Array(1), [1, 1, 1]);
    }
    this.encoding = encoding;
    this.cell = cell;
    this.macroDims = macro.dims;
    this.textureBytes = w * h * d * (encoding === 'uint8' ? 1 : 2);
    return { uploadMs: t1 - t0, macroMs: t2 - t1, textureBytes: this.textureBytes };
  }

  /**
   * The selection mask into its texture. Its storage is kept and only grown
   * (to maskCapacityFor's rounded size, never past the cube's own grid):
   * each new selection is a sub-image upload into it, not a new allocation
   * (issue #226: fast repeated selections re-specified it every time). The
   * shader reads only [origin, origin + dims), so what lies beyond is never seen.
   */
  _uploadMask(data, dims) {
    const gl = this.gl;
    gl.bindTexture(gl.TEXTURE_3D, this.maskTex);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
    const cap = this.maskCapacity;
    if (!cap || dims[0] > cap[0] || dims[1] > cap[1] || dims[2] > cap[2]) {
      const want = maskCapacityFor(dims, cap, this.dims);
      gl.texImage3D(gl.TEXTURE_3D, 0, gl.R8, want[0], want[1], want[2], 0, gl.RED, gl.UNSIGNED_BYTE, null);
      gl.texParameteri(gl.TEXTURE_3D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
      gl.texParameteri(gl.TEXTURE_3D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
      for (const k of [gl.TEXTURE_WRAP_S, gl.TEXTURE_WRAP_T, gl.TEXTURE_WRAP_R]) {
        gl.texParameteri(gl.TEXTURE_3D, k, gl.CLAMP_TO_EDGE);
      }
      this.maskCapacity = want;
    }
    gl.texSubImage3D(gl.TEXTURE_3D, 0, 0, 0, 0, dims[0], dims[1], dims[2], gl.RED, gl.UNSIGNED_BYTE, data);
  }

  /** The selection on the drawn grid (selection.reduceMask's answer), or null
   *  to clear it. */
  setMask(reduced) {
    this._maskInput = reduced && reduced.count ? reduced : null;
    if (!reduced || !reduced.count) {
      this.mask = null;
      return;
    }
    this._uploadMask(reduced.data, reduced.dims);
    this.mask = { origin: reduced.origin, dims: reduced.dims, bytes: reduced.data.length };
  }

  /** Segments: array of {a: [x,y,z], b: [x,y,z], color: [r,g,b,a]} in model space. */
  setLines(segments, target = this.boxLines) {
    const gl = this.gl;
    for (const name of ['boxLines', 'tmpLines', 'selLines']) if (this[name] === target) this._lineSegs[name] = segments;
    const arr = new Float32Array(segments.length * 14);
    segments.forEach((s, i) => {
      arr.set([...s.a, ...s.color, ...s.b, ...s.color], i * 14);
    });
    gl.bindBuffer(gl.ARRAY_BUFFER, target.buf);
    // The buffer's storage is kept while it is large enough (the controllers'
    // lines are rebuilt every frame).
    if (arr.byteLength > (target.capacity || 0)) {
      target.capacity = Math.max(arr.byteLength, 2 * (target.capacity || 0), 4096);
      gl.bufferData(gl.ARRAY_BUFFER, target.capacity, gl.DYNAMIC_DRAW);
    }
    if (arr.byteLength) gl.bufferSubData(gl.ARRAY_BUFFER, 0, arr);
    target.count = segments.length * 2;
  }

  /** The controllers and hands of this frame (models.js meshes, world space). */
  setMesh(arr) {
    const gl = this.gl;
    gl.bindBuffer(gl.ARRAY_BUFFER, this.meshBuf);
    const data = arr instanceof Float32Array ? arr : new Float32Array(arr);
    if (data.byteLength > this.meshCapacity) {
      this.meshCapacity = Math.max(data.byteLength, 2 * this.meshCapacity, 65536);
      gl.bufferData(gl.ARRAY_BUFFER, this.meshCapacity, gl.DYNAMIC_DRAW);
    }
    if (data.byteLength) gl.bufferSubData(gl.ARRAY_BUFFER, 0, data);
    this.meshCount = Math.floor(data.length / MESH_STRIDE);
  }

  /** The mesh of this frame for one eye, with depth among its own triangles. */
  drawMesh(view) {
    const gl = this.gl;
    if (!this.meshCount) return;
    const { u } = this.meshProg;
    gl.useProgram(this.meshProg.p);
    gl.uniformMatrix4fv(u.uVP, false, M.multiply(view.proj, view.view));
    gl.uniform3fv(u.uEye, M.transformPoint(M.invert(view.view), [0, 0, 0]));
    gl.enable(gl.DEPTH_TEST);
    gl.depthFunc(gl.LEQUAL);
    gl.depthMask(true);
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA);
    gl.bindVertexArray(this.meshVao);
    gl.drawArrays(gl.TRIANGLES, 0, this.meshCount);
    gl.disable(gl.DEPTH_TEST);
  }

  /**
   * Labels: array of {pos: [x,y,z] model space, text, color, height (metres
   * at scale 1), anchor: 'left'|'center'|'right'}. Text is drawn into one atlas.
   */
  setLabels(labels) {
    const gl = this.gl;
    this._labels = labels;
    const rowH = 40, pad = 6, W = 1024;
    const rows = Math.max(1, labels.length);
    const H = Math.min(4096, 2 ** Math.ceil(Math.log2(rows * rowH)));
    // One canvas, kept (and resized only when the atlas needs more rows).
    let canvas = this._labelCanvas;
    if (!canvas || canvas.width !== W || canvas.height !== H) {
      canvas = (typeof OffscreenCanvas !== 'undefined') ? new OffscreenCanvas(W, H)
        : Object.assign(document.createElement('canvas'), { width: W, height: H });
      this._labelCanvas = canvas;
    }
    const ctx = canvas.getContext('2d');
    ctx.clearRect(0, 0, W, H);
    ctx.font = `${rowH - 2 * pad}px -apple-system, system-ui, sans-serif`;
    ctx.textBaseline = 'middle';
    const cells = [];
    labels.forEach((l, i) => {
      const y0 = i * rowH;
      if (y0 + rowH > H) return;
      const wpx = Math.min(W - 2 * pad, Math.ceil(ctx.measureText(l.text).width));
      ctx.fillStyle = l.color || '#ffffff';
      ctx.shadowColor = 'rgba(0,0,0,0.9)';
      ctx.shadowBlur = 4;
      ctx.fillText(l.text, pad, y0 + rowH / 2);
      cells[i] = { u0: 0, u1: (wpx + 2 * pad) / W, v0: y0 / H, v1: (y0 + rowH) / H, widthPx: wpx + 2 * pad, rowPx: rowH };
    });
    gl.bindTexture(gl.TEXTURE_2D, this.labelTex);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 4);
    if (!this.labelTexSize || this.labelTexSize[0] !== W || this.labelTexSize[1] !== H) {
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, gl.RGBA, gl.UNSIGNED_BYTE, canvas);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
      this.labelTexSize = [W, H];
    } else {
      gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, gl.RGBA, gl.UNSIGNED_BYTE, canvas);
    }
    const verts = labelVertices(labels, cells);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.labelBuf);
    gl.bufferData(gl.ARRAY_BUFFER, verts, gl.DYNAMIC_DRAW);
    this.labelCount = verts.length / LABEL_STRIDE;
  }

  /** A panel's texture (HUD, panels, message card): an opaque handle for
   *  updatePanel and drawPanel, kept valid across a context loss. Its
   *  `version` counts the canvases uploaded (the composition layers copy a
   *  canvas only when it changed). */
  createPanel() {
    const h = { tex: this._panelTexture(), canvas: null, size: null, version: 0 };
    this._panels.add(h);
    return h;
  }

  /** A panel no longer used: its texture goes (issue #226). */
  releasePanel(h) {
    if (!h || !this._panels.has(h)) return;
    this._panels.delete(h);
    try { this.gl.deleteTexture(h.tex); } catch { /* lost */ }
    h.tex = null;
    h.size = null;
  }

  _panelTexture() {
    const gl = this.gl;
    const tex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, tex);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    return tex;
  }

  /**
   * The canvas into the panel's texture. Its storage is allocated once per
   * canvas size and then written in place (issue #226: re-specifying it at
   * every redraw, up to 30 times a second for the menu, made the driver
   * allocate new storage for each redraw).
   */
  updatePanel(handle, canvas) {
    const gl = this.gl;
    handle.canvas = canvas;
    handle.version = (handle.version || 0) + 1;
    gl.bindTexture(gl.TEXTURE_2D, handle.tex);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 4);
    const w = canvas.width, h = canvas.height;
    if (!handle.size || handle.size[0] !== w || handle.size[1] !== h) {
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, gl.RGBA, gl.UNSIGNED_BYTE, canvas);
      handle.size = [w, h];
    } else {
      gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, gl.RGBA, gl.UNSIGNED_BYTE, canvas);
    }
  }

  beginFrame() {
    const gl = this.gl;
    if (!this.timer) return;
    // Read back the oldest finished query (a few frames late, never blocking).
    while (this.queries.length) {
      const q = this.queries[0];
      if (!gl.getQueryParameter(q, gl.QUERY_RESULT_AVAILABLE)) break;
      const disjoint = gl.getParameter(this.timer.GPU_DISJOINT_EXT);
      const ns = gl.getQueryParameter(q, gl.QUERY_RESULT);
      if (!disjoint) this.gpuMs = ns / 1e6;
      gl.deleteQuery(q);
      this.queries.shift();
    }
    if (this.queries.length < 4) {
      this.activeQuery = gl.createQuery();
      gl.beginQuery(this.timer.TIME_ELAPSED_EXT, this.activeQuery);
    } else {
      this.activeQuery = null;
    }
  }

  endFrame() {
    const gl = this.gl;
    if (this.timer && this.activeQuery) {
      gl.endQuery(this.timer.TIME_ELAPSED_EXT);
      this.queries.push(this.activeQuery);
      this.activeQuery = null;
    }
  }

  /**
   * Draw the volume for one eye. `view` = {proj, view} (Float32Array(16));
   * `model` maps model space (box centred at 0, sides `p.extent`) to world.
   */
  drawVolume(view, model, p) {
    const gl = this.gl;
    if (!this.volTex) return;
    const mv = M.multiply(view.view, model);
    const mvp = M.multiply(view.proj, mv);
    const inv = M.invert(mv);
    const camModel = M.transformPoint(inv, [0, 0, 0]);
    const camTex = [0, 1, 2].map((i) => camModel[i] / p.extent[i] + 0.5);
    const { u } = this.vol;
    gl.useProgram(this.vol.p);
    gl.uniformMatrix4fv(u.uMVP, false, mvp);
    gl.uniform3fv(u.uExtent, p.extent);
    gl.uniform3fv(u.uBoxMin, p.boxMin);
    gl.uniform3fv(u.uBoxMax, p.boxMax);
    gl.uniform3fv(u.uCamTex, camTex);
    gl.uniform3fv(u.uVoxDims, this.dims);
    gl.uniform3fv(u.uTexScale, p.texScale || [1, 1, 1]);
    gl.uniform3fv(u.uCellDims, this.macroDims);
    gl.uniform1f(u.uCell, this.cell);
    gl.uniform1f(u.uThr, p.thr);
    gl.uniform1f(u.uCmin, p.cmin);
    gl.uniform1f(u.uCmax, p.cmax);
    gl.uniform1f(u.uGamma, p.gamma);
    gl.uniform1f(u.uOpacity, p.opacity);
    gl.uniform1f(u.uSpv, p.spv);
    gl.uniform1i(u.uEnc, this.encoding === 'uint8' ? 0 : 1);
    gl.uniform1i(u.uMaxSteps, Math.min(MAX_LOOP, p.maxSteps || MAX_LOOP));
    gl.uniform1i(u.uMode, p.mode || 0);
    gl.uniform1i(u.uSkip, p.skip ? 1 : 0);
    gl.activeTexture(gl.TEXTURE0); gl.bindTexture(gl.TEXTURE_3D, this.volTex);
    gl.uniform1i(u.uVol, 0);
    gl.activeTexture(gl.TEXTURE1); gl.bindTexture(gl.TEXTURE_3D, this.macroTex);
    gl.uniform1i(u.uMacro, 1);
    gl.activeTexture(gl.TEXTURE2); gl.bindTexture(gl.TEXTURE_2D, this.lut);
    gl.uniform1i(u.uLut, 2);
    gl.activeTexture(gl.TEXTURE3); gl.bindTexture(gl.TEXTURE_3D, this.maskTex);
    gl.uniform1i(u.uMask, 3);
    const mk = p.showMask !== false ? this.mask : null;
    gl.uniform1i(u.uMaskOn, mk ? 1 : 0);
    gl.uniform1i(u.uMaskOnly, p.maskOnly && mk ? 1 : 0);
    gl.uniform3i(u.uMaskOrigin, ...(mk ? mk.origin : [0, 0, 0]));
    gl.uniform3i(u.uMaskDims, ...(mk ? mk.dims : [0, 0, 0]));
    gl.enable(gl.CULL_FACE);
    gl.cullFace(gl.FRONT);             // back faces: works with the eye inside the box
    gl.disable(gl.DEPTH_TEST);
    gl.depthMask(false);
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA);
    gl.bindVertexArray(this.cubeVao);
    gl.drawArrays(gl.TRIANGLES, 0, 36);
    gl.disable(gl.CULL_FACE);
  }

  drawLines(view, model, target = this.boxLines) {
    const gl = this.gl;
    if (!target.count) return;
    gl.useProgram(this.line.p);
    gl.uniformMatrix4fv(this.line.u.uMVP, false, M.multiply(view.proj, M.multiply(view.view, model)));
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA);
    gl.disable(gl.DEPTH_TEST);
    gl.bindVertexArray(target.vao);
    gl.drawArrays(gl.LINES, 0, target.count);
  }

  /** Lines in world space (controllers), from the per-frame buffer: call
   *  setLines(segments, renderer.tmpLines) once per frame, then this per eye. */
  drawWorldLines(view) {
    this.drawLines(view, M.identity(), this.tmpLines);
  }

  /** The cube's labels for one eye, fixed to the cube (labels.js). `scale`
   *  is no longer used: the model matrix scales them. */
  drawLabels(view, model) {
    const gl = this.gl;
    if (!this.labelCount) return;
    const { u } = this.label;
    gl.useProgram(this.label.p);
    const mv = M.multiply(view.view, model);
    gl.uniformMatrix4fv(u.uMVP, false, M.multiply(view.proj, mv));
    gl.uniform3fv(u.uEye, M.transformPoint(M.invert(mv), [0, 0, 0]));
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, this.labelTex);
    gl.uniform1i(u.uTex, 0);
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA);
    gl.disable(gl.DEPTH_TEST);
    gl.bindVertexArray(this.labelVao);
    gl.drawArrays(gl.TRIANGLES, 0, this.labelCount);
  }

  /** A textured panel of `size` metres, centred on `model`'s origin in its xy plane. */
  drawPanel(view, model, handle, size) {
    const gl = this.gl;
    const tex = handle.tex;
    const { u } = this.quad;
    gl.useProgram(this.quad.p);
    gl.uniformMatrix4fv(u.uMVP, false, M.multiply(view.proj, M.multiply(view.view, model)));
    gl.uniform2fv(u.uSize, size);
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, tex);
    gl.uniform1i(u.uTex, 0);
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA);
    gl.disable(gl.DEPTH_TEST);
    gl.bindVertexArray(this.quadVao);
    gl.drawArrays(gl.TRIANGLES, 0, 6);
  }
}

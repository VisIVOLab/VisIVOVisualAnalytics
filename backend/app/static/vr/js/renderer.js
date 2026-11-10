// WebGL2 ray marcher of the VR page: one 3-D texture (R8 or R16F), an
// empty-space-skipping grid, a colour map, front-to-back compositing with
// early termination, a clipping box, and the lines, labels and panels drawn
// around it. One draw per eye; the same code serves the desktop (no XR) mode.

import * as M from './mat4.js';
import { colormapTable } from './colormaps.js';
import { buildMacrocells } from './volume.js';

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
  int i = 0;
  for (; i < ${MAX_LOOP}; i++) {
    if (i >= uMaxSteps || t > tf) break;
    vec3 p = o + d * t;
    if (uSkip == 1) {
      vec3 cf = clamp(floor(p * vox / uCell), vec3(0.0), uCellDims - 1.0);
      float m = texelFetch(uMacro, ivec3(cf), 0).r;
      float need = uMode == 1 ? max(uThr, best) : uThr;
      if (m < need) {
        vec3 a = (cf * uCell / vox - o) * inv;
        vec3 b = ((cf + 1.0) * uCell / vox - o) * inv;
        float texit = min(min(max(a.x, b.x), max(a.y, b.y)), max(a.z, b.z));
        t = max(t, texit) + 0.01 * dt;
        continue;
      }
    }
    float v = normAt(p);
    if (uMode == 1) {
      best = max(best, v);
    } else if (v >= uThr) {
      float x = clamp((v - uCmin) / max(uCmax - uCmin, 1e-6), 0.0, 1.0);
      x = pow(x, uGamma);
      vec3 col = texture(uLut, vec2(x, 0.5)).rgb;
      float a = 1.0 - exp(-uOpacity * (0.04 + x) * stepVox);
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
    outColor = vec4(texture(uLut, vec2(x, 0.5)).rgb * a, a);
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

// Billboarded text: the centre is placed in view space, the corner offset in
// metres is added there, so labels always face the eye.
const LABEL_VS = `#version 300 es
precision highp float;
in vec3 aCenter;
in vec2 aOffset;
in vec2 aUV;
uniform mat4 uMV, uP;
uniform float uScale;
out vec2 vUV;
void main() {
  vec4 c = uMV * vec4(aCenter, 1.0);
  vUV = aUV;
  gl_Position = uP * (c + vec4(aOffset * uScale, 0.0, 0.0));
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

export class Renderer {
  constructor(gl) {
    this.gl = gl;
    this.vol = compile(gl, VOL_VS, VOL_FS);
    this.line = compile(gl, LINE_VS, LINE_FS);
    this.label = compile(gl, LABEL_VS, TEX_FS);
    this.quad = compile(gl, QUAD_VS, TEX_FS);
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
      return { vao, buf, count: 0 };
    };
    this.boxLines = lineVao();     // box, ticks: rebuilt when the clip box changes
    this.tmpLines = lineVao();     // controllers: rebuilt every frame

    this.labelVao = gl.createVertexArray();
    this.labelBuf = gl.createBuffer();
    gl.bindVertexArray(this.labelVao);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.labelBuf);
    const a0 = gl.getAttribLocation(this.label.p, 'aCenter');
    const a1 = gl.getAttribLocation(this.label.p, 'aOffset');
    const a2 = gl.getAttribLocation(this.label.p, 'aUV');
    gl.enableVertexAttribArray(a0); gl.vertexAttribPointer(a0, 3, gl.FLOAT, false, 28, 0);
    gl.enableVertexAttribArray(a1); gl.vertexAttribPointer(a1, 2, gl.FLOAT, false, 28, 12);
    gl.enableVertexAttribArray(a2); gl.vertexAttribPointer(a2, 2, gl.FLOAT, false, 28, 20);
    this.labelCount = 0;
    this.labelTex = gl.createTexture();

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
    this.setColormap('viridis');
    this.volTex = null;
    this.macroTex = null;
    this.timer = gl.getExtension('EXT_disjoint_timer_query_webgl2');
    this.queries = [];
    this.gpuMs = -1;
  }

  setColormap(name) {
    const gl = this.gl;
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
    this.encoding = encoding;
    this.cell = cell;
    this.macroDims = macro.dims;
    this.textureBytes = w * h * d * (encoding === 'uint8' ? 1 : 2);
    return { uploadMs: t1 - t0, macroMs: t2 - t1, textureBytes: this.textureBytes };
  }

  /** Segments: array of {a: [x,y,z], b: [x,y,z], color: [r,g,b,a]} in model space. */
  setLines(segments, target = this.boxLines) {
    const gl = this.gl;
    const arr = new Float32Array(segments.length * 14);
    segments.forEach((s, i) => {
      arr.set([...s.a, ...s.color, ...s.b, ...s.color], i * 14);
    });
    gl.bindBuffer(gl.ARRAY_BUFFER, target.buf);
    gl.bufferData(gl.ARRAY_BUFFER, arr, gl.DYNAMIC_DRAW);
    target.count = segments.length * 2;
  }

  /**
   * Labels: array of {pos: [x,y,z] model space, text, color, height (metres
   * at scale 1), anchor: 'left'|'center'|'right'}. Text is drawn into one atlas.
   */
  setLabels(labels) {
    const gl = this.gl;
    const rowH = 40, pad = 6, W = 1024;
    const rows = Math.max(1, labels.length);
    const H = Math.min(4096, 2 ** Math.ceil(Math.log2(rows * rowH)));
    const canvas = (typeof OffscreenCanvas !== 'undefined') ? new OffscreenCanvas(W, H)
      : Object.assign(document.createElement('canvas'), { width: W, height: H });
    const ctx = canvas.getContext('2d');
    ctx.clearRect(0, 0, W, H);
    ctx.font = `${rowH - 2 * pad}px -apple-system, system-ui, sans-serif`;
    ctx.textBaseline = 'middle';
    const verts = [];
    labels.forEach((l, i) => {
      const y0 = i * rowH;
      if (y0 + rowH > H) return;
      const wpx = Math.min(W - 2 * pad, Math.ceil(ctx.measureText(l.text).width));
      ctx.fillStyle = l.color || '#ffffff';
      ctx.shadowColor = 'rgba(0,0,0,0.9)';
      ctx.shadowBlur = 4;
      ctx.fillText(l.text, pad, y0 + rowH / 2);
      const u0 = 0, u1 = (wpx + 2 * pad) / W, v0 = y0 / H, v1 = (y0 + rowH) / H;
      const hgt = l.height || 0.025, wid = hgt * (wpx + 2 * pad) / rowH;
      const ax = l.anchor === 'right' ? -wid : l.anchor === 'center' ? -wid / 2 : 0;
      const corners = [[ax, -hgt / 2, u0, v1], [ax + wid, -hgt / 2, u1, v1], [ax + wid, hgt / 2, u1, v0],
        [ax, -hgt / 2, u0, v1], [ax + wid, hgt / 2, u1, v0], [ax, hgt / 2, u0, v0]];
      for (const [ox, oy, u, v] of corners) verts.push(...l.pos, ox, oy, u, v);
    });
    gl.bindTexture(gl.TEXTURE_2D, this.labelTex);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 4);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, gl.RGBA, gl.UNSIGNED_BYTE, canvas);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.labelBuf);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(verts), gl.DYNAMIC_DRAW);
    this.labelCount = verts.length / 7;
  }

  /** A texture for a panel (HUD); update with updatePanel. */
  createPanel() {
    const gl = this.gl;
    const tex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, tex);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    return tex;
  }

  updatePanel(tex, canvas) {
    const gl = this.gl;
    gl.bindTexture(gl.TEXTURE_2D, tex);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 4);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, gl.RGBA, gl.UNSIGNED_BYTE, canvas);
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

  drawLabels(view, model, scale) {
    const gl = this.gl;
    if (!this.labelCount) return;
    const { u } = this.label;
    gl.useProgram(this.label.p);
    gl.uniformMatrix4fv(u.uMV, false, M.multiply(view.view, model));
    gl.uniformMatrix4fv(u.uP, false, view.proj);
    gl.uniform1f(u.uScale, scale);
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
  drawPanel(view, model, tex, size) {
    const gl = this.gl;
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

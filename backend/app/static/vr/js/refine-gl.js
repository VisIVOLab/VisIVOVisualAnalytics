// Drawing a refined frame (issue #170): the backend's stereo frame for an
// earlier eye pose, warped to the current one through its depth and blended
// over the headset's own rendering of the cube.
//
// The warp is a mesh with one vertex per corner of the depth image's texels:
// each vertex is unprojected at the nearest depth of its (up to four)
// texels, through the frame's projection, carried to the current eye by
// E = (view * model)_now * inverse((view * model)_frame), and projected
// again. A vertex whose texels have no sample (an empty ray) takes the depth
// of the cube's centre. The colour is the frame's JPEG, which the backend
// composites over black: it replaces the headset's own picture where the
// mesh lands, with a weight that fades in and out (blend constant alpha).

import * as M from './mat4.js';
import { reprojectionMatrix, centreDistance } from './refine.js';

const WARP_VS = `#version 300 es
precision highp float;
precision highp int;
in vec2 aUV;                       // 0..1 across one eye, v down from the top row
uniform highp sampler2D uDepth;    // R32F metres along -z, 0 = no sample; eyes side by side
uniform ivec2 uDepthEye;           // one eye's depth image size
uniform int uEye, uUseDepth;
uniform float uFallback;
uniform mat4 uInvProjF;            // inverse projection of the frame's eye
uniform mat4 uProjNowE;            // current projection * E
out vec2 vUV;
float depthAt(ivec2 p) {
  p = clamp(p, ivec2(0), uDepthEye - 1);
  return texelFetch(uDepth, ivec2(p.x + uEye * uDepthEye.x, p.y), 0).r;
}
void main() {
  float d = 1e9;
  if (uUseDepth == 1) {
    ivec2 c = ivec2(floor(aUV * vec2(uDepthEye) + 0.5));
    for (int j = -1; j <= 0; j++) for (int i = -1; i <= 0; i++) {
      float s = depthAt(c + ivec2(i, j));
      if (s > 0.0) d = min(d, s);
    }
  }
  if (d > 1e8) d = uFallback;
  vec4 p = uInvProjF * vec4(aUV.x * 2.0 - 1.0, 1.0 - 2.0 * aUV.y, -1.0, 1.0);
  p.xyz /= p.w;
  vec3 x = p.xyz * (d / -p.z);
  gl_Position = uProjNowE * vec4(x, 1.0);
  vUV = aUV;
}`;

const WARP_FS = `#version 300 es
precision mediump float;
precision highp int;               // uEye is shared with the vertex shader
in vec2 vUV;
uniform sampler2D uColor;          // both eyes side by side, rows top first
uniform int uEye;
uniform vec2 uHalfTexel;           // keeps the filter inside this eye's half
uniform float uVScale;             // the eyes' rows over the texture's (a video frame's stripe below)
out vec4 outColor;
void main() {
  vec2 uv = vec2(clamp(vUV.x, uHalfTexel.x, 1.0 - uHalfTexel.x) * 0.5 + 0.5 * float(uEye),
                 clamp(vUV.y, uHalfTexel.y, 1.0 - uHalfTexel.y) * uVScale);
  outColor = vec4(texture(uColor, uv).rgb, 1.0);
}`;

function compile(gl, vs, fs) {
  const p = gl.createProgram();
  for (const [type, src] of [[gl.VERTEX_SHADER, vs], [gl.FRAGMENT_SHADER, fs]]) {
    const s = gl.createShader(type);
    gl.shaderSource(s, src);
    gl.compileShader(s);
    if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) throw new Error(`refine shader: ${gl.getShaderInfoLog(s)}`);
    gl.attachShader(p, s);
  }
  gl.linkProgram(p);
  if (!gl.getProgramParameter(p, gl.LINK_STATUS)) throw new Error(`refine link: ${gl.getProgramInfoLog(p)}`);
  const u = {};
  const n = gl.getProgramParameter(p, gl.ACTIVE_UNIFORMS);
  for (let i = 0; i < n; i++) {
    const info = gl.getActiveUniform(p, i);
    u[info.name] = gl.getUniformLocation(p, info.name);
  }
  return { p, u };
}

export class RefineDraw {
  constructor(gl) {
    this.gl = gl;
    this.prog = compile(gl, WARP_VS, WARP_FS);
    this.grids = new Map();
    this.slots = [this._slot(), this._slot()];
    this.current = -1;          // index of the slot shown
  }

  _slot() {
    const gl = this.gl;
    const mk = () => {
      const t = gl.createTexture();
      gl.bindTexture(gl.TEXTURE_2D, t);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
      return t;
    };
    const color = mk();
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    const depth = mk();
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    return { color, depth, colorSize: [0, 0], depthSize: [0, 0], frame: null };
  }

  /** A mesh of gw x gh cells over one eye (vertices at the cell corners). */
  _grid(gw, gh) {
    const key = `${gw}x${gh}`;
    if (this.grids.has(key)) return this.grids.get(key);
    const gl = this.gl;
    const uv = new Float32Array((gw + 1) * (gh + 1) * 2);
    let k = 0;
    for (let j = 0; j <= gh; j++) for (let i = 0; i <= gw; i++) { uv[k++] = i / gw; uv[k++] = j / gh; }
    const idx = new Uint32Array(gw * gh * 6);
    k = 0;
    for (let j = 0; j < gh; j++) {
      for (let i = 0; i < gw; i++) {
        const a = j * (gw + 1) + i, b = a + 1, c = a + gw + 1, d = c + 1;
        idx[k++] = a; idx[k++] = c; idx[k++] = b; idx[k++] = b; idx[k++] = c; idx[k++] = d;
      }
    }
    const vao = gl.createVertexArray();
    gl.bindVertexArray(vao);
    const vb = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, vb);
    gl.bufferData(gl.ARRAY_BUFFER, uv, gl.STATIC_DRAW);
    const loc = gl.getAttribLocation(this.prog.p, 'aUV');
    gl.enableVertexAttribArray(loc);
    gl.vertexAttribPointer(loc, 2, gl.FLOAT, false, 0, 0);
    const ib = gl.createBuffer();
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, ib);
    gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, idx, gl.STATIC_DRAW);
    gl.bindVertexArray(null);
    const g = { vao, count: idx.length, gw, gh };
    this.grids.set(key, g);
    return g;
  }

  /**
   * Uploads a decoded frame ({bitmap, depth: Float32Array | null, header, rec})
   * into the slot not shown, and shows it. Returns the upload time in ms.
   */
  upload(frame) {
    const gl = this.gl;
    const t0 = performance.now();
    const i = this.current === 0 ? 1 : 0;
    const s = this.slots[i];
    const bm = frame.bitmap;          // an ImageBitmap, or a VideoFrame (WebRTC, #189)
    const bw = bm.displayWidth || bm.width, bh = bm.displayHeight || bm.height;
    gl.bindTexture(gl.TEXTURE_2D, s.color);
    gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, false);
    gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, false);
    if (s.colorSize[0] !== bw || s.colorSize[1] !== bh) {
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, gl.RGBA, gl.UNSIGNED_BYTE, bm);
      s.colorSize = [bw, bh];
    } else {
      gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, gl.RGBA, gl.UNSIGNED_BYTE, bm);
    }
    const d = frame.header.depth;
    if (frame.depth && d) {
      gl.bindTexture(gl.TEXTURE_2D, s.depth);
      gl.pixelStorei(gl.UNPACK_ALIGNMENT, 4);
      if (s.depthSize[0] !== d.width || s.depthSize[1] !== d.height) {
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.R32F, d.width, d.height, 0, gl.RED, gl.FLOAT, frame.depth);
        s.depthSize = [d.width, d.height];
      } else {
        gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, d.width, d.height, gl.RED, gl.FLOAT, frame.depth);
      }
    }
    if (bm.close) bm.close();
    s.frame = { header: frame.header, rec: frame.rec, hasDepth: !!(frame.depth && d), vScale: frame.vScale || 1 };
    this.current = i;
    return performance.now() - t0;
  }

  get shown() { return this.current >= 0 ? this.slots[this.current].frame : null; }

  forget() { this.current = -1; }

  /**
   * Draws the shown frame's eye `eyeIndex` (0 left, 1 right) for the current
   * eye `view` ({proj, view}) and cube `model`, with weight `w` over what is
   * in the framebuffer. `useDepth` false: a plane through the cube's centre
   * instead (for comparisons).
   */
  draw(view, model, eyeIndex, w, useDepth = true) {
    const s = this.current >= 0 ? this.slots[this.current] : null;
    if (!s || !s.frame || w <= 0) return false;
    const gl = this.gl;
    const { rec, header, hasDepth, vScale } = s.frame;
    const ef = rec.eyes[Math.min(eyeIndex, rec.eyes.length - 1)];
    const E = reprojectionMatrix(ef.view, rec.model, view.view, model);
    const invProj = M.invert(ef.proj);
    if (!E || !invProj) return false;
    const eyeHdr = header.eyes[eyeIndex] || header.eyes[0];
    const depthOn = useDepth && hasDepth;
    const dw = depthOn ? header.depth.width / 2 : 0, dh = depthOn ? header.depth.height : 0;
    const grid = depthOn ? this._grid(dw, dh) : this._grid(32, 32);
    const { u } = this.prog;
    gl.useProgram(this.prog.p);
    gl.uniformMatrix4fv(u.uInvProjF, false, invProj);
    gl.uniformMatrix4fv(u.uProjNowE, false, M.multiply(view.proj, E));
    gl.uniform1i(u.uEye, eyeIndex);
    gl.uniform1i(u.uUseDepth, depthOn ? 1 : 0);
    gl.uniform2i(u.uDepthEye, Math.max(1, dw), Math.max(1, dh));
    gl.uniform1f(u.uFallback, centreDistance(ef.view, rec.model));
    gl.uniform2f(u.uHalfTexel, 0.5 / eyeHdr.width, 0.5 / eyeHdr.height);
    gl.uniform1f(u.uVScale, vScale);
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, s.color);
    gl.uniform1i(u.uColor, 0);
    gl.activeTexture(gl.TEXTURE1);
    gl.bindTexture(gl.TEXTURE_2D, s.depth);
    gl.uniform1i(u.uDepth, 1);
    gl.disable(gl.DEPTH_TEST);
    gl.disable(gl.CULL_FACE);
    gl.enable(gl.BLEND);
    gl.blendColor(0, 0, 0, w);
    // Colour: w * refined + (1 - w) * the headset's own; the alpha is left as
    // it is (the page draws no refined frame in passthrough, where it counts).
    gl.blendFuncSeparate(gl.CONSTANT_ALPHA, gl.ONE_MINUS_CONSTANT_ALPHA, gl.ZERO, gl.ONE);
    gl.bindVertexArray(grid.vao);
    gl.drawElements(gl.TRIANGLES, grid.count, gl.UNSIGNED_INT, 0);
    gl.bindVertexArray(null);
    gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA);
    return true;
  }
}

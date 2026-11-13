// WebGL2 ray marcher of the VR page: one 3-D texture (R8 or R16F), an
// empty-space-skipping grid, a colour map, front-to-back compositing with
// early termination, a clipping box, and the lines, labels and panels drawn
// around it. One draw per eye; the same code serves the desktop (no XR) mode.

import * as M from './mat4.js';
import { colormapTable } from './colormaps.js';
import { buildMacrocells } from './volume.js';
import { labelVertices, LABEL_STRIDE } from './labels.js';
import { MESH_STRIDE } from './models.js';
import { mulInto, imageUses } from './gltf.js';
import { guardTex3D, tex3dInXRError } from './xr-gl-guard.js';

/** The eye's position from a rigid view matrix (−Rᵀt), into `out`. */
function eyeOf(v, out) {
  for (let i = 0; i < 3; i++) out[i] = -(v[4 * i] * v[12] + v[4 * i + 1] * v[13] + v[4 * i + 2] * v[14]);
  return out;
}

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

// The real controller models (WebXR Input Profiles, gltf.js): indexed,
// textured triangles in a node's frame (uModel: grip pose × node chain).
//
// All in highp. On the Quest's GPU mediump is half precision (fp16): the
// models' nodes are scaled by 0.01 (centimetres to metres), so mat3(uModel)
// × normal is a vector of length 0.01, whose squared length (1e-4, less
// between vertices) is at the edge of fp16's normal range and flushes to 0;
// normalize() then gives Inf/NaN and the pixel saturates to white, where the
// normals bend, so it follows the view. The normal is made unit length here,
// in the vertex shader, with the inverse transpose (non-uniform and negative
// scales too), and the UVs keep full precision (fp16 is about half a texel at
// 1024 px).
export const MODEL_VS = `#version 300 es
precision highp float;
in vec3 aPos;
in vec3 aNormal;
in vec2 aUV;
uniform mat4 uVP;
uniform mat4 uModel;
out vec3 vPos;
out vec3 vNormal;
out vec2 vUV;
void main() {
  vec4 w = uModel * vec4(aPos, 1.0);
  vPos = w.xyz;
  vec3 n = transpose(inverse(mat3(uModel))) * aNormal;
  float l2 = dot(n, n);
  vNormal = l2 > 0.0 ? n * inversesqrt(l2) : vec3(0.0, 1.0, 0.0);
  vUV = aUV;
  gl_Position = uVP * w;
}`;

// glTF metallic-roughness, approximated for a small hand-held object: the
// base colour and emissive textures are sRGB (decoded by the sampler, the
// textures being SRGB8_ALPHA8), lit in linear light by a sky/ground ambient,
// a key light from above and a light at the eye, with a GGX highlight whose
// size follows the roughness and whose strength the Fresnel term at normal
// incidence sets (4 % for a dielectric, the base colour for a metal: dark
// plastic stays dark, with a soft sheen); then encoded to sRGB, as the rest
// of the page writes its colours. Alpha modes: OPAQUE writes alpha 1 (the
// texture's alpha ignored, as glTF says), MASK discards below the cutoff and
// writes 1, BLEND writes premultiplied colour (drawn after, blended).
// Back-facing triangles of a double-sided material use the reversed normal
// (glTF); drawModels sets the front face per node (clockwise under a
// mirroring node), so gl_FrontFacing is the triangle's own side. Single-sided
// materials are culled.
export const MODEL_FS = `#version 300 es
precision highp float;
in vec3 vPos;
in vec3 vNormal;
in vec2 vUV;
uniform sampler2D uTex;        // base colour (sRGB)
uniform sampler2D uMRTex;      // G roughness, B metallic (linear)
uniform sampler2D uEmTex;      // emissive (sRGB)
uniform vec4 uColor;           // base colour factor
uniform vec2 uMR;              // metallic, roughness factors
uniform vec3 uEmissive;
uniform int uAlphaMode;        // 0 opaque, 1 mask, 2 blend
uniform float uCutoff;
uniform vec3 uEye;
out vec4 outColor;
const float PI = 3.14159265;
const vec3 KEY = vec3(0.32, 0.88, 0.35);      // normalized below
vec3 toSrgb(vec3 c) {
  c = clamp(c, 0.0, 1.0);
  return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}
float ggx(float nh, float a2) { float d = nh * nh * (a2 - 1.0) + 1.0; return a2 / (PI * d * d); }
float vis(float nl, float nv, float a) { float k = a * 0.5; return 0.25 / ((nl * (1.0 - k) + k) * (nv * (1.0 - k) + k)); }
vec3 specular(vec3 n, vec3 v, vec3 l, vec3 f0, float a) {
  float nl = max(dot(n, l), 0.0);
  if (nl <= 0.0) return vec3(0.0);
  vec3 h = normalize(l + v);
  float nv = max(dot(n, v), 1e-3), nh = max(dot(n, h), 0.0), vh = max(dot(v, h), 0.0);
  vec3 f = f0 + (1.0 - f0) * pow(1.0 - vh, 5.0);
  return f * (ggx(nh, a * a) * vis(nl, nv, a) * nl);
}
void main() {
  vec4 base = texture(uTex, vUV) * uColor;
  if (uAlphaMode == 1 && base.a < uCutoff) discard;
  vec4 mr = texture(uMRTex, vUV);
  float metal = clamp(uMR.x * mr.b, 0.0, 1.0);
  float rough = clamp(uMR.y * mr.g, 0.04, 1.0);
  vec3 n = normalize(vNormal);
  vec3 v = normalize(uEye - vPos);
  if (!gl_FrontFacing) n = -n;
  vec3 key = normalize(KEY);
  vec3 diffuse = base.rgb * (1.0 - metal);
  vec3 f0 = mix(vec3(0.04), base.rgb, metal);
  float a = rough * rough;
  float amb = mix(0.12, 0.3, 0.5 + 0.5 * n.y);
  vec3 c = diffuse * (amb + 0.6 * max(dot(n, key), 0.0) + 0.3 * max(dot(n, v), 0.0));
  c += specular(n, v, key, f0, a) * 0.6 + specular(n, v, v, f0, a) * 0.3;
  // Metals have no diffuse: a little of their colour as ambient reflection.
  c += f0 * amb * metal;
  c += uEmissive * texture(uEmTex, vUV).rgb;
  vec3 srgb = toSrgb(c);
  outColor = uAlphaMode == 2 ? vec4(srgb * base.a, base.a) : vec4(srgb, 1.0);
}`;

/** Floats per vertex of an uploaded controller model: position 3, normal 3, uv 2. */
const MODEL_STRIDE = 8;
/**
 * The largest side of a controller model's texture on the GPU (the Touch
 * Plus ships 2048²). Kept at 1024 while the Quest's context loss (#229) is
 * open: a 2048² map with its mipmaps is 21.3 MB, 1024² is 5.3 MB, and each
 * hand has its own map (10.7 MB for the pair instead of 42.7 MB). The
 * controller is at most a few hundred pixels tall on the eye buffer, so 1024
 * with mipmaps and anisotropic filtering loses little.
 */
export const MODEL_TEXTURE_MAX = 1024;
/** The anisotropy of the controller-model textures (when the extension exists; 4 is cheap on a tiler). */
export const MODEL_ANISOTROPY = 4;
const MODEL_ALPHA_MODES = { OPAQUE: 0, MASK: 1, BLEND: 2 };
// glTF's defaults, for a primitive without a material.
const MODEL_DEFAULT_MATERIAL = { baseColor: [1, 1, 1, 1], texture: -1, metallic: 1, roughness: 1, mrTexture: -1,
  emissive: [0, 0, 0], emissiveTexture: -1, alphaMode: 'OPAQUE', alphaCutoff: 0.5, doubleSided: false };

/** The sign of the determinant of a 4×4's upper 3×3 (column-major): negative when it mirrors. */
export function mirrors(m) {
  const det = m[0] * (m[5] * m[10] - m[9] * m[6]) - m[4] * (m[1] * m[10] - m[9] * m[2]) + m[8] * (m[1] * m[6] - m[5] * m[2]);
  return det < 0;
}

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

/** Voxels of a 3-D storage of `dims`. */
function voxels(dims) { return dims[0] * dims[1] * dims[2]; }

/** At most this many bytes of zeros a call when a mask storage is cleared. */
const ZERO_SLAB_BYTES = 4 * 1024 * 1024;

/**
 * Clears the bound TEXTURE_3D's level 0 of `dims` R8 voxels with
 * texSubImage3D of zeros, a slab of whole planes at a time. A texture made
 * with texStorage3D is otherwise "uninitialised" for the browser, which
 * then clears it by itself at its first use, which may be in VR (issue #229).
 */
function zeroR8(gl, dims) {
  const plane = dims[0] * dims[1];
  const planes = Math.max(1, Math.min(dims[2], Math.floor(ZERO_SLAB_BYTES / Math.max(plane, 1))));
  const zeros = new Uint8Array(plane * planes);
  for (let z = 0; z < dims[2]; z += planes) {
    const n = Math.min(planes, dims[2] - z);
    gl.texSubImage3D(gl.TEXTURE_3D, 0, 0, 0, z, dims[0], dims[1], n, gl.RED, gl.UNSIGNED_BYTE,
      n === planes ? zeros : zeros.subarray(0, plane * n));
  }
}

function params3D(gl, filter) {
  gl.texParameteri(gl.TEXTURE_3D, gl.TEXTURE_MIN_FILTER, filter);
  gl.texParameteri(gl.TEXTURE_3D, gl.TEXTURE_MAG_FILTER, filter);
  for (const k of [gl.TEXTURE_WRAP_S, gl.TEXTURE_WRAP_T, gl.TEXTURE_WRAP_R]) gl.texParameteri(gl.TEXTURE_3D, k, gl.CLAMP_TO_EDGE);
}

export class Renderer {
  constructor(gl) {
    this.gl = gl;
    // True while a WebXR session runs (main.js): no 3-D texture is allocated
    // then (issue #229, xr-gl-guard.js). The guard is on the context itself,
    // so the layers' and any other code's 3-D allocations are refused too.
    this.xrActive = false;
    this.onTex3DInXR = null;       // (call name) => void: the page logs it
    this.tex3dGuard = guardTex3D(gl, () => this.xrActive, (name) => { if (this.onTex3DInXR) this.onTex3DInXR(name); });
    this._panels = new Set();      // createPanel() handles, re-made by restore()
    this._cmap = 'viridis';
    this._volume = null;           // the last setVolume() input, for restore()
    this.slot = null;              // the volume on show (prepareVolume), its textures
    this._slots = new Set();       // every volume prepared and not released
    this._maskInput = null;
    this._lineSegs = {};           // target name -> the last segments
    this._labels = null;
    this._labelCanvas = null;      // one canvas for the label atlas, reused (issue #226)
    this._models = new Set();      // uploadModel() handles, uploaded again by restore()
    this._vp = new Float32Array(16);
    this._eye = new Float32Array(3);
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
    this.modelProg = compile(gl, MODEL_VS, MODEL_FS);
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
    // A white texel for the controller-model materials without a texture.
    this.whiteTex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, this.whiteTex);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, 1, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE, new Uint8Array([255, 255, 255, 255]));
    this.modelDraws = 0;           // controller-model draw calls in the last drawModels()
    this._faceState = { saved: false, cull: false, mode: 0, front: 0 };   // drawModels' culling state, kept to restore
    this.aniso = gl.getExtension('EXT_texture_filter_anisotropic');
    this.modelAniso = this.aniso ? Math.min(MODEL_ANISOTROPY, gl.getParameter(this.aniso.MAX_TEXTURE_MAX_ANISOTROPY_EXT)) : 1;

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
    // The selection overlay of a volume without a mask storage of its own
    // (none on show, the benchmark's): one zero voxel, so the sampler is
    // never unbound. Allocated here, never in VR (issue #229).
    this.emptyMaskTex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_3D, this.emptyMaskTex);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
    gl.texStorage3D(gl.TEXTURE_3D, 1, gl.R8, 1, 1, 1);
    params3D(gl, gl.NEAREST);
    zeroR8(gl, [1, 1, 1]);
    this.maskTex = this.emptyMaskTex;
    this.mask = null;
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
    // Every volume prepared before the loss is gone with its textures; the
    // one on show is prepared again from its voxels (outside VR: a session
    // cannot outlive a lost context, and the page restores after it ends).
    for (const s of this._slots) s.lost = true;
    this._slots.clear();
    this.slot = null;
    this.textureBytes = 0;
    this.dims = null;
    this.macroDims = null;
    const maskInput = this._maskInput;     // useVolume() clears it
    this._init();
    if (this._volume) this.setVolume(this._volume, this._volume.cell);
    if (maskInput && this.slot) this.setMask(maskInput);
    for (const [name, segs] of Object.entries(this._lineSegs)) if (this[name]) this.setLines(segs, this[name]);
    if (this._labels) this.setLabels(this._labels);
    for (const h of this._panels) {
      h.tex = this._panelTexture();
      h.size = null;
      if (h.canvas) this.updatePanel(h, h.canvas);
    }
    for (const h of this._models) this._restoreModel(h);
  }

  /**
   * What the renderer holds on the GPU now (issue #226): textures and their
   * bytes by kind, as allocated (not as the driver may pad them).
   */
  gpuStats() {
    const panelBytes = [...this._panels].reduce((a, h) => a + (h.size ? h.size[0] * h.size[1] * 4 : 0), 0);
    const s = this.slot;
    const maskBytes = s ? s.maskBytes : 0;
    const labelBytes = this.labelTexSize ? this.labelTexSize[0] * this.labelTexSize[1] * 4 : 0;
    const vol = s ? s.textureBytes : 0;
    const macro = s ? s.macroBytes : 0;
    // Volumes prepared and kept beside the one on show (the benchmark's).
    let otherBytes = 0;
    for (const o of this._slots) if (o !== s) otherBytes += o.textureBytes + o.macroBytes + o.maskBytes;
    let modelBytes = 0;
    for (const h of this._models) modelBytes += h.bytes;
    return { panels: this._panels.size, panelBytes, maskBytes, labelBytes, volumeBytes: vol, macroBytes: macro,
      otherVolumes: this._slots.size - (s && this._slots.has(s) ? 1 : 0), otherVolumeBytes: otherBytes,
      models: this._models.size, modelBytes,
      totalBytes: panelBytes + maskBytes + labelBytes + vol + macro + otherBytes + modelBytes + 256 * 4 + 1 };
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

  /**
   * A volume's textures, allocated and filled now: the cube (R8 or R16F),
   * its empty-space grid (R32F) and, unless `mask` is false, the selection
   * mask's storage over the whole reduced grid (R8, one byte a voxel, zeroed).
   * Every one is immutable storage (texStorage3D) filled with texSubImage3D,
   * so nothing is left for the browser to clear later. Never in VR (issue
   * #229): throws a `tex3d-in-xr` Error before touching the GPU. Returns the
   * slot, to be shown with useVolume() and given back with releaseVolume().
   */
  prepareVolume({ data, dims, encoding }, cell = 8, { mask = true } = {}) {
    if (this.xrActive) throw tex3dInXRError('prepareVolume');
    const gl = this.gl;
    const [w, h, d] = dims;
    if (Math.max(w, h, d) > this.maxTex3D) {
      throw new Error(`${w} x ${h} x ${d} exceeds this GPU's 3-D texture limit (${this.maxTex3D}): use a larger binning factor.`);
    }
    const u8 = encoding === 'uint8';
    const t0 = performance.now();
    const volTex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_3D, volTex);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
    gl.texStorage3D(gl.TEXTURE_3D, 1, u8 ? gl.R8 : gl.R16F, w, h, d);
    gl.texSubImage3D(gl.TEXTURE_3D, 0, 0, 0, 0, w, h, d, gl.RED, u8 ? gl.UNSIGNED_BYTE : gl.HALF_FLOAT, data);
    params3D(gl, gl.LINEAR);
    const t1 = performance.now();
    const macro = buildMacrocells(data, dims, encoding, cell);
    const t2 = performance.now();
    const macroTex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_3D, macroTex);
    gl.texStorage3D(gl.TEXTURE_3D, 1, gl.R32F, macro.dims[0], macro.dims[1], macro.dims[2]);
    gl.texSubImage3D(gl.TEXTURE_3D, 0, 0, 0, 0, macro.dims[0], macro.dims[1], macro.dims[2], gl.RED, gl.FLOAT, macro.data);
    params3D(gl, gl.NEAREST);
    let maskTex = null;
    if (mask) {
      maskTex = gl.createTexture();
      gl.bindTexture(gl.TEXTURE_3D, maskTex);
      gl.texStorage3D(gl.TEXTURE_3D, 1, gl.R8, w, h, d);
      params3D(gl, gl.NEAREST);
      zeroR8(gl, dims);
    }
    const t3 = performance.now();
    const slot = {
      volTex, macroTex, maskTex, dims: [w, h, d], encoding, cell, macroDims: macro.dims,
      textureBytes: voxels(dims) * (u8 ? 1 : 2), macroBytes: voxels(macro.dims) * 4, maskBytes: mask ? voxels(dims) : 0,
      uploadMs: t1 - t0, macroMs: t2 - t1, maskMs: t3 - t2, lost: false, released: false,
    };
    this._slots.add(slot);
    return slot;
  }

  /**
   * Shows a prepared volume: no GPU allocation, so it may be called in VR.
   * The selection shown on the previous volume is cleared.
   */
  useVolume(slot) {
    if (!slot || slot.lost || slot.released) throw new Error('that volume is no longer on the GPU');
    if (slot !== this.slot) { this.mask = null; this._maskInput = null; }
    this.slot = slot;
    this.volTex = slot.volTex;
    this.macroTex = slot.macroTex;
    this.maskTex = slot.maskTex || this.emptyMaskTex;
    this.dims = slot.dims;
    this.encoding = slot.encoding;
    this.cell = slot.cell;
    this.macroDims = slot.macroDims;
    this.textureBytes = slot.textureBytes;
    return slot;
  }

  /** Gives a prepared volume's textures back (not the one on show). */
  releaseVolume(slot) {
    if (!slot || slot.released || slot === this.slot) return;
    slot.released = true;
    this._slots.delete(slot);
    if (slot.lost) return;
    const gl = this.gl;
    for (const t of [slot.volTex, slot.macroTex, slot.maskTex]) if (t) gl.deleteTexture(t);
  }

  /**
   * Upload the reduced cube and show it (prepareVolume, then useVolume; the
   * volume shown before is released). Not in VR (issue #229): the page keeps
   * a cube that arrives in VR until the session ends (main.js installCube).
   * Returns the timings and the texture size.
   */
  setVolume({ data, dims, encoding }, cell = 8) {
    const slot = this.prepareVolume({ data, dims, encoding }, cell);
    const old = this.slot;
    this._volume = { data, dims, encoding, cell };
    this.useVolume(slot);
    if (old) this.releaseVolume(old);
    return { uploadMs: slot.uploadMs, macroMs: slot.macroMs, maskMs: slot.maskMs, textureBytes: slot.textureBytes };
  }

  /** The selection on the drawn grid (selection.reduceMask's answer), or null
   *  to clear it. A sub-image upload into the volume's own mask storage
   *  (prepareVolume: the whole reduced grid, allocated with the cube), never
   *  an allocation, so it is safe in VR (issue #229). The shader reads only
   *  [origin, origin + dims), so what lies beyond in the storage is never seen. */
  setMask(reduced) {
    if (!reduced || !reduced.count) {
      this._maskInput = null;
      this.mask = null;
      return;
    }
    const s = this.slot;
    const dims = reduced.dims;
    if (!s || !s.maskTex) throw new Error('no volume with a selection mask on show');
    if (dims[0] > s.dims[0] || dims[1] > s.dims[1] || dims[2] > s.dims[2]) {
      throw new Error(`a selection of ${dims.join('×')} reduced voxels does not fit the cube's grid ${s.dims.join('×')}`);
    }
    const gl = this.gl;
    gl.bindTexture(gl.TEXTURE_3D, s.maskTex);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
    gl.texSubImage3D(gl.TEXTURE_3D, 0, 0, 0, 0, dims[0], dims[1], dims[2], gl.RED, gl.UNSIGNED_BYTE, reduced.data);
    this._maskInput = reduced;
    this.mask = { origin: reduced.origin, dims, bytes: reduced.data.length };
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
   * A controller model (gltf.js readGLTF output) on the GPU, once: one
   * vertex buffer and one index buffer for all its primitives, one texture
   * per base-colour image (`bitmaps[imageIndex]`, an ImageBitmap already at
   * most MODEL_TEXTURE_MAX a side; null leaves the factor alone). The
   * bitmaps are closed once uploaded: after a lost context the geometry is
   * uploaded again from the model and the textures from `redecode()` (a
   * promise of new bitmaps, decoded again from the file's bytes), white
   * until then.
   */
  uploadModel(model, bitmaps = [], { redecode = null } = {}) {
    const h = { model, redecode, gen: 0, vao: null, vbo: null, ibo: null, textures: [], meshes: [], geomBytes: 0, bytes: 0 };
    this._uploadModel(h);
    this._setModelTextures(h, bitmaps);
    this._models.add(h);
    return h;
  }

  /** After a lost context: the geometry now, the textures when decoded again. */
  _restoreModel(h) {
    this._uploadModel(h);
    h.textures = [];
    h.bytes = h.geomBytes;
    const gen = ++h.gen;
    if (!h.redecode) return;
    h.redecode().then((bitmaps) => {
      if (gen !== h.gen || this.gl.isContextLost()) { for (const b of bitmaps) if (b && b.close) b.close(); return; }
      this._setModelTextures(h, bitmaps);
    }).catch(() => { /* the model stays white: still drawn */ });
  }

  /**
   * The material textures, mipmapped and anisotropically filtered (they are
   * seen small and at a slant: without, the maps shimmer); base colour and
   * emissive as SRGB8_ALPHA8 (the sampler decodes to linear, the mipmaps are
   * averaged in linear light), metallic-roughness as RGBA8. An image used
   * both ways gets both. The bitmaps are closed after.
   */
  _setModelTextures(h, bitmaps) {
    const gl = this.gl;
    const flip = gl.getParameter(gl.UNPACK_FLIP_Y_WEBGL), pre = gl.getParameter(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL);
    gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, false);       // glTF's UV origin is the image's top-left, as WebGL's upload
    gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, false);
    const uses = imageUses(h.model);
    let bytes = 0;
    const make = (bm, internal) => {
      const t = gl.createTexture();
      gl.bindTexture(gl.TEXTURE_2D, t);
      gl.texImage2D(gl.TEXTURE_2D, 0, internal, gl.RGBA, gl.UNSIGNED_BYTE, bm);
      gl.generateMipmap(gl.TEXTURE_2D);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR_MIPMAP_LINEAR);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.REPEAT);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.REPEAT);
      if (this.aniso) gl.texParameterf(gl.TEXTURE_2D, this.aniso.TEXTURE_MAX_ANISOTROPY_EXT, this.modelAniso);
      bytes += Math.round(bm.width * bm.height * 4 * 4 / 3);
      return t;
    };
    h.textures = h.model.images.map((im, i) => {
      const bm = bitmaps[i];
      if (!bm) return null;
      const use = uses.get(i) || { srgb: true, linear: false };
      const t = { srgb: use.srgb ? make(bm, gl.SRGB8_ALPHA8) : null, linear: use.linear ? make(bm, gl.RGBA8) : null };
      if (bm.close) bm.close();
      return t;
    });
    gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, flip);
    gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, pre);
    h.bytes = h.geomBytes + bytes;
  }

  _uploadModel(h) {
    const gl = this.gl;
    const { model } = h;
    const prims = model.meshes.flat();
    const nVerts = prims.reduce((a, p) => a + p.positions.length / 3, 0);
    const nIdx = prims.reduce((a, p) => a + p.indices.length, 0);
    const wide = nVerts > 65536;
    const verts = new Float32Array(nVerts * MODEL_STRIDE);
    const idx = wide ? new Uint32Array(nIdx) : new Uint16Array(nIdx);
    const type = wide ? gl.UNSIGNED_INT : gl.UNSIGNED_SHORT;
    let v0 = 0, i0 = 0;
    const placed = new Map();
    for (const p of prims) {
      const n = p.positions.length / 3;
      for (let i = 0; i < n; i++) {
        const o = (v0 + i) * MODEL_STRIDE;
        verts[o] = p.positions[3 * i]; verts[o + 1] = p.positions[3 * i + 1]; verts[o + 2] = p.positions[3 * i + 2];
        if (p.normals) { verts[o + 3] = p.normals[3 * i]; verts[o + 4] = p.normals[3 * i + 1]; verts[o + 5] = p.normals[3 * i + 2]; } else verts[o + 4] = 1;
        if (p.uvs) { verts[o + 6] = p.uvs[2 * i]; verts[o + 7] = p.uvs[2 * i + 1]; }
      }
      for (let k = 0; k < p.indices.length; k++) idx[i0 + k] = p.indices[k] + v0;
      placed.set(p, { offset: i0 * idx.BYTES_PER_ELEMENT, count: p.indices.length });
      v0 += n; i0 += p.indices.length;
    }
    h.vao = gl.createVertexArray();
    gl.bindVertexArray(h.vao);
    h.vbo = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, h.vbo);
    gl.bufferData(gl.ARRAY_BUFFER, verts, gl.STATIC_DRAW);
    const S = MODEL_STRIDE * 4;
    for (const [name, k, off] of [['aPos', 3, 0], ['aNormal', 3, 3], ['aUV', 2, 6]]) {
      const loc = gl.getAttribLocation(this.modelProg.p, name);
      if (loc < 0) continue;
      gl.enableVertexAttribArray(loc);
      gl.vertexAttribPointer(loc, k, gl.FLOAT, false, S, off * 4);
    }
    h.ibo = gl.createBuffer();
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, h.ibo);
    gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, idx, gl.STATIC_DRAW);
    gl.bindVertexArray(null);
    h.geomBytes = verts.byteLength + idx.byteLength;
    h.bytes = h.geomBytes;
    h.meshes = model.meshes.map((prs) => prs.map((p) => {
      const mat = { ...MODEL_DEFAULT_MATERIAL, ...(model.materials[p.material] || {}) };
      const at = placed.get(p);
      return { offset: at.offset, count: at.count, type, color: Float32Array.from(mat.baseColor), texture: mat.texture,
        mr: new Float32Array([mat.metallic, mat.roughness]), mrTexture: mat.mrTexture,
        emissive: Float32Array.from(mat.emissive), emissiveTexture: mat.emissiveTexture,
        alphaMode: MODEL_ALPHA_MODES[mat.alphaMode] || 0, cutoff: mat.alphaCutoff, doubleSided: !!mat.doubleSided };
    }));
    h.blended = h.meshes.some((prs) => prs.some((p) => p.alphaMode === 2));
    h.singleSided = h.meshes.some((prs) => prs.some((p) => !p.doubleSided));
  }

  /**
   * The controller models of this frame for one eye: `list[0..count)` of
   * {handle, rig} (controller-profiles.js), each node's world matrix in
   * rig.world, hidden nodes (rig.drawn 0) left out. Depth-tested among
   * themselves and with the hands' mesh. Opaque and masked primitives first,
   * with blending off (they write alpha 1: the XR layer has an alpha
   * channel), then blended ones (premultiplied, no depth writes). Single-sided
   * materials are culled, as glTF says (front faces clockwise under a
   * mirroring node). Blending and depth test are left as they were found (the
   * page draws with ONE, ONE_MINUS_SRC_ALPHA, which the blended pass sets),
   * culling and the front face; the cull mode is set, and then restored,
   * only when a model has single-sided materials (the Touch Plus has none).
   * Allocates nothing.
   */
  drawModels(view, list, count = list.length) {
    const gl = this.gl;
    this.modelDraws = 0;
    if (!count) return;
    const blend = gl.isEnabled(gl.BLEND), depth = gl.isEnabled(gl.DEPTH_TEST);
    // Culling and the front face, restored after: one state query an eye (the
    // front face, which gl_FrontFacing depends on), one more when a model has
    // single-sided materials (the cull mode; the Touch Plus has none).
    const face = this._faceState;
    face.cull = gl.isEnabled(gl.CULL_FACE);
    face.front = gl.getParameter(gl.FRONT_FACE);
    face.saved = false;
    for (let k = 0; k < count && !face.saved; k++) {
      if (!list[k].handle.singleSided) continue;
      face.saved = true;
      face.mode = gl.getParameter(gl.CULL_FACE_MODE);
      gl.cullFace(gl.BACK);
    }
    const { u } = this.modelProg;
    gl.useProgram(this.modelProg.p);
    mulInto(this._vp, view.proj, view.view);
    gl.uniformMatrix4fv(u.uVP, false, this._vp);
    gl.uniform3fv(u.uEye, eyeOf(view.view, this._eye));
    gl.uniform1i(u.uTex, 0);
    gl.uniform1i(u.uMRTex, 1);
    gl.uniform1i(u.uEmTex, 2);
    gl.enable(gl.DEPTH_TEST);
    gl.depthFunc(gl.LEQUAL);
    gl.depthMask(true);
    gl.disable(gl.BLEND);
    let blended = false;
    for (let k = 0; k < count; k++) blended = this._drawModel(list[k], false) || blended;
    if (blended) {
      gl.enable(gl.BLEND);
      gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA);
      gl.depthMask(false);
      for (let k = 0; k < count; k++) this._drawModel(list[k], true);
      gl.depthMask(true);
    }
    gl.bindVertexArray(null);
    gl.activeTexture(gl.TEXTURE0);
    if (blend) gl.enable(gl.BLEND); else gl.disable(gl.BLEND);
    if (depth) gl.enable(gl.DEPTH_TEST); else gl.disable(gl.DEPTH_TEST);
    if (face.cull) gl.enable(gl.CULL_FACE); else gl.disable(gl.CULL_FACE);
    gl.frontFace(face.front);
    if (face.saved) gl.cullFace(face.mode);
  }

  /** A model's texture of image `i` sampled as `kind` ('srgb' or 'linear'), else white. */
  _modelTex(h, i, kind) {
    return (i >= 0 && h.textures[i] && h.textures[i][kind]) || this.whiteTex;
  }

  /** One model's primitives of one pass (blended or not); true when it has blended ones. */
  _drawModel({ handle: h, rig }, blendPass) {
    const gl = this.gl;
    const { u } = this.modelProg;
    if (blendPass && !h.blended) return false;
    gl.bindVertexArray(h.vao);
    const nodes = rig.model.nodes;
    for (const i of rig.order) {
      const mi = nodes[i].mesh;
      if (mi < 0 || !rig.drawn[i]) continue;
      const world = rig.world[i];
      let set = false;
      for (const p of h.meshes[mi]) {
        if ((p.alphaMode === 2) !== blendPass) continue;
        if (!set) {
          gl.uniformMatrix4fv(u.uModel, false, world);
          gl.frontFace(mirrors(world) ? gl.CW : gl.CCW);
          set = true;
        }
        if (p.doubleSided) gl.disable(gl.CULL_FACE); else gl.enable(gl.CULL_FACE);
        gl.uniform4fv(u.uColor, p.color);
        gl.uniform2fv(u.uMR, p.mr);
        gl.uniform3fv(u.uEmissive, p.emissive);
        gl.uniform1i(u.uAlphaMode, p.alphaMode);
        gl.uniform1f(u.uCutoff, p.cutoff);
        gl.activeTexture(gl.TEXTURE0); gl.bindTexture(gl.TEXTURE_2D, this._modelTex(h, p.texture, 'srgb'));
        gl.activeTexture(gl.TEXTURE1); gl.bindTexture(gl.TEXTURE_2D, this._modelTex(h, p.mrTexture, 'linear'));
        gl.activeTexture(gl.TEXTURE2); gl.bindTexture(gl.TEXTURE_2D, this._modelTex(h, p.emissiveTexture, 'srgb'));
        gl.drawElements(gl.TRIANGLES, p.count, p.type, p.offset);
        this.modelDraws++;
      }
    }
    return h.blended;
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

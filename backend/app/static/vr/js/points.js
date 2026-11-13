// Point tables in the headset (issue #190): an even sample of the table's
// rows within a budget (/v1/points/sample_bin, 8 bytes a row), drawn on the
// headset's own GPU as points or additive splats, coloured by a column through
// the page's colour maps; while the view is still the backend's point render
// session draws every row for the eyes (render_session/_stereo_points.py) and
// the page blends that frame in, as for a cube.
//
// Model space, as for a cube: a box centred on the origin, its longest side 1,
// over the sample's bounding box (dataToModel). The backend gets the same map
// with every pose (data_to_model), so its frames land on the page's points.
// The pure functions are unit-tested (backend/tests/vr/layers.test.mjs).

import * as M from './mat4.js';

/** Rows drawn, per budget: the headset's (a Quest 3 at 72-90 Hz in stereo) and
 *  a desktop browser's. Measured in docs/user-guide/vr-headset.md. */
export const BUDGETS = {
  'quest-low': 500_000, quest: 1_000_000, 'quest-high': 2_000_000,
  desktop: 5_000_000, 'desktop-high': 10_000_000,
};

const Q = 65535;

/**
 * The rows to draw next frame: the sample arrives in a low-discrepancy (van
 * der Corput) order, every prefix spread over the whole sample (app/
 * point_sample.py prefix_order: no gap wider than 2 n / m), so when the
 * GPU time of a frame (`gpuMs`, both eyes) passes `share` of the frame period
 * fewer rows are drawn, and more again when there is room, never below
 * `min` nor above the sample. A negative gpuMs (no timer query) keeps the count.
 */
export function governRows(current, sampleRows, gpuMs, targetHz, { share = 0.6, min = 100_000 } = {}) {
  if (!(gpuMs > 0) || !(targetHz > 0)) return Math.min(current, sampleRows);
  const budget = (1000 / targetHz) * share;
  const f = Math.max(0.6, Math.min(1.1, budget / gpuMs));
  return Math.round(Math.max(Math.min(min, sampleRows), Math.min(sampleRows, current * f)));
}

/** The page's box for the bounds [xmin, xmax, ymin, ymax, zmin, zmax]:
 *  {centre, side, extent (box sides), matrix: data -> model, column-major}. */
export function dataToModel(bounds) {
  const lo = [bounds[0], bounds[2], bounds[4]], hi = [bounds[1], bounds[3], bounds[5]];
  const span = [0, 1, 2].map((i) => hi[i] - lo[i]);
  const side = Math.max(...span, 1e-30);
  const centre = [0, 1, 2].map((i) => 0.5 * (lo[i] + hi[i]));
  const s = 1 / side;
  const matrix = new Float64Array([s, 0, 0, 0, 0, s, 0, 0, 0, 0, s, 0,
    -centre[0] * s, -centre[1] * s, -centre[2] * s, 1]);
  return { centre, side, extent: span.map((v) => Math.max(v * s, 1e-6)), matrix, lo, hi };
}

/** Headers of a sample (a fetch Response's, or a plain object of them). */
export function sampleInfo(get) {
  const num = (k, d = 0) => { const v = get(k); return v === null || v === undefined || v === '' ? d : Number(v); };
  const list = (k) => (get(k) || '').split(',').filter((v) => v !== '').map(Number);
  const field = get('X-Visivo-Color-Field') || null;
  return {
    count: num('X-Visivo-Count'), totalRows: num('X-Visivo-Total-Rows'), step: num('X-Visivo-Step', 1),
    bounds: list('X-Visivo-Bounds'), axes: (get('X-Visivo-Axes') || 'X,Y,Z').split(','),
    colourField: field, colourRange: field ? list('X-Visivo-Color-Range') : null,
    colourLog: get('X-Visivo-Color-Log') === '1', dataRange: field ? list('X-Visivo-Data-Range') : null,
  };
}

/** The sample's arrays over its body: positions (3 per row) then colours. */
export function sampleArrays(buf, info) {
  const n = info.count;
  const positions = new Uint16Array(buf, 0, 3 * n);
  const colour = info.colourField && buf.byteLength >= 8 * n ? new Uint16Array(buf, 6 * n, n) : null;
  return { positions, colour };
}

/** Data coordinates of row i (a check of the quantisation, and the picks). */
export function rowPosition(positions, i, bounds) {
  return [0, 1, 2].map((a) => bounds[2 * a] + (positions[3 * i + a] / Q) * (bounds[2 * a + 1] - bounds[2 * a]));
}

/** The data value a colour code stands for (log: 10 ** the log value). */
export function codeValue(code, range, log) {
  const [a, b] = log ? range.map(Math.log10) : range;
  const v = a + (code / Q) * (b - a);
  return log ? 10 ** v : v;
}

/**
 * The page's colours as the point session's `set_color` points: `lut`, the
 * 256 x RGBA8 table; the codes' range `range` [cmin, cmax] (data values) with
 * the page's own window over it, [c0, c1] fractions; log as the sample's.
 * Points sit at data values, evenly in the log of the value on a log scale
 * (the session interpolates them on that scale, set_color's log).
 */
export function colourCommand(field, range, log, lut, c0 = 0, c1 = 1, n = 32) {
  if (!field || !range) return { type: 'set_color', field: null };
  const [a, b] = log ? range.map(Math.log10) : range;
  const at = (f) => { const v = a + f * (b - a); return log ? 10 ** v : v; };
  const lo = Math.min(c0, c1 - 1e-6), hi = Math.max(c1, lo + 1e-6);
  const lutAt = (L) => {
    const x = Math.max(0, Math.min(255, L * 255));
    const i = Math.min(254, Math.floor(x)), t = x - i;
    return [0, 1, 2].map((c) => ((1 - t) * lut[i * 4 + c] + t * lut[(i + 1) * 4 + c]) / 255);
  };
  const points = [];
  for (let j = 0; j <= n; j++) {
    const L = j / n;
    points.push([at(lo + L * (hi - lo)), ...lutAt(L)]);
  }
  return { type: 'set_color', field, min: at(lo), max: at(hi), points, log };
}

/**
 * Splat radius (data units) and opacity, as the desktop and the point session
 * size them (render_session/points.py gaussian_splat): spacing ∛(volume / N)
 * of the box the points fill, radius = spacing × size / 8; opacity
 * min(0.9, 20 / D) × intensity / 100, D = longest side / spacing. N is the
 * table's rows: a sample of fewer rows is drawn with splats larger by
 * ∛(rows per sample row), so it covers the same volume.
 */
export function splatStyle(bounds, totalRows, sampleRows, size, intensity) {
  const dims = [bounds[1] - bounds[0], bounds[3] - bounds[2], bounds[5] - bounds[4]];
  const longest = Math.max(...dims, 1e-6);
  const volume = dims.reduce((p, d) => p * Math.max(d, longest * 1e-3), 1);
  const spacing = Math.cbrt(volume / Math.max(1, totalRows));
  const radius = (spacing * size) / 8;
  const depth = Math.max(longest / spacing, 1);
  const opacity = Math.min(Math.max(Math.min(0.9, 20 / depth) * intensity / 100, 0.05), 0.9);
  const grow = Math.cbrt(Math.max(1, totalRows / Math.max(1, sampleRows)));
  return { radius, opacity, sampleRadius: radius * grow };
}

/** The commands that give the backend's point session the page's look. */
export function pointLook(p) {
  return [
    { type: 'set_point_fields', x: p.axes[0], y: p.axes[1], z: p.axes[2] },
    colourCommand(p.colourField, p.colourRange, p.colourLog, p.lut, p.c0, p.c1),
    { type: 'set_point_style', mode: p.style === 'splats' ? 'gaussian' : 'points',
      size: p.style === 'splats' ? p.splatSize : Math.max(1, Math.round(p.pointPx)), intensity: p.intensity },
    { type: 'set_show_outline', visible: false },
  ];
}

/** Box edges and ticks in data units ({segs, labels}) for a box `d` (dataToModel). */
export function boxDecorations(d, axes, niceTicks) {
  const e = d.extent, h = e.map((v) => v / 2);
  const segs = [], labels = [];
  const col = [0.55, 0.6, 0.7, 0.45];
  const c = (x, y, z) => [x ? h[0] : -h[0], y ? h[1] : -h[1], z ? h[2] : -h[2]];
  const E = [[0, 0, 0, 1, 0, 0], [0, 1, 0, 1, 1, 0], [0, 0, 1, 1, 0, 1], [0, 1, 1, 1, 1, 1],
    [0, 0, 0, 0, 1, 0], [1, 0, 0, 1, 1, 0], [0, 0, 1, 0, 1, 1], [1, 0, 1, 1, 1, 1],
    [0, 0, 0, 0, 0, 1], [1, 0, 0, 1, 0, 1], [0, 1, 0, 0, 1, 1], [1, 1, 0, 1, 1, 1]];
  for (const [a, b, g, x, y, z] of E) segs.push({ a: c(a, b, g), b: c(x, y, z), color: col });
  const L = 0.02, H = 0.03, tick = [0.85, 0.88, 0.95, 0.9];
  const fmt = (v) => (Math.abs(v) >= 1e4 || (Math.abs(v) < 1e-2 && v !== 0) ? v.toExponential(1) : String(+v.toPrecision(4)));
  const toModel = (i, v) => (v - d.centre[i]) / d.side;
  for (const i of [0, 1, 2]) {
    const nt = Math.max(2, Math.min(5, Math.round(e[i] / 0.25)));
    for (const v of niceTicks(d.lo[i], d.hi[i], nt)) {
      const m = toModel(i, v);
      if (m < -h[i] - 1e-9 || m > h[i] + 1e-9) continue;
      const p = i === 0 ? [m, -h[1], h[2]] : i === 1 ? [-h[0], m, h[2]] : [h[0], -h[1], m];
      const q = i === 0 ? [m, -h[1] - L, h[2]] : i === 1 ? [-h[0] - L, m, h[2]] : [h[0] + L, -h[1], m];
      segs.push({ a: p, b: q, color: tick });
      const lp = i === 0 ? [m, -h[1] - 2.2 * L, h[2]] : i === 1 ? [-h[0] - 1.6 * L, m, h[2]] : [h[0] + 1.6 * L, -h[1], m];
      labels.push({ pos: lp, text: fmt(v), height: H, anchor: i === 1 ? 'right' : i === 2 ? 'left' : 'center', ...(i === 2 ? { face: 'side' } : {}) });
    }
  }
  const title = [[0, -h[1] - 4.5 * L, h[2]], [-h[0], h[1] + 2.2 * L, h[2]], [h[0] + 1.6 * L, -h[1] - 3 * L, 0]];
  axes.forEach((name, i) => labels.push({ pos: title[i], text: name, height: H * 1.1,
    anchor: i === 0 ? 'center' : 'left', color: '#9fd0ff', ...(i === 2 ? { face: 'side' } : {}) }));
  return { segs, labels };
}

// ── GL ─────────────────────────────────────────────────────────────────────

const VS = `#version 300 es
precision highp float;
in vec3 aPos;          // 0..1 over the bounds (normalised uint16)
in float aVal;         // 0..1 colour code
uniform mat4 uMV;
uniform mat4 uP;
uniform vec3 uLo;      // model space of code 0 and the span to code 1
uniform vec3 uSpan;
uniform float uPx;     // points: pixels; splats: 0
uniform float uRadius; // splats: world radius (metres) * projection scale
uniform float uMaxPx;
uniform float uC0;
uniform float uC1;
out float vT;
out float vSplat;
void main() {
  vec4 eye = uMV * vec4(uLo + aPos * uSpan, 1.0);
  gl_Position = uP * eye;
  float px = uPx > 0.0 ? uPx : uRadius / max(1e-4, -eye.z);
  gl_PointSize = clamp(px, 1.0, uMaxPx);
  vSplat = uPx > 0.0 ? 0.0 : clamp(px, 0.0, 1.0);   // sub-pixel splats: their light, not a pixel's
  vT = clamp((aVal - uC0) / max(1e-6, uC1 - uC0), 0.0, 1.0);
}`;

const FS = `#version 300 es
precision mediump float;
in float vT;
in float vSplat;
uniform sampler2D uLut;
uniform float uSolid;
uniform float uOpacity;
uniform int uMode;     // 0 points, 1 splats
uniform int uRound;    // round points (a discard, which costs the early depth test)
out vec4 outColor;
void main() {
  vec3 c = uSolid > 0.5 ? vec3(0.8, 0.85, 1.0) : texture(uLut, vec2(vT, 0.5)).rgb;
  if (uMode == 0) {
    if (uRound == 1) { vec2 d = gl_PointCoord - 0.5; if (dot(d, d) > 0.25) discard; }
    outColor = vec4(c, 1.0);
  } else {
    vec2 d = (gl_PointCoord - 0.5) * 6.0;      // out to 3 sigma, as the backend's splats
    float g = exp(-0.5 * dot(d, d)) * uOpacity * max(vSplat, 0.25);
    outColor = vec4(c * g, g);
  }
}`;

function compile(gl, vs, fs) {
  const p = gl.createProgram();
  for (const [type, src] of [[gl.VERTEX_SHADER, vs], [gl.FRAGMENT_SHADER, fs]]) {
    const s = gl.createShader(type);
    gl.shaderSource(s, src);
    gl.compileShader(s);
    if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) throw new Error(`points shader: ${gl.getShaderInfoLog(s)}`);
    gl.attachShader(p, s);
  }
  gl.bindAttribLocation(p, 0, 'aPos');
  gl.bindAttribLocation(p, 1, 'aVal');
  gl.linkProgram(p);
  if (!gl.getProgramParameter(p, gl.LINK_STATUS)) throw new Error(`points link: ${gl.getProgramInfoLog(p)}`);
  const u = {};
  const n = gl.getProgramParameter(p, gl.ACTIVE_UNIFORMS);
  for (let i = 0; i < n; i++) { const info = gl.getActiveUniform(p, i); u[info.name] = gl.getUniformLocation(p, info.name); }
  return { p, u };
}

/** The sample on the GPU and its drawing. */
export class PointCloud {
  constructor(gl, lutTexture) {
    this.gl = gl;
    this.lut = lutTexture;
    this.prog = compile(gl, VS, FS);
    this.vao = gl.createVertexArray();
    this.posBuf = gl.createBuffer();
    this.valBuf = gl.createBuffer();
    this.count = 0;
    this.hasColour = false;
    this.maxPx = Math.min(64, gl.getParameter(gl.ALIASED_POINT_SIZE_RANGE)[1] || 64);
    this.bytes = 0;
  }

  /** Uploads a sample; returns the milliseconds it took (CPU side). */
  upload(positions, colour) {
    const gl = this.gl, t0 = performance.now();
    gl.bindVertexArray(this.vao);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.posBuf);
    gl.bufferData(gl.ARRAY_BUFFER, positions, gl.STATIC_DRAW);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 3, gl.UNSIGNED_SHORT, true, 6, 0);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.valBuf);
    gl.bufferData(gl.ARRAY_BUFFER, colour || new Uint16Array(positions.length / 3), gl.STATIC_DRAW);
    gl.enableVertexAttribArray(1);
    gl.vertexAttribPointer(1, 1, gl.UNSIGNED_SHORT, true, 2, 0);
    gl.bindVertexArray(null);
    this.count = positions.length / 3;
    this.hasColour = !!colour;
    this.bytes = positions.byteLength + (colour ? colour.byteLength : 2 * this.count);
    return performance.now() - t0;
  }

  /**
   * One eye. `p`: {box (dataToModel), bounds, style 'points' | 'splats', pointPx,
   * radius (data units), opacity, c0, c1, viewportH, count (rows to draw)}.
   */
  draw(view, model, p) {
    const gl = this.gl;
    if (!this.count) return;
    const { u } = this.prog;
    gl.useProgram(this.prog.p);
    gl.uniformMatrix4fv(u.uMV, false, M.multiply(view.view, model));
    gl.uniformMatrix4fv(u.uP, false, view.proj);
    const b = p.box;
    const lo = [0, 1, 2].map((i) => (b.lo[i] - b.centre[i]) / b.side);
    const span = [0, 1, 2].map((i) => (b.hi[i] - b.lo[i]) / b.side);
    gl.uniform3fv(u.uLo, lo);
    gl.uniform3fv(u.uSpan, span);
    const splats = p.style === 'splats';
    // A splat's diameter in pixels: 2 r (model) x model scale x P11 x H / 2,
    // over the eye distance; drawn out to 3 sigma, as the session's (r = sigma).
    const scale = M.uniformScale(model);
    const projScale = view.proj[5] * p.viewportH * 0.5;
    gl.uniform1f(u.uPx, splats ? 0 : p.pointPx);
    gl.uniform1f(u.uRadius, splats ? 6 * (p.radius / b.side) * scale * projScale : 0);
    gl.uniform1f(u.uMaxPx, this.maxPx);
    gl.uniform1f(u.uC0, p.c0);
    gl.uniform1f(u.uC1, p.c1);
    gl.uniform1f(u.uSolid, this.hasColour ? 0 : 1);
    gl.uniform1f(u.uOpacity, p.opacity);
    gl.uniform1i(u.uMode, splats ? 1 : 0);
    // Squares up to 3 px (no discard: the early depth test stays on); discs above.
    gl.uniform1i(u.uRound, !splats && p.pointPx > 3 ? 1 : 0);
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, this.lut);
    gl.uniform1i(u.uLut, 0);
    if (splats) {
      gl.enable(gl.BLEND);
      gl.blendFunc(gl.ONE, gl.ONE);
      gl.disable(gl.DEPTH_TEST);
      gl.depthMask(false);
    } else {
      gl.disable(gl.BLEND);
      gl.enable(gl.DEPTH_TEST);
      gl.depthFunc(gl.LESS);
      gl.depthMask(true);
    }
    gl.bindVertexArray(this.vao);
    gl.drawArrays(gl.POINTS, 0, Math.min(this.count, p.count ?? this.count));
    gl.bindVertexArray(null);
    gl.depthMask(true);
    gl.disable(gl.DEPTH_TEST);
  }
}

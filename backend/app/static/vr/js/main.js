// VisIVO VR page (issue #167): a reduced cube rendered on the headset's GPU.
// Desktop mode (no XR) for development; WebXR immersive-vr / immersive-ar on
// the Quest. See docs/user-guide/vr-headset.md.

import * as M from './mat4.js';
import { Renderer } from './renderer.js';
import { Backend, takeToken, saveToken, tokenError } from './api.js';
import * as V from './volume.js';
import { COLORMAP_NAMES } from './colormaps.js';

const $ = (id) => document.getElementById(id);
const query = new URLSearchParams(window.location.search);

const canvas = $('gl');
const gl = canvas.getContext('webgl2', {
  xrCompatible: true, antialias: false, alpha: true, premultipliedAlpha: true,
  preserveDrawingBuffer: query.has('bench'),
});
if (!gl) {
  document.body.innerHTML = '<p style="padding:2em">This browser has no WebGL2: the VR page needs it.</p>';
  throw new Error('no WebGL2');
}
const R = new Renderer(gl);

// ── State ───────────────────────────────────────────────────────────────────
const state = {
  info: null,          // /v1/cube/reduced_info answer (or the synthetic one)
  cube: null,          // {data, dims, encoding, lo, hi, factors, mode, reducedThreshold, ...}
  thrNorm: 0.1, cmaxNorm: 1, gamma: 0.5, opacity: 0.4,
  spv: 1, skip: true, governorOn: true, mode: 0, zStretch: 1,
  colormap: 'viridis',
  clip: { min: [0, 0, 0], max: [1, 1, 1] },
  extent: [1, 1, 1],
  texScale: [1, 1, 1],   // box coordinate -> texture coordinate (partial last blocks)
  model: M.identity(),
  moving: false,
  hud: true,
  load: {},            // load timings
};
const governor = new V.Governor({ start: 1, max: 1 });
const stats = new V.FrameStats(240);
let backend = null;

// ── Panel wiring ────────────────────────────────────────────────────────────
for (const name of COLORMAP_NAMES) {
  const o = document.createElement('option');
  o.value = o.textContent = name;
  $('cmap').appendChild(o);
}

const tokenField = $('token');
tokenField.value = takeToken();
if (tokenError()) setTimeout(() => setStatus(tokenError()), 0);
tokenField.addEventListener('change', () => saveToken(tokenField.value.trim(), $('remember').checked));
$('remember').addEventListener('change', () => saveToken(tokenField.value.trim(), $('remember').checked));
$('toggle-panel').addEventListener('click', () => $('panel').classList.toggle('collapsed'));

function setStatus(text) { $('load-status').textContent = text; }

function ensureBackend() {
  const token = tokenField.value.trim();
  if (!token) throw new Error('Paste the backend token first (or open the page with #token=…).');
  if (!backend || backend.token !== token) backend = new Backend(token);
  return backend;
}

$('browse').addEventListener('click', async () => {
  try {
    const list = $('files');
    const b = ensureBackend();
    const show = async (path) => {
      const r = await b.listFiles(path);
      if (!r.valid) throw new Error(r.error);
      list.hidden = false;
      list.innerHTML = '';
      const up = document.createElement('li');
      up.textContent = '..';
      up.className = 'dir';
      const fail = (err) => setStatus(String(err.message || err));
      up.onclick = () => show(r.current_path.replace(/\/[^/]+\/?$/, '') || '/').catch(fail);
      list.appendChild(up);
      for (const e of r.entries) {
        if (e.type !== 'directory' && !e.is_fits) continue;
        const li = document.createElement('li');
        li.textContent = e.name;
        li.className = e.type === 'directory' ? 'dir' : '';
        li.onclick = () => {
          if (e.type === 'directory') show(e.path).catch(fail);
          else { $('path').value = e.path; list.hidden = true; }
        };
        list.appendChild(li);
      }
    };
    await show($('path').value.replace(/\/[^/]*$/, ''));
  } catch (err) { setStatus(String(err.message || err)); }
});

$('load').addEventListener('click', () => loadFromBackend().catch((err) => setStatus(`Failed: ${err.message || err}`)));
$('synthetic').addEventListener('click', () => loadSynthetic(Number($('synthetic-size').value)));

function readNumber(id) { return Number($(id).value); }

$('thr').addEventListener('input', () => { state.thrNorm = sliderToNorm(readNumber('thr')); updateLabels(); });
$('cmax').addEventListener('input', () => { state.cmaxNorm = sliderToNorm(readNumber('cmax')); updateLabels(); });
$('thr-source').addEventListener('change', () => resetThreshold());
$('thr-reset').addEventListener('click', () => resetThreshold());
$('cmap').addEventListener('change', () => { state.colormap = $('cmap').value; R.setColormap(state.colormap); });
$('gamma').addEventListener('change', () => { state.gamma = readNumber('gamma'); });
$('opacity').addEventListener('input', () => { state.opacity = readNumber('opacity') / 100; });
$('rmode').addEventListener('change', () => { state.mode = readNumber('rmode'); });
$('spv').addEventListener('change', () => {
  state.spv = readNumber('spv');
  governor.max = state.spv;
  governor.value = state.spv;
});
$('skip').addEventListener('change', () => { state.skip = $('skip').checked; });
$('governor').addEventListener('change', () => { state.governorOn = $('governor').checked; governor.value = state.spv; });
$('zstretch').addEventListener('input', () => {
  state.zStretch = readNumber('zstretch') / 100;
  applyExtent();
});

// Clipping box sliders (texture coordinates 0..1 per axis).
const clipInputs = [];
for (const [i, axis] of ['x', 'y', 'z'].entries()) {
  for (const [end, val] of [['min', 0], ['max', 1000]]) {
    const label = document.createElement('label');
    const span = document.createElement('span');
    span.className = 'value';
    const input = Object.assign(document.createElement('input'), { type: 'range', min: 0, max: 1000, value: val });
    label.append(`${axis} ${end} `, input, span);
    $('clip').appendChild(label);
    input.addEventListener('input', () => {
      state.clip[end][i] = Number(input.value) / 1000;
      if (state.clip.min[i] > state.clip.max[i] - 0.01) {
        if (end === 'min') state.clip.min[i] = state.clip.max[i] - 0.01;
        else state.clip.max[i] = state.clip.min[i] + 0.01;
      }
      rebuildDecorations();
      updateLabels();
    });
    clipInputs.push({ i, end, input, span });
  }
}

// Threshold and colour range sliders are quadratic: fine steps near the noise.
function sliderToNorm(s) { return (s / 1000) ** 2; }
function normToSlider(n) { return Math.round(Math.sqrt(Math.max(0, Math.min(1, n))) * 1000); }

function fmt(v) {
  if (!Number.isFinite(v)) return 'n/a';
  const a = Math.abs(v);
  return (a !== 0 && (a < 1e-2 || a >= 1e4)) ? v.toExponential(2) : v.toFixed(a < 1 ? 4 : 2);
}

function dataValue(norm) {
  const c = state.cube;
  return c ? V.normToData(norm, c.lo, c.hi) : norm;
}

function updateLabels() {
  const c = state.cube;
  const unit = c && c.bunit ? ` ${c.bunit}` : '';
  $('thr-value').textContent = `${fmt(dataValue(state.thrNorm))}${unit}`;
  $('cmax-value').textContent = `${fmt(dataValue(state.cmaxNorm))}${unit}`;
  $('zs-value').textContent = `×${state.zStretch.toFixed(2)}`;
  $('thr').value = normToSlider(state.thrNorm);
  $('cmax').value = normToSlider(state.cmaxNorm);
  for (const ci of clipInputs) {
    const t = state.clip[ci.end][ci.i];
    ci.input.value = Math.round(t * 1000);
    const axis = state.info && state.info.axes[['x', 'y', 'z'][ci.i]];
    let w = '';
    if (axis) {
      const arr = ci.i === 2 && axis.velocity_kms ? axis.velocity_kms : axis.world;
      const val = V.texToWorld(arr, V.boxToTex(t, state.info.full_shape[ci.i], state.info.factors[ci.i]));
      w = val === null || val === undefined ? '' : ` ${fmt(val)} ${ci.i === 2 && axis.velocity_kms ? 'km/s' : axis.unit || ''}`;
    }
    ci.span.textContent = w;
  }
  if (state.info) $('scale-text').textContent = V.velocityScaleText(state.info, state.zStretch);
}

function resetThreshold() {
  const c = state.cube;
  if (!c) return;
  const src = $('thr-source').value;
  const thr = src === 'cube' ? state.info.default_threshold : c.reducedThreshold;
  const fallback = state.info.default_threshold ?? c.reducedThreshold;
  const t = thr ?? fallback;
  state.thrNorm = t === null || t === undefined ? 0.1 : Math.max(0, V.dataToNorm(t, c.lo, c.hi));
  // Colour range: from the threshold to the 99th percentile of what is above it
  // (emission is sparse: a percentile of the whole cube is still noise).
  state.cmaxNorm = Math.max(state.thrNorm + 0.01,
    V.normQuantile(c.data, c.encoding, 0.99, 4_000_000, state.thrNorm));
  updateLabels();
}

// ── Loading ─────────────────────────────────────────────────────────────────
async function loadFromBackend() {
  const b = ensureBackend();
  const path = $('path').value.trim();
  if (!path) throw new Error('Give the path of a cube on the backend.');
  const t0 = performance.now();
  setStatus('Opening…');
  const open = await b.open(path);
  const req = {
    dataset_id: open.dataset_id,
    factor_xy: readNumber('fxy'), factor_z: readNumber('fz'), max_dim: readNumber('maxdim'),
    mode: $('mode').value, encoding: $('encoding').value,
  };
  setStatus('Reading the axes…');
  const info = await b.reducedInfo(req);
  const t1 = performance.now();
  // The backend caps voxels and bytes; the GPU caps each side. Refuse before
  // reading the cube rather than after.
  if (Math.max(...info.reduced_shape) > R.maxTex3D) {
    throw new Error(`${info.reduced_shape.join('×')} exceeds this GPU's 3-D texture limit `
      + `(${R.maxTex3D} per side): choose a larger binning.`);
  }
  setStatus(`Reducing ${info.full_shape.join('×')} by ${info.factors.join('×')} (${req.mode})…`);
  const cube = await b.reducedCube({ ...req, factor_xy: info.factors[0], factor_z: info.factors[2] },
    (f) => setStatus(`Transferring… ${(100 * f).toFixed(0)} %`));
  state.load = { openInfoMs: t1 - t0, reduceMs: cube.timing.serverMs, transferMs: cube.timing.totalMs - cube.timing.serverMs, mb: cube.bytes / 1e6 };
  installCube(info, cube, open);
}

function loadSynthetic(n) {
  const t0 = performance.now();
  const data = V.syntheticCube(n);
  const info = V.syntheticInfo(n);
  state.load = { generateMs: performance.now() - t0, mb: data.length / 1e6 };
  installCube(info, {
    data, dims: [n, n, n], encoding: 'uint8', lo: 0, hi: 1, factors: [1, 1, 1], mode: 'synthetic',
    reducedThreshold: 0.25, bunit: '', fullShape: [n, n, n],
  }, null);
}

function installCube(info, cube, open) {
  const up = R.setVolume(cube);
  state.info = info;
  state.cube = cube;
  state.open = open;
  state.load = { ...state.load, uploadMs: up.uploadMs, macroMs: up.macroMs, textureMB: up.textureBytes / 1e6 };
  state.clip = { min: [0, 0, 0], max: [1, 1, 1] };
  applyExtent();
  resetThreshold();
  const l = state.load;
  setStatus([
    `${cube.dims.join('×')} ${cube.encoding}, binned ${cube.factors.join('×')} (${cube.mode}) from ${cube.fullShape.join('×')}`,
    l.reduceMs !== undefined ? `reduce ${l.reduceMs.toFixed(0)} ms, transfer ${l.transferMs.toFixed(0)} ms (${l.mb.toFixed(1)} MB)` : `generated in ${l.generateMs.toFixed(0)} ms`,
    `upload ${l.uploadMs.toFixed(0)} ms, skip grid ${l.macroMs.toFixed(0)} ms`,
  ].join('\n'));
}

function applyExtent() {
  if (!state.info) return;
  state.extent = V.boxExtent(state.info.full_shape, state.zStretch);
  state.texScale = V.texScale(state.info.full_shape, state.info.factors, state.info.reduced_shape);
  rebuildDecorations();
  updateLabels();
}

// ── Axes, ticks, labels (model space: box centred at 0, sides = extent) ─────
function boxEdges(lo, hi, color, out) {
  const c = (x, y, z) => [x ? hi[0] : lo[0], y ? hi[1] : lo[1], z ? hi[2] : lo[2]];
  const e = [[0, 0, 0, 1, 0, 0], [0, 1, 0, 1, 1, 0], [0, 0, 1, 1, 0, 1], [0, 1, 1, 1, 1, 1],
    [0, 0, 0, 0, 1, 0], [1, 0, 0, 1, 1, 0], [0, 0, 1, 0, 1, 1], [1, 0, 1, 1, 1, 1],
    [0, 0, 0, 0, 0, 1], [1, 0, 0, 1, 0, 1], [0, 1, 0, 0, 1, 1], [1, 1, 0, 1, 1, 1]];
  for (const [a, b, cc, d, f, g] of e) out.push({ a: c(a, b, cc), b: c(d, f, g), color });
}

function rebuildDecorations() {
  const info = state.info;
  if (!info) return;
  const e = state.extent, h = e.map((v) => v / 2);
  const segs = [];
  boxEdges([-h[0], -h[1], -h[2]], h, [0.55, 0.6, 0.7, 0.45], segs);
  const cl = state.clip;
  const full = cl.min.every((v) => v <= 0) && cl.max.every((v) => v >= 1);
  if (!full) {
    boxEdges(cl.min.map((t, i) => (t - 0.5) * e[i]), cl.max.map((t, i) => (t - 0.5) * e[i]),
      [0.35, 0.75, 1, 0.95], segs);
  }
  const L = 0.02, H = 0.032, tickColor = [0.85, 0.88, 0.95, 0.9];
  const labels = [];
  // About one tick per quarter of the longest side, two at least: a short
  // velocity axis must not crowd its labels.
  const nt = (len) => Math.max(2, Math.min(5, Math.round(len / 0.25)));
  const ax = V.axisTicks(info.axes.x, 'x', nt(e[0])), ay = V.axisTicks(info.axes.y, 'y', nt(e[1]));
  const az = V.axisTicks(info.axes.z, 'z', nt(e[2]));
  // Tick positions from voxel-centre (texture-index) to box coordinates, through
  // the block centres, so a partial last block puts its label on its pixels.
  const inBox = (tk, i) => {
    const u = V.texToBox(tk.t, info.full_shape[i], info.factors[i]);
    return u >= 0 && u <= 1 ? { ...tk, t: u } : null;
  };
  ax.ticks = ax.ticks.map((tk) => inBox(tk, 0)).filter(Boolean);
  ay.ticks = ay.ticks.map((tk) => inBox(tk, 1)).filter(Boolean);
  az.ticks = az.ticks.map((tk) => inBox(tk, 2)).filter(Boolean);
  for (const tk of ax.ticks) {
    const p = [(tk.t - 0.5) * e[0], -h[1], h[2]];
    segs.push({ a: p, b: [p[0], p[1] - L, p[2]], color: tickColor });
    labels.push({ pos: [p[0], p[1] - 2.2 * L, p[2]], text: tk.label, height: H, anchor: 'center' });
  }
  labels.push({ pos: [0, -h[1] - 4.5 * L, h[2]], text: V.axisTitle(ax), height: H * 1.1, anchor: 'center', color: '#9fd0ff' });
  for (const tk of ay.ticks) {
    const p = [-h[0], (tk.t - 0.5) * e[1], h[2]];
    segs.push({ a: p, b: [p[0] - L, p[1], p[2]], color: tickColor });
    labels.push({ pos: [p[0] - 1.6 * L, p[1], p[2]], text: tk.label, height: H, anchor: 'right' });
  }
  labels.push({ pos: [-h[0], h[1] + 2.2 * L, h[2]], text: V.axisTitle(ay), height: H * 1.1, anchor: 'left', color: '#9fd0ff' });
  for (const tk of az.ticks) {
    const p = [h[0], -h[1], (tk.t - 0.5) * e[2]];
    segs.push({ a: p, b: [p[0] + L, p[1], p[2]], color: tickColor });
    labels.push({ pos: [p[0] + 1.6 * L, p[1], p[2]], text: tk.label, height: H, anchor: 'left' });
  }
  labels.push({ pos: [h[0] + 1.6 * L, -h[1] - 3 * L, 0], text: V.axisTitle(az), height: H * 1.1, anchor: 'left', color: '#9fd0ff' });
  const c = state.cube;
  if (c) {
    const bin = c.mode === 'synthetic' ? 'synthetic cube, not binned'
      : `binned ${c.factors.join('×')} (${c.mode}), ${c.dims.join('×')} of ${c.fullShape.join('×')}`;
    labels.push({ pos: [0, h[1] + 2.2 * L, -h[2]], text: bin, height: H, anchor: 'center', color: '#ffd27a' });
    labels.push({ pos: [0, h[1] + 4.6 * L, -h[2]], text: V.velocityScaleShort(info, state.zStretch), height: H * 0.9, anchor: 'center', color: '#c8ccd6' });
  }
  R.setLines(segs);
  R.setLabels(labels);
}

// ── Drawing ─────────────────────────────────────────────────────────────────
function effectiveSpv() {
  const base = state.governorOn ? governor.value : state.spv;
  return state.moving ? Math.max(0.25, base * 0.6) : base;
}

function volParams(override = {}) {
  return {
    extent: state.extent, boxMin: state.clip.min, boxMax: state.clip.max,
    thr: state.thrNorm, cmin: state.thrNorm, cmax: Math.max(state.cmaxNorm, state.thrNorm + 1e-3),
    gamma: state.gamma, opacity: state.opacity, spv: effectiveSpv(),
    mode: state.mode, skip: state.skip, maxSteps: 4096, texScale: state.texScale, ...override,
  };
}

function drawScene(view, override) {
  R.drawVolume(view, state.model, volParams(override));
  R.drawLines(view, state.model);
  R.drawLabels(view, state.model, M.uniformScale(state.model));
}

// ── HUD (frame-rate and latency overlay) ─────────────────────────────────────
const hudCanvas = Object.assign(document.createElement('canvas'), { width: 640, height: 400 });
const hudTex = R.createPanel();
let hudLast = 0;

function hudLines(targetHz) {
  const s = stats.summary(targetHz ? 1000 / targetHz : 0);
  const c = state.cube;
  const lines = [];
  lines.push(`FPS ${s.fps.toFixed(1)}${targetHz ? `  (display ${targetHz} Hz)` : ''}   missed ${(100 * s.missedFraction).toFixed(1)} %`);
  if (s.interval) lines.push(`frame ${s.interval.mean.toFixed(2)} ms mean, ${s.interval.p95.toFixed(2)} p95`);
  lines.push(`CPU ${s.cpu ? s.cpu.mean.toFixed(2) : 'n/a'} ms   GPU ${s.gpu ? `${s.gpu.mean.toFixed(2)} ms` : 'n/a (no timer query)'}`);
  if (c) {
    const si = V.stepInfo(c.dims, effectiveSpv());
    lines.push(`samples/voxel ${effectiveSpv().toFixed(2)}${state.governorOn ? ' (governor)' : ''}   max steps/ray ${si.maxSteps}`);
    lines.push(`volume ${c.dims.join('×')} ${c.encoding} ${(R.textureBytes / 1e6).toFixed(1)} MB`);
    lines.push(c.mode === 'synthetic' ? 'synthetic cube' : `binning ${c.factors.join('×')} ${c.mode} of ${c.fullShape.join('×')}`);
    lines.push(`threshold ${fmt(dataValue(state.thrNorm))} ${c.bunit || ''}  (${$('thr-source').selectedOptions[0].text})`);
  }
  if (xr) {
    const L = xr.layer;
    lines.push(`eye buffer ${L.framebufferWidth}×${L.framebufferHeight}  scale ${xr.fbScale}  foveation ${'fixedFoveation' in L ? L.fixedFoveation : 'n/a'}`);
    if (xr.bench) lines.push(xr.bench.status || '');
  } else {
    lines.push(`canvas ${canvas.width}×${canvas.height}`);
  }
  return lines;
}

function drawHudCanvas(lines) {
  const ctx = hudCanvas.getContext('2d');
  ctx.clearRect(0, 0, hudCanvas.width, hudCanvas.height);
  ctx.fillStyle = 'rgba(10, 14, 22, 0.82)';
  ctx.fillRect(0, 0, hudCanvas.width, hudCanvas.height);
  ctx.fillStyle = '#e6e9ef';
  ctx.font = '24px ui-monospace, Menlo, monospace';
  ctx.textBaseline = 'top';
  lines.forEach((l, i) => ctx.fillText(l, 14, 12 + i * 31));
}

// ── Desktop mode ────────────────────────────────────────────────────────────
const cam = { yaw: -0.5, pitch: 0.35, dist: 2.0 };
let dragging = null;
canvas.addEventListener('pointerdown', (e) => { dragging = { x: e.clientX, y: e.clientY }; canvas.setPointerCapture(e.pointerId); state.moving = true; });
canvas.addEventListener('pointermove', (e) => {
  if (!dragging) return;
  cam.yaw -= (e.clientX - dragging.x) * 0.008;
  cam.pitch = Math.max(-1.5, Math.min(1.5, cam.pitch - (e.clientY - dragging.y) * 0.008));
  dragging = { x: e.clientX, y: e.clientY };
});
canvas.addEventListener('pointerup', () => { dragging = null; state.moving = false; });
canvas.addEventListener('wheel', (e) => { cam.dist = Math.max(0.3, Math.min(8, cam.dist * Math.exp(e.deltaY * 0.001))); e.preventDefault(); }, { passive: false });

function desktopView() {
  const w = canvas.width, h = canvas.height;
  let camM = M.translation(0, 0, cam.dist);
  camM = M.multiply(M.rotation([1, 0, 0], -cam.pitch), camM);
  camM = M.multiply(M.rotation([0, 1, 0], cam.yaw), camM);
  return { proj: M.perspective(50 * Math.PI / 180, w / h, 0.02, 50), view: M.invert(camM) };
}

let lastDesktop = 0;
let desktopRunning = false;
function desktopFrame(time) {
  if (xr) { desktopRunning = false; return; }
  requestAnimationFrame(desktopFrame);
  const t0 = performance.now();
  const dpr = Math.min(2, window.devicePixelRatio || 1);
  const w = Math.floor(canvas.clientWidth * dpr), h = Math.floor(canvas.clientHeight * dpr);
  if (canvas.width !== w || canvas.height !== h) { canvas.width = w; canvas.height = h; }
  gl.bindFramebuffer(gl.FRAMEBUFFER, null);
  gl.viewport(0, 0, w, h);
  gl.clearColor(0.043, 0.051, 0.07, 1);
  gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
  state.model = M.identity();
  R.beginFrame();
  if (state.cube) drawScene(desktopView());
  R.endFrame();
  const interval = lastDesktop ? time - lastDesktop : 0;
  lastDesktop = time;
  stats.push(interval, performance.now() - t0, R.gpuMs);
  if (state.governorOn) governor.update(interval, 1000 / 60);
  if (time - hudLast > 250) { hudLast = time; $('hud-text').textContent = hudLines(0).join('\n'); }
}
function startDesktop() {
  if (!desktopRunning) { desktopRunning = true; lastDesktop = 0; requestAnimationFrame(desktopFrame); }
}

// ── WebXR ───────────────────────────────────────────────────────────────────
let xr = null;

async function checkXR() {
  if (!navigator.xr) {
    $('xr-status').textContent = window.isSecureContext ? 'no WebXR in this browser' : 'not a secure context: WebXR needs https or localhost';
    return;
  }
  const vr = await navigator.xr.isSessionSupported('immersive-vr').catch(() => false);
  const ar = await navigator.xr.isSessionSupported('immersive-ar').catch(() => false);
  $('xr-status').textContent = vr ? `WebXR ready${ar ? ' (VR and passthrough)' : ''}` : 'WebXR present, no headset';
  $('enter-vr').disabled = !vr;
  $('bench-xr').disabled = !vr;
  $('enter-ar').disabled = !ar;
}

$('enter-vr').addEventListener('click', () => enterXR('immersive-vr', false).catch((e) => setStatus(`XR: ${e.message}`)));
$('enter-ar').addEventListener('click', () => enterXR('immersive-ar', false).catch((e) => setStatus(`XR: ${e.message}`)));
$('bench-xr').addEventListener('click', () => enterXR('immersive-vr', true).catch((e) => setStatus(`XR: ${e.message}`)));

async function enterXR(mode, bench) {
  const session = await navigator.xr.requestSession(mode, { optionalFeatures: ['local-floor'] });
  await gl.makeXRCompatible();
  const fbScale = readNumber('fbscale');
  const layer = new XRWebGLLayer(session, gl, { framebufferScaleFactor: fbScale, antialias: false, alpha: mode === 'immersive-ar' });
  if ('fixedFoveation' in layer) layer.fixedFoveation = readNumber('foveation');
  session.updateRenderState({ baseLayer: layer, depthNear: 0.05, depthFar: 50 });
  let ref;
  try { ref = await session.requestReferenceSpace('local-floor'); } catch { ref = await session.requestReferenceSpace('local'); }
  let targetHz = 0;
  if (session.supportedFrameRates && session.updateTargetFrameRate) {
    const want = readNumber('rate');
    const rates = Array.from(session.supportedFrameRates);
    const pick = rates.reduce((a, b) => (Math.abs(b - want) < Math.abs(a - want) ? b : a), rates[0]);
    try { await session.updateTargetFrameRate(pick); } catch { /* keep the default */ }
  }
  targetHz = session.frameRate || readNumber('rate');
  xr = { session, ref, layer, mode, fbScale, targetHz, placed: false, last: 0, hands: {}, grab: null,
    buttons: {}, slab: { c: 0.5, w: 0.5 }, bench: bench ? makeBench() : null, place: null };
  stats.buf.length = stats.cpu.length = stats.gpu.length = 0;
  governor.max = state.spv; governor.value = state.spv;
  session.addEventListener('end', () => {
    const b = xr && xr.bench;
    xr = null;
    if (b) showBench(b);
    startDesktop();
  });
  session.requestAnimationFrame(onXRFrame);
}

function placeInFront(pose, dist = 0.75, scale = 0.6) {
  const m = pose.transform.matrix;
  const pos = [m[12], m[13], m[14]];
  const fwd = M.normalize([-m[8], 0, -m[10]]);
  const p = M.add(pos, M.add(M.scale3(fwd, dist), [0, -0.12, 0]));
  const yaw = Math.atan2(-fwd[0], -fwd[2]);
  return { p, yaw, scale };
}

function placedModel(pl, spin = 0) {
  let m = M.scaling(pl.scale);
  m = M.multiply(M.rotation([0, 1, 0], pl.yaw + spin), m);
  return M.multiply(M.translation(...pl.p), m);
}

function pressedEdge(key, pressed) {
  const was = xr.buttons[key];
  xr.buttons[key] = pressed;
  return pressed && !was;
}

function handleInput(frame, dtS) {
  const hands = {};
  for (const src of xr.session.inputSources) {
    if (!src.gripSpace || !src.gamepad) continue;
    const gp = frame.getPose(src.gripSpace, xr.ref);
    if (!gp) continue;
    const m = gp.transform.matrix;
    hands[src.handedness] = { m: new Float32Array(m), pos: [m[12], m[13], m[14]], g: src.gamepad };
  }
  xr.hands = hands;
  const sq = (h) => hands[h] && hands[h].g.buttons[1] && hands[h].g.buttons[1].pressed;
  const L = sq('left'), Rr = sq('right');
  if (xr.bench) { xr.grab = null; state.moving = false; return; }
  if (L && Rr) {
    if (!xr.grab || xr.grab.kind !== 'two') {
      xr.grab = { kind: 'two', model0: state.model, l0: hands.left.pos, r0: hands.right.pos };
    }
    state.model = M.grabTwo(xr.grab.model0, xr.grab.l0, xr.grab.r0, hands.left.pos, hands.right.pos);
  } else if (L || Rr) {
    const h = L ? 'left' : 'right';
    if (!xr.grab || xr.grab.kind !== h) xr.grab = { kind: h, rel: M.multiply(M.invert(hands[h].m), state.model) };
    state.model = M.grabOne(hands[h].m, xr.grab.rel);
  } else {
    xr.grab = null;
  }
  state.moving = !!xr.grab;

  const stick = (g) => (g.axes.length >= 4 ? [g.axes[2], g.axes[3]] : [g.axes[0] || 0, g.axes[1] || 0]);
  const dead = (v) => (Math.abs(v) < 0.15 ? 0 : v);
  let changed = false;
  if (hands.right) {
    const g = hands.right.g;
    const [sx, sy] = stick(g).map(dead);
    if (sy) { state.thrNorm = Math.max(0, Math.min(state.cmaxNorm - 1e-3, state.thrNorm - sy * dtS * 0.25 * Math.max(0.02, state.thrNorm))); changed = true; }
    if (sx) { state.opacity = Math.max(0.01, Math.min(3, state.opacity * Math.exp(sx * dtS))); }
    if (pressedEdge('rA', g.buttons[4] && g.buttons[4].pressed)) {
      const i = (COLORMAP_NAMES.indexOf(state.colormap) + 1) % COLORMAP_NAMES.length;
      state.colormap = COLORMAP_NAMES[i];
      R.setColormap(state.colormap);
      $('cmap').value = state.colormap;
    }
    if (pressedEdge('rB', g.buttons[5] && g.buttons[5].pressed)) {
      state.clip = { min: [0, 0, 0], max: [1, 1, 1] };
      xr.slab = { c: 0.5, w: 0.5 };
      rebuildDecorations(); changed = true;
    }
    if (pressedEdge('rStick', g.buttons[3] && g.buttons[3].pressed)) {
      state.governorOn = !state.governorOn; $('governor').checked = state.governorOn; governor.value = state.spv;
    }
  }
  if (hands.left) {
    const g = hands.left.g;
    const [sx, sy] = stick(g).map(dead);
    if (sx || sy) {
      // The velocity slab: up/down moves it along the spectral axis, left/right widens it.
      xr.slab.w = Math.max(0.01, Math.min(0.5, xr.slab.w * Math.exp(sx * dtS)));
      xr.slab.c = Math.max(xr.slab.w, Math.min(1 - xr.slab.w, xr.slab.c - sy * dtS * 0.3));
      state.clip.min[2] = xr.slab.c - xr.slab.w;
      state.clip.max[2] = xr.slab.c + xr.slab.w;
      xr.slabDirty = true;
      changed = true;
    }
    if (pressedEdge('lX', g.buttons[4] && g.buttons[4].pressed)) {
      state.mode = state.mode === 1 ? 0 : 1; $('rmode').value = String(state.mode);
    }
    if (pressedEdge('lY', g.buttons[5] && g.buttons[5].pressed)) state.hud = !state.hud;
  }
  if (xr.slabDirty && (performance.now() - (xr.slabBuilt || 0) > 100)) {
    rebuildDecorations(); xr.slabDirty = false; xr.slabBuilt = performance.now();
  }
  if (changed) xr.labelsDirty = true;
}

function controllerSegments() {
  const segs = [];
  for (const [hand, h] of Object.entries(xr.hands)) {
    const m = h.m;
    const p = (x, y, z) => M.transformPoint(m, [x, y, z]);
    const col = hand === 'left' ? [0.5, 0.8, 1, 1] : [1, 0.75, 0.4, 1];
    segs.push({ a: p(0, 0, 0), b: p(0, 0, -0.08), color: col });
    segs.push({ a: p(-0.015, 0, 0), b: p(0.015, 0, 0), color: col });
    segs.push({ a: p(0, -0.015, 0), b: p(0, 0.015, 0), color: col });
  }
  return segs;
}

function hudModel() {
  if (xr.hands.left) {
    let m = M.translation(0, 0.085, 0.01);
    m = M.multiply(m, M.rotation([1, 0, 0], -0.6));
    return M.multiply(xr.hands.left.m, m);
  }
  const pl = xr.place;
  const right = [Math.cos(pl.yaw), 0, -Math.sin(pl.yaw)];
  const p = M.add(pl.p, M.add(M.scale3(right, -0.5), [0, 0.0, 0]));
  return M.multiply(M.translation(...p), M.rotation([0, 1, 0], pl.yaw + 0.5));
}

function onXRFrame(time, frame) {
  if (!xr) return;
  const t0 = performance.now();
  const session = frame.session;
  session.requestAnimationFrame(onXRFrame);
  const pose = frame.getViewerPose(xr.ref);
  if (!pose) return;
  const interval = xr.last ? time - xr.last : 0;
  xr.last = time;
  const dtS = Math.min(0.1, interval / 1000);
  if (!xr.placed) {
    xr.place = placeInFront(pose);
    state.model = placedModel(xr.place);
    xr.placed = true;
  }
  handleInput(frame, dtS);
  if (xr.bench) benchStep(xr.bench, time, interval);

  const layer = xr.layer;
  gl.bindFramebuffer(gl.FRAMEBUFFER, layer.framebuffer);
  gl.clearColor(0, 0, 0, xr.mode === 'immersive-ar' ? 0 : 1);
  gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
  R.beginFrame();
  R.setLines(controllerSegments(), R.tmpLines);
  if (state.hud && time - hudLast > 250) {
    hudLast = time;
    const lines = hudLines(xr.targetHz);
    drawHudCanvas(lines);
    R.updatePanel(hudTex, hudCanvas);
    if (xr.labelsDirty) { updateLabels(); xr.labelsDirty = false; }
    $('hud-text').textContent = lines.join('\n');
  }
  const hm = state.hud ? hudModel() : null;
  for (const view of pose.views) {
    const vp = layer.getViewport(view);
    gl.viewport(vp.x, vp.y, vp.width, vp.height);
    const v = { proj: view.projectionMatrix, view: view.transform.inverse.matrix };
    if (state.cube) drawScene(v);
    R.drawWorldLines(v);
    if (hm) R.drawPanel(v, hm, hudTex, [0.16, 0.1]);
  }
  R.endFrame();
  const cpu = performance.now() - t0;
  stats.push(interval, cpu, R.gpuMs);
  if (xr.bench) xr.bench.record(interval, cpu, R.gpuMs);
  if (state.governorOn && !xr.bench) governor.update(interval, 1000 / xr.targetHz);
  if (session.frameRate) xr.targetHz = session.frameRate;
}

// ── Benchmarks ──────────────────────────────────────────────────────────────
// In the headset: synthetic cubes of four sizes at three sampling rates (and
// the loaded cube, if any), 0.75 s to settle then 3 s measured each, with the
// cube turning slowly in front of the viewer. Results on the HUD and, after
// leaving VR, on the page.
function benchPlan(includeLoaded) {
  const plan = [];
  for (const n of [128, 256, 384, 512]) for (const spv of [0.5, 1, 2]) plan.push({ n, spv });
  if (includeLoaded) for (const spv of [0.5, 1, 2]) plan.push({ n: 0, spv });
  return plan;
}

function makeBench() {
  const saved = state.cube ? { info: state.info, cube: state.cube, open: state.open } : null;
  const b = {
    plan: benchPlan(!!saved), i: -1, t0: 0, saved, results: [], status: 'benchmark starting',
    intervals: [], gpu: [], cpu: [], currentN: -1,
    savedParams: { spv: state.spv, governorOn: state.governorOn, mode: state.mode, skip: state.skip },
    record(interval, cpu, gpu) {
      if (this.i < 0 || this.measuring !== true) return;
      if (interval > 0) this.intervals.push(interval);
      this.cpu.push(cpu);
      if (gpu >= 0) this.gpu.push(gpu);
    },
  };
  state.governorOn = false;
  state.mode = 0;
  state.skip = true;
  return b;
}

function benchStep(b, time) {
  const cfg = b.plan[b.i];
  // A new configuration starts on the frame AFTER its cube was made, so the
  // seconds spent generating a 512^3 cube are not counted as a frame.
  if (b.t0 === null) b.t0 = time;
  const elapsed = time - b.t0;
  if (b.i < 0 || elapsed > 3750) {
    if (b.i >= 0) {
      const iv = V.FrameStats.stats(b.intervals);
      const target = 1000 / xr.targetHz;
      b.results.push({
        cube: cfg.n ? `synthetic ${cfg.n}^3` : `${state.cube.dims.join('x')} ${state.cube.mode}`,
        spv: cfg.spv, maxSteps: V.stepInfo(state.cube.dims, cfg.spv).maxSteps,
        fps: iv ? 1000 / iv.mean : 0, frameMean: iv ? iv.mean : 0, frameP95: iv ? iv.p95 : 0,
        missedPct: b.intervals.length ? (100 * b.intervals.filter((v) => v > target * 1.5).length) / b.intervals.length : 0,
        gpuMs: b.gpu.length ? b.gpu.reduce((a, v) => a + v, 0) / b.gpu.length : null,
        cpuMs: b.cpu.length ? b.cpu.reduce((a, v) => a + v, 0) / b.cpu.length : null,
        displayHz: xr.targetHz, eye: `${xr.layer.framebufferWidth}x${xr.layer.framebufferHeight}`,
        foveation: 'fixedFoveation' in xr.layer ? xr.layer.fixedFoveation : null,
      });
    }
    b.i++;
    if (b.i >= b.plan.length) {
      b.status = 'benchmark done: take the headset off and read the page';
      finishBench(b);
      xr.session.end();
      return;
    }
    const next = b.plan[b.i];
    if (next.n && next.n !== b.currentN) { loadSynthetic(next.n); b.currentN = next.n; }
    if (!next.n && b.saved) { R.setVolume(b.saved.cube); state.cube = b.saved.cube; state.info = b.saved.info; applyExtent(); resetThreshold(); b.currentN = 0; }
    state.spv = next.spv;
    b.t0 = null; b.intervals = []; b.gpu = []; b.cpu = []; b.measuring = false;
    b.status = `benchmark ${b.i + 1}/${b.plan.length}: preparing`;
    return;
  }
  b.measuring = (time - b.t0) > 750;
  const c = b.plan[b.i];
  b.status = `benchmark ${b.i + 1}/${b.plan.length}: ${c.n ? `${c.n}^3` : 'loaded cube'} at ${c.spv} samples/voxel`;
  state.model = placedModel(xr.place, (time / 1000) * 0.35);
}

function finishBench(b) {
  state.spv = b.savedParams.spv;
  state.governorOn = b.savedParams.governorOn;
  state.mode = b.savedParams.mode;
  state.skip = b.savedParams.skip;
  if (b.saved) {
    R.setVolume(b.saved.cube);
    state.cube = b.saved.cube; state.info = b.saved.info; state.open = b.saved.open;
    applyExtent(); resetThreshold();
  }
}

function benchTable(rows) {
  const head = 'cube | samples/voxel | max steps/ray | FPS | frame ms mean | p95 | missed % | GPU ms | CPU ms | display Hz | eye buffer | foveation';
  const lines = [head, head.replace(/[^|]/g, '-')];
  for (const r of rows) {
    lines.push([r.cube, r.spv, r.maxSteps, r.fps.toFixed(1), r.frameMean.toFixed(2), r.frameP95.toFixed(2),
      r.missedPct.toFixed(1), r.gpuMs === null ? 'n/a' : r.gpuMs.toFixed(2), r.cpuMs === null ? 'n/a' : r.cpuMs.toFixed(2),
      r.displayHz, r.eye, r.foveation ?? 'n/a'].join(' | '));
  }
  return lines.join('\n');
}

function showBench(b) {
  const text = `${navigator.userAgent}\n\n${benchTable(b.results)}`;
  $('bench-out').hidden = false;
  $('bench-out').textContent = text;
  $('bench-copy').hidden = false;
  $('bench-copy').onclick = () => navigator.clipboard && navigator.clipboard.writeText(text);
  window.__vrBench = b.results;
}

// Desktop benchmark (?bench): two eye-sized viewports side by side, each frame
// timed from a one-pixel readPixels before to one after (gl.finish does not
// wait in Chrome), less the same round trip around an empty frame, plus the
// mean number of steps per ray read back from a step-count frame.
async function desktopBench() {
  const eyeW = Number(query.get('eyew') || 1680), eyeH = Number(query.get('eyeh') || 1760);
  canvas.style.width = `${(2 * eyeW) / (window.devicePixelRatio || 1)}px`;
  canvas.width = 2 * eyeW; canvas.height = eyeH;
  const px1 = new Uint8Array(4);
  const sync = () => gl.readPixels(eyeW >> 1, eyeH >> 1, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, px1);
  const base = [];
  for (let f = 0; f < 20; f++) {
    sync();
    const t0 = performance.now();
    gl.clear(gl.COLOR_BUFFER_BIT);
    sync();
    base.push(performance.now() - t0);
  }
  const baseline = V.FrameStats.stats(base).p50;
  const sizes = (query.get('sizes') || '128,256,384,512').split(',').map(Number);
  const spvs = (query.get('spvs') || '0.5,1,2').split(',').map(Number);
  const frames = Number(query.get('frames') || 20);
  const rows = [];
  const proj = M.perspective(90 * Math.PI / 180, eyeW / eyeH, 0.02, 50);
  const ipd = 0.063;
  for (const n of sizes) {
    loadSynthetic(n);
    for (const spv of spvs) {
      for (const skip of [true, false]) {
        const times = [];
        for (let f = 0; f < frames + 2; f++) {
          const spin = f * 0.05;
          state.model = M.multiply(M.translation(0, 0, -0.75), M.multiply(M.rotation([0, 1, 0], spin), M.scaling(0.6)));
          gl.bindFramebuffer(gl.FRAMEBUFFER, null);
          gl.clearColor(0, 0, 0, 1);
          gl.clear(gl.COLOR_BUFFER_BIT);
          sync();
          const t0 = performance.now();
          for (let e = 0; e < 2; e++) {
            gl.viewport(e * eyeW, 0, eyeW, eyeH);
            const view = M.translation((e ? -1 : 1) * ipd / 2, 0, 0);
            R.drawVolume({ proj, view }, state.model, volParams({ spv, skip, mode: 0 }));
          }
          sync();
          if (f >= 2) times.push(performance.now() - t0 - baseline);
        }
        // Steps per ray, one eye (alpha 0 where no ray hits the box).
        gl.clearColor(0, 0, 0, 0);
        gl.clear(gl.COLOR_BUFFER_BIT);
        gl.viewport(0, 0, eyeW, eyeH);
        R.drawVolume({ proj, view: M.translation(ipd / 2, 0, 0) }, state.model, volParams({ spv, skip, mode: 2 }));
        const px = new Uint8Array(eyeW * eyeH * 4);
        gl.readPixels(0, 0, eyeW, eyeH, gl.RGBA, gl.UNSIGNED_BYTE, px);
        let sum = 0, cnt = 0, max = 0;
        for (let i = 0; i < px.length; i += 4) {
          if (px[i + 3] === 0) continue;
          const s = (px[i] / 255) * 4096;
          sum += s; cnt++; if (s > max) max = s;
        }
        const st = V.FrameStats.stats(times);
        rows.push({ n, spv, skip, frameMs: st.mean, p95: st.p95, meanSteps: cnt ? sum / cnt : 0, maxSteps: max, rays: cnt,
          maxStepsBound: V.stepInfo([n, n, n], spv).maxSteps });
        await new Promise((r) => setTimeout(r, 0));
      }
    }
  }
  const lines = ['size | samples/voxel | skipping | stereo frame ms (mean) | p95 | mean steps/ray | max steps/ray | rays/eye'];
  for (const r of rows) {
    lines.push(`${r.n}^3 | ${r.spv} | ${r.skip ? 'on' : 'off'} | ${r.frameMs.toFixed(2)} | ${r.p95.toFixed(2)} | ${r.meanSteps.toFixed(0)} | ${r.maxSteps.toFixed(0)} | ${r.rays}`);
  }
  const dbg = gl.getExtension('WEBGL_debug_renderer_info');
  const gpuName = dbg ? gl.getParameter(dbg.UNMASKED_RENDERER_WEBGL) : gl.getParameter(gl.RENDERER);
  const text = `${gpuName}\neye ${eyeW}x${eyeH} x2, fov 90 deg, cube 0.6 m at 0.75 m, `
    + `sync round trip ${baseline.toFixed(2)} ms subtracted\n${lines.join('\n')}`;
  $('bench-out').hidden = false;
  $('bench-out').textContent = text;
  window.__vrBench = { gpu: gpuName, rows, text };
  document.title = 'BENCH DONE';
}

// ── Start ───────────────────────────────────────────────────────────────────
checkXR();
updateLabels();
if (query.has('bench')) {
  desktopBench().catch((e) => { document.title = `BENCH FAILED ${e.message}`; console.error(e); });
} else {
  startDesktop();
  if (query.has('synthetic')) loadSynthetic(Number(query.get('synthetic')) || 256);
}

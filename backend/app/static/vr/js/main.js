// VisIVO VR page (issue #167): a reduced cube rendered on the headset's GPU.
// Desktop mode (no XR) for development; WebXR immersive-vr / immersive-ar on
// the Quest. See docs/user-guide/vr-headset.md.

import * as M from './mat4.js';
import { Renderer } from './renderer.js';
import { Backend, takeToken, saveToken, tokenError, takePairing, forgetPairing, requestHooks } from './api.js';
import * as V from './volume.js';
import { COLORMAP_NAMES, colormapTable } from './colormaps.js';
import { createSelection, TOOLS, TOOL_NAMES } from './select-ui.js';
import { Refiner, desktopEyes } from './refine-client.js';
import { transferFunction, cropBox, cropError } from './refine.js';
import { pairingFallback } from './pairing.js';
import { loadSettings, saveSettings, clampPanelOffset } from './settings.js';
import * as P from './vr-panel.js';
import { createLayers, pairedMode } from './layers.js';
import { createAnalysis } from './analysis.js';
import * as TP from './tools-panel.js';
import { HandTracker, handSegments } from './hands.js';
import { createShare, boxGeometry, lassoMeta, boxMeta } from './share.js';
import { RefineDraw } from './refine-gl.js';
// Lifecycle, notices, guide, diagnostics, quality (epic #166).
import { createShell } from './shell.js';
import { CARD_PX, CARD_SIZE } from './notice.js';
import { pageBuild, PAGE_VR_API, isHeadsetBrowser, reopenCube, LoadTracker, supersededLoad } from './lifecycle.js';
import { shouldShowGuide } from './onboarding.js';
import { EyeBufferGovernor, qualityNote, cubeBudget, spvAtFloor } from './quality.js';

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
  loading: 0,          // loads under way (a 404 then is the load's own business)
  loadGen: 0,          // bumped by every load: a recovery started before it is superseded
};
const governor = new V.Governor({ start: 1, max: 1 });
const stats = new V.FrameStats(240);
let backend = null;

// Selections on the full cube (issue #171): the triggers, or a mouse click.
const selection = createSelection({
  state, renderer: R,
  backend: () => (state.open ? backend : null),
  path: () => state.openPath || '',
  onChange: () => { selectionDirty = true; },
  onSelection: (s) => publishSelection(s),
  onClear: () => share.dismiss(),   // only called after the share controller exists
});
let selectionDirty = true;

// More analysis in the headset (issue #191): lasso seeds, Isolate/Remove, the
// moment of the selection, the PV path; their panel hangs under the control panel.
const analysis = createAnalysis({
  state, selection, renderer: R,
  backend: () => (state.open ? backend : null),
  onChange: () => { selectionDirty = true; },
  showDataset: (id, path) => showDataset(id, path),
  mount: $('selection'),
});
// Hand tracking (issue #191): a pinch is the trigger, a closed hand the grip.
const handTracker = new HandTracker();
// Selections shared with the desktop's cube viewer (issue #188): what is made
// here is published to the session's hub, what the desktop sends is drawn.
const share = createShare({
  backend: () => (state.open ? backend : null),
  datasetId: () => (state.open ? state.open.dataset_id : ''),
  fullShape: () => (state.info ? state.info.full_shape : null),
  onReceive: (info, m, own) => selection.showShared(info, m, own),   // false: the cube is not drawn yet
  onChange: () => { selectionDirty = true; },
});
function publishSelection(s) {
  if (s.kind === 'lasso') {
    share.publish('lasso', { bytes: s.bytes, origin: s.origin, dims: s.dims, meta: lassoMeta(s.request, s.parameters, s.trimBox) });
  } else if (s.kind === 'box') {
    share.publish('box', { ...boxGeometry(s.range), meta: boxMeta(s.range, s.parameters) });
  }
}

// Refinement (issue #170): the backend's full-quality stereo frames, blended in
// while the view is still or slow. The page's own rendering is always drawn.
const refiner = new Refiner(gl, () => backend);
let refineMode = 'auto';      // 'auto' | 'off' (debug: the local picture alone)
let lookCache = { key: null, look: null };

/** The page's look as the session's commands, and its key (a change of key
 *  makes every refined frame stale). */
function currentLook() {
  const c = state.cube;
  const key = JSON.stringify([state.thrNorm, state.cmaxNorm, state.gamma, state.opacity, state.mode,
    state.colormap, state.clip, state.zStretch, c ? [c.lo, c.hi, c.factors] : null]);
  if (lookCache.key !== key && c) {
    lookCache = {
      key,
      look: {
        tf: transferFunction({ thrNorm: state.thrNorm, cmaxNorm: state.cmaxNorm, gamma: state.gamma,
          opacity: state.opacity, mode: state.mode, lo: c.lo, hi: c.hi, factors: c.factors,
          lut: colormapTable(state.colormap) }),
        crop: cropBox(state.clip),
        cropError: cropError(state.clip),
      },
    };
  }
  return lookCache;
}

/** Once per displayed frame: the eyes this frame is drawn for. */
function refineFrame(t, eyes, centreView, background) {
  // A point table (issue #190) is refined by the backend's point session.
  if (!state.cube) { if (layers) layers.refineFrame(t, eyes, centreView, background); return; }
  const look = currentLook();
  refiner.frame({ t, eyes, aCentre: M.multiply(centreView, state.model), model: state.model,
    zStretch: state.zStretch, look: look.look, lookKey: look.key, background });
}

// ── Settings kept in this browser (issue #172) ──────────────────────────────
const settingsStore = (() => { try { return window.localStorage; } catch { return null; } })();
const settings = loadSettings(settingsStore, { colormaps: COLORMAP_NAMES, tools: TOOLS });
function persist() { saveSettings(settingsStore, settings); }

// ── The shell: lifecycle, notices, guide, diagnostics (epic #166) ──────────
const PAGE = { build: pageBuild(import.meta.url), api: PAGE_VR_API };
const eyeGovernor = new EyeBufferGovernor();
const shell = createShell({
  page: PAGE,
  renderer: () => R,
  backend: () => { const t = tokenField.value.trim(); return t ? (backend && backend.token === t ? backend : ensureBackend()) : null; },
  token: () => tokenField.value.trim(),
  ctx: {
    inVR: () => !!xr,
    exitVR: () => endXR('the user'),
    enterVR: () => { if (!$('enter-vr').disabled) enterXR('immersive-vr', false).catch((e) => setStatus(`XR: ${e.message}`)); },
    recoverDataset: (reason) => recoverDataset(reason),
    hasBackendCube: () => !!(state.open && state.openPath),
    loading: () => state.loading > 0,
    loadGen: () => state.loadGen,
    onGuideSeen: () => { if (!settings.guideSeen) { settings.guideSeen = true; persist(); } },
    onRetry: () => refiner.retrySoon(),
    publicInfo: (opts) => new Backend('').info(opts),
    summary: () => diagnosticsSummary(),
  },
});
requestHooks.onFailure = (err, path) => shell.requestFailed(err, path);
refiner.onLifecycle = (name, detail) => shell.log('warning', name, detail);

// ── Panel wiring ────────────────────────────────────────────────────────────
for (const name of COLORMAP_NAMES) {
  const o = document.createElement('option');
  o.value = o.textContent = name;
  $('cmap').appendChild(o);
}

// Point tables, velocity fields, catalogues over a cube (issue #190): made
// once the page's controls exist (below).
let layers = null;

/** The colour map, from the page, the A button or the in-VR panel. */
function setColormap(name) {
  state.colormap = name;
  R.setColormap(name);
  if (layers) layers.setColormap(name);
  $('cmap').value = name;
  if (settings.colormap !== name) { settings.colormap = name; persist(); }
}
setColormap(settings.colormap);
$('thr-source').value = settings.thrSource;

// The pairing from the desktop's "Open in VR Headset…" (read before the token:
// both come from the same fragment, which is then removed from the address).
const pairing = takePairing();
if (pairing.path) $('path').value = pairing.path;
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
        if (e.type !== 'directory' && !e.is_fits && !(layers && layers.listable(e.name))) continue;
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

$('load').addEventListener('click', () => {
  forgetPairing();      // a reload of this tab now opens nothing by itself
  withLoading(loadFromBackend).catch((err) => { if (!err.superseded) setStatus(`Failed: ${err.message || err}`); });
});
$('synthetic').addEventListener('click', () => loadSynthetic(Number($('synthetic-size').value)));

function readNumber(id) { return Number($(id).value); }

$('thr').addEventListener('input', () => { state.thrNorm = sliderToNorm(readNumber('thr')); updateLabels(); });
$('cmax').addEventListener('input', () => { state.cmaxNorm = sliderToNorm(readNumber('cmax')); updateLabels(); });
$('thr-source').addEventListener('change', () => {
  settings.thrSource = $('thr-source').value;
  persist();
  resetThreshold();
});
$('thr-reset').addEventListener('click', () => resetThreshold());
$('cmap').addEventListener('change', () => setColormap($('cmap').value));
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

// Selection controls (issue #171).
if (selection.domCanvas) $('sel-panel').appendChild(selection.domCanvas);
$('sel-tool').addEventListener('change', () => selection.setTool($('sel-tool').value));
$('sel-clear').addEventListener('click', () => { selection.clear(); analysis.sync(); });
$('sel-seedat').addEventListener('change', () => { selection.sel.seedAt = $('sel-seedat').value; });
$('sel-snap').addEventListener('change', () => { selection.sel.snap = $('sel-snap').checked; });
function readLassoParams() {
  const p = selection.sel.params;
  const num = (id, lo, hi, dflt) => {
    const v = Number($(id).value);
    return Number.isFinite(v) ? Math.max(lo, Math.min(hi, v)) : dflt;
  };
  p.reach = Math.round(num('lasso-reach', 1, 64, 8));
  p.lambda_v = num('lasso-lambda', 0.1, 5, 1);
  p.nsigma = num('lasso-nsigma', 0.5, 10, 2.5);
  p.sigma = num('lasso-sigma', 0, Infinity, 0);
  const [sv, sxy] = $('lasso-smooth').value.split(',').map(Number);
  p.smooth_v = sv; p.smooth_xy = sxy;
}
for (const id of ['lasso-reach', 'lasso-lambda', 'lasso-nsigma', 'lasso-sigma', 'lasso-smooth']) {
  $(id).addEventListener('change', readLassoParams);
}
readLassoParams();
$('sel-export').addEventListener('click', () => {
  const blob = new Blob([selection.recipesJson('all')], { type: 'application/json' });
  const a = Object.assign(document.createElement('a'), {
    href: URL.createObjectURL(blob),
    download: `visivo-vr-recipes-${new Date().toISOString().replace(/[:.]/g, '-')}.json`,
  });
  document.body.appendChild(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(a.href), 10000);
});
$('sel-copy').addEventListener('click', async () => {
  const text = selection.recipesJson('last');
  try { await navigator.clipboard.writeText(text); setStatus('Last recipe copied.'); }
  catch { setStatus(text); }
});
for (const t of TOOLS) {
  const o = $('sel-tool').querySelector(`option[value="${t}"]`);
  if (o) o.title = TOOL_NAMES[t];
}

// Refinement controls (issue #170).
// Stored settings first; ?refine=0 / ?refine=1 in the address override them.
$('refine').checked = query.get('refine') === '0' ? false : query.get('refine') === '1' ? true : settings.refine.enabled;
$('refine-scale').value = String(settings.refine.scale);
$('refine-quality').value = String(settings.refine.quality);
$('refine-depth').checked = settings.refine.depth;
function saveRefine() {
  settings.refine = { enabled: $('refine').checked, scale: readNumber('refine-scale'),
    quality: readNumber('refine-quality'), depth: $('refine-depth').checked };
  persist();
}
/** Refinement on or off, from the page or the in-VR panel. */
function setRefine(on) {
  $('refine').checked = on;
  refiner.setEnabled(on);
  saveRefine();
}
refiner.setEnabled($('refine').checked);
$('refine').addEventListener('change', () => setRefine($('refine').checked));
$('refine-scale').addEventListener('change', () => { refiner.o.scale = readNumber('refine-scale'); saveRefine(); });
$('refine-quality').addEventListener('change', () => { refiner.o.quality = readNumber('refine-quality'); saveRefine(); });
$('refine-depth').addEventListener('change', () => { refiner.useDepth = $('refine-depth').checked; saveRefine(); });
refiner.o.scale = readNumber('refine-scale');
refiner.o.quality = readNumber('refine-quality');
refiner.useDepth = $('refine-depth').checked;
selection.setTool(settings.tool);
window.addEventListener('pagehide', () => refiner.closeNow());
// Back from the back-forward cache: pagehide closed the session, open a new one.
window.addEventListener('pageshow', (e) => { if (e.persisted) refiner.resume(); });

layers = createLayers({
  gl, R, state, $, refiner, ensureBackend,
  onNonCube: () => { selection.clear(); analysis.reset(); share.stop(); },
  colormap: () => state.colormap, refineOn: () => refineMode !== 'off',
  rebuild: () => rebuildDecorations(),
  targetHz: () => (xr ? xr.targetHz : 60),
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
      const val = V.axisTexToWorld(axis, arr, V.boxToTex(t, state.info.full_shape[ci.i], state.info.factors[ci.i]));
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
function reducedRequest(datasetId) {
  return {
    dataset_id: datasetId,
    factor_xy: readNumber('fxy'), factor_z: readNumber('fz'), max_dim: readNumber('maxdim'),
    mode: $('mode').value, encoding: $('encoding').value,
  };
}

async function withLoading(fn) {
  // A recovery under way (recoverDataset) sees the generation change and
  // leaves the cube of this load alone. Bumped when a load starts and when a
  // cube goes on show (installCube), never when a load ends: one that failed
  // installed nothing, and a recovery started after it is still valid.
  state.loading++;
  state.loadGen++;
  try { return await fn(); } finally { state.loading--; }
}

/**
 * A backend handle of the page's own for one load or recovery: the same
 * token and, to begin with, the same session. It may adopt or create
 * sessions freely; they become the page's only when this load wins
 * (commitHandle, right before its cube goes on show). A superseded load
 * never changes the session the page and the newer load are using.
 */
function loadHandle() {
  const h = new Backend(ensureBackend().token);
  h.adoptSession(ensureBackend().session);
  return h;
}
/** False (nothing changed) when the token was changed during the load: its
 *  session belongs to the old token, so the load counts as superseded. */
function commitHandle(h) {
  const page = ensureBackend();
  if (h.token !== page.token) return false;
  page.adoptSession(h.session);
  if (h.sessionNote) state.sessionNote = h.sessionNote;
  return true;
}

// The newest load wins; each carries the path it was started for.
const loads = new LoadTracker();
function stillCurrent(ticket) { if (!loads.isCurrent(ticket)) throw supersededLoad(); }

// Layers install their GPU data inside layers.js. Commit their session there,
// before starting refinement, with the same ownership check as a cube load.
function layerLoad(b, ticket) {
  return {
    check: () => stillCurrent(ticket),
    commit: () => {
      stillCurrent(ticket);
      if (!commitHandle(b)) throw supersededLoad();
      state.loadGen++;
    },
  };
}

async function loadFromBackend() {
  ensureBackend();
  const path = $('path').value.trim();
  if (!path) throw new Error('Give the path of a cube on the backend.');
  const ticket = loads.begin(path);
  const b = loadHandle();
  const t0 = performance.now();
  setStatus('Opening…');
  stillCurrent(ticket);
  if (!b.session) await b.newSession();
  stillCurrent(ticket);
  // A point table or a velocity field (issue #190), by the page's choice or the name.
  if (await layers.openPath(b, path, layerLoad(b, ticket))) {
    stillCurrent(ticket);
    state.openPath = path;
    setStatus(`${path.split('/').pop()}: ${layers.hudLines().join('\n')}\nin ${state.sessionNote || 'own session'}`);
    return;
  }
  const open = await b.open(path);
  stillCurrent(ticket);
  b.sessionNote = state.sessionNote || 'own session';
  setStatus('Reading the axes…');
  const info = await b.reducedInfo(reducedRequest(open.dataset_id));
  stillCurrent(ticket);
  await loadReduced(b, open, info, t0, ticket);
}

/**
 * The cube the desktop has open (issue #172), loaded with nothing typed: in
 * the desktop's session, the desktop's dataset when that session still holds
 * it, else its path opened again (in that session, or in a new one when the
 * session is not available here). Without a token, the page says so.
 */
async function loadPaired() {
  if (!pairing.dataset && !pairing.path) return;
  if (!tokenField.value.trim()) {
    setStatus('The link from the desktop has no token: paste it, then press "Open and load".');
    return;
  }
  ensureBackend();
  const ticket = loads.begin(pairing.path);
  const b = loadHandle();          // the desktop's session is adopted here, the page's only if this load wins
  const t0 = performance.now();
  if (pairing.session) b.adoptSession(pairing.session);
  if (pairedMode(pairing.kind) !== 'cube') {
    // The desktop's point table or velocity field (issue #190): its dataset in
    // the session, else its path again, as for a cube.
    stillCurrent(ticket);
    if (!pairing.session) await b.newSession();
    stillCurrent(ticket);
    b.sessionNote = 'the desktop\'s session';
    setStatus('Opening the desktop\'s data…');
    await layers.openPaired(b, pairing, async (err) => {
      stillCurrent(ticket);
      const next = pairingFallback(err, !!pairing.path);
      if (next === 'new-session') {
        await b.newSession();
        b.sessionNote = 'a new session (the desktop\'s is not available here)';
      } else if (next === 'open') {
        b.sessionNote = 'the desktop\'s session (opened again)';
      }
      return next;
    }, layerLoad(b, ticket));
    stillCurrent(ticket);
    state.openPath = pairing.path || '';
    setStatus(`${layers.hudLines().join('\n')}\nin ${state.sessionNote}`);
    return;
  }
  let open = null, info = null, err = null;
  if (pairing.dataset && pairing.session) {
    setStatus('Opening the desktop\'s cube…');
    try {
      info = await b.reducedInfo(reducedRequest(pairing.dataset));
      open = { dataset_id: pairing.dataset, kind: 'cube', valid: true };
      b.sessionNote = 'the desktop\'s session';
    } catch (e) { err = e; }
  } else {
    err = new Error(pairing.session ? '404: no dataset in the link' : '404: Session not found.');
  }
  if (!open) {
    const next = pairingFallback(err, !!pairing.path);
    if (next === 'fail') throw err;
    stillCurrent(ticket);
    if (next === 'new-session') {
      await b.newSession();
      stillCurrent(ticket);
      b.sessionNote = 'a new session (the desktop\'s is not available here)';
    } else {
      b.sessionNote = 'the desktop\'s session (cube opened again)';
    }
    setStatus('Opening the desktop\'s cube by its path…');
    open = await b.open(pairing.path);
    stillCurrent(ticket);
    info = await b.reducedInfo(reducedRequest(open.dataset_id));
  }
  stillCurrent(ticket);
  await loadReduced(b, open, info, t0, ticket);
}

/** A cube made in the headset (issue #191: Isolate, Remove), shown in its place. */
async function showDataset(id, path) {
  return withLoading(async () => {
    const ticket = loads.begin(path || '');
    const b = loadHandle();
    const t0 = performance.now();
    if (path) $('path').value = path;
    setStatus('Reading the axes…');
    const info = await b.reducedInfo(reducedRequest(id));
    stillCurrent(ticket);
    await loadReduced(b, { dataset_id: id, kind: 'cube', valid: true }, info, t0, ticket);
  });
}

async function loadReduced(b, open, info, t0, ticket) {
  const req = reducedRequest(open.dataset_id);
  const t1 = performance.now();
  // The backend caps voxels and bytes; the GPU caps each side and this
  // device its memory. Refuse before reading the cube rather than after.
  checkBudget(info.reduced_shape, req.encoding);
  setStatus(`Reducing ${info.full_shape.join('×')} by ${info.factors.join('×')} (${req.mode})…`);
  const cube = await b.reducedCube({ ...req, factor_xy: info.factors[0], factor_z: info.factors[2] },
    (f) => { if (loads.isCurrent(ticket)) setStatus(`Transferring… ${(100 * f).toFixed(0)} %`); });
  // A load started meanwhile owns the page: this cube is not shown.
  stillCurrent(ticket);
  // This load won: its session becomes the page's (unless the token changed).
  if (!commitHandle(b)) throw supersededLoad();
  state.load = { openInfoMs: t1 - t0, reduceMs: cube.timing.serverMs, transferMs: cube.timing.totalMs - cube.timing.serverMs, mb: cube.bytes / 1e6 };
  installCube(info, cube, open, ticket.path);
}

/** Throws (and says so on the card) when a cube of `dims` would not fit this device. */
function checkBudget(dims, encoding) {
  const b = cubeBudget({ dims, encoding }, { maxTex3D: R.maxTex3D, headset: isHeadsetBrowser(navigator.userAgent),
    deviceMemoryGB: navigator.deviceMemory });
  if (!b.ok) {
    shell.notify('budget', { detail: b.reason });
    shell.log('warning', 'cube-refused', b.reason);
    throw new Error(`${b.reason} Choose a larger binning.`);
  }
  return b;
}

/**
 * The backend lost the cube's dataset (it restarted, or the desktop's session
 * ended): the same file is opened again, in this session or a new one, and
 * the cube on show is kept (nothing is transferred again) when the backend
 * describes it as before. {ok, detail} ({superseded: true} when a load
 * replaced the cube meanwhile: then nothing is changed).
 */
/** The deadline of each request of a recovery (opening a large cube can take a while). */
const RECOVERY_TIMEOUT_MS = 30000;

async function recoverDataset(reason) {
  const path = state.openPath;
  if (!path || !state.open || !state.info || !state.cube) return { ok: false, detail: 'no cube from the backend on show' };
  // Everything read from the page before the first await (lifecycle.js reopenCube).
  const c = state.cube;
  const snap = { path, fullShape: state.info.full_shape, gen: state.loadGen };
  const req = { factor_xy: c.factors[0], factor_z: c.factors[2], max_dim: readNumber('maxdim'), mode: c.mode, encoding: c.encoding };
  // Its own handle: a new session it makes becomes the page's only if it wins.
  // And a deadline on each of its requests (datasets/open, sessions/new,
  // reduced_info): a hung connection must not stall the recovery; the next
  // check of the lifecycle tries again.
  const b = loadHandle();
  b.timeoutMs = RECOVERY_TIMEOUT_MS;
  const r = await reopenCube(snap, {
    open: (p) => b.open(p),
    newSession: () => b.newSession(),
    reducedInfo: (o) => b.reducedInfo({ dataset_id: o.dataset_id, ...req }),
    classify: (err) => shell.classifyFailure(err),
    loadGen: () => state.loadGen,
  });
  if (!r.ok) return r;
  const { open, info } = r;
  if (!commitHandle(b)) return { ok: false, superseded: true, detail: 'the token changed during the reopen' };
  state.open = open;
  state.sessionNote = reason === 'restarted' ? 'a new session (the backend restarted)' : 'a new session (the old one ended)';
  refiner.setDataset(open.dataset_id, { outOfCore: !!info.render_out_of_core });
  return { ok: true, detail: `dataset ${open.dataset_id}` };
}

function loadSynthetic(n) {
  loads.begin('');              // a newer load than any backend load under way
  const t0 = performance.now();
  checkBudget([n, n, n], 'uint8');
  const data = V.syntheticCube(n);
  const info = V.syntheticInfo(n);
  state.load = { generateMs: performance.now() - t0, mb: data.length / 1e6 };
  installCube(info, {
    data, dims: [n, n, n], encoding: 'uint8', lo: 0, hi: 1, factors: [1, 1, 1], mode: 'synthetic',
    reducedThreshold: 0.25, bunit: '', fullShape: [n, n, n],
  }, null, '');
}

/** `path`: the file the load was started for (never the path field now). */
function installCube(info, cube, open, path = '') {
  // Every cube put on show (from the backend, synthetic, the benchmark's)
  // supersedes a recovery under way for the previous one (reopenCube).
  state.loadGen++;
  layers.onCube();
  const up = R.setVolume(cube);
  state.info = info;
  state.cube = cube;
  state.open = open;
  state.openPath = open ? path : '';
  selection.clear();
  analysis.reset();
  if (open) share.start(open.dataset_id); else share.stop();
  refiner.setDataset(open ? open.dataset_id : null, { outOfCore: !!(info && info.render_out_of_core) });
  state.load = { ...state.load, uploadMs: up.uploadMs, macroMs: up.macroMs, textureMB: up.textureBytes / 1e6 };
  state.clip = { min: [0, 0, 0], max: [1, 1, 1] };
  applyExtent();
  resetThreshold();
  const l = state.load;
  setStatus([
    `${cube.dims.join('×')} ${cube.encoding}, binned ${cube.factors.join('×')} (${cube.mode}) from ${cube.fullShape.join('×')}`,
    l.reduceMs !== undefined ? `reduce ${l.reduceMs.toFixed(0)} ms, transfer ${l.transferMs.toFixed(0)} ms (${l.mb.toFixed(1)} MB)` : `generated in ${l.generateMs.toFixed(0)} ms`,
    `upload ${l.uploadMs.toFixed(0)} ms, skip grid ${l.macroMs.toFixed(0)} ms`,
    ...(open ? [`in ${state.sessionNote || 'own session'}`] : []),
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
  labels.push(...layers.cubeLabels());     // a catalogue over the cube (issue #190)
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

function drawScene(view, override, eye = 0) {
  // With a refined frame on show (fading in or out included) the selection is
  // left out of the headset's own volume and drawn once, over the refined
  // frame (the backend's frame has none); then the box, ticks and labels.
  const refined = refineMode !== 'off' && refiner.showing;
  R.drawVolume(view, state.model, volParams(refined ? { ...override, showMask: false } : override));
  if (refined) {
    refiner.composite(view, state.model, eye);
    if (R.mask) R.drawVolume(view, state.model, volParams({ ...override, mode: 0, maskOnly: true }));
  }
  R.drawLines(view, state.model);
  R.drawLines(view, state.model, R.selLines);
  layers.drawOverCube(view, state.model);
  R.drawLabels(view, state.model, M.uniformScale(state.model));
}

/** Rebuild what changes with the selections: their lines, the page's panel. */
function syncSelection() {
  if (!selectionDirty) return;
  selectionDirty = false;
  R.setLines([...selection.segments(), ...analysis.segments()], R.selLines);
  selection.refreshDom();
  analysis.refreshDom();
  const s = selection.sel;
  $('sel-tool').value = s.tool;
  if (settings.tool !== s.tool) { settings.tool = s.tool; persist(); }
  $('sel-recipes').textContent = s.recipes.length
    ? `${s.recipes.length} recorded: ${s.recipes.map((r) => r.product.kind).join(', ')}` : 'nothing recorded yet';
  $('sel-export').disabled = $('sel-copy').disabled = !s.recipes.length;
}

// ── Quality and diagnostics (epic #166) ─────────────────────────────────────
function currentQualityNote() {
  return qualityNote({ governorOn: state.governorOn, spv: governor.value, spvChosen: state.spv,
    eyeScale: xr ? xr.fbScale : undefined, eyeChosen: xr ? xr.fbChosen : undefined, targetHz: xr ? xr.targetHz : 0 });
}

function gpuInfo() {
  const dbg = gl.getExtension('WEBGL_debug_renderer_info');
  return {
    renderer: gl.isContextLost() ? 'context lost' : dbg ? gl.getParameter(dbg.UNMASKED_RENDERER_WEBGL) : gl.getParameter(gl.RENDERER),
    maxTex3D: R.maxTex3D, maxTexture: gl.getParameter(gl.MAX_TEXTURE_SIZE), timerQuery: !!R.timer,
    deviceMemoryGB: navigator.deviceMemory,
  };
}

function diagnosticsSummary() {
  const c = state.cube;
  const s = stats.summary(xr ? 1000 / xr.targetHz : 0);
  return {
    gpu: gpuInfo(),
    xr: xr ? `${xr.mode}, ${xr.targetHz} Hz, eye buffer ${xr.layer.framebufferWidth}×${xr.layer.framebufferHeight} (scale ${xr.fbScale} of ${xr.fbChosen}), foveation ${'fixedFoveation' in xr.layer ? xr.layer.fixedFoveation : 'n/a'}`
      : `not in VR; ${$('xr-status').textContent}`,
    cube: c ? `${c.dims.join('×')} ${c.encoding}, binned ${c.factors.join('×')} (${c.mode}) of ${c.fullShape.join('×')}, texture ${(R.textureBytes / 1e6).toFixed(1)} MB, ${state.open ? `dataset ${state.open.dataset_id}` : 'no backend dataset'}` : 'none',
    fps: `${s.fps.toFixed(1)} fps, frame ${s.interval ? s.interval.mean.toFixed(2) : 'n/a'} ms mean, missed ${(100 * s.missedFraction).toFixed(1)} %, CPU ${s.cpu ? s.cpu.mean.toFixed(2) : 'n/a'} ms, GPU ${s.gpu ? s.gpu.mean.toFixed(2) : 'n/a'} ms`,
    quality: `${currentQualityNote() || 'as chosen'}; samples/voxel ${governor.value.toFixed(2)} of ${state.spv}, governor ${state.governorOn ? 'on' : 'off'}`,
    refine: refineShort(),
    settings,
  };
}

/** Leaves VR on purpose (the card's Exit VR, the benchmark's end): no "ended" notice. */
function endXR(why) {
  if (!xr) return;
  xr.endedBy = why || 'the page';
  xr.session.end().catch(() => {});
}

// ── HUD (frame-rate and latency overlay) ─────────────────────────────────────
const hudCanvas = Object.assign(document.createElement('canvas'), { width: 640, height: 600 });
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
  lines.push(...selection.hudLines());
  lines.push(...layers.hudLines());
  lines.push(...analysis.hudLines());
  lines.push(...share.hudLines());
  const qn = currentQualityNote();
  if (qn) lines.push(qn.slice(0, 60));
  if (state.cube || layers.mode === 'points') lines.push(...refiner.hudLines());
  if (vrPanel.draws) {
    lines.push(`panel redraw ${median(vrPanel.drawMs).toFixed(2)} ms + upload ${median(vrPanel.uploadMs).toFixed(2)} ms (${vrPanel.draws} redraws)`);
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

// ── The in-VR panel (issue #172) ────────────────────────────────────────────
// The page's main controls on a quad in the room, used with the right ray and
// trigger. It stays where it was put (left of the cube by default; drag its
// title bar to move it, the place is kept); left stick click hides it. In a
// desktop browser it is shown in the 3-D view with ?panel3d (or the box on
// the page) and used with the mouse, through the same ray code.
const DESKTOP_ANCHOR = { p: [0, 0, 0], yaw: 0 };
const DESKTOP_OFFSET = { right: 0.95, up: 0, fwd: -0.3, yaw: -0.45 };   // right: the page's panel is on the left
const vrPanel = {
  layout: P.panelLayout(),
  canvas: Object.assign(document.createElement('canvas'), { width: P.PANEL_PX[0], height: P.PANEL_PX[1] }),
  tex: null, key: '', drawnAt: 0,
  visible: true,
  desktop: query.has('panel3d'),
  offset: settings.panel || { ...P.DEFAULT_OFFSET },
  desktopOffset: { ...DESKTOP_OFFSET },
  hit: null, drag: null,
  drawMs: [], uploadMs: [], draws: 0,
};
vrPanel.pointer = new P.PanelPointer(vrPanel.layout, { rangeValue: () => [state.clip.min[2], state.clip.max[2]] });
$('panel3d').checked = vrPanel.desktop;
$('panel3d').addEventListener('change', () => { vrPanel.desktop = $('panel3d').checked; });

function panelAnchor() { return xr ? xr.place : DESKTOP_ANCHOR; }
function panelOffset() { return xr ? vrPanel.offset : vrPanel.desktopOffset; }
function panelWorld() { return P.panelModel(panelAnchor(), panelOffset()); }

/** What the panel shows, as plain values (its key decides a redraw). */
function panelView() {
  const c = state.cube, info = state.info;
  const unit = c && c.bunit ? ` ${c.bunit}` : '';
  const name = state.openPath ? state.openPath.split('/').pop()
    : c ? (c.mode === 'synthetic' ? 'synthetic cube' : `dataset ${state.open ? state.open.dataset_id : ''}`) : 'no cube';
  const v = {
    title: `VisIVO VR · ${name}`,
    info: c ? [
      `${(info.full_shape || c.fullShape).join('×')} → ${c.dims.join('×')}, binned ${c.factors.join('×')} (${c.mode}), ${c.encoding === 'float16' ? '16' : '8'}-bit`,
      c.mode === 'synthetic' ? 'no backend: selections need a cube from the backend' : `in ${state.sessionNote || 'own session'}`,
    ] : ['No cube yet: open one from the desktop or on the page.', ''],
    cmap: state.colormap, mode: state.mode, thrSource: $('thr-source').value, tool: selection.sel.tool,
    thr: { f: normToSlider(state.thrNorm) / 1000, text: c ? `${fmt(dataValue(state.thrNorm))}${unit}` : '' },
    cmax: { f: normToSlider(state.cmaxNorm) / 1000, text: c ? `${fmt(dataValue(state.cmaxNorm))}${unit}` : '' },
    opacity: { f: state.opacity / 2, text: state.opacity.toFixed(2) },
    slab: { lo: state.clip.min[2], hi: state.clip.max[2], text: '', world: '' },
    refine: { on: refiner.enabled, text: refineShort(), warn: !!(refiner.unavailable || refiner.suspended) },
    // A hidden lifecycle notice first: it is the state of the backend now,
    // a selection's error may be older than it.
    status: shell.badge() || selection.sel.busy || selection.sel.error || currentQualityNote(),
    statusError: !!shell.badge() || (!selection.sel.busy && !!selection.sel.error),
    hint: 'Right trigger: use   Left trigger: next tool   Left stick click: hide',
  };
  if (info && info.full_shape) {
    const nz = info.full_shape[2];
    const a = Math.min(nz - 1, Math.round(state.clip.min[2] * nz));
    const b = Math.max(a, Math.round(state.clip.max[2] * nz) - 1);
    v.slab.text = `ch ${a}–${b} of ${nz}`;
    const axis = info.axes && info.axes.z;
    if (axis) {
      const arr = axis.velocity_kms || axis.world;
      const at = (t) => V.texToWorld(arr, V.boxToTex(t, nz, info.factors[2]));
      const lo = at(state.clip.min[2]), hi = at(state.clip.max[2]);
      const u = axis.velocity_kms ? 'km/s' : axis.unit || '';
      if (lo !== null && lo !== undefined && hi !== null && hi !== undefined) {
        v.slab.world = `${fmt(lo)} to ${fmt(hi)} ${u}${axis.velocity_kms && axis.velocity_convention ? `, ${axis.velocity_convention}` : ''}`;
      }
    }
  }
  return v;
}

function refineShort() {
  if (!refiner.enabled) return 'the headset\'s own rendering only';
  if (refiner.suspended) return `paused ${refiner.suspended}`;
  if (refiner.unavailable) return `unavailable: ${refiner.unavailable}`;
  if (refiner.retryAt && (refiner.status === 'reconnecting' || refiner.status === 'unavailable')) {
    const s = Math.max(0, Math.ceil((refiner.retryAt - performance.now()) / 1000));
    return `lost: retrying in ${s} s, the headset's own rendering meanwhile`;
  }
  return refiner.status === 'refined' ? 'refined (still view)' : refiner.status;
}

/** The panel's texture, redrawn when what it shows changed (at most 30 times a second). */
function panelTexture(now) {
  if (!vrPanel.tex) vrPanel.tex = R.createPanel();
  const view = panelView();
  const ui = { hover: vrPanel.pointer.hover, active: vrPanel.pointer.active ? vrPanel.pointer.active.id : null };
  const key = JSON.stringify([view, ui]);
  if (key !== vrPanel.key && (now - vrPanel.drawnAt >= 33 || !vrPanel.draws)) {
    const t0 = performance.now();
    P.drawPanel(vrPanel.canvas.getContext('2d'), vrPanel.layout, view, ui);
    const t1 = performance.now();
    R.updatePanel(vrPanel.tex, vrPanel.canvas);
    const t2 = performance.now();
    vrPanel.drawMs.push(t1 - t0); vrPanel.uploadMs.push(t2 - t1);
    if (vrPanel.drawMs.length > 60) { vrPanel.drawMs.shift(); vrPanel.uploadMs.shift(); }
    vrPanel.key = key; vrPanel.drawnAt = now; vrPanel.draws++;
  }
  return vrPanel.tex;
}

const median = (a) => { if (!a.length) return NaN; const s = [...a].sort((x, y) => x - y); return s[s.length >> 1]; };

/**
 * The pointer on the panel for this frame: `ray` {o, d} in world space (or
 * null), `pressed` the trigger (or mouse button), `eye` the viewer's position
 * (a dragged panel turns to face it). Returns whether the panel takes the
 * ray (the selection tool then neither hovers nor fires).
 */
function panelPointer(ray, pressed, eye) {
  const show = vrPanel.visible && (xr ? !xr.bench : vrPanel.desktop);
  let hit = show && ray ? P.rayPanel(panelWorld(), P.PANEL_SIZE, ray.o, ray.d) : null;
  // The cube in front of the panel takes the ray (a slider already held keeps it).
  if (hit && state.cube && !vrPanel.pointer.active
      && P.panelBehindCube(hit.t, state.model, state.extent, ray.o, ray.d, state.clip.min, state.clip.max)) {
    hit = null;
  }
  // The analysis panel under it (issue #191): the nearer of the two takes the
  // ray; a slider held on this one keeps it.
  const blocked = (t) => !!state.cube && P.panelBehindCube(t, state.model, state.extent, ray.o, ray.d, state.clip.min, state.clip.max);
  const tools = analysis.pointer(ray, pressed, panelWorld(), show && !!state.cube, blocked, vrPanel.pointer.active ? { t: -Infinity } : hit);
  if (tools.hit) hit = null;
  const strip = layers.stripPointer(show && ray && !hit && !tools.takes && !vrPanel.pointer.active ? ray : null,
    pressed && show && !tools.takes && !vrPanel.pointer.active, panelWorld());
  vrPanel.hit = hit || tools.hit || (strip ? layers.stripHit() : null);
  for (const a of vrPanel.pointer.update(hit, pressed && show)) applyPanel(a, ray, eye);
  return show && (vrPanel.pointer.takesRay() || vrPanel.pointer.owns || tools.takes || strip);
}

function applyPanel(a, ray, eye) {
  if (a.type === 'press') {
    const id = a.id;
    if (id === 'cmap-prev' || id === 'cmap-next') {
      const n = COLORMAP_NAMES.length, i = COLORMAP_NAMES.indexOf(state.colormap);
      setColormap(COLORMAP_NAMES[(i + (id === 'cmap-next' ? 1 : n - 1)) % n]);
    } else if (a.widget.group === 'mode') {
      state.mode = a.widget.value; $('rmode').value = String(state.mode);
    } else if (id === 'thr-reset') {
      resetThreshold();
    } else if (a.widget.group === 'thrsrc') {
      $('thr-source').value = a.widget.value;
      settings.thrSource = a.widget.value; persist();
      resetThreshold();
    } else if (id === 'slab-all') {
      state.clip.min[2] = 0; state.clip.max[2] = 1;
      if (xr) xr.slab = { c: 0.5, w: 0.5 };
      decorationsChanged();
    } else if (id === 'refine') {
      setRefine(!refiner.enabled);
    } else if (a.widget.group === 'tool') {
      selection.setTool(a.widget.value);
    } else if (id === 'sel-clear') {
      selection.clear();
      analysis.sync();      // the moment and the implicit cube of the cleared selection go
    } else if (id === 'guide') {
      shell.help();
    }
  } else if (a.type === 'slide') {
    if (a.id === 'thr') state.thrNorm = Math.min(state.cmaxNorm - 1e-3, sliderToNorm(a.value * 1000));
    else if (a.id === 'cmax') state.cmaxNorm = Math.max(state.thrNorm + 1e-3, sliderToNorm(a.value * 1000));
    else if (a.id === 'opacity') { state.opacity = Math.max(0.01, a.value * 2); $('opacity').value = Math.round(state.opacity * 100); }
    labelsChanged();
  } else if (a.type === 'range') {
    [state.clip.min[2], state.clip.max[2]] = a.value;
    if (xr) xr.slab = { c: (a.value[0] + a.value[1]) / 2, w: (a.value[1] - a.value[0]) / 2 };
    decorationsChanged();
  } else if (a.type === 'drag-start') {
    const m = panelWorld();
    vrPanel.drag = { t: a.hit.t, rel: M.sub(M.position(m), a.hit.point) };
  } else if ((a.type === 'drag' || a.type === 'drag-end') && vrPanel.drag && ray && eye) {
    const p = M.add(M.add(ray.o, M.scale3(ray.d, vrPanel.drag.t)), vrPanel.drag.rel);
    // Within the limits the settings keep, so a panel let go far away is
    // saved where it stops, not silently dropped on the next load.
    const off = clampPanelOffset(P.offsetFor(panelAnchor(), p, eye));
    if (xr) vrPanel.offset = off; else vrPanel.desktopOffset = off;
  }
  if (a.type === 'drag-end') {
    // Kept for the next sessions in the headset only (the desktop's place is
    // a development view), even when the controller was lost at the release.
    if (vrPanel.drag && xr) { settings.panel = { ...vrPanel.offset }; persist(); }
    vrPanel.drag = null;
  }
}

/** The clipping box changed: its lines and labels (throttled in VR). */
function decorationsChanged() {
  if (xr) { xr.slabDirty = true; xr.labelsDirty = true; } else { rebuildDecorations(); updateLabels(); }
}
function labelsChanged() {
  if (xr) xr.labelsDirty = true; else updateLabels();
}

// ── Desktop mode ────────────────────────────────────────────────────────────
const cam = { yaw: -0.5, pitch: 0.35, dist: 2.0, turn: 0 };   // turn: a head turn about the eye (debug)
const DESKTOP_BG = [0.043, 0.051, 0.07];
let dragging = null;
let pressAt = null;
let mouseOnPanel = false;
let mouseOnCard = false;
/** The message card in the desktop view (?panel3d) takes the mouse first. */
function cardMouse(e, pressed) {
  return !xr && vrPanel.desktop && shell.cardPointer(mouseWorldRay(e), pressed);
}
canvas.addEventListener('pointerdown', (e) => {
  canvas.setPointerCapture(e.pointerId);
  if (cardMouse(e, true)) { mouseOnCard = true; return; }
  if (!xr && panelPointer(mouseWorldRay(e), true, camEye())) {
    // On the panel (?panel3d): its widget, not the camera or a selection.
    mouseOnPanel = true;
    return;
  }
  dragging = { x: e.clientX, y: e.clientY };
  pressAt = { x: e.clientX, y: e.clientY, t: performance.now() };
  state.moving = true;
});
canvas.addEventListener('pointermove', (e) => {
  if (cardMouse(e, mouseOnCard) || mouseOnCard) return;
  if (!xr && (mouseOnPanel || !dragging)) {
    const onPanel = panelPointer(mouseWorldRay(e), mouseOnPanel, camEye());
    if (mouseOnPanel) return;
    if (state.cube) selection.hover(onPanel ? null : mouseRay(e));   // where a click would land
    selectionDirty = true;
    return;
  }
  if (!dragging) return;
  cam.yaw -= (e.clientX - dragging.x) * 0.008;
  cam.pitch = Math.max(-1.5, Math.min(1.5, cam.pitch - (e.clientY - dragging.y) * 0.008));
  dragging = { x: e.clientX, y: e.clientY };
});
canvas.addEventListener('pointerup', (e) => {
  if (mouseOnCard) { mouseOnCard = false; cardMouse(e, false); cardMouse(e, false); return; }
  if (mouseOnPanel) {
    mouseOnPanel = false;
    if (!xr) { panelPointer(mouseWorldRay(e), false, camEye()); panelPointer(mouseWorldRay(e), false, camEye()); }
    return;
  }
  // The press began off the panel, but the panel's pointer saw it down: let it
  // go, or the next click on the panel without moving the mouse is lost.
  if (!xr) panelPointer(mouseWorldRay(e), false, camEye());
  dragging = null;
  state.moving = false;
  // A click (no drag) is the desktop's trigger: the current selection tool at
  // the pointer, through the same ray pick as the controllers.
  const p = pressAt;
  pressAt = null;
  if (p && !xr && state.cube && Math.hypot(e.clientX - p.x, e.clientY - p.y) < 5
      && performance.now() - p.t < 600) {
    const r = mouseRay(e);
    if (!analysis.act(r)) selection.act(r);   // a PV path being drawn takes the click
  }
});

/** The box-space ray under the mouse, from the desktop camera. */
function mouseRay(e) {
  const rect = canvas.getBoundingClientRect();
  const nx = ((e.clientX - rect.left) / rect.width) * 2 - 1;
  const ny = 1 - ((e.clientY - rect.top) / rect.height) * 2;
  const v = lastDesktopView || desktopView();
  const inv = M.invert(M.multiply(v.proj, M.multiply(v.view, state.model)));
  const a = M.transformPoint(inv, [nx, ny, -1]);
  const b = M.transformPoint(inv, [nx, ny, 1]);
  return selection.modelRayToBox(a, M.sub(b, a));
}
/** The world-space ray under the mouse (the panel's; the cube's model is the identity here). */
function mouseWorldRay(e) {
  const rect = canvas.getBoundingClientRect();
  const nx = ((e.clientX - rect.left) / rect.width) * 2 - 1;
  const ny = 1 - ((e.clientY - rect.top) / rect.height) * 2;
  const v = lastDesktopView || desktopView();
  const inv = M.invert(M.multiply(v.proj, v.view));
  const a = M.transformPoint(inv, [nx, ny, -1]);
  const b = M.transformPoint(inv, [nx, ny, 1]);
  return { o: a, d: M.sub(b, a) };
}

/** The desktop camera's position (a dragged panel turns to face it). */
function camEye() {
  const v = lastDesktopView || desktopView();
  return M.position(M.invert(v.view));
}
canvas.addEventListener('wheel', (e) => { cam.dist = Math.max(0.3, Math.min(8, cam.dist * Math.exp(e.deltaY * 0.001))); e.preventDefault(); }, { passive: false });

function desktopView() {
  const w = canvas.width, h = canvas.height;
  let camM = M.translation(0, 0, cam.dist);
  camM = M.multiply(M.rotation([1, 0, 0], -cam.pitch), camM);
  camM = M.multiply(M.rotation([0, 1, 0], cam.yaw), camM);
  if (cam.turn) camM = M.multiply(camM, M.rotation([0, 1, 0], cam.turn));
  return { proj: M.perspective(50 * Math.PI / 180, w / h, 0.02, 50), view: M.invert(camM) };
}

let lastDesktop = 0;
let lastDesktopView = null;
let desktopRunning = false;
function desktopFrame(time) {
  if (xr) { desktopRunning = false; return; }
  requestAnimationFrame(desktopFrame);
  if (gl.isContextLost()) return;      // until it is restored (epic #166)
  const t0 = performance.now();
  const dpr = Math.min(2, window.devicePixelRatio || 1);
  const w = Math.floor(canvas.clientWidth * dpr), h = Math.floor(canvas.clientHeight * dpr);
  if (canvas.width !== w || canvas.height !== h) { canvas.width = w; canvas.height = h; }
  gl.bindFramebuffer(gl.FRAMEBUFFER, null);
  gl.viewport(0, 0, w, h);
  gl.clearColor(...DESKTOP_BG, 1);
  gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
  state.model = M.identity();
  syncSelection();
  R.beginFrame();
  lastDesktopView = desktopView();
  refiner.setSuspended('');
  refineFrame(time, desktopEyes(lastDesktopView, w, h), lastDesktopView.view, DESKTOP_BG);
  if (state.cube) drawScene(lastDesktopView, undefined, 0);
  else if (layers.hasScene()) layers.drawScene(lastDesktopView, 0, h);
  if (vrPanel.visible && vrPanel.desktop) {
    R.drawPanel(lastDesktopView, panelWorld(), panelTexture(time), P.PANEL_SIZE);
    layers.drawStrip(lastDesktopView, panelWorld());
    if (state.cube) analysis.draw(lastDesktopView, panelWorld(), DESKTOP_ANCHOR, true, true);   // planes on the left here
  }
  // The headset's message card, in front of the desktop camera (?panel3d).
  const card = vrPanel.desktop ? shell.cardForFrame(M.invert(lastDesktopView.view)) : null;
  if (card) R.drawPanel(lastDesktopView, card.model, card.tex, card.size);
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
  const session = await navigator.xr.requestSession(mode, { optionalFeatures: ['local-floor', 'hand-tracking'] });
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
  xr = { session, ref, layer, mode, fbScale, fbChosen: fbScale, targetHz, placed: false, last: 0, hands: {}, grab: null,
    buttons: {}, slab: { c: 0.5, w: 0.5 }, bench: bench ? makeBench() : null, place: null, endedBy: '' };
  stats.buf.length = stats.cpu.length = stats.gpu.length = 0;
  governor.max = state.spv; governor.value = state.spv;
  eyeGovernor.reset(fbScale);
  shell.xrStarted();
  shell.clearNotice('xr-ended');
  shell.log('info', 'xr-start', `${mode}${bench ? ' (benchmark)' : ''}, ${targetHz} Hz, eye buffer ${layer.framebufferWidth}×${layer.framebufferHeight}`);
  // The controls guide, the first time in VR (kept in the settings).
  if (shouldShowGuide(settings, { bench })) shell.showGuide(0);
  session.addEventListener('end', () => {
    const b = xr && xr.bench;
    const endedBy = xr ? xr.endedBy : '';
    const summary = `${fpsLine()}; ${currentQualityNote() || 'quality as chosen'}`;
    if (vrPanel.drag) { settings.panel = { ...vrPanel.offset }; persist(); }
    xr = null;
    if (endedBy) {
      shell.log('info', 'xr-end', `by ${endedBy}; ${summary}`);
    } else {
      // Not asked for by the page: the system menu, the headset taken off, the
      // browser. The 2-D page (which the headset shows next) says so.
      shell.log('warning', 'xr-ended', `unexpectedly; ${summary}`);
      if (!gl.isContextLost()) shell.notify('xr-ended');
    }
    // A press held on the panel when the session ended is released.
    vrPanel.pointer.update(null, false); vrPanel.pointer.update(null, false);
    vrPanel.hit = null; vrPanel.drag = null;
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
  const handKeys = new Set();
  for (const src of xr.session.inputSources) {
    if (src.hand) {
      // A tracked hand (issue #191): the same hand object as a controller's,
      // button 0 the pinch, button 1 the closed hand (hands.js).
      handKeys.add(src.handedness);
      const h = handTracker.update(frame, src, xr.ref, performance.now(), xr.headPos, xr.headRight);
      if (h) hands[src.handedness] = h;
      continue;
    }
    if (!src.gripSpace || !src.gamepad) continue;
    const gp = frame.getPose(src.gripSpace, xr.ref);
    if (!gp) continue;
    const m = gp.transform.matrix;
    const hand = { m: new Float32Array(m), pos: [m[12], m[13], m[14]], g: src.gamepad, ray: null };
    const rp = src.targetRaySpace && frame.getPose(src.targetRaySpace, xr.ref);
    if (rp) {
      const r = rp.transform.matrix;
      hand.ray = { o: [r[12], r[13], r[14]], d: [-r[8], -r[9], -r[10]] };
    }
    hands[src.handedness] = hand;
  }
  xr.hands = hands;
  handTracker.keep(handKeys);
  const sq = (h) => hands[h] && hands[h].g.buttons[1] && hands[h].g.buttons[1].pressed;
  const L = sq('left'), Rr = sq('right');
  if (xr.bench) { xr.grab = null; state.moving = false; vrPanel.hit = null; return; }

  // Triggers (issue #171): right = the selection tool along the right ray,
  // left = the next tool. The pick runs on the reduced cube in this frame;
  // what it asks of the backend never holds a frame back.
  const trig = (h) => hands[h] && hands[h].g.buttons[0] && hands[h].g.buttons[0].pressed;
  // The in-VR panel first (issue #172): a ray on it, or a press that began on
  // it, is the panel's, not a selection.
  // The message card before everything (epic #166): a notice or the guide.
  const onCard = shell.cardPointer(hands.right ? hands.right.ray : null, !!trig('right'));
  const onPanel = onCard ? (panelPointer(null, false, xr.headPos), true)
    : panelPointer(hands.right ? hands.right.ray : null, !!trig('right'), xr.headPos);
  const rightRay = hands.right && hands.right.ray && state.cube && !onPanel
    ? selection.worldRayToBox(state.model, hands.right.ray.o, hands.right.ray.d) : null;
  if (onPanel && selection.sel.hover) { selection.hover(null); selectionDirty = true; }
  if (rightRay && (xr.hoverFrame = (xr.hoverFrame || 0) + 1) % 2 === 0) {
    selection.hover(rightRay);
    selectionDirty = true;
  }
  if (pressedEdge('rTrig', trig('right')) && rightRay) {
    // The controller's tip, for a box corner placed inside the cube.
    if (!analysis.act(rightRay)) selection.act(rightRay, rightRay.o);   // a PV path being drawn takes it
  }
  // A left pinch does not change the tool (it happens while grabbing); the panel does.
  if (pressedEdge('lTrig', trig('left')) && !(hands.left && hands.left.isHand)) selection.cycleTool();
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
      setColormap(COLORMAP_NAMES[(COLORMAP_NAMES.indexOf(state.colormap) + 1) % COLORMAP_NAMES.length]);
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
    if (pressedEdge('lStick', g.buttons[3] && g.buttons[3].pressed)) vrPanel.visible = !vrPanel.visible;
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
    if (h.isHand) {
      segs.push(...handSegments(h, col));
    } else {
      segs.push({ a: p(0, 0, 0), b: p(0, 0, -0.08), color: col });
      segs.push({ a: p(-0.015, 0, 0), b: p(0.015, 0, 0), color: col });
      segs.push({ a: p(0, -0.015, 0), b: p(0, 0.015, 0), color: col });
    }
    if (hand === 'right' && h.ray && shell.cardHit) {
      segs.push({ a: h.ray.o, b: shell.cardHit.point, color: [1, 1, 1, 0.7] });
    } else if (hand === 'right' && h.ray && vrPanel.hit) {
      // On the panel: the ray to the point it points at, and a small cross there.
      const q = vrPanel.hit.point;
      segs.push({ a: h.ray.o, b: q, color: [1, 1, 1, 0.6] });
      const pm = panelWorld();
      const ax = M.normalize(M.sub(M.transformPoint(pm, [1, 0, 0]), M.position(pm)));
      const ay = M.normalize(M.sub(M.transformPoint(pm, [0, 1, 0]), M.position(pm)));
      const az = M.normalize(M.sub(M.transformPoint(pm, [0, 0, 1]), M.position(pm)));
      const lift = M.add(q, M.scale3(az, 0.002));   // just in front of the panel
      segs.push({ a: M.add(lift, M.scale3(ax, -0.008)), b: M.add(lift, M.scale3(ax, 0.008)), color: [1, 0.85, 0.3, 1] });
      segs.push({ a: M.add(lift, M.scale3(ay, -0.008)), b: M.add(lift, M.scale3(ay, 0.008)), color: [1, 0.85, 0.3, 1] });
    } else if (hand === 'right' && h.ray && state.cube) {
      // The selection ray, to the point the trigger would pick (or 1.5 m).
      const hv = selection.sel.hover;
      let end = M.add(h.ray.o, M.scale3(M.normalize(h.ray.d), 1.5));
      if (hv) end = M.transformPoint(state.model, hv.p.map((t, i) => (t - 0.5) * state.extent[i]));
      segs.push({ a: h.ray.o, b: end, color: [1, 1, 1, 0.35] });
    }
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

/** The selection panel: to the right of where the cube was placed, turned
 *  toward the viewer; it stays when the cube is moved. */
function selectionPanelModel() {
  const pl = xr.place;
  const right = [Math.cos(pl.yaw), 0, -Math.sin(pl.yaw)];
  const p = M.add(pl.p, M.add(M.scale3(right, 0.55), [0, 0.12, 0]));
  return M.multiply(M.translation(...p), M.rotation([0, 1, 0], pl.yaw - 0.5));
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
  const hp = pose.transform.position;
  xr.headPos = [hp.x, hp.y, hp.z];
  { const hm = pose.transform.matrix; xr.headRight = M.normalize([hm[0], 0, hm[2]]); }
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
    $('hud-text').textContent = lines.join('\n');
  }
  // The page's labels (DOM) four times a second at most, HUD shown or not.
  if (xr.labelsDirty && time - (xr.labelsAt || 0) > 250) { xr.labelsAt = time; updateLabels(); xr.labelsDirty = false; }
  const hm = state.hud ? hudModel() : null;
  syncSelection();
  const selTex = state.cube && !xr.bench ? selection.panelTexture() : null;
  const sm = selTex ? selectionPanelModel() : null;
  const ctlTex = vrPanel.visible && !xr.bench ? panelTexture(time) : null;
  const cm = ctlTex ? panelWorld() : null;
  const card = xr.bench ? null : shell.cardForFrame(pose.transform.matrix);
  // No refinement during the benchmark, nor in passthrough: the backend's frame
  // has no alpha (it is composited over black), so over the room it would
  // either darken it where the headset draws emission the full cube has not,
  // or glow where only the full cube has emission. The headset's own
  // rendering is shown there (docs/user-guide/vr-headset.md).
  // Suspended, the page's render session is closed (no render slot held) and
  // made again on resume.
  refiner.setSuspended(xr.bench ? 'during the benchmark' : xr.mode === 'immersive-ar' ? 'in passthrough' : '');
  if (!refiner.suspended) {
    refineFrame(time, pose.views.map((view) => {
      const vp = layer.getViewport(view);
      return { eye: view.eye === 'right' ? 'right' : 'left', view: view.transform.inverse.matrix,
        proj: view.projectionMatrix, width: vp.width, height: vp.height };
    }), pose.transform.inverse.matrix, [0, 0, 0]);
  }
  for (const view of pose.views) {
    const vp = layer.getViewport(view);
    gl.viewport(vp.x, vp.y, vp.width, vp.height);
    const v = { proj: view.projectionMatrix, view: view.transform.inverse.matrix };
    if (state.cube) drawScene(v, undefined, view.eye === 'right' ? 1 : 0);
    else if (layers.hasScene()) layers.drawScene(v, view.eye === 'right' ? 1 : 0, vp.height);
    if (hm) R.drawPanel(v, hm, hudTex, [0.16, 0.15]);
    if (sm) R.drawPanel(v, sm, selTex, [0.34, 0.248]);
    if (cm) { R.drawPanel(v, cm, ctlTex, P.PANEL_SIZE); layers.drawStrip(v, cm); }
    if (!xr.bench && state.cube) analysis.draw(v, panelWorld(), xr.place, !!cm);
    if (card) R.drawPanel(v, card.model, card.tex, card.size);
    R.drawWorldLines(v);      // the controllers and their rays over the panels
  }
  R.endFrame();
  const cpu = performance.now() - t0;
  stats.push(interval, cpu, R.gpuMs);
  if (xr.bench) xr.bench.record(interval, cpu, R.gpuMs);
  if (state.governorOn && !xr.bench) {
    governor.update(interval, 1000 / xr.targetHz);
    // Second stage (epic #166): samples per voxel at 0.5 or less and frames
    // still missed: a smaller eye buffer, one step at a time.
    const scale = eyeGovernor.update(interval, 1000 / xr.targetHz, spvAtFloor(governor));
    if (scale < xr.fbScale - 1e-6) setEyeBufferScale(scale);
  }
  if (session.frameRate) xr.targetHz = session.frameRate;
  if (!xr.bench) shell.xrFrame(time, stats.buf, xr.targetHz, currentQualityNote());
}

/** The eye buffer made smaller in the running session (a new base layer). */
function setEyeBufferScale(scale) {
  const old = xr.layer;
  try {
    const layer = new XRWebGLLayer(xr.session, gl, { framebufferScaleFactor: scale, antialias: false, alpha: xr.mode === 'immersive-ar' });
    if ('fixedFoveation' in layer) layer.fixedFoveation = 'fixedFoveation' in old ? old.fixedFoveation : readNumber('foveation');
    xr.session.updateRenderState({ baseLayer: layer });
    xr.layer = layer;
    xr.fbScale = scale;
    shell.log('info', 'quality-eye-buffer', `scale ${scale} (${layer.framebufferWidth}×${layer.framebufferHeight}) at ${governor.value.toFixed(2)} samples/voxel, ${fpsLine()}`);
  } catch (err) {
    eyeGovernor.scale = xr.fbScale;      // not lowered: no further try in this session
    eyeGovernor.disabled = true;
    shell.log('warning', 'quality-eye-buffer-failed', String(err.message || err));
  }
}

function fpsLine() {
  const s = stats.summary(xr ? 1000 / xr.targetHz : 0);
  return `${s.fps.toFixed(1)} fps, missed ${(100 * s.missedFraction).toFixed(1)} %, GPU ${s.gpu ? `${s.gpu.mean.toFixed(2)} ms` : 'n/a'}`;
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
  const saved = state.cube ? { info: state.info, cube: state.cube, open: state.open, openPath: state.openPath } : null;
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
      endXR('the benchmark');
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
    state.cube = b.saved.cube; state.info = b.saved.info; state.open = b.saved.open; state.openPath = b.saved.openPath;
    applyExtent(); resetThreshold();
    if (b.saved.open) share.start(b.saved.open.dataset_id);   // the synthetic cubes stopped it
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

// For checks driven from outside (headless browser over CDP): the selection
// state, and a pick through a page-pixel position as a click makes it. Only
// with ?debug in the address.
if (query.has('debug')) {
  window.__vrLayers = layers;
  window.__vrStats = (hz = 0) => stats.summary(hz ? 1000 / hz : 0);
  window.__vrShell = shell;
  window.__vrState = state;
  window.__vrGL = gl;
  // The message card (in the desktop view with ?panel3d): press one of its
  // buttons through a ray from the camera, as a controller would.
  window.__vrCard = {
    current: () => shell.notices.current(),
    press(action) {
      const c = shell.card;
      const w = c.model && c.layout.find((x) => x.action === action);
      if (!w) return false;
      const [x, y, ww, hh] = w.rect;
      const local = [((x + ww / 2) / CARD_PX[0] - 0.5) * CARD_SIZE[0], (0.5 - (y + hh / 2) / CARD_PX[1]) * CARD_SIZE[1], 0];
      const eye = camEye();
      const ray = { o: eye, d: M.sub(M.transformPoint(c.model, local), eye) };
      const took = shell.cardPointer(ray, true);
      shell.cardPointer(ray, false); shell.cardPointer(ray, false);
      return took;
    },
  };
  /** The share of canvas pixels the cube draws over the background, now. */
  window.__vrDrawnFraction = () => {
    const w = canvas.width, h = canvas.height;
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.viewport(0, 0, w, h);
    gl.clearColor(...DESKTOP_BG, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    R.drawVolume(desktopView(), state.model, volParams());
    const px = new Uint8Array(w * h * 4);
    gl.readPixels(0, 0, w, h, gl.RGBA, gl.UNSIGNED_BYTE, px);
    const bg = DESKTOP_BG.map((v) => Math.round(v * 255));
    let n = 0;
    for (let i = 0; i < px.length; i += 4) {
      if (Math.abs(px[i] - bg[0]) + Math.abs(px[i + 1] - bg[1]) + Math.abs(px[i + 2] - bg[2]) > 12) n++;
    }
    return { fraction: n / (w * h), w, h, lost: gl.isContextLost() };
  };
  window.__vrSelection = selection;
  window.__vrAnalysis = analysis;
  window.__vrShare = share;
  window.__vrClick = (x, y) => selection.act(mouseRay({ clientX: x, clientY: y }));
  window.__vrProject = (p) => {      // box coordinate -> page pixel, desktop camera
    const v = lastDesktopView || desktopView();
    const m = p.map((t, i) => (t - 0.5) * state.extent[i]);
    const c = M.transformPoint(M.multiply(v.proj, M.multiply(v.view, state.model)), m);
    const rect = canvas.getBoundingClientRect();
    return [rect.left + (c[0] + 1) / 2 * rect.width, rect.top + (1 - c[1]) / 2 * rect.height];
  };
  // Refinement measurements (issue #170, validation/vr_refinement.mjs).
  const pixelsToPng = (px, w, h) => {
    const c2 = Object.assign(document.createElement('canvas'), { width: w, height: h });
    const ctx = c2.getContext('2d');
    const img = ctx.createImageData(w, h);
    for (let y = 0; y < h; y++) img.data.set(px.subarray((h - 1 - y) * w * 4, (h - y) * w * 4), y * w * 4);
    ctx.putImageData(img, 0, 0);
    return c2.toDataURL('image/png');
  };
  // The in-VR panel in the desktop view (?panel3d): canvas pixel -> page
  // pixel through the panel's quad, and a click there through the same ray
  // code the controllers use; its redraw cost.
  const panelPagePixel = (x, y) => {
    const v = lastDesktopView || desktopView();
    const local = [(x / P.PANEL_PX[0] - 0.5) * P.PANEL_SIZE[0], (0.5 - y / P.PANEL_PX[1]) * P.PANEL_SIZE[1], 0];
    const c = M.transformPoint(M.multiply(v.proj, M.multiply(v.view, panelWorld())), local);
    const rect = canvas.getBoundingClientRect();
    return [rect.left + (c[0] + 1) / 2 * rect.width, rect.top + (1 - c[1]) / 2 * rect.height];
  };
  const panelRayAt = (px, py) => mouseWorldRay({ clientX: px, clientY: py });
  // The analysis panel under it (issue #191): canvas pixel -> page pixel.
  window.__vrTools = {
    layout: analysis.layout,
    pagePixel(x, y) {
      const v = lastDesktopView || desktopView();
      const tm = TP.toolsModel(panelWorld());
      const local = [(x / TP.TOOLS_PX[0] - 0.5) * TP.TOOLS_SIZE[0], (0.5 - y / TP.TOOLS_PX[1]) * TP.TOOLS_SIZE[1], 0];
      const c = M.transformPoint(M.multiply(v.proj, M.multiply(v.view, tm)), local);
      const rect = canvas.getBoundingClientRect();
      return [rect.left + (c[0] + 1) / 2 * rect.width, rect.top + (1 - c[1]) / 2 * rect.height];
    },
  };
  window.__vrPanel = {
    layout: vrPanel.layout, state: vrPanel, view: panelView, settings: () => JSON.parse(JSON.stringify(settings)),
    pagePixel: panelPagePixel,
    /** Where the ray through page pixel (px, py) meets the panel (canvas pixels), or null. */
    hitAt: (px, py) => { const r = panelRayAt(px, py); const h = P.rayPanel(panelWorld(), P.PANEL_SIZE, r.o, r.d); return h && { x: h.x, y: h.y }; },
    /** Press at canvas pixel (x, y) (optionally drag to (x2, y2)) and release, through page pixels and rays. */
    click(x, y, x2 = x, y2 = y) {
      const [p0, q0] = panelPagePixel(x, y), [p1, q1] = panelPagePixel(x2, y2);
      const took = panelPointer(panelRayAt(p0, q0), true, camEye());
      panelPointer(panelRayAt(p1, q1), true, camEye());
      panelPointer(panelRayAt(p1, q1), false, camEye());
      panelPointer(panelRayAt(p1, q1), false, camEye());
      return took;
    },
    /** Times n forced redraws: drawPanel (CPU), texImage2D (CPU side), and both
     *  with a GPU sync (one-pixel readPixels) after the upload. */
    bench(n = 60) {
      const ctx2 = vrPanel.canvas.getContext('2d');
      const draw = [], upload = [], synced = [];
      const px = new Uint8Array(4);
      if (!vrPanel.tex) vrPanel.tex = R.createPanel();
      for (let i = 0; i < n; i++) {
        const view = panelView();
        view.thr = { ...view.thr, f: (i % 50) / 50 };      // a slider moving: a real change each time
        gl.readPixels(0, 0, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, px);
        const t0 = performance.now();
        P.drawPanel(ctx2, vrPanel.layout, view, { hover: 'thr', active: 'thr' });
        const t1 = performance.now();
        R.updatePanel(vrPanel.tex, vrPanel.canvas);
        const t2 = performance.now();
        R.drawPanel(lastDesktopView || desktopView(), panelWorld(), vrPanel.tex, P.PANEL_SIZE);
        gl.readPixels(0, 0, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, px);
        const t3 = performance.now();
        draw.push(t1 - t0); upload.push(t2 - t1); synced.push(t3 - t0);
      }
      vrPanel.key = '';
      const q = (a, f) => { const s = [...a].sort((x, y) => x - y); return s[Math.min(s.length - 1, Math.floor(f * s.length))]; };
      const sum = (a) => ({ p50: q(a, 0.5), p95: q(a, 0.95), max: Math.max(...a) });
      return { n, draw: sum(draw), upload: sum(upload), drawUploadSynced: sum(synced), px: P.PANEL_PX };
    },
  };
  window.__vrRefine = {
    refiner,
    setMode: (m) => { refineMode = m; },
    setCam: (c) => Object.assign(cam, c),
    cam: () => ({ ...cam }),
    stats: () => stats.summary(0),
    resetStats: () => { stats.buf.length = stats.cpu.length = stats.gpu.length = 0; },
    /** One desktop frame drawn now, without the refinement's per-frame update:
     *  'local' (the page alone) or 'refined' (the frame shown, weight 1, with
     *  depth or as a plane), no box or labels; a PNG data URL. */
    snapshot(mode = 'local', useDepth = true) {
      const w = canvas.width, h = canvas.height;
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.viewport(0, 0, w, h);
      gl.clearColor(...DESKTOP_BG, 1);
      gl.clear(gl.COLOR_BUFFER_BIT);
      const v = desktopView();
      R.drawVolume(v, state.model, volParams());
      let drew = false;
      if (mode === 'refined') drew = refiner.draw.draw(v, state.model, 0, 1, useDepth);
      const px = new Uint8Array(w * h * 4);
      gl.readPixels(0, 0, w, h, gl.RGBA, gl.UNSIGNED_BYTE, px);
      return { png: pixelsToPng(px, w, h), drew, w, h };
    },
    /** The backend's frame for the current desktop view with other settings,
     *  never shown: the left eye as a PNG data URL, and its header. */
    async reference(extra = {}, scale = 1) {
      const w = canvas.width, h = canvas.height;
      const v = desktopView();
      const sz = [Math.round(w * scale), Math.round(h * scale)];
      const f = await refiner.requestOnce({ eyes: desktopEyes(v, w, h), aCentre: M.multiply(v.view, state.model),
        model: state.model, zStretch: state.zStretch, background: DESKTOP_BG }, extra, sz);
      const bm = await createImageBitmap(new Blob([f.color], { type: 'image/jpeg' }));
      const c2 = Object.assign(document.createElement('canvas'), { width: sz[0], height: sz[1] });
      c2.getContext('2d').drawImage(bm, 0, 0, sz[0], sz[1], 0, 0, sz[0], sz[1]);
      return { png: c2.toDataURL('image/png'), header: f.header };
    },
    /** Turns the desktop camera at `degPerS` for `ms`, then holds it; resolves
     *  with the refinement's stop-to-refined times (or a timeout). */
    sweep(degPerS, ms, timeoutMs = 5000) {
      return new Promise((resolve) => {
        const t0 = performance.now();
        let last = t0, first = null;
        const step = (t) => {
          if (t - t0 < ms) {
            cam.yaw += (degPerS * Math.PI / 180) * (t - last) / 1000;
            last = t;
            requestAnimationFrame(step);
            return;
          }
          const tStop = performance.now();
          // Times from the camera's stop (the HUD's count from the detector's).
          const handler = (name, d) => {
            if (name === 'refined-first') first = { ms: d.ms, sinceStop: performance.now() - tStop };
            if (name === 'refined-full') {
              refiner.onEvent = null;
              resolve({ first: first && first.ms, full: d.ms, firstSinceStop: first && first.sinceStop, fullSinceStop: performance.now() - tStop });
            }
          };
          refiner.onEvent = handler;
          setTimeout(() => {
            if (refiner.onEvent !== handler) return;      // resolved, or another sweep's
            refiner.onEvent = null;
            resolve({ first: first && first.ms, full: null, timeout: true });
          }, timeoutMs);
        };
        requestAnimationFrame(step);
      });
    },
  };
}

// ── WebGL context loss (epic #166) ──────────────────────────────────────────
// The browser may take the GPU away (memory pressure, another app): every GL
// object is gone. The page asks for it back (preventDefault), stops drawing,
// says so, and on restore makes every object again from what it still holds
// (the cube's voxels, masks, lines, panels). A VR session cannot outlive its
// layer, so it is ended; Enter VR goes back in.
let contextTimer = null;
let contextLostInVR = false;
canvas.addEventListener('webglcontextlost', (e) => {
  e.preventDefault();
  const wasInVR = !!xr;
  shell.log('error', 'webgl-context-lost', wasInVR ? 'in VR' : 'on the page');
  shell.notify('context-lost');
  if (xr) { contextLostInVR = true; endXR('a WebGL context loss'); }
  clearTimeout(contextTimer);
  contextTimer = setTimeout(() => {
    if (gl.isContextLost()) {
      shell.log('error', 'webgl-context-not-restored', 'after 10 s');
      shell.clearNotice('context-lost');
      shell.notify('context-failed', { detail: 'not restored after 10 s' });
    }
  }, 10000);
});
canvas.addEventListener('webglcontextrestored', () => {
  clearTimeout(contextTimer);
  const t0 = performance.now();
  try {
    R.restore();
    layers.restore();
    refiner.draw = new RefineDraw(gl);
    refiner.weight = 0;
    vrPanel.key = '';
    hudLast = 0;
    selectionDirty = true;
    rebuildDecorations();
    shell.clearNotice('context-lost');
    shell.clearNotice('context-failed');
    shell.notify('context-restored', { wasInVR: contextLostInVR });
    shell.log('info', 'webgl-context-restored', `rebuilt in ${(performance.now() - t0).toFixed(0)} ms`);
  } catch (err) {
    shell.clearNotice('context-lost');
    shell.notify('context-failed', { detail: String(err.message || err) });
    shell.log('error', 'webgl-context-restore-failed', String(err.message || err));
  }
  contextLostInVR = false;
  startDesktop();
});

// ── Guide and diagnostics on the page (epic #166) ───────────────────────────
$('show-guide').addEventListener('click', () => {
  const box = $('guide-text');
  box.textContent = shell.guideText();
  box.hidden = !box.hidden;
});
$('copy-diag').addEventListener('click', async () => {
  const text = shell.diagnostics();
  $('diag-out').hidden = false;
  $('diag-out').textContent = text;
  try { await navigator.clipboard.writeText(text); setStatus('Diagnostics copied: paste them into the support request.'); }
  catch { setStatus('Diagnostics shown below the buttons: select and copy them (the clipboard needs https or localhost).'); }
});

// ── Start ───────────────────────────────────────────────────────────────────
shell.start();
checkXR();
updateLabels();
if (query.has('bench')) {
  desktopBench().catch((e) => { document.title = `BENCH FAILED ${e.message}`; console.error(e); });
} else {
  startDesktop();
  if (query.has('synthetic')) loadSynthetic(Number(query.get('synthetic')) || 256);
  else if (pairing.dataset || pairing.path) {
    withLoading(loadPaired).catch((err) => {
      if (!err.superseded) setStatus(`The desktop's cube could not be opened: ${err.message || err}`);
    });
  }
}

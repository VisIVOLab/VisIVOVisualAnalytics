// VisIVO VR page (issue #167): a reduced cube rendered on the headset's GPU.
// Desktop mode (no XR) for development; WebXR immersive-vr / immersive-ar on
// the Quest. Controls: fixed hand roles, one hand menu, explicit modes
// (issue #201). See docs/user-guide/vr/.

import * as M from './mat4.js';
import { Renderer } from './renderer.js';
import { Backend, takeToken, saveToken, tokenError, takePairing, forgetPairing, keepPairing, requestHooks } from './api.js';
import { isPaired, PAIRED_TOKEN, onLinkChange, requestNewCode, transportInfo, linkState } from './transport.js';
import * as V from './volume.js';
import { CubePrep, Newest, prepareCube, CMAX_QUANTILE, CMAX_SAMPLE } from './cube-prep.js';
import { COLORMAP_NAMES, colormapTable } from './colormaps.js';
import { createSelection, createSelectionSync, TOOLS, TOOL_NAMES } from './select-ui.js';
import { channelsOfClip, pixelArcsec } from './selection.js';
import { Refiner, desktopEyes } from './refine-client.js';
import { transferFunction, cropBox, cropError } from './refine.js';
import { pairingFallback } from './pairing.js';
import { loadSettings, saveSettings, clampPanelOffset, thresholdSourceFor, WINDOW_IDS } from './settings.js';
import * as P from './vr-panel.js';
import { createLayers, pairedMode } from './layers.js';
import { createAnalysis } from './analysis.js';
import { PLANE_SIZE } from './product-canvas.js';
// The controls of issue #201: roles and modes, the hand menu, result windows, readout.
import * as C from './controls.js';
import * as HM from './hand-menu.js';
import * as W from './windows.js';
import * as RO from './readout.js';
// The wrist cards and the toasts (issue #256).
import * as WR from './wrist.js';
import { HandTracker } from './hands.js';
import { createShare, boxGeometry, lassoMeta, boxMeta } from './share.js';
import { RefineDraw } from './refine-gl.js';
// Lifecycle, notices, guide, diagnostics, quality (epic #166).
import { createShell } from './shell.js';
import { CARD_PX, CARD_SIZE } from './notice.js';
import { pageBuild, PAGE_VR_API, isHeadsetBrowser, reopenCube, LoadTracker, supersededLoad, PendingReopen } from './lifecycle.js';
import { shouldShowGuide } from './onboarding.js';
import { EyeBufferGovernor, qualityNote, cubeBudget, spvAtFloor, qualityPolicy, textureLimitBytes, planCapacity, sameCapacity } from './quality.js';
// The first Quest 3 session's fixes (issue #226): composition layers for the
// text, solid controllers and hands, one active page.
import { QuadLayers, layersAvailable } from './xr-layers.js';
import { controllerMesh, handMesh } from './models.js';
// The real controller models (WebXR Input Profiles), the page's own as the fallback.
import { ControllerModels } from './controller-models.js';
import { SinglePage, CHANNEL_NAME, stoodDownText } from './single-page.js';
import * as HUB from './headset.js';
import * as XE from './xr-entry.js';
// Measurement records and the guided measurement run (issue #224).
import { createMeasure, recordMessage } from './measure-run.js';

const $ = (id) => document.getElementById(id);
const query = new URLSearchParams(window.location.search);
// Debug-only cuts (issue #229): with ?debug, ?nohover, ?nohud, ... each leave
// one per-frame path out, to find on the headset which one loses the WebGL
// context. None of them clears or changes a selection.
const cut = (name) => query.has('debug') && query.has(name);
const DEBUG_CUTS = ['nohover', 'nohud', 'noreadout', 'noselpanel', 'noseloutline', 'novolume'];
const CUT = Object.fromEntries(DEBUG_CUTS.map((n) => [n, cut(n)]));
const activeCuts = DEBUG_CUTS.filter((n) => CUT[n]);
// The frame-rate and technical numbers in the headset only with ?debug (issue
// #256), and during the headset benchmark, whose results they show.
const DEBUG_HUD = query.has('debug');
// What the cuts leave out, counted (read by validation/vr_cuts.mjs with ?debug).
const cutCounts = { volumeDraws: 0, selLineDraws: 0, selLineSegments: 0, hudUploads: 0, hudQuads: 0, readoutCards: 0, hoverPicks: 0 };

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
// Issue #229: no 3-D texture is allocated during a VR session (on a Quest 3
// a texImage3D then resets the browser's GPU process). R.xrActive is set
// while a session runs; any 3-D allocation then is refused and logged here.
R.onTex3DInXR = (name) => shell.log('error', 'tex3d-in-xr', `${name} refused during the VR session`);
const controllerModels = new ControllerModels(R, { log: (level, event, detail) => shell.log(level === 'warn' ? 'warning' : level, event, detail) });
// In a headset's browser (or the desktop's WebXR stand-in), the controller
// models used last time load now, on the 2-D page, not at session start.
if (isHeadsetBrowser(navigator.userAgent) || query.has('xrfake')) setTimeout(() => controllerModels.preload(), 0);

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
  hud: true,           // the cube card on the secondary wrist (issue #256; the menu's More → Wrist readout)
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
  onChange: () => selSync.mark(),
  onSelection: (s) => publishSelection(s),
  onClear: () => share.dismiss(),   // only called after the share controller exists
  onRecord: (r, detail) => reportProduct(r, detail),   // issue #237: the product goes to the desktop
  noPanel: CUT.noselpanel,
});
// The selection's lines and the page's panel are rebuilt after a change or a
// hover that moved, not at every hover update (issue #229).
const selSync = createSelectionSync(() => syncSelectionNow());

// More analysis in the headset (issue #191): lasso seeds, Isolate/Remove, the
// moment of the selection, the PV path; their page is the menu's Analysis tab.
const analysis = createAnalysis({
  state, selection, renderer: R,
  backend: () => (state.open ? backend : null),
  onChange: () => selSync.mark(),
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
  onChange: () => selSync.mark(),
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
// ?refine_transport=webrtc|jpeg|auto: the refined colour as H.264 video (#189).
const refiner = new Refiner(gl, () => backend, { transport: query.get('refine_transport') || 'auto' });
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
    guideOptions: () => ({ leftHanded: settings.leftHanded }),
    onRetry: () => refiner.retrySoon(),
    publicInfo: (opts) => new Backend('').info(opts),
    summary: () => diagnosticsSummary(),
    paired: () => isPaired(),
    newCode: () => requestNewCode(),
    transport: () => transportInfo(),
    onAction: (id) => {
      if (id === 'measure-cancel') measure.cancel('cancelled by the viewer');
      else if (id === 'take-over') takeOver();
    },
    guidePlacement: () => guidePlacement(),
    beforeReload: () => keepForReload(),
  },
});

// ── Reload after a lost graphics context (issue #226) ───────────────────────
// When the browser does not give the GPU back, the notice's Reload reopens
// the cube on show by its path (a cube from the desktop's link opens again
// from the link anyway). Kept in this tab's session storage for 5 minutes.
const RELOAD_KEY = 'visivo-vr-reopen';
function keepForReload() {
  // Paired (#213) the loader opens the app again; else the cube on show by its
  // path (taken first at the next start, over the link the page came from).
  if (!state.openPath || isPaired()) return;
  try {
    window.sessionStorage.setItem(RELOAD_KEY, JSON.stringify({ path: state.openPath, t: Date.now() }));
    // The token from the link's fragment is gone from the address: this tab's
    // session storage keeps it across the reload (as typing it does).
    const t = tokenField.value.trim();
    if (t) saveToken(t, $('remember').checked);
  } catch { /* no storage: the path is typed again */ }
}
function takeReloadPath() {
  try {
    const raw = window.sessionStorage.getItem(RELOAD_KEY);
    window.sessionStorage.removeItem(RELOAD_KEY);
    const v = raw ? JSON.parse(raw) : null;
    return v && typeof v.path === 'string' && Date.now() - v.t < 300000 ? v.path : '';
  } catch { return ''; }
}
requestHooks.onFailure = (err, path) => shell.requestFailed(err, path);
refiner.onLifecycle = (name, detail) => shell.log('warning', name, detail);
// Paired through the public loader (#213): the link's own states. A drop is
// the lifecycle's "not reachable" (the loader resumes by itself); a pairing
// that cannot be resumed asks for a new code.
onLinkChange((state, reason) => {
  shell.log(state === 'up' ? 'info' : 'warning', `pair-link-${state}`, reason);
  measure.event('link', { state, reason: String(reason || '').slice(0, 160) });
  if (state === 'lost') shell.notify('pair-lost', { detail: reason });
  else if (state === 'up') { shell.clearNotice('pair-lost'); shell.life.retryNow(); refiner.retrySoon(); }
});

// ── Panel wiring ────────────────────────────────────────────────────────────
for (const name of COLORMAP_NAMES) {
  const o = document.createElement('option');
  o.value = o.textContent = name;
  $('cmap').appendChild(o);
}

// Point tables, velocity fields, catalogues over a cube (issue #190): made
// once the page's controls exist (below).
let layers = null;

/** The colour map, from the page, the secondary stick or the menu. */
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
if (isPaired()) {
  // The bridge authenticates for the page: no token to paste (#213).
  tokenField.value = PAIRED_TOKEN;
  $('connect').hidden = true;
}
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
$('synthetic').addEventListener('click', () => loadSyntheticSaying(Number($('synthetic-size').value)));
// The capacity follows the Max side and Texture chosen (issue #235): outside
// VR at once, else at the next Enter VR.
for (const id of ['maxdim', 'encoding']) $(id).addEventListener('change', () => { if (!R.xrActive) ensureCapacity(state.cube, { page: true }); });

function readNumber(id) { return Number($(id).value); }

$('thr').addEventListener('input', () => { state.thrNorm = sliderToNorm(readNumber('thr')); updateLabels(); });
$('cmax').addEventListener('input', () => { state.cmaxNorm = sliderToNorm(readNumber('cmax')); updateLabels(); });
$('thr-source').addEventListener('change', () => {
  settings.thrSource = $('thr-source').value;
  settings.thrSourceChosen = true;
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
$('sel-tool').addEventListener('change', () => { setMode('select'); selection.setTool($('sel-tool').value); });
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
/** Refinement on or off, from the page or the menu. */
function setRefine(on) {
  $('refine').checked = on;
  refiner.setEnabled(on);
  saveRefine();
  // Not chosen by the viewer: the threshold follows (thresholdSourceFor).
  if (state.cube && !settings.thrSourceChosen && $('thr-source').value !== currentThresholdSource()) {
    $('thr-source').value = currentThresholdSource();
    resetThreshold();
  }
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
  onNonCube: () => { selection.clear(); analysis.reset(); share.stop(); ui.undo.clear(); },
  colormap: () => state.colormap, refineOn: () => refineMode !== 'off',
  rebuild: () => rebuildDecorations(),
  targetHz: () => (xr ? xr.targetHz : 60),
  onLayer: (detail) => reportProduct(null, detail),   // issue #237: a catalogue over the cube
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
  // The benchmark's cubes keep no voxels on the page: their values were
  // worked out when they were prepared (prepareBench).
  const n = c.norms || thresholdNorms(c, state.info, $('thr-source').value);
  state.thrNorm = n.thrNorm;
  state.cmaxNorm = n.cmaxNorm;
  updateLabels();
}

/** The threshold and the top of the colour range (0..1) of cube `c` for threshold source `src`. */
function thresholdNorms(c, info, src) {
  const thrNorm = thresholdNorm(c, info, src);
  // Colour range: from the threshold to the 99th percentile of what is above it
  // (emission is sparse: a percentile of the whole cube is still noise). Worked
  // out with the cube's skip grid, off the main thread (issue #246), for each
  // threshold source; computed here for another threshold.
  const known = c.prep && c.prep.quantiles.find(([a]) => a === thrNorm);
  const q = known ? known[1] : V.normQuantile(c.data, c.encoding, CMAX_QUANTILE, CMAX_SAMPLE, thrNorm);
  const cmaxNorm = Math.max(thrNorm + 0.01, q);
  return { thrNorm, cmaxNorm };
}

/** The threshold (0..1) of cube `c` for threshold source `src`. */
function thresholdNorm(c, info, src) {
  const thr = src === 'cube' ? info.default_threshold : c.reducedThreshold;
  const fallback = info.default_threshold ?? c.reducedThreshold;
  const t = thr ?? fallback;
  return t === null || t === undefined ? 0.1 : Math.max(0, V.dataToNorm(t, c.lo, c.hi));
}

// ── Loading ─────────────────────────────────────────────────────────────────
function reducedRequest(datasetId) {
  return {
    dataset_id: datasetId,
    factor_xy: readNumber('fxy'), factor_z: readNumber('fz'), max_dim: readNumber('maxdim'),
    mode: $('mode').value, encoding: $('encoding').value, source: $('source').value,
  };
}

async function withLoading(fn) {
  // A recovery under way (recoverDataset) sees the generation change and
  // leaves the cube of this load alone. Bumped when a load starts and when a
  // cube goes on show (installCube), never when a load ends: one that failed
  // installed nothing, and a recovery started after it is still valid.
  state.loading++;
  state.loadGen++;
  try { return await measuredLoad(fn); } finally { state.loading--; }
}

/** A load's start and end in the measurement record (issue #224), with the
 *  figures of the cube it put on show (installCube's state.load). */
async function measuredLoad(fn) {
  const t0 = performance.now();
  const n = state.loadGen;
  measure.event('load-start', { gen: n });
  let ok = false;
  try {
    const r = await fn();
    ok = true;
    return r;
  } finally {
    const l = state.load || {};
    const c = state.cube;
    measure.event('load-end', { gen: n, ok, ms: Math.round(performance.now() - t0),
      ...(ok && c ? { mb: l.mb ?? null, reduce_ms: l.reduceMs ?? null, transfer_ms: l.transferMs ?? null,
        upload_ms: l.uploadMs ?? null, open_ms: l.openInfoMs ?? null, pyramid_build_ms: l.pyramidBuildMs ?? null,
        dims: c.dims, factors: c.factors, encoding: c.encoding } : {}) });
  }
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

/**
 * reduced_info, waiting while the backend builds the cube's brick pyramid
 * (202 with the fraction done): a large cube's reduced cube is read from a
 * pyramid level, not from the whole cube. The status shows the progress; a
 * newer load ends the wait (stillCurrent), and a build that does not move is
 * given up (V.buildWatch).
 */
async function reducedInfoWhenReady(b, req, ticket) {
  const t0 = performance.now();
  const watch = V.buildWatch();
  let waited = false;
  for (;;) {
    let info = null, progress = 0;
    try {
      // The first request without a deadline of its own (opening a large
      // cube can take long); the polls of a build, two minutes each.
      info = await b.reducedInfo(req, waited ? { timeoutMs: REDUCED_POLL_TIMEOUT_MS } : {});
    } catch (e) {
      if (e && e.preparing) {
        progress = e.progress;              // 202: the pyramid is being built
      } else if (e && e.timedOut && waited) {
        progress = watch.progress();        // a poll that did not answer: asked again
      } else {
        // Any other error ends the load. One met after the build started is
        // not the pairing's dataset check (pairingFallback: loadPaired must
        // not open the path again for it).
        if (e && waited) e.duringBuild = true;
        throw e;
      }
    }
    stillCurrent(ticket);
    if (info) {
      ticket.pyramidBuildMs = waited ? performance.now() - t0 : undefined;
      return info;
    }
    waited = true;
    const verdict = watch.see(progress, performance.now());
    if (verdict) {
      throw new Error(`the brick pyramid's build ${verdict === 'stalled' ? 'has not moved for 5 minutes' : 'took over 2 hours'}`
        + ` (at ${(100 * watch.progress()).toFixed(0)} %): choose "Read from: whole cube" and load again`);
    }
    const sec = ((performance.now() - t0) / 1000).toFixed(0);
    setStatus(`Building the cube's brick pyramid on the backend… ${(100 * (progress || 0)).toFixed(0)} % (${sec} s)\n`
      + 'once for this cube: the headset\'s reduced cube is then read from one of its levels');
    await new Promise((res) => setTimeout(res, 1000));
    stillCurrent(ticket);
  }
}

/** The deadline of each poll of reduced_info while a brick pyramid builds. */
const REDUCED_POLL_TIMEOUT_MS = 120000;

async function loadFromBackend() {
  ensureBackend();
  const path = $('path').value.trim();
  if (!path) throw new Error('Give the path of a cube on the backend.');
  const ticket = loads.begin(path);
  ticket.reduction = reducedRequest('');
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
    // Opened here (not sent by the desktop): its file is reported, so the
    // desktop can open it too (issue #237).
    const Ls = layers.state;
    if (Ls.points) reportProduct(null, { product: 'catalogue', path, dataset_id: Ls.points.datasetId, request: { path } });
    else if (Ls.flow) reportProduct(null, { product: 'flow', path, dataset_id: Ls.flow.datasetId, request: { path, box_mpc: Ls.flow.boxMpc } });
    return;
  }
  const open = await b.open(path);
  stillCurrent(ticket);
  b.sessionNote = state.sessionNote || 'own session';
  setStatus('Reading the axes…');
  const info = await reducedInfoWhenReady(b, { ...ticket.reduction, dataset_id: open.dataset_id }, ticket);
  stillCurrent(ticket);
  await loadReduced(b, open, info, t0, ticket);
}

/**
 * The cube the desktop has open (issue #172), loaded with nothing typed: in
 * the desktop's session, the desktop's dataset when that session still holds
 * it, else its path opened again (in that session, or in a new one when the
 * session is not available here). Without a token, the page says so.
 */
async function loadPaired() { return loadLinked(pairing); }

/** The same for a link {session, dataset, path, kind, box}: the pairing's,
 *  or a cube the desktop sent to this page (its `show` command, issue #237). */
async function loadLinked(link) {
  if (!link.dataset && !link.path) return;
  if (!tokenField.value.trim()) {
    setStatus('The link from the desktop has no token: paste it, then press "Open and load".');
    return;
  }
  ensureBackend();
  const ticket = loads.begin(link.path);
  ticket.reduction = reducedRequest('');
  const b = loadHandle();          // the desktop's session is adopted here, the page's only if this load wins
  const t0 = performance.now();
  if (link.session) b.adoptSession(link.session);
  if (pairedMode(link.kind) !== 'cube') {
    // The desktop's point table or velocity field (issue #190): its dataset in
    // the session, else its path again, as for a cube.
    stillCurrent(ticket);
    if (!link.session) await b.newSession();
    stillCurrent(ticket);
    b.sessionNote = 'the desktop\'s session';
    setStatus('Opening the desktop\'s data…');
    await layers.openPaired(b, link, async (err) => {
      stillCurrent(ticket);
      const next = pairingFallback(err, !!link.path);
      if (next === 'new-session') {
        await b.newSession();
        b.sessionNote = 'a new session (the desktop\'s is not available here)';
      } else if (next === 'open') {
        b.sessionNote = 'the desktop\'s session (opened again)';
      }
      return next;
    }, layerLoad(b, ticket));
    stillCurrent(ticket);
    state.openPath = link.path || '';
    setStatus(`${layers.hudLines().join('\n')}\nin ${state.sessionNote}`);
    return;
  }
  let open = null, info = null, err = null;
  if (link.dataset && link.session) {
    setStatus('Opening the desktop\'s cube…');
    try {
      info = await reducedInfoWhenReady(b, { ...ticket.reduction, dataset_id: link.dataset }, ticket);
      open = { dataset_id: link.dataset, kind: 'cube', valid: true };
      b.sessionNote = 'the desktop\'s session';
    } catch (e) { err = e; }
  } else {
    err = new Error(link.session ? '404: no dataset in the link' : '404: Session not found.');
  }
  if (!open) {
    const next = pairingFallback(err, !!link.path);
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
    open = await b.open(link.path);
    stillCurrent(ticket);
    info = await reducedInfoWhenReady(b, { ...ticket.reduction, dataset_id: open.dataset_id }, ticket);
  }
  stillCurrent(ticket);
  await loadReduced(b, open, info, t0, ticket);
}

/** A cube made in the headset (issue #191: Isolate, Remove), shown in its place. */
async function showDataset(id, path) {
  return withLoading(async () => {
    const ticket = loads.begin(path || '');
    ticket.reduction = reducedRequest('');
    const b = loadHandle();
    const t0 = performance.now();
    if (path) $('path').value = path;
    setStatus('Reading the axes…');
    // An Isolate/Remove output of a large cube is read from its own brick
    // pyramid too: wait for it as a load does.
    const info = await reducedInfoWhenReady(b, { ...ticket.reduction, dataset_id: id }, ticket);
    stillCurrent(ticket);
    await loadReduced(b, { dataset_id: id, kind: 'cube', valid: true }, info, t0, ticket);
  });
}

async function loadReduced(b, open, info, t0, ticket) {
  const req = { ...ticket.reduction, dataset_id: open.dataset_id };
  const t1 = performance.now();
  // The backend caps voxels and bytes; the GPU caps each side and this
  // device its memory. Refuse before reading the cube rather than after.
  checkBudget(info.reduced_shape, req.encoding);
  // Keep automatic factors as 0: the binary route plans them with the same
  // rule as reduced_info (from the cube, or the same pyramid level).
  const fromPyramid = info.source === 'pyramid';
  const what = `Reducing ${info.full_shape.join('×')} by ${info.factors.join('×')} (${req.mode})`
    + `${fromPyramid ? `, from brick pyramid level ${info.pyramid.level}` : ''}`;
  setStatus(`${what}…`);
  // Seconds elapsed while the backend reduces (a whole-cube pass of a large
  // cube takes minutes), until the transfer shows its own progress.
  const tRed = performance.now();
  let receiving = false;
  const ticker = setInterval(() => {
    if (!receiving && loads.isCurrent(ticket)) setStatus(`${what}… ${((performance.now() - tRed) / 1000).toFixed(0)} s`);
  }, 1000);
  let cube;
  try {
    cube = await b.reducedCube(req,
      (f) => { receiving = true; if (loads.isCurrent(ticket)) setStatus(`Transferring… ${(100 * f).toFixed(0)} %`); });
  } finally {
    clearInterval(ticker);
  }
  // A load started meanwhile owns the page: this cube is not shown.
  stillCurrent(ticket);
  if (cube.factors.join() !== info.factors.join()) {
    throw new Error(`the reduced cube came binned ${cube.factors.join('×')}, not ${info.factors.join('×')}: load it again`);
  }
  if (cube.fileRevision && info.file_revision && cube.fileRevision !== info.file_revision) {
    // The file was written again between the axes and the voxels (an
    // Isolate rerun to the same path): the two do not belong together.
    throw new Error('the file changed while it was being loaded: load it again');
  }
  // This load won: its session becomes the page's (unless the token changed).
  if (!commitHandle(b)) throw supersededLoad();
  const { dataset_id, ...reduction } = req;
  cube.reduction = reduction;
  state.load = { openInfoMs: t1 - t0, reduceMs: cube.timing.serverMs, transferMs: cube.timing.totalMs - cube.timing.serverMs, mb: cube.bytes / 1e6, pyramidBuildMs: ticket.pyramidBuildMs };
  if (await installCube(info, cube, open, ticket.path) === 'superseded') throw supersededLoad();
}

/** Throws (and says so on the card) when a cube of `dims` would not fit this device. */
function checkBudget(dims, encoding) {
  const b = cubeBudget({ dims, encoding }, { maxTex3D: R.maxTex3D, headset: isHeadsetBrowser(navigator.userAgent),
    deviceMemoryGB: navigator.deviceMemory, capacity: R.cap });
  if (!b.ok) {
    shell.notify('budget', { detail: b.reason });
    shell.log('warning', 'cube-refused', b.reason);
    throw new Error(`${b.reason} Choose a larger binning.`);
  }
  return b;
}

/** This device's share of GPU memory for the cubes (quality.textureLimitBytes). */
function cubeLimitBytes() {
  return textureLimitBytes({ headset: isHeadsetBrowser(navigator.userAgent), deviceMemoryGB: navigator.deviceMemory });
}

/**
 * Outside VR: the fixed capacity (issue #235) the cubes are written into,
 * so that one arriving in a VR session is shown at once (no 3-D allocation
 * in VR, #229). Planned from the page's Max side and Texture, widened to
 * `cube` (the cube about to be shown, else the one on show) when two banks
 * of it still fit this device's share (quality.planCapacity), and
 * (re)allocated only when that plan changes. A cube that fits no capacity
 * is held in storage of its own; the capacity is then kept only when both
 * fit the share together. `reshow`: the cube on show is put back into the
 * new capacity (false when another cube is shown right after). `page`: the
 * texture is the page's Texture choice (Enter VR, a change of the choices),
 * not `cube`'s. Returns whether `cube` fits the capacity.
 */
let capacitySide = 0;   // the Max side the capacity on the GPU was planned for

function ensureCapacity(cube, { reshow = true, page = false } = {}) {
  if (R.xrActive || gl.isContextLost()) return !!cube && R.fitsCapacity(cube);
  // The share less what other storages of their own hold (the benchmark's
  // cubes, a cube shown from them): the cube shown from its voxels is given
  // back or moved into the capacity, so it is not counted.
  const g = R.gpuStats();
  let held = g.cubeBytes - g.capacityBytes;
  const s = R.slot;
  if (s && !s.cap && s.volume && s.volume === R._volume) held -= s.textureBytes + s.macroBytes + s.maskBytes;
  const limitBytes = cubeLimitBytes() - held;
  const side = Math.min(readNumber('maxdim'), R.maxTex3D);
  const encoding = cube && !page ? cube.encoding : $('encoding').value;
  // The capacity held is kept while it still serves (no reallocation back and
  // forth between a larger cube and smaller ones): same Max side and texture,
  // the cube fits it, and it fits the share.
  if (R.cap && capacitySide === side && R.cap.encoding === encoding && R.cap.totalBytes <= limitBytes
      && (!cube || R.fitsCapacity(cube))) return moveIntoCapacity(cube, reshow);
  let plan = planCapacity({ side, encoding,
    atLeast: cube && cube.encoding === encoding ? cube.dims : null, maxTex3D: R.maxTex3D, limitBytes });
  if (plan && cube && !(cube.encoding === plan.encoding && V.fitsStore(cube.dims, plan.dims))) {
    const own = cubeBudget(cube, { maxTex3D: R.maxTex3D, headset: isHeadsetBrowser(navigator.userAgent), deviceMemoryGB: navigator.deviceMemory });
    if (plan.totalBytes + own.gpuBytes > limitBytes) plan = null;
  }
  capacitySide = side;
  if (!sameCapacity(plan, R.cap)) {
    const mask = R._maskInput;      // the selection shown, kept through a failure below
    try {
      R.reallocCapacity(plan, { reshow });
    } catch (err) {
      // Not enough GPU memory, or a lost context: no capacity (cubes in VR wait for its end).
      try { R.releaseCapacity(); } catch { /* the context is lost */ }
      shell.log('warning', 'capacity-failed', String(err.message || err));
      // The cube on show was in the old capacity: back in storage of its own.
      if (reshow && !R.slot && R._volume && R._volume.data) {
        try {
          R.setVolume(R._volume, R._volume.cell);
          if (mask) R.setMask(mask);
        } catch { /* reported by the next load */ }
      }
    }
    const c = R.cap;
    shell.log('info', 'capacity', c ? `${c.dims.join('×')} ${c.encoding}, two banks and a mask, ${(c.totalBytes / 1e6).toFixed(1)} MB, ${c.allocMs.toFixed(0)} ms`
      : 'none: cubes that arrive in VR wait for its end');
  }
  return moveIntoCapacity(cube, reshow);
}

/** The cube on show, held in storage of its own though the capacity holds
 *  it (shown before the capacity grew), moved into it with its selection.
 *  Returns whether `cube` fits the capacity. */
function moveIntoCapacity(cube, reshow) {
  if (!cube || !R.fitsCapacity(cube)) return false;
  const s = R.slot, v = R._volume;
  if (reshow && cube === state.cube && s && !s.cap && s.volume && s.volume === v && v.data) {
    const mask = R._maskInput;
    R.showVolume(v, v.cell);
    if (mask) R.setMask(mask);
  }
  return true;
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

/** A recovery that met a brick pyramid build, to be continued (lifecycle.js). */
const pendingReopen = new PendingReopen();

async function recoverDataset(reason) {
  const path = state.openPath;
  if (!path || !state.open || !state.info || !state.cube) return { ok: false, detail: 'no cube from the backend on show' };
  // Everything read from the page before the first await (lifecycle.js reopenCube).
  const c = state.cube;
  const snap = { path, fullShape: state.info.full_shape, gen: state.loadGen, fileRevision: state.info.file_revision };
  const req = c.reduction || { factor_xy: c.factors[0], factor_z: c.factors[2], max_dim: readNumber('maxdim'), mode: c.mode, encoding: c.encoding };
  // Its own handle: a new session it makes becomes the page's only if it wins.
  // And a deadline on each of its requests (datasets/open, sessions/new,
  // reduced_info): a hung connection must not stall the recovery; the next
  // check of the lifecycle tries again.
  // A try after one that met a pyramid build continues it: the same handle
  // (its session already the page's) and the dataset it opened, asked again.
  const kept = pendingReopen.take(snap);
  const b = kept ? kept.handle : loadHandle();
  b.timeoutMs = RECOVERY_TIMEOUT_MS;
  const why = kept ? kept.reason : reason;
  const r = await reopenCube(snap, {
    open: (p) => b.open(p),
    newSession: () => b.newSession(),
    reducedInfo: (o) => b.reducedInfo({ dataset_id: o.dataset_id, ...req }),
    classify: (err) => shell.classifyFailure(err),
    loadGen: () => state.loadGen,
  }, kept ? kept.open : null);
  if (r.superseded) return r;            // a newer load: nothing of this one is kept
  if (r.preparing) {
    // The session it made is the page's now (not left behind for the next
    // try to make another); the cube is reopened when the pyramid is ready.
    if (!commitHandle(b)) return { ok: false, superseded: true, detail: 'the token changed during the reopen' };
    pendingReopen.keep(snap, { handle: b, open: r.open, reason: why });
    return { ...r, reason: why };
  }
  if (!r.ok) return r;
  const { open, info } = r;
  if (!commitHandle(b)) return { ok: false, superseded: true, detail: 'the token changed during the reopen' };
  state.open = open;
  state.info = info;
  // Pending selections belong to the old dataset/session. Resume sharing on
  // the reopened id and invalidate old mask/tool work before accepting more.
  selection.clear();
  analysis.reset();
  share.start(open.dataset_id);
  state.sessionNote = why === 'restarted' ? 'a new session (the backend restarted)' : 'a new session (the old one ended)';
  refiner.setDataset(open.dataset_id);
  return { ok: true, detail: `dataset ${open.dataset_id}`, reason: why };
}

/** A synthetic cube of side `n` put on show (installCube's promise; `sync`: at once). */
function loadSynthetic(n, { sync = false } = {}) {
  loads.begin('');              // a newer load than any backend load under way
  const t0 = performance.now();
  checkBudget([n, n, n], 'uint8');
  const data = V.syntheticCube(n);
  const info = V.syntheticInfo(n);
  state.load = { generateMs: performance.now() - t0, mb: data.length / 1e6 };
  return installCube(info, {
    data, dims: [n, n, n], encoding: 'uint8', lo: 0, hi: 1, factors: [1, 1, 1], mode: 'synthetic',
    reducedThreshold: 0.25, bunit: '', fullShape: [n, n, n],
  }, null, '', null, { sync });
}

/** loadSynthetic from a button or the address: a failure said on the card. */
function loadSyntheticSaying(n) {
  try {
    loadSynthetic(n).catch((err) => setStatus(`Failed: ${err.message || err}`));
  } catch (err) {
    setStatus(`Failed: ${err.message || err}`);
  }
}

/**
 * A cube that arrived during a VR session (an Isolate or Remove shown, a
 * load the desktop started): kept here and put on show when the session
 * ends (issue #229: its textures cannot be allocated in VR on a Quest 3).
 * The newest one wins. {info, cube, open, path, load}.
 */
let stagedCube = null;

function stageCube(info, cube, open, path) {
  const replaced = !!stagedCube;
  // The newest load's ticket now: the load being staged (installCube runs at its end).
  stagedCube = { info, cube, open, path, load: state.load, ticket: { id: loads.latest } };
  const what = `${cube.dims.join('×')} ${cube.encoding}${path ? ` (${path.split('/').pop()})` : ''}`;
  shell.log('info', 'cube-waits-for-exit', `${what}${replaced ? ', replacing one already waiting' : ''}`);
  measure.event('cube-staged', { dims: cube.dims, encoding: cube.encoding });
  // Why it waits (issue #235): larger than the capacity, another texture, or the benchmark running.
  const c = R.cap;
  const why = xr && xr.bench ? 'the headset benchmark is running'
    : !c ? 'no room was kept for cubes in VR on this device'
      : c.encoding !== cube.encoding ? `the room kept for cubes in VR is ${c.encoding === 'float16' ? '16-bit' : '8-bit'}, this cube ${cube.encoding === 'float16' ? '16-bit' : '8-bit'}`
        : `it is larger than the room kept for cubes in VR (${c.dims.join('×')})`;
  shell.notify('cube-waiting', { detail: what, why });
  setStatus(`The new cube (${what}) is ready: it is shown when you leave VR.`);
}

/** After a VR session: what had to wait for its end (issue #229). */
function afterXREnd(wasBench = false) {
  if (R.xrActive) return;             // a new session started meanwhile
  if (restoreAfterXR) { restoreAfterXR = false; rebuildGraphics(); }
  if (wasBench) releaseBench();
  installStaged();
}

/** The cube that waited for the end of a VR session, put on show now:
 *  outside VR, with a working context, and unless a newer load began since. */
async function installStaged() {
  if (!stagedCube || R.xrActive || gl.isContextLost()) return;
  const s = stagedCube;
  stagedCube = null;
  shell.clearNotice('cube-waiting');
  if (!loads.isCurrent(s.ticket)) {
    shell.log('info', 'cube-waits-dropped', 'a newer load began after it');
    return;
  }
  state.load = s.load;
  try {
    if (await installCube(s.info, s.cube, s.open, s.path) !== 'shown') return;
    // The desktop's box for it, kept while it waited (issue #237).
    const pb = pendingBox;
    pendingBox = null;
    if (pb && pb.path === s.path) applyCommandBox(pb.cmd);
  } catch (err) {
    setStatus(`Failed: ${err.message || err}`);
    shell.log('error', 'cube-install-failed', String(err.message || err));
  }
}

// Issue #246: a cube's passes over all its voxels (skip grid, colour range,
// apron) run in a module worker, so a switch in VR never stops the frames.
// ?noworker: the synchronous path, as where no worker can be made.
const cubePrep = new CubePrep(typeof Worker === 'function' && !query.has('noworker')
  ? () => new Worker(new URL('./cube-prep-worker.js', import.meta.url), { type: 'module' }) : null);
/** The newest cube put on show wins over one still being prepared. */
const installs = new Newest();
/** The time a step of a write into the capacity may take in VR before the
 *  frame loop runs again (the rest in the next tasks). */
const WRITE_STEP_MS = 4;
const nextTask = () => new Promise((r) => setTimeout(r, 0));

/** The main-thread time of an install, in slices between its waits: the longest is what a frame may wait. */
function blockClock() {
  let t = performance.now(), max = 0, total = 0;
  return {
    pause() { const d = performance.now() - t; max = Math.max(max, d); total += d; },
    resume() { t = performance.now(); },
    end() { this.pause(); return { blockMs: max, mainMs: total }; },
  };
}

/** Would a cube shown now in VR have to wait for the session's end? */
function mustWait(cube) {
  return R.xrActive && ((xr && xr.bench) || !R.fitsCapacity(cube));
}

/** Another cube on the GPU: the benchmark's preparation was sized beside the old one. */
function dropBenchPrep() {
  if (!benchPrep) return;
  if (!benchPrep.busy) releaseBench(true);
  else { benchPrep.cancelled = true; for (const e of benchPrep.cubes.splice(0)) R.releaseVolume(e.slot); }
}

/**
 * Puts a cube on show. `path`: the file the load was started for (never the
 * path field now). `slot`: a volume already on the GPU (the benchmark's,
 * prepared before VR), shown at once. Otherwise the cube's skip grid and
 * colour range are worked out off the main thread first (issue #246), then,
 * in VR, it is written into the capacity in steps between frames. Returns a
 * promise of 'shown', 'staged' (it waits for the end of the session) or
 * 'superseded' (a newer cube was put on show, or a newer load began, while
 * it was prepared or written: the newest wins). `sync`: everything now, on
 * this thread (the desktop benchmark).
 */
function installCube(info, cube, open, path = '', slot = null, { sync = false } = {}) {
  const load = state.load;
  // Fenced by both: a newer install and a newer load (including a desktop
  // command for the cube already on show) make this one stale.
  const ticket = { install: installs.begin(), load: loads.latest };
  if (slot || sync) {
    if (!slot && mustWait(cube)) { stageCube(info, cube, open, path); return Promise.resolve('staged'); }
    if (!slot) dropBenchPrep();
    const clock = blockClock();
    if (!slot && !R.xrActive) ensureCapacity(cube, { reshow: false });
    const up = slot ? R.useVolume(slot) : R.showVolume(cube);
    showInstalled(info, cube, open, path, slot, up, load, clock);
    return Promise.resolve('shown');
  }
  return installPrepared(info, cube, open, path, load, ticket);
}

/** Is an install (installCube's ticket) still the newest, and no load begun since? */
function installCurrent(ticket) {
  return installs.isCurrent(ticket.install) && loads.latest === ticket.load;
}

async function installPrepared(info, cube, open, path, load, ticket) {
  const clock = blockClock();
  // Issue #235: in VR a cube that fits the fixed capacity is written into it
  // and shown at once (texSubImage3D only); a larger one, or any cube during
  // the headset benchmark (its cubes and the one it returns to stay as
  // prepared), waits for the end of the session (#229).
  if (mustWait(cube)) { stageCube(info, cube, open, path); return 'staged'; }
  dropBenchPrep();
  const cell = 8;
  if (!cube.prep || cube.prep.cell !== cell) {
    const aboves = ['cube', 'reduced'].map((src) => thresholdNorm(cube, info, src));
    const opts = { cell, aboves, store: R.cap ? R.cap.dims.slice() : null };
    // The cube's voxels go to the worker and come back (cube.data): this cube
    // is no one else's yet. The one on show is never sent (it is drawn from).
    // Without a worker the work is done in this call and the wait below yields
    // to no frame: one blocking slice with the write that follows.
    const sent = cubePrep.stats.worker;
    const pending = cube === state.cube || (R._volume && R._volume.data === cube.data)
      ? Promise.resolve(prepNow(cube, opts)) : cubePrep.prepare(cube, opts);
    const offThread = cubePrep.stats.worker > sent;
    if (offThread) clock.pause();
    const prep = await pending;
    if (offThread) clock.resume();
    cube.prep = prep;
    if (!installCurrent(ticket)) return 'superseded';
  }
  // A session that began meanwhile.
  if (mustWait(cube)) { stageCube(info, cube, open, path); return 'staged'; }
  let up;
  if (!R.xrActive) {
    // Outside VR: the capacity follows the page's Max side and this cube, and
    // the cube is written at once (the capacity may just have changed).
    ensureCapacity(cube, { reshow: false });
    up = R.showVolume(cube, cell, cube.prep);
  } else {
    // In VR (it fits the capacity): written into the bank not on show, a few
    // milliseconds at a time, the frames drawn in between from the other bank.
    const job = R.beginWrite(cube, cell, cube.prep);
    try {
      while (!job.step(WRITE_STEP_MS)) {
        clock.pause();
        await nextTask();
        clock.resume();
        if (!installCurrent(ticket)) { job.cancelled = true; return 'superseded'; }
      }
      up = R.showWritten(job.slot, cube);
    } catch (err) {
      if (err.code !== 'write-cancelled' || gl.isContextLost()) throw err;
      // The capacity changed under the write (the session ended and the
      // capacity was planned again): the newest cube decides.
      if (!installCurrent(ticket)) return 'superseded';
      if (mustWait(cube)) { stageCube(info, cube, open, path); return 'staged'; }
      if (!R.xrActive) ensureCapacity(cube, { reshow: false });
      up = R.showVolume(cube, cell, cube.prep);
    }
    up.writeSteps = job.steps;
    up.writeStepMs = job.maxStepMs;
  }
  showInstalled(info, cube, open, path, null, up, load, clock);
  return 'shown';
}

/** The cube's values here, on this thread (cube-prep.js prepareCube). */
function prepNow(cube, opts) {
  return prepareCube(cube.data, cube.dims, cube.encoding, opts);
}

/** The page's state after a cube went on show (`up`: its timings). */
function showInstalled(info, cube, open, path, slot, up, load, clock) {
  // Every cube put on show (from the backend, synthetic, the benchmark's)
  // supersedes a recovery under way for the previous one (reopenCube).
  state.loadGen++;
  layers.onCube();
  measure.cubeChanged();
  const inXR = R.xrActive;
  // A cube waiting for the end of the session is older than this one: dropped
  // now, with its notice (installStaged clears it before it gets here).
  if (stagedCube && !slot) {
    stagedCube = null;
    shell.clearNotice('cube-waiting');
    shell.log('info', 'cube-waits-dropped', 'a newer cube was shown');
  }
  state.info = info;
  state.cube = cube;
  state.open = open;
  state.openPath = open ? path : '';
  selection.clear();
  analysis.reset();
  if (open) share.start(open.dataset_id); else share.stop();
  refiner.setDataset(open ? open.dataset_id : null);
  state.clip = { min: [0, 0, 0], max: [1, 1, 1] };
  ui.undo.clear();
  ui.pin = null;
  ui.clipDrag = null;
  applyExtent();
  $('thr-source').value = currentThresholdSource();
  resetThreshold();
  const { blockMs, mainMs } = clock.end();
  const offMain = !!(cube.prep && cube.prep.offMain && !slot);
  state.load = { ...load, uploadMs: up.uploadMs, macroMs: up.macroMs, textureMB: up.textureBytes / 1e6, blockMs, mainMs, offMain,
    writeSteps: up.writeSteps, writeStepMs: up.writeStepMs };
  if (inXR && !slot) {
    const grid = offMain ? `skip grid ${up.macroMs.toFixed(0)} ms off the main thread` : `skip grid ${up.macroMs.toFixed(0)} ms`;
    shell.log('info', 'cube-switched-in-vr', `${cube.dims.join('×')} ${cube.encoding}, written into the capacity in ${up.uploadMs.toFixed(0)} ms (${grid}); main thread ${mainMs.toFixed(0)} ms, longest slice ${blockMs.toFixed(0)} ms`);
    measure.event('cube-switched-in-vr', { dims: cube.dims, encoding: cube.encoding, ms: Math.round(up.uploadMs + (offMain ? 0 : up.macroMs)), blockMs: Math.round(blockMs) });
  }
  const l = state.load;
  setStatus([
    ...(info && info.source_note ? [info.source_note] : []),
    `${cube.dims.join('×')} ${cube.encoding}, binned ${cube.factors.join('×')} (${cube.mode}${V.sourceNote(cube)}) from ${cube.fullShape.join('×')}`,
    ...(l.pyramidBuildMs !== undefined ? [`brick pyramid built in ${(l.pyramidBuildMs / 1000).toFixed(0)} s`] : []),
    l.reduceMs !== undefined ? `reduce ${l.reduceMs.toFixed(0)} ms, transfer ${l.transferMs.toFixed(0)} ms (${l.mb.toFixed(1)} MB)` : `generated in ${l.generateMs.toFixed(0)} ms`,
    `upload ${l.uploadMs.toFixed(0)} ms, skip grid ${l.macroMs.toFixed(0)} ms${offMain ? ' (in a worker)' : ''}, page blocked ${blockMs.toFixed(0)} ms at most${up.capacity ? `, into the ${R.cap.dims.join('×')} capacity (can change in VR)` : !slot ? ', own storage (larger than the capacity: in VR it waits for the end of the session)' : ''}`,
    ...(open ? [`in ${state.sessionNote || 'own session'}`] : []),
  ].join('\n'));
}

/** The threshold source the cube on show starts with (thresholdSourceFor). */
function currentThresholdSource() {
  const c = state.cube, info = state.info;
  return thresholdSourceFor(settings, { synthetic: !c || c.mode === 'synthetic' || !info, debug: DEBUG_HUD });
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
  // The selection's and the analysis' lines are in model space too (extent,
  // clipping box): rebuilt with the box, not only after a hover (issue #229).
  selSync.mark();
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
    labels.push({ pos: [p[0] + 1.6 * L, p[1], p[2]], text: tk.label, height: H, anchor: 'left', face: 'side' });
  }
  labels.push({ pos: [h[0] + 1.6 * L, -h[1] - 3 * L, 0], text: V.axisTitle(az), height: H * 1.1, anchor: 'left', color: '#9fd0ff', face: 'side' });
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
  if (!CUT.novolume) { R.drawVolume(view, state.model, volParams(refined ? { ...override, showMask: false } : override)); cutCounts.volumeDraws++; }
  if (refined) {
    refiner.composite(view, state.model, eye);
    if (R.mask && !CUT.novolume) { R.drawVolume(view, state.model, volParams({ ...override, mode: 0, maskOnly: true })); cutCounts.volumeDraws++; }
  }
  R.drawLines(view, state.model);
  if (!CUT.noseloutline) { R.drawLines(view, state.model, R.selLines); cutCounts.selLineDraws++; }
  layers.drawOverCube(view, state.model);
  R.drawLabels(view, state.model, M.uniformScale(state.model));
}

/** Rebuild what changes with the selections, when it changed (selSync). */
function syncSelection() { selSync.sync(); }
/** Rebuild what changes with the selections: their lines, the page's panel. */
function syncSelectionNow() {
  const own = CUT.noseloutline ? [] : selection.segments();
  cutCounts.selLineSegments += own.length;
  R.setLines([...own, ...analysis.segments()], R.selLines);
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
    quality: `${currentQualityNote() || 'as chosen'}; samples/voxel ${governor.value.toFixed(2)} of ${state.spv}, governor ${state.governorOn ? 'on' : 'off'}; policy: ${qualityPolicy({ targetHz: xr ? xr.targetHz : readNumber('rate'), spvMin: governor.min, spvChosen: state.spv, eyeChosen: xr ? xr.fbChosen : readNumber('fbscale') })}`,
    text: xr ? (xr.quads ? xr.quads.describe() : xr.layersNote) : 'not in VR',
    memory: gpuMemoryLine(),
    refine: refineShort(),
    hub: `${hub.hudLines().join('; ') || 'not linked'}; client ${hub.st.client}`,
    entry: entryLine(),
    cuts: activeCuts.length ? activeCuts.join(' ') : '',
    settings,
  };
}

/** The page's own GPU textures (renderer.gpuStats, issue #226), one line. */
function gpuMemoryLine() {
  const g = R.gpuStats();
  const c = g.capacity;
  return `GPU textures ${(g.totalBytes / 1e6).toFixed(1)} MB: cube ${((g.volumeBytes + g.macroBytes) / 1e6).toFixed(1)}, `
    + `${g.panels} panels ${(g.panelBytes / 1e6).toFixed(1)}, mask ${(g.maskBytes / 1e6).toFixed(2)}; `
    + (c ? `VR capacity ${c.dims.join('×')} ${c.encoding === 'float16' ? '16-bit' : '8-bit'} × ${c.banks} banks + mask ${(c.totalBytes / 1e6).toFixed(1)} MB`
      + ` (${c.onShow ? 'cube in it' : 'cube in its own storage'}, spare ${(c.spareBytes / 1e6).toFixed(1)} MB)` : 'no VR capacity: cubes arriving in VR wait for its end')
    + ` of ${(cubeLimitBytes() / 1048576).toFixed(0)} MiB for cubes`;
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
  if (activeCuts.length) lines.push(`debug cuts: ${activeCuts.join(' ')}`);
  lines.push(`FPS ${s.fps.toFixed(1)}${targetHz ? `  (display ${targetHz} Hz)` : ''}   missed ${(100 * s.missedFraction).toFixed(1)} %`);
  if (s.interval) lines.push(`frame ${s.interval.mean.toFixed(2)} ms mean, ${s.interval.p95.toFixed(2)} p95`);
  lines.push(`CPU ${s.cpu ? s.cpu.mean.toFixed(2) : 'n/a'} ms   GPU ${s.gpu ? `${s.gpu.mean.toFixed(2)} ms` : 'n/a (no timer query)'}`);
  if (c) {
    const si = V.stepInfo(c.dims, effectiveSpv());
    lines.push(`samples/voxel ${effectiveSpv().toFixed(2)}${state.governorOn ? ' (governor)' : ''}   max steps/ray ${si.maxSteps}`);
    lines.push(`volume ${c.dims.join('×')} ${c.encoding} ${(R.textureBytes / 1e6).toFixed(1)} MB`);
    lines.push(c.mode === 'synthetic' ? 'synthetic cube' : `binning ${c.factors.join('×')} ${c.mode}${V.sourceNote(c)} of ${c.fullShape.join('×')}`);
    lines.push(`threshold ${fmt(dataValue(state.thrNorm))} ${c.bunit || ''}  (${$('thr-source').selectedOptions[0].text})`);
  }
  lines.push(...selection.hudLines());
  lines.push(...layers.hudLines());
  lines.push(...analysis.hudLines());
  lines.push(...share.hudLines());
  lines.push(...hub.hudLines());
  const qn = currentQualityNote();
  if (qn) lines.push(qn.slice(0, 60));
  if (state.cube || layers.mode === 'points') lines.push(...refiner.hudLines());
  if (menu.draws) {
    lines.push(`menu redraw ${median(menu.drawMs).toFixed(2)} ms + upload ${median(menu.uploadMs).toFixed(2)} ms (${menu.draws} redraws)`);
  }
  if (readout.ms.length) lines.push(`readout redraw ${median(readout.ms).toFixed(2)} ms   mode ${modeText()}`);
  if (xr) {
    const L = xr.layer;
    lines.push(`eye buffer ${L.framebufferWidth}×${L.framebufferHeight}  scale ${xr.fbScale}  foveation ${'fixedFoveation' in L ? L.fixedFoveation : 'n/a'}`);
    lines.push(`text ${xr.quads ? xr.quads.describe() : xr.layersNote}`.slice(0, 60));
    if (xr.bench) lines.push(xr.bench.status || '');
  } else {
    lines.push(`canvas ${canvas.width}×${canvas.height}`);
  }
  lines.push(gpuMemoryLine());
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

// ── The hand menu, result windows, labels and readout (issues #201, #257) ───
// One menu, held open by the secondary hand's lower button (or its palm turned
// up), standing about a metre in front of the user from the moment it opens
// until it closes (world-locked; its title bar moves it), used with the
// primary ray and trigger. Its tabs follow the work: Display, Range, Select,
// Measure, More (hand-menu.js); choosing one sets the mode. The results stand
// in windows that the ray moves by their title bar and closes with ×. In a
// desktop browser the menu and the windows are drawn in the 3-D view with
// ?panel3d (or the box on the page) and used with the mouse, through the same
// ray code.
const DESKTOP_ANCHOR = { p: [0, 0, 0], yaw: 0 };
const DESKTOP_MENU = { right: 0.95, up: 0, fwd: -0.3, yaw: -0.45 };   // right: the page's panel is on the left
/** In the desktop view the menu is drawn at half its headset size (seen from 2 m, not 1). */
const DESKTOP_MENU_SCALE = 0.5;
const ui = {
  mode: 'explore', clipTool: 'slab', page: 'display',
  menu: { pinned: false, armed: false, wasHeld: false, open: false },
  menuModel: null,     // the menu's pose, taken when it opens (world-locked until it closes)
  menuDrag: null,      // its title bar held: {t, rel} (hand-menu.js dragMenu)
  desktop: query.has('panel3d'),
  undo: new C.UndoStack(),
  pin: null,           // the pinned readout: {p} (box coordinates)
  sliding: null,       // the menu slider held: one undo step per drag
  clipDrag: null,      // a face of the clipping box dragged (Range, Spatial crop tool)
  hit: null,           // where the primary ray meets the menu or a window: {point, model}
  busy: { busy: '', error: '' },   // the tools' state last frame, for the haptics
};
/** The [lo, hi] a range widget of the menu shows now. */
function menuRangeValue(id) {
  if (id === 'slab') return [state.clip.min[2], state.clip.max[2]];
  const crop = /^crop:(\d)$/.exec(id);
  if (crop) return [state.clip.min[+crop[1]], state.clip.max[+crop[1]]];
  const v = layers ? layers.menuView().values[id] : null;
  return Array.isArray(v) ? v : [0, 1];
}
const menu = {
  canvas: Object.assign(document.createElement('canvas'), { width: HM.MENU_PX[0], height: HM.MENU_PX[1] }),
  tex: null, key: '', drawnAt: 0, draws: 0, drawMs: [], uploadMs: [], spec: null, ctx: null,
  pointer: new P.PanelPointer([], { rangeValue: menuRangeValue }),
};
$('panel3d').checked = ui.desktop;
$('panel3d').addEventListener('change', () => { ui.desktop = $('panel3d').checked; });
$('left-handed').checked = settings.leftHanded;
$('left-handed').addEventListener('change', () => setSetting('leftHanded', $('left-handed').checked));
$('labels').checked = settings.labels;
$('labels').addEventListener('change', () => setSetting('labels', $('labels').checked));
$('ui-mode').addEventListener('change', () => setMode($('ui-mode').value));

function setSetting(key, value) {
  settings[key] = value;
  persist();
  if (key === 'leftHanded') {
    $('left-handed').checked = value;
    // Pressed on the menu, which the old secondary hand holds open: kept open
    // (the next press of the new menu button closes it).
    if (xr && ui.menu.open) Object.assign(ui.menu, { pinned: true, armed: false });
  }
  if (key === 'labels') $('labels').checked = value;
}

/** The tool of the mode: the selection tool in Select, slab or box in Clip. */
function currentSub() {
  return ui.mode === 'select' ? selection.sel.tool : ui.mode === 'clip' ? ui.clipTool : null;
}
function inputKinds() {
  const r = C.roles(settings.leftHanded);
  const kind = (h) => (xr && xr.hands[h] && xr.hands[h].isHand ? 'hand' : 'controller');
  return { primary: kind(r.primary), secondary: kind(r.secondary) };
}
function mapOpts(mode = ui.mode, sub = currentSub()) {
  return { mode, sub, leftHanded: settings.leftHanded, pvArmed: analysis.pv.state.armed, input: inputKinds() };
}
function modeText() { return C.modeLine(ui.mode, currentSub(), { pvArmed: analysis.pv.state.armed }); }

function setMode(m) {
  if (!C.MODES.includes(m) || m === ui.mode) return;
  ui.mode = m;
  $('ui-mode').value = m;
  ui.clipDrag = null;
  ui.pin = null;
}
function setClipTool(t) { ui.clipTool = t; ui.clipDrag = null; }

/** Haptics on the hand in `role` (issue #201): 'press', 'done' or 'error'. */
function buzz(role, kind) {
  if (!xr || !settings.haptics) return;
  const r = C.roles(settings.leftHanded);
  const h = xr.hands[role === 'primary' || role === 'secondary' ? r[role] : role];
  if (h && !h.isHand) C.pulse(h.g, kind);
}

// ── Undo: the look before each change ──────────────────────────────────────
/** What is shown: 'cube', or a point table's or velocity field's mode (#257: they have an undo too). */
function lookKind() { return state.cube ? 'cube' : layers.hasScene() ? layers.mode : ''; }
function lookSnapshot() {
  const kind = lookKind();
  const clip = { min: [...state.clip.min], max: [...state.clip.max] };
  if (kind !== 'cube') return { kind, colormap: state.colormap, clip, layer: layers.undoState() };
  return { kind, thrNorm: state.thrNorm, cmaxNorm: state.cmaxNorm, opacity: state.opacity, colormap: state.colormap,
    mode: state.mode, gamma: state.gamma, thrSource: $('thr-source').value, clip, layer: layers.undoState() };   // the catalogue's toggles too
}
function pushUndo() { if (lookKind()) ui.undo.push(lookSnapshot()); }
function undo() {
  const s = ui.undo.pop();
  if (!s) { buzz('primary', 'error'); return false; }
  // A step of other data (a history left from before a switch) is not applied.
  if ((s.kind || 'cube') !== lookKind()) { ui.undo.clear(); buzz('primary', 'error'); return false; }
  ui.clipDrag = null;
  if ((s.kind || 'cube') !== 'cube') {
    state.clip = { min: [...s.clip.min], max: [...s.clip.max] };
    setColormap(s.colormap);
    layers.restoreUndo(s.layer);
    decorationsChanged();
    return true;
  }
  if (s.thrSource && s.thrSource !== $('thr-source').value) {
    $('thr-source').value = s.thrSource;
    settings.thrSource = s.thrSource; persist();
  }
  Object.assign(state, { thrNorm: s.thrNorm, cmaxNorm: s.cmaxNorm, opacity: s.opacity, mode: s.mode, gamma: s.gamma,
    clip: { min: [...s.clip.min], max: [...s.clip.max] } });
  setColormap(s.colormap);
  if (s.layer) layers.restoreUndo(s.layer);
  $('rmode').value = String(state.mode);
  $('opacity').value = Math.round(state.opacity * 100);
  $('gamma').value = String(state.gamma);
  decorationsChanged();
  labelsChanged();
  return true;
}

/** The view back to its start: in the headset the cube in front of you again
 *  (at its first size; the windows follow it), on the page the camera. */
function resetView() {
  if (xr && xr.pose) {
    xr.place = placeInFront(xr.pose);
    state.model = placedModel(xr.place);
    xr.grab = null;
  } else if (!xr) {
    Object.assign(cam, { yaw: -0.5, pitch: 0.35, dist: 2.0, turn: 0 });
  }
}

const slabOf = () => ({ c: (state.clip.min[2] + state.clip.max[2]) / 2, w: (state.clip.max[2] - state.clip.min[2]) / 2 });
function setSlab(s) {
  const k = C.clampSlab(s.c, s.w);
  state.clip.min[2] = k.c - k.w;
  state.clip.max[2] = k.c + k.w;
  decorationsChanged();
}

// ── What the menu shows (issue #257) ───────────────────────────────────────
function cubeName() {
  const c = state.cube;
  return state.openPath ? state.openPath.split('/').pop()
    : c ? (c.mode === 'synthetic' ? 'synthetic cube' : `dataset ${state.open ? state.open.dataset_id : ''}`) : 'no cube';
}

/** The refinement and the status line, for the lifecycle checks (window.__vrPanel). */
function panelView() {
  return {
    refine: { on: refiner.enabled, text: refineShort(), warn: !!(refiner.unavailable || refiner.suspended) },
    // A hidden lifecycle notice first: it is the state of the backend now,
    // a selection's error may be older than it.
    status: shell.badge() || selection.sel.busy || selection.sel.error || currentQualityNote(),
    statusError: !!shell.badge() || (!selection.sel.busy && !!selection.sel.error),
  };
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

/** The range of the spectral axis, named as the desktop names it. */
function spectralName() {
  const z = state.info && state.info.axes && state.info.axes.z;
  return z && (z.velocity_kms || z.full_velocity_kms) ? 'Velocity range' : 'Spectral range';
}

/** The velocity (or spectral) range shown: its world values with the
 *  convention, and its channels. */
function slabNote() {
  const info = state.info;
  if (!info || !info.full_shape) return '';
  const nz = info.full_shape[2];
  const [a, b] = channelsOfClip([state.clip.min[2], state.clip.max[2]], nz);
  const ch = `ch ${a}–${b} of ${nz}`;
  const axis = info.axes && info.axes.z;
  if (!axis) return ch;
  const arr = axis.velocity_kms || axis.world;
  const at = (t) => V.texToWorld(arr, V.boxToTex(t, nz, info.factors[2]));
  const lo = at(state.clip.min[2]), hi = at(state.clip.max[2]);
  const u = axis.velocity_kms ? 'km/s' : axis.unit || '';
  if (lo === null || lo === undefined || hi === null || hi === undefined) return ch;
  return `${fmt(lo)} to ${fmt(hi)} ${u}${axis.velocity_kms && axis.velocity_convention ? ` (${axis.velocity_convention})` : ''} · ${ch}`;
}

/** What the crop keeps on axis i: full pixels of a cube, data units of a point table or field. */
function cropNote(i) {
  const lo = state.clip.min[i], hi = state.clip.max[i];
  if (state.cube && state.info && state.info.full_shape) {
    const n = state.info.full_shape[i];
    const a = Math.max(0, Math.min(n - 1, Math.round(lo * n))), b = Math.max(a, Math.min(n - 1, Math.round(hi * n) - 1));
    return `${'xyz'[i]} ${a}–${b} of ${n} px`;
  }
  const box = layers.state.box;
  if (!box) return '';
  const at = (t) => box.lo[i] + t * (box.hi[i] - box.lo[i]);
  return `${fmt(at(lo))} to ${fmt(at(hi))}`;
}

let pixelScale = { info: null, arcsec: null };
function pvWidthNote() {
  const w = analysis.pv.state.width || 1;
  if (pixelScale.info !== state.info) pixelScale = { info: state.info, arcsec: state.info ? pixelArcsec(state.info) : null };
  return pixelScale.arcsec ? `${w} px = ${(w * pixelScale.arcsec).toFixed(1)}″` : `${w} px`;
}

/** The context the menu's layout is a function of (hand-menu.js). */
function menuContext() {
  const lc = layers.menuContext();
  const s = selection.sel, a = analysis;
  const kind = state.cube ? 'cube' : lc.kind === 'cube' ? 'none' : lc.kind;
  const lasso = !!(s.lasso && (s.lasso.bytes || s.lasso.request));
  const av = a.view();
  return {
    ...HM.defaultContext(),
    kind, debug: DEBUG_HUD, colormap: state.colormap, render: state.mode, spectralName: spectralName(),
    cat: lc.cat, clipTool: ui.clipTool,
    sel: { tool: s.tool, seedMode: s.seedMode || 'new', lasso, box: !!s.box, applied: !!a.lasso.state.applied },
    meas: { lasso, moment: av.moment, pvArmed: av.pvArmed, spectrum: !!s.spectrum, pinned: selection.hasPin() },
    pts: lc.pts, flow: lc.flow,
    more: { leftHanded: settings.leftHanded, labels: settings.labels, haptics: settings.haptics, hud: state.hud },
    dbg: { governor: state.governorOn, refine: refiner.enabled, refineDepth: refiner.useDepth, refineScale: `${refiner.o.scale}×`,
      record: measure.recording, run: measure.running ? 'Cancel run' : 'Run protocol', thrSource: $('thr-source').value },
    keep: ui.menu.pinned, canUndo: ui.undo.size > 0,
  };
}

/** What the menu shows besides its layout: the title, the rows' values and
 *  notes, the sliders' and ranges' positions, the status lines. */
function menuView(ctx = menuContext()) {
  const c = state.cube;
  const lv = layers.menuView();
  const notes = { ...lv.notes }, values = { ...lv.values };
  const status = [];
  const pv = panelView();
  if (pv.status) status.push([pv.status, pv.statusError ? '#ff8a8a' : '#ffd27a']);
  status.push(...lv.status);
  for (const i of [0, 1, 2]) {
    values[`crop:${i}`] = [state.clip.min[i], state.clip.max[i]];
    notes[`crop${i}`] = ctx.kind === 'none' ? '' : cropNote(i);
  }
  const page = ui.page;
  if (c) {
    const unit = c.bunit ? ` ${c.bunit}` : '';
    values.thr = normToSlider(state.thrNorm) / 1000;
    values.cmax = normToSlider(state.cmaxNorm) / 1000;
    values.opacity = state.opacity / 2;
    values.slab = [state.clip.min[2], state.clip.max[2]];
    notes.cmap = state.colormap;
    const src = $('thr-source').value === 'cube' ? 'the full cube\'s noise, as on the desktop' : 'this binning\'s noise';
    notes.thr = `${fmt(dataValue(state.thrNorm))}${unit} (starts from ${src})`;
    notes.cmax = `${fmt(dataValue(state.cmaxNorm))}${unit}`;
    notes.opacity = state.opacity.toFixed(2);
    notes.render = state.mode === 1 ? 'MIP: the brightest value along each ray' : 'Emission: the voxels add up along each ray';
    notes.slab = slabNote();
    notes.trigger = ui.clipTool === 'box' ? 'Trigger held near a face of the crop: drag it.' : 'Trigger: the range centred on the emission it meets.';
    notes.crop = 'Reset crop keeps the range.';
    notes.tool = TOOL_NAMES[selection.sel.tool] ? `Trigger: ${TOOL_NAMES[selection.sel.tool]}` : '';
    const sum = selectionSummary();
    notes.selection = sum && sum.length ? sum[0] : '';
    notes.moment = ctx.meas.lasso ? 'Of the lasso object (its mask).'
      : 'Whole field over the velocity range, the desktop\'s automatic threshold mask.';
    notes.pv = analysis.pv.state.armed ? 'Trigger: the next point of the slice.' : '';
    notes.pvw = pvWidthNote();
    const info = (t) => [t, '#b9c2d0'];
    if (page === 'select') status.push(...[...(sum || []).slice(1), ...analysis.lasso.lines()].map(info));
    if (page === 'measure') {
      const av = analysis.view();
      if (av.error) status.push([av.error, '#ff8a8a']);
      status.push(...[...analysis.moment.lines(), ...analysis.pv.lines()].filter((l) => l !== av.error).map(info));
    }
  }
  if (page === 'debug') status.push(`Refinement: ${refineShort()}`);
  return { title: `VisIVO VR · ${ctx.kind === 'points' || ctx.kind === 'flow' ? (state.openPath || '').split('/').pop() || ctx.kind : cubeName()}`,
    notes, values, status };
}

/** The menu's widgets for the page on show; the page and the context kept for the drawing. */
function menuLayout() {
  const ctx = menuContext();
  if (ui.page === 'debug' && !ctx.debug) ui.page = 'more';
  menu.ctx = ctx;
  menu.spec = HM.pageSpec(ui.page, ctx);
  menu.pointer.layout = HM.menuLayout(ui.page, ctx);
  return menu.pointer.layout;
}

/** The menu's texture, redrawn when what it shows changed (at most 30 times a second). */
function menuTexture(now) {
  if (!menu.tex) menu.tex = R.createPanel();
  menuLayout();      // the page may have changed since the pointer's update
  const view = menuView(menu.ctx);
  const uiState = { hover: menu.pointer.hover, active: menu.pointer.active ? menu.pointer.active.id : null };
  const key = JSON.stringify([ui.page, menu.pointer.layout, view, uiState]);
  if (key !== menu.key && (now - menu.drawnAt >= 33 || !menu.draws)) {
    const t0 = performance.now();
    HM.drawMenu(menu.canvas.getContext('2d'), ui.page, menu.spec, view, uiState, menu.ctx);
    const t1 = performance.now();
    R.updatePanel(menu.tex, menu.canvas);
    const t2 = performance.now();
    menu.drawMs.push(t1 - t0); menu.uploadMs.push(t2 - t1);
    if (menu.drawMs.length > 60) { menu.drawMs.shift(); menu.uploadMs.shift(); }
    menu.key = key; menu.drawnAt = now; menu.draws++;
  }
  return menu.tex;
}

const median = (a) => { if (!a.length) return NaN; const s = [...a].sort((x, y) => x - y); return s[s.length >> 1]; };

/** Is the menu on show (held in the headset, or in the desktop's 3-D view)? */
function menuShown() { return xr ? (!xr.bench && ui.menu.open && !!ui.menuModel) : ui.desktop; }
/** The menu's size in metres: in the headset, its angular size at MENU_DIST. */
function menuSize() { return xr ? HM.MENU_SIZE : HM.MENU_SIZE.map((v) => v * DESKTOP_MENU_SCALE); }
/** The menu's pose: where it opened (or was moved to) in the headset. */
function menuWorld() {
  if (!xr) return P.panelModel(DESKTOP_ANCHOR, DESKTOP_MENU);
  return ui.menuModel;
}
/** The menu opening: in front of the eyes, on the tab of the mode, world-locked until it closes. */
function updateMenuPose(opened) {
  if (!xr || !ui.menu.open) { ui.menuDrag = null; return; }
  if (opened) {
    ui.page = HM.pageOnOpen(ui.page, ui.mode);
    layers.state.s.fieldPage = 0;      // the first page of columns, with the velocity ones (hand-menu.js fieldChips)
  }
  if ((opened || !ui.menuModel) && xr.pose) ui.menuModel = HM.menuFront(xr.pose.transform.matrix);
}
// ── The result windows ──────────────────────────────────────────────────────
const wins = Object.fromEntries(WINDOW_IDS.map((id) => [id, {
  id, def: W.WINDOWS[id], pointer: new W.WindowPointer(), drag: null, closedKey: null, model: null, size: null,
  bar: Object.assign(document.createElement('canvas'), { width: W.BAR_PX[0], height: W.BAR_PX[1] }), barTex: null, barKey: '',
  offset: settings.windows[id] || { ...W.WINDOWS[id].place }, desktopOffset: { ...W.WINDOWS[id].desktop },
}]));

/** A window's content this frame: {tex, size}, or null when it has nothing to show or was closed. */
function windowContent(w) {
  if (!state.cube || (xr && xr.bench)) return null;
  if (w.id === 'result') {
    if (CUT.noselpanel) return null;      // debug (issue #229): no panel painted nor uploaded
    const s = selection.sel;
    const has = s.pick || s.lasso || s.spectrum || s.box || s.busy || s.error || s.corner;
    // Only with something to show (issue #256): Select mode alone no longer
    // opens it (the legend on the primary wrist says what the trigger does).
    if (!has) return null;
    const k = resultKey();
    if (w.closedKey && k.every((v, i) => v === w.closedKey[i])) return null;
    return { tex: selection.panelTexture(), size: w.def.size };
  }
  const tex = analysis.productTexture(w.id);
  return tex ? { tex, size: PLANE_SIZE } : null;
}
/** What the Selection window shows: a new pick, result, state or error opens it again. */
function resultKey() {
  const s = selection.sel;
  return [s.pick, s.spectrum, s.lasso, s.box, s.corner, s.busy, s.error];
}
function closeWindow(w) {
  if (w.id === 'result') w.closedKey = resultKey();
  else analysis.setShown(w.id, false);
}
function windowAnchor() { return xr ? xr.place : DESKTOP_ANCHOR; }
/** The windows on show, with their poses for this frame. */
function shownWindows() {
  if (!xr && !ui.desktop) return [];
  const out = [];
  for (const w of Object.values(wins)) {
    const c = windowContent(w);
    if (!c) { w.model = null; continue; }
    w.size = c.size; w.tex = c.tex;
    w.model = W.windowModel(windowAnchor(), xr ? w.offset : w.desktopOffset);
    out.push(w);
  }
  return out;
}
function barTexture(w) {
  if (!w.barTex) w.barTex = R.createPanel();
  const uiState = { hover: w.pointer.bar.hover, active: w.pointer.bar.active ? w.pointer.bar.active.id : null };
  const key = JSON.stringify([w.def.title, uiState]);
  if (key !== w.barKey) {
    W.drawBar(w.bar.getContext('2d'), w.def.title, uiState);
    R.updatePanel(w.barTex, w.bar);
    w.barKey = key;
  }
  return w.barTex;
}
function drawWindows(v, list) {
  for (const w of list) {
    R.drawPanel(v, w.model, w.tex, w.size);
    R.drawPanel(v, W.barModel(w.model, w.size), barTexture(w), W.barSize(w.size[0]));
  }
}

function applyWindow(w, a, ray, eye) {
  if (a.type === 'press' && a.id === 'close') { closeWindow(w); buzz('primary', 'press'); return; }
  if (a.type === 'drag-start') {
    w.drag = { t: a.hit.t, rel: M.sub(M.position(w.model), a.hit.point) };
    buzz('primary', 'press');
  } else if ((a.type === 'drag' || a.type === 'drag-end') && w.drag && ray && eye) {
    const p = M.add(M.add(ray.o, M.scale3(ray.d, w.drag.t)), w.drag.rel);
    // Within the limits the settings keep, so a window let go far away is
    // saved where it stops, not silently dropped on the next load.
    const off = clampPanelOffset(P.offsetFor(windowAnchor(), p, eye));
    if (xr) w.offset = off; else w.desktopOffset = off;
  }
  if (a.type === 'drag-end') {
    // Kept for the next sessions in the headset only (the desktop's place is a development view).
    if (w.drag && xr) { settings.windows = { ...settings.windows, [w.id]: { ...w.offset } }; persist(); }
    w.drag = null;
  }
}

/**
 * The primary ray on the menu and the windows this frame (world space, or
 * null), `pressed` the trigger (or the mouse button), `eye` the viewer's
 * position (a dragged window turns to face it). The nearest surface takes the
 * ray; one holding a press keeps it. Returns whether they take the ray (the
 * selection tool then neither hovers nor fires).
 */
function uiPointer(ray, pressed, eye) {
  const surfaces = [];
  if (menuShown()) {
    menuLayout();
    const m = menuWorld();
    surfaces.push({ kind: 'menu', model: m, hit: ray ? P.rayPanel(m, menuSize(), ray.o, ray.d, HM.MENU_PX) : null,
      captured: !!menu.pointer.active });
  }
  const list = shownWindows();
  for (const w of list) {
    const h = W.windowHits(w.model, w.size, ray);
    let hit = h.t !== null ? { t: h.t, point: h.point } : null;
    // The cube in front of a window takes the ray (a window held keeps it).
    if (hit && state.cube && !w.pointer.captured
        && P.panelBehindCube(hit.t, state.model, state.extent, ray.o, ray.d, state.clip.min, state.clip.max)) hit = null;
    surfaces.push({ kind: 'window', w, hits: hit ? h : { bar: null, content: null }, hit, captured: w.pointer.captured });
  }
  const cap = surfaces.findIndex((s) => s.captured);
  const win = W.pickSurface(surfaces.map((s) => s.hit), cap);
  ui.hit = null;
  let takes = false;
  surfaces.forEach((s, i) => {
    const mine = i === win;
    if (s.kind === 'menu') {
      for (const a of menu.pointer.update(mine ? s.hit : null, pressed)) applyMenu(a, ray, eye);
      if (mine && s.hit) ui.hit = { point: s.hit.point, model: s.model };
      takes = takes || menu.pointer.takesRay() || menu.pointer.owns;
    } else {
      for (const a of s.w.pointer.update(mine ? s.hits : { bar: null, content: null }, pressed)) applyWindow(s.w, a, ray, eye);
      if (mine && s.hit) ui.hit = { point: s.hit.point, model: s.w.model };
      takes = takes || s.w.pointer.takesRay();
    }
  });
  // Surfaces not on show follow the trigger, so one shown again under a held
  // trigger does not take that press as a new one.
  if (!menuShown()) menu.pointer.update(null, pressed);
  for (const w of Object.values(wins)) if (!list.includes(w)) { w.pointer.update({ bar: null, content: null }, pressed); w.drag = null; }
  if (!menu.pointer.active) ui.sliding = null;
  return takes;
}

/** A tab chosen: its page, and the mode it sets (Display and Measure:
 *  explore; Range: clip; Select: select). */
function setPage(page) {
  ui.page = page;
  const m = HM.TAB_MODE[page];
  if (m) setMode(m);
}

/** The menu moved by its title bar (headset only: the desktop view keeps it beside the cube). */
function moveMenu(a, ray, eye) {
  if (!xr) return;
  if (a.type === 'drag-start' && a.hit && ui.menuModel) {
    ui.menuDrag = { t: a.hit.t, rel: M.sub(M.position(ui.menuModel), a.hit.point) };
    buzz('primary', 'press');
  } else if ((a.type === 'drag' || a.type === 'drag-end') && ui.menuDrag && ray && eye) {
    ui.menuModel = HM.dragMenu(ui.menuDrag, ray, eye);
  }
  if (a.type === 'drag-end') ui.menuDrag = null;
}

/** A press, slide, range or drag on the menu. A greyed control does nothing
 *  but an error pulse (its reason is written under it). */
function applyMenu(a, ray = null, eye = null) {
  if (a.type === 'drag-start' || a.type === 'drag' || a.type === 'drag-end') { moveMenu(a, ray, eye); return; }
  const id = a.id;
  const w = a.widget || HM.widgetById(menu.pointer.layout, id) || {};
  if (w.disabled) { if (a.type === 'press') buzz('primary', 'error'); return; }
  if (a.type === 'slide' || a.type === 'range') { menuSlide(a); return; }
  if (a.type !== 'press') return;
  buzz('primary', 'press');
  if (w.group === 'tab') { setPage(w.value); return; }
  // The footer.
  if (id === 'undo') { undo(); return; }
  if (id === 'reset-view') { resetView(); return; }
  if (id === 'menu-pin') { ui.menu.pinned = !ui.menu.pinned; ui.menu.armed = false; return; }
  // Display: the cube's look (one undo step each); a point table's, a field's, the catalogue's.
  if (w.group === 'cmap') { pushUndo(); setColormap(w.value); return; }
  if (w.group === 'render') { pushUndo(); state.mode = w.value; $('rmode').value = String(state.mode); return; }
  if (id === 'thr-reset') { pushUndo(); resetThreshold(); return; }
  // A point table's or a field's look (not a new sample or new lines): one undo step.
  if (layers.undoable(id, w)) pushUndo();
  if (layers.press(id, w)) return;
  // Range.
  if (id === 'slab-all') { pushUndo(); state.clip.min[2] = 0; state.clip.max[2] = 1; decorationsChanged(); return; }
  if (w.group === 'sub-clip') { setMode('clip'); setClipTool(w.value); return; }
  if (id === 'crop-reset') {
    pushUndo();
    // On a cube the third axis is the velocity range: Full band resets it.
    for (const i of state.cube ? [0, 1] : [0, 1, 2]) { state.clip.min[i] = 0; state.clip.max[i] = 1; }
    decorationsChanged();
    return;
  }
  // Select.
  if (w.group === 'tool') { setMode('select'); selection.setTool(w.value); return; }
  if (w.group === 'seed') {
    // A seed mode is for the lasso: Select with the Kinematic Lasso.
    setMode('select');
    if (selection.sel.tool !== 'lasso') selection.setTool('lasso');
    analysis.press(id, w);
    return;
  }
  if (id === 'sel-clear') { selection.clear(); analysis.sync(); return; }
  if (id === 'isolate' || id === 'remove' || id === 'show-applied') { analysis.press(id, w); return; }
  // Measure.
  if (w.group === 'moment' || id === 'moment-hide' || id.startsWith('pv-')) { analysis.press(id, w); return; }
  if (id === 'spec-pin') { selection.pinSpectrum(); return; }
  // More.
  if (w.group === 'hand') { setSetting('leftHanded', w.value); return; }
  if (id === 'guide') { shell.help(); return; }
  if (id === 'debug-page') { ui.page = 'debug'; return; }
  if (id === 'debug-back') { ui.page = 'more'; return; }
  // The technical settings (?debug).
  if (w.group === 'thrsrc') {
    pushUndo();
    $('thr-source').value = w.value;
    settings.thrSource = w.value; settings.thrSourceChosen = true; persist();
    resetThreshold();
    return;
  }
  const k = w.key;
  if (k === 'labels' || k === 'haptics') setSetting(k, !settings[k]);
  else if (k === 'hud') state.hud = !state.hud;
  else if (k === 'governor') { state.governorOn = !state.governorOn; $('governor').checked = state.governorOn; governor.value = state.spv; }
  else if (k === 'refine') setRefine(!refiner.enabled);
  else if (k === 'record') { measure.toggleRecord(); updateMeasureDom(); }
  else if (k === 'measureRun') startOrCancelRun();
  else if (k === 'refineDepth') { $('refine-depth').checked = !refiner.useDepth; refiner.useDepth = $('refine-depth').checked; saveRefine(); }
  else if (k === 'refineScale') {
    const steps = [0.4, 0.6, 0.8, 1];
    const next = steps[(steps.indexOf(refiner.o.scale) + 1) % steps.length];
    $('refine-scale').value = String(next); refiner.o.scale = next; saveRefine();
  }
}

/** A slider or a range held on the menu: one undo step for the whole drag. */
function menuSlide(a) {
  if (ui.sliding !== a.id) { pushUndo(); ui.sliding = a.id; }
  if (a.type === 'slide') {
    if (a.id === 'thr') state.thrNorm = Math.min(state.cmaxNorm - 1e-3, sliderToNorm(a.value * 1000));
    else if (a.id === 'cmax') state.cmaxNorm = Math.max(state.thrNorm + 1e-3, sliderToNorm(a.value * 1000));
    else if (a.id === 'opacity') { state.opacity = Math.max(0.01, a.value * 2); $('opacity').value = Math.round(state.opacity * 100); }
    else layers.slide(a.id, a.value);
    labelsChanged();
    return;
  }
  const crop = /^crop:(\d)$/.exec(a.id);
  if (a.id === 'slab' || crop) {
    const i = crop ? +crop[1] : 2;
    [state.clip.min[i], state.clip.max[i]] = a.value;
    decorationsChanged();
  } else {
    layers.setRange(a.id, a.value);
  }
}

/** The clipping box changed: its lines and labels (throttled in VR). */
function decorationsChanged() {
  if (xr) { xr.slabDirty = true; xr.labelsDirty = true; } else { rebuildDecorations(); updateLabels(); }
}
function labelsChanged() {
  if (xr) xr.labelsDirty = true; else updateLabels();
}

// ── The cursor readout and the controller labels ───────────────────────────
const readout = {
  live: { canvas: Object.assign(document.createElement('canvas'), { width: RO.READOUT_PX[0], height: RO.READOUT_PX[1] }), tex: null, key: '', at: 0 },
  pin: { canvas: Object.assign(document.createElement('canvas'), { width: RO.READOUT_PX[0], height: RO.READOUT_PX[1] }), tex: null, key: '', at: 0 },
  ms: [],
};
/** The readout's lines at box point p (or the mode alone). */
function readoutAt(p) {
  return RO.readoutLines(modeText(), p, { info: state.info, cube: state.cube, texScale: state.texScale });
}
/** The same, kept while the point, the mode and the cube's look are the same (per frame in VR). */
function readoutCached(card, p) {
  const mt = modeText();
  if (card.p !== p || card.mt !== mt || card.cube !== state.cube || card.thr !== state.thrNorm) {
    Object.assign(card, { p, mt, cube: state.cube, thr: state.thrNorm, lines: readoutAt(p) });
  }
  return card.lines;
}
/** A readout card's texture, redrawn when its text changed (at most 15 times a second). */
function readoutTexture(card, lines, pinned, now) {
  if (!card.tex) card.tex = R.createPanel();
  if (lines === card.shown) return card.tex;       // the same text as drawn (readoutCached)
  const key = JSON.stringify(lines);
  if (key !== card.key && (now - card.at >= 66 || !card.key)) {
    const t0 = performance.now();
    RO.drawReadout(card.canvas.getContext('2d'), lines, pinned);
    R.updatePanel(card.tex, card.canvas);
    readout.ms.push(performance.now() - t0);
    if (readout.ms.length > 60) readout.ms.shift();
    card.key = key; card.at = now; card.shown = lines;
  }
  return card.tex;
}
const boxToWorld = (p) => M.transformPoint(state.model, p.map((t, i) => (t - 0.5) * state.extent[i]));

// ── The wrist cards and the toast (issue #256) ─────────────────────────────
// The cube card on the secondary wrist, the button legend on the primary
// wrist, each shown while its wrist is turned towards the eyes (wrist.js, the
// watch gesture); a short message as a toast low in the view. Each card's
// text is drawn on its own canvas when it changes; the canvas uploaded is that
// text at one of a few opacities while it fades (2-D textures only, issue #229).
const makeCanvas = (wh) => Object.assign(document.createElement('canvas'), { width: wh[0], height: wh[1] });
const wristCards = Object.fromEntries([['secondary', WR.WRIST_PX], ['primary', WR.LEGEND_PX]].map(([role, px]) => [role, {
  role, px, size: [px[0] / WR.WRIST_PPM, px[1] / WR.WRIST_PPM], gesture: new WR.WatchGesture(), alpha: 0,
  text: makeCanvas(px), shown: makeCanvas(px), tex: null, key: '', level: -1, model: null,
}]));
const toasts = new WR.Toasts(() => performance.now());
const toastCard = { text: makeCanvas(WR.TOAST_PX), shown: makeCanvas(WR.TOAST_PX), tex: null, id: -1, level: -1 };
let toastMode = '';     // the mode line last told by a toast (a change of mode is told)
let wristSession = null;  // the session the gestures' state belongs to

/** A new session: no card, gesture or waiting message carried over from the
 *  last one, the mode not told again. */
function syncWristSession() {
  if (wristSession === xr) return;
  wristSession = xr;
  for (const card of Object.values(wristCards)) { card.gesture = new WR.WatchGesture(); card.alpha = 0; }
  toasts.clear();
  toastMode = xr ? modeText() : '';
}

/** A short message in the headset (issue #256); one with the `key` of one
 *  waiting or on show replaces it. */
function toast(text, key = '') {
  if (!xr || xr.bench) return;
  syncWristSession();
  toasts.push(text, key);
}

/** The cube on show, for the secondary wrist: {name, shape} or null. */
function wristCube() {
  const c = state.cube;
  if (!c) return null;
  const name = state.openPath ? state.openPath.split('/').pop() : c.mode === 'synthetic' ? 'synthetic cube' : `dataset ${state.open ? state.open.dataset_id : ''}`;
  const full = (state.info && state.info.full_shape) || c.fullShape || c.dims;
  const bin = c.factors && c.factors.some((f) => f > 1) ? `, bin ${c.factors.join('×')}` : '';
  return { name, shape: `${full.join('×')}${bin}` };
}

/** The selection's numbers (wrist.js selectionLines) for the cube's unit. */
function selectionSummary() {
  return WR.selectionLines(selection.sel, { bunit: (state.info && state.info.bunit) || (state.cube && state.cube.bunit) || '' });
}

/** The text of a wrist card: [title, rows]. */
function wristContent(role) {
  if (role === 'secondary') {
    const lines = WR.infoLines({ cube: wristCube(), modeLine: modeText(), selection: selectionSummary() });
    return [lines[0], lines.slice(1)];
  }
  return [modeText(), WR.legendRows(C.controlMap(mapOpts()))];
}

/** The card's texture at its opacity level now: the text redrawn when it
 *  changed, the canvas uploaded when the text or the level changed. */
function wristTexture(card, level) {
  if (!card.tex) card.tex = R.createPanel();
  const [title, rows] = wristContent(card.role);
  const key = JSON.stringify([title, rows]);
  if (key !== card.key) { WR.drawWristCard(card.text.getContext('2d'), title, rows, card.px); card.key = key; card.level = -1; }
  if (level !== card.level) {
    WR.drawFaded(card.shown, card.text, level);
    R.updatePanel(card.tex, card.shown);
    card.level = level;
  }
  return card.tex;
}

/** The wrist cards and the toast this frame: [{id, model, tex, size}]. */
function wristOverlays(now, dtS) {
  const out = [];
  if (!xr || xr.bench || !xr.pose) return out;
  const head = xr.pose.transform.matrix;
  const r = C.roles(settings.leftHanded);
  syncWristSession();
  const ml = modeText();
  // A change of mode or tool, told (the label above the controller is gone).
  if (ml !== toastMode) { toast(ml, 'mode'); toastMode = ml; }
  for (const card of Object.values(wristCards)) {
    const handedness = r[card.role];
    const h = xr.hands[handedness];
    const enabled = card.role === 'secondary' ? state.hud : settings.labels;
    const wf = enabled ? WR.wristFrame(h, handedness) : null;
    // The palm up is the menu's gesture; the secondary card makes way for the menu.
    const blocked = !!(h && h.palmUp) || (card.role === 'secondary' && ui.menu.open);
    const on = card.gesture.update(wf ? WR.watchMeasure(wf, head) : null, now, blocked);
    card.alpha = WR.fadeStep(card.alpha, on ? 1 : 0, dtS);
    const level = WR.alphaLevel(card.alpha);
    if (!wf || level <= 0) { card.model = null; continue; }
    card.model = WR.wristCardModel(wf, xr.headPos, xr.headRight || [1, 0, 0]);
    out.push({ id: `wrist-${card.role}`, model: card.model, tex: wristTexture(card, level), size: card.size });
  }
  const t = toasts.current();
  if (t) {
    if (!toastCard.tex) toastCard.tex = R.createPanel();
    if (toastCard.id !== t.id) { WR.drawToast(toastCard.text.getContext('2d'), t.text); toastCard.id = t.id; toastCard.level = -1; }
    const level = WR.alphaLevel(t.alpha);
    if (level !== toastCard.level) { WR.drawFaded(toastCard.shown, toastCard.text, level); R.updatePanel(toastCard.tex, toastCard.shown); toastCard.level = level; }
    out.push({ id: 'toast', model: WR.toastModel(head), tex: toastCard.tex, size: WR.TOAST_SIZE });
  }
  return out;
}

/** What to draw by the hands this frame: [{model, tex, size}]. The cursor
 *  readout where the primary ray meets the emission, the pinned one, and
 *  the wrist cards and the toast (issue #256: no card stands above the
 *  controllers any more). */
function handCards(now, dtS = 0) {
  const out = [];
  if (!xr || xr.bench) return out;
  const eye = xr.headPos;
  const hv = selection.sel.hover;
  if (!CUT.noreadout && state.cube && hv && !ui.hit) {
    cutCounts.readoutCards++;
    out.push({ id: 'readout-live', model: RO.readoutModel(boxToWorld(hv.p), eye), tex: readoutTexture(readout.live, readoutCached(readout.live, hv.p), false, now),
      size: RO.READOUT_SIZE });
  }
  if (!CUT.noreadout && state.cube && ui.pin) {
    cutCounts.readoutCards++;
    out.push({ id: 'readout-pin', model: RO.readoutModel(boxToWorld(ui.pin.p), eye), tex: readoutTexture(readout.pin, readoutCached(readout.pin, ui.pin.p), true, now),
      size: RO.READOUT_SIZE });
  }
  out.push(...wristOverlays(now, dtS));
  return out;
}

/** The primary trigger (or a click on the page) on the cube, by the mode. */
function primaryAct(action, boxRay) {
  if (!state.cube || !boxRay) return;
  if (action === 'pin-readout') {
    const r = selection.pickBox(boxRay, 0.6);
    ui.pin = r ? { p: r.hit.p } : null;
    buzz('primary', r ? 'press' : 'error');
  } else if (action === 'select' || action === 'pv-point') {
    buzz('primary', 'press');
    // The controller's tip, for a box corner placed inside the cube; a PV path being drawn takes it.
    if (!analysis.act(boxRay)) selection.act(boxRay, xr ? boxRay.o : null);
  } else if (action === 'slab-at') {
    // Along every channel (the slab hides the others): the emission the ray
    // meets first, its brightest channel becomes the slab's centre.
    const all = { min: [state.clip.min[0], state.clip.min[1], 0], max: [state.clip.max[0], state.clip.max[1], 1] };
    const r = selection.pickBox(boxRay, 0.25, all);
    if (!r) { buzz('primary', 'error'); return; }
    pushUndo();
    setSlab({ c: r.peak.p[2], w: slabOf().w });
    buzz('primary', 'press');
  } else if (action === 'clip-drag') {
    const tip = boxRay.o;
    const inside = tip.every((v, i) => v >= state.clip.min[i] && v <= state.clip.max[i]);
    let point = inside ? tip : null;
    if (!point) { const r = selection.pickBox(boxRay, 0.6); if (r) point = r.hit.p; }
    if (!point) { buzz('primary', 'error'); return; }
    const face = C.nearestFace(point, state.clip);
    pushUndo();
    ui.clipDrag = { face, start: tip[face.axis], v0: state.clip[face.end][face.axis] };
    buzz('primary', 'press');
  }
}

/** A stick's continuous action, `v` (-1..1, dead zone applied) over dtS seconds. */
function stickAction(action, v, dtS) {
  if (!v || !action) return false;
  if (action === 'threshold') {
    state.thrNorm = Math.max(0, Math.min(state.cmaxNorm - 1e-3, state.thrNorm - v * dtS * 0.25 * Math.max(0.02, state.thrNorm)));
  } else if (action === 'opacity') {
    state.opacity = Math.max(0.01, Math.min(3, state.opacity * Math.exp(v * dtS)));
  } else if (action === 'cmax') {
    state.cmaxNorm = Math.max(state.thrNorm + 1e-3, Math.min(1, state.cmaxNorm * Math.exp(-v * dtS * 0.5)));
  } else if (action === 'slab-move') {
    const s = slabOf();
    setSlab({ c: s.c - v * dtS * 0.3, w: s.w });
  } else if (action === 'slab-width') {
    const s = slabOf();
    setSlab({ c: s.c, w: s.w * Math.exp(v * dtS) });
  } else if (action === 'clip-size') {
    state.clip = C.scaleClip(state.clip, Math.exp(-v * dtS * 0.5));
    decorationsChanged();
  } else {
    return false;
  }
  labelsChanged();
  return true;
}

/** How long a sideways push must be held to change the tool (ms), and how
 *  much more sideways than up/down it must be (any step). */
const FLICK_TOOL_HOLD_MS = 180;
const FLICK_SIDEWAYS_RATIO = 2;

/** A stick pushed sideways as a step: the colour map, or the mode's next tool. */
function stickStep(action, step) {
  if (!step || !action) return;
  if (action === 'colormap') {
    pushUndo();
    const n = COLORMAP_NAMES.length, i = COLORMAP_NAMES.indexOf(state.colormap);
    setColormap(COLORMAP_NAMES[(i + step + n) % n]);
  } else if (action === 'next-tool') {
    const next = C.nextSub(ui.mode, currentSub(), step);
    if (ui.mode === 'select') selection.setTool(next); else if (ui.mode === 'clip') setClipTool(next);
  } else {
    return;
  }
  buzz('secondary', 'press');
}

/** The tools' work in progress and errors, for a pulse when it ends or fails. */
function toolsBusy() {
  const s = selection.sel, a = analysis;
  return {
    busy: s.busy || a.lasso.state.busy || a.moment.state.busy || a.pv.state.busy || '',
    selBusy: !!s.busy,
    error: s.error || a.lasso.state.error || a.moment.state.error || a.pv.state.error || '',
  };
}

// ── Desktop mode ────────────────────────────────────────────────────────────
const cam = { yaw: -0.5, pitch: 0.35, dist: 2.0, turn: 0 };   // turn: a head turn about the eye (debug)
const DESKTOP_BG = [0.043, 0.051, 0.07];
let dragging = null;
let pressAt = null;
let mouseOnPanel = false;      // a press on the menu or a window (?panel3d)
let mouseOnCard = false;
/** The message card in the desktop view (?panel3d) takes the mouse first. */
function cardMouse(e, pressed) {
  return !xr && ui.desktop && shell.cardPointer(mouseWorldRay(e), pressed);
}
canvas.addEventListener('pointerdown', (e) => {
  canvas.setPointerCapture(e.pointerId);
  if (cardMouse(e, true)) { mouseOnCard = true; return; }
  if (!xr && uiPointer(mouseWorldRay(e), true, camEye())) {
    // On the menu or a window (?panel3d): its widget, not the camera or a selection.
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
    const onPanel = uiPointer(mouseWorldRay(e), mouseOnPanel, camEye());
    if (mouseOnPanel) return;
    if (state.cube) selSync.hover(selection, onPanel ? null : mouseRay(e));   // where a click would land
    showReadout();
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
    if (!xr) { uiPointer(mouseWorldRay(e), false, camEye()); uiPointer(mouseWorldRay(e), false, camEye()); }
    return;
  }
  // The press began off the menu, but its pointer saw it down: let it go, or
  // the next click on the menu without moving the mouse is lost.
  if (!xr) uiPointer(mouseWorldRay(e), false, camEye());
  dragging = null;
  state.moving = false;
  // A click (no drag) is the desktop's trigger: the mode's action at the
  // pointer (issue #201), through the same ray pick as the controllers. A face
  // of the clipping box is not dragged with the mouse (a drag turns the view).
  const p = pressAt;
  pressAt = null;
  if (p && !xr && state.cube && Math.hypot(e.clientX - p.x, e.clientY - p.y) < 5
      && performance.now() - p.t < 600) {
    const r = mouseRay(e);
    const action = C.actionFor(C.controlMap(mapOpts()), 'primary', 'trigger');
    if (action !== 'clip-drag') primaryAct(action, r);
    showReadout();
  }
});

/** The cursor readout on the page (desktop mode): under the mouse, and the pinned one. */
function showReadout() {
  const hv = selection.sel.hover;
  const live = readoutAt(hv ? hv.p : null);
  const pin = ui.pin ? readoutAt(ui.pin.p).slice(1) : null;
  $('cursor-readout').textContent = `${live.join('   ')}${pin ? `\npinned: ${pin.join('   ')}` : ''}`;
}

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
  if (xr || !single.active) { desktopRunning = false; return; }
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
  // The measurement run's desktop motion (?debug, or its auto option): the camera turns.
  if (measure.running) cam.yaw += measure.autoYaw(lastDesktop ? time - lastDesktop : 0);
  syncSelection();
  R.beginFrame();
  lastDesktopView = desktopView();
  refiner.setSuspended('');
  refineFrame(time, desktopEyes(lastDesktopView, w, h), lastDesktopView.view, DESKTOP_BG);
  if (state.cube) drawScene(lastDesktopView, undefined, 0);
  else if (layers.hasScene()) layers.drawScene(lastDesktopView, 0, h);
  if (ui.desktop) {
    // The headset's menu and result windows (issue #201), used with the mouse.
    menuLayout();
    R.drawPanel(lastDesktopView, menuWorld(), menuTexture(time), menuSize());
    drawWindows(lastDesktopView, shownWindows());
  }
  // The headset's message card, in front of the desktop camera (?panel3d).
  const card = ui.desktop ? shell.cardForFrame(M.invert(lastDesktopView.view)) : null;
  if (card) R.drawPanel(lastDesktopView, card.model, card.tex, card.size);
  R.endFrame();
  const interval = lastDesktop ? time - lastDesktop : 0;
  lastDesktop = time;
  const cpuMs = performance.now() - t0;
  stats.push(interval, cpuMs, R.gpuMs);
  measure.frame(interval, cpuMs, R.gpuMs);
  if (state.governorOn) governor.update(interval, 1000 / 60);
  if (time - hudLast > 250) { hudLast = time; $('hud-text').textContent = hudLines(0).join('\n'); }
}
function startDesktop() {
  if (!desktopRunning) { desktopRunning = true; lastDesktop = 0; requestAnimationFrame(desktopFrame); }
}

// ── WebXR ───────────────────────────────────────────────────────────────────
let xr = null;

// ── Measurement records and the guided run (issue #224) ─────────────────────
const measure = createMeasure({
  backend: () => { try { return tokenField.value.trim() ? ensureBackend() : null; } catch { return null; } },
  page: PAGE,
  backendInfo: () => shell.life.info,
  gpu: () => gpuInfo(),
  xr: () => xr,
  canvas,
  wrist: (hz) => stats.summary(hz ? 1000 / hz : 0),
  refiner,
  governorState: () => state.governorOn,
  setGovernor: (on) => { state.governorOn = on; $('governor').checked = on; governor.value = state.spv; },
  effectiveSpv: () => effectiveSpv(),
  state,
  cubeInfo: () => state.info,
  lookKey: () => currentLook().key,
  mode: () => ({ mode: ui.mode, tool: ui.mode === 'select' ? selection.sel.tool : ui.mode === 'clip' ? ui.clipTool : null }),
  uiFlags: () => ({ menu: !!ui.menu.open, card: !!shell.cardOnShow, hud: !!state.hud }),
  notices: { show: (n) => { shell.notices.show(n, { force: true }); shell.render(); }, clear: (id) => shell.clearNotice(id) },
  buzz: (kind) => { buzz('primary', kind); buzz('secondary', kind); },
  log: (level, event, message) => shell.log(level, event, message),
  paired: () => isPaired(),
  hostname: window.location.hostname,
  headset: isHeadsetBrowser(navigator.userAgent),
  onChange: () => updateMeasureDom(),
});
// What the record keeps besides the samples: the refinement's stop→refined
// times and the page's own log events (scrubbed: no token, no path).
refiner.observers.push((name, d) => measure.event(name, d));
shell.diag.onAdd((e) => { if (e.level !== 'debug' && !/^measure-/.test(e.event)) measure.event('log', { level: e.level, event: e.event, message: recordMessage(e.message) }); });

function updateMeasureDom() {
  $('record').checked = measure.recording;
  $('measure-run').textContent = measure.running ? 'Cancel the measurement run' : 'Run measurement protocol';
  $('measure-status').textContent = measure.statusText();
}
$('record').addEventListener('change', () => {
  if ($('record').checked !== measure.recording) measure.toggleRecord();
  updateMeasureDom();
});
$('measure-run').addEventListener('click', () => startOrCancelRun());
function startOrCancelRun(opts = {}) {
  if (measure.running) { measure.cancel('cancelled by the viewer'); return; }
  const r = measure.runProtocol({ second: $('measure-second').checked || query.has('measure_second'),
    auto: query.has('measure_auto'), ...opts });
  if (!r.ok) shell.notices.show({ id: 'measure-msg', level: 'info', autoHideMs: 5000, title: 'Measurement run',
    lines: [`Not started: ${r.why}.`], actions: [{ id: 'dismiss', label: 'OK' }] }, { force: true });
  shell.render();
  updateMeasureDom();
}
setInterval(updateMeasureDom, 1000);

async function checkXR() {
  if (!navigator.xr) {
    $('xr-status').textContent = window.isSecureContext ? 'no WebXR in this browser' : 'not a secure context: WebXR needs https or localhost';
    return;
  }
  const vr = await navigator.xr.isSessionSupported('immersive-vr').catch(() => false);
  const ar = await navigator.xr.isSessionSupported('immersive-ar').catch(() => false);
  xrSupport = { vr, ar };
  $('xr-status').textContent = vr ? `WebXR ready${ar ? ' (VR and passthrough)' : ''}` : 'WebXR present, no headset';
  updateXRButtons();
}
let xrSupport = { vr: false, ar: false };
/** Enter VR and the others: on with a headset, off on a page that stood down (issue #226). */
function updateXRButtons() {
  const on = single.active;
  $('enter-vr').disabled = !xrSupport.vr || !on;
  $('bench-xr').disabled = !xrSupport.vr || !on;
  $('enter-ar').disabled = !xrSupport.ar || !on;
}

// ── One active page (issue #226) ────────────────────────────────────────────
// A newer page of this origin (another Open on Headset, another adb open, a
// second loader window) makes this one stand down: VR off, nothing drawn,
// refinement closed, a notice "Open in another window" with "Use this window".
const single = new SinglePage({
  channel: typeof BroadcastChannel === 'function' ? new BroadcastChannel(CHANNEL_NAME) : null,
  onChange: (active, info) => onSingleChange(active, info),
});
/** Bumped when the page stands down: a session being made then is ended (enterXR). */
let xrStartGen = 0;
function onSingleChange(active, info = {}) {
  if (!active) xrStartGen++;
  updateXRButtons();
  $('stood-down').hidden = active;
  if (!active) {
    // Issue #237: a page with nothing on show stands down for the one with a
    // cube, and says which; the hub forgets it, so the desktop's commands go
    // to the page in use.
    const text = stoodDownText(info);
    shell.log('info', 'page-stood-down', info.reason === 'content'
      ? `another VR page of this origin shows ${info.cube || 'a cube'} and this one nothing` : 'a newer VR page of this origin was opened');
    if (xr) endXR('a newer page');
    refiner.setSuspended('in another window');
    hub.stop();
    $('stood-down-text').textContent = `${text.title}: ${text.lines[0]}`;
    shell.notices.show({ id: 'stood-down', level: 'warning', sticky: true, title: text.title,
      lines: text.lines, actions: [{ id: 'take-over', label: 'Use this window' }] }, { force: true });
  } else {
    shell.log('info', 'page-took-over', 'this window is the VR page in use again');
    shell.clearNotice('stood-down');
    refiner.setSuspended('');
    hub.start();
    startDesktop();
  }
  shell.render();
}
function takeOver() { single.claim({ force: true }); }   // the user's choice: whatever the others show
$('take-over').addEventListener('click', () => takeOver());
window.addEventListener('pageshow', (e) => { if (e.persisted) { single.claim(); if (single.active) hub.start(); } });
// The hub forgets a page that closes (best effort, not a long poll).
window.addEventListener('pagehide', () => hub.stop({ unloading: true }));

// ── The headset hub: one page that the desktop drives (issue #237) ─────────
// The page announces itself to the backend session's hub, tells it what it
// shows, takes the desktop's `show` commands (the cube loads here, in VR when
// in VR) and reports back what is made and chosen here. All of it from
// timers (hubTick, the hub's own), never inside a frame.
const hubStore = (() => { try { return window.sessionStorage; } catch { return null; } })();
function hubAuth() {
  const token = tokenField.value.trim();
  const session = (backend && backend.session) || pairing.session || '';
  return token && session ? { token, session } : null;
}
const hub = HUB.createHub({
  client: HUB.hubClientId(hubStore),
  transport: HUB.transportOf(window.location, isPaired()),
  label: HUB.headsetLabel(navigator.userAgent),
  auth: hubAuth,
  onCommand: (cmd) => showFromDesktop(cmd),
  log: (level, event, message) => shell.log(level, event, message),
  onClient: (id) => { try { if (hubStore) hubStore.setItem(HUB.CLIENT_KEY, id); } catch { /* this load only */ } },
});

/** The dataset on show (a cube, a point table or a velocity field), '' none. */
function shownDatasetId() {
  if (state.open) return state.open.dataset_id;
  const L = layers.state;
  return (L.points && L.points.datasetId) || (L.flow && L.flow.datasetId) || '';
}
/** What it is, under the desktop's kinds. */
function shownKind() {
  if (state.cube) return 'cube';
  return layers.mode === 'points' ? 'points' : layers.mode === 'flow' ? 'velocity' : 'cube';
}
/** Does the page show something, or is it about to (a load under way)? The one-page claim's content. */
function pageHasContent() {
  return !!(state.cube || state.loading > 0 || layers.hasScene() || stagedCube);
}
function shownName() {
  if (state.openPath) return HUB.fileName(state.openPath);
  if (state.cube) return state.cube.mode === 'synthetic' ? 'the synthetic cube' : 'a cube';
  return '';
}
/** The look as the desktop reads it (data values of the cube), or null without a cube from the backend. */
function hubLook(id) {
  const c = state.cube;
  if (!c || !state.open) return null;
  return HUB.lookPayload({ colormap: state.colormap, threshold: dataValue(state.thrNorm), colourMax: dataValue(state.cmaxNorm),
    opacity: state.opacity, gamma: state.gamma, mode: state.mode, zStretch: state.zStretch, datasetId: id });
}
function hubClip(id) {
  if (!state.cube || !state.open || !state.info || !state.info.full_shape) return null;
  return HUB.clipPayload(HUB.voxelBox(state.clip, state.info.full_shape), id);
}

let hubShownKey = '';
function hubTick() {
  single.setContent(pageHasContent(), shownName());
  if (!single.active) return;
  const id = shownDatasetId();
  const path = state.openPath || '';
  const staged = stagedCube && stagedCube.open ? HUB.cubeRecord(stagedCube.open.dataset_id, stagedCube.path) : null;
  hub.noteState({ in_vr: !!xr, cube: HUB.cubeRecord(id, path, shownKind()), staged_cube: staged });
  // The page moved to another session (a load made one): hello there now.
  const a = hubAuth();
  if (a && hub.st.running && hub.st.helloSession && hub.st.helloSession !== a.session) hub.resync();
  maybeOffer();
  maybeAutoEnter();
  if (!id || !path) { hubShownKey = ''; return; }
  const look = hubLook(id), clip = hubClip(id);
  const key = JSON.stringify([id, path, shownKind(), state.cube ? state.cube.factors : null]);
  if (key !== hubShownKey) {
    // Switched: the desktop is told, and the new cube's look and clip as the
    // page starts them are its baseline (only later changes are sent).
    hubShownKey = key;
    hub.cube(path, { path, dataset_id: id, kind: shownKind(), binning: state.cube ? state.cube.factors.slice() : null },
      `Switched to ${HUB.fileName(path)}`);
    if (look) hub.rebase('look', path, look);
    if (clip) hub.rebase('clip', path, clip);
    return;
  }
  if (look) hub.look(path, look, `Look: ${state.colormap}, threshold ${fmt(look.threshold)}${state.cube.bunit ? ` ${state.cube.bunit}` : ''}`);
  if (clip) hub.clip(path, clip);
}
setInterval(hubTick, 250);

/** A product made here (or a catalogue over the cube): saved for the desktop and reported. */
function reportProduct(recipe, detail) {
  if (!detail || !single.active) return;
  const path = state.openPath || '';
  const key = recipe ? `recipe:${recipe.product.id}:${path}` : `${detail.product}:${detail.path || detail.file || ''}:${performance.now()}`;
  hub.product(detail, recipe, { cubePath: path, cubeName: HUB.fileName(path), stem: HUB.stemOf(path) }, key);
}

/** The desktop's `show`: that cube in this page (in VR when in VR, #235). */
function showFromDesktop(cmd) {
  const name = HUB.fileName(cmd.path) || cmd.dataset_id;
  const kind = cmd.kind || 'cube';
  const session = (backend && backend.session) || pairing.session || '';
  shell.log('info', 'hub-show', `${name} (${kind}), command ${cmd.seq}${xr ? ', in VR' : ''}`);
  measure.event('hub-show', { kind, in_vr: !!xr });
  // A reload of this tab opens it, as one after the desktop's link does.
  keepPairing({ session, dataset: cmd.dataset_id, path: cmd.path, kind, box: '' });
  pendingBox = null;
  if (shownDatasetId() === cmd.dataset_id && state.openPath === cmd.path) {
    // The cube on show: this command is the newest, so a load under way and a
    // cube waiting for the end of the session (older commands) are dropped.
    loads.begin(cmd.path);
    if (stagedCube) {
      stagedCube = null;
      shell.clearNotice('cube-waiting');
      shell.log('info', 'cube-waits-dropped', 'the desktop sent the cube on show again');
    }
    applyCommandBox(cmd);
    shell.notify('cube-from-desktop', { detail: name, done: true });
    return;
  }
  shell.notify('cube-from-desktop', { detail: name });
  $('path').value = cmd.path;
  withLoading(() => loadLinked({ session, dataset: cmd.dataset_id, path: cmd.path, kind, box: '' }))
    .then(() => {
      // Waiting for the end of the VR session (larger than the room kept for
      // cubes in VR): its box is applied when it is shown (installStaged).
      if (stagedCube && stagedCube.path === cmd.path) { pendingBox = cmd.box ? { path: cmd.path, cmd } : null; shell.clearNotice('cube-from-desktop'); return; }
      if (state.openPath !== cmd.path) { shell.clearNotice('cube-from-desktop'); return; }
      applyCommandBox(cmd);
      shell.notify('cube-from-desktop', { detail: name, done: true });
    })
    .catch((err) => {
      shell.clearNotice('cube-from-desktop');
      if (err.superseded) return;
      setStatus(`The cube sent from the desktop could not be opened: ${err.message || err}`);
      shell.log('warning', 'hub-show-failed', `${name}: ${err.message || err}`);
    });
}

/** A commanded box for a cube that waits for the end of the VR session: {path, cmd} or null. */
let pendingBox = null;

/** A `show` with a voxel box: the cube's clipping box (not reported back). */
function applyCommandBox(cmd) {
  if (!cmd.box || !state.cube || !state.info || !state.open) return;
  const clip = HUB.clipOfBox(cmd.box, state.info.full_shape);
  if (!clip) { shell.log('warning', 'hub-box-ignored', `not a box of ${state.info.full_shape.join('×')}`); return; }
  pushUndo();
  state.clip = clip;
  decorationsChanged();
  labelsChanged();
  hub.rebase('clip', state.openPath, hubClip(state.open.dataset_id));
}

// ── Fewer Enter VR presses (issue #237) ─────────────────────────────────────
// Launched as an installed app, the page enters VR by itself once its first
// cube is on show (the browser may refuse without a press: the button stays);
// where the browser offers sessions, its own Enter VR is always at hand.
const entry = {
  launched: XE.launchedAsApp(window.location.search, typeof window.matchMedia === 'function' ? (q) => window.matchMedia(q) : null),
  tried: false, outcome: '', offered: false, offerStopped: false, offerOutcome: '', offers: 0,
};
function entryLine() {
  const parts = [entry.launched ? `launched as an app: ${entry.tried ? `tried by itself, ${entry.outcome || 'waiting for the browser'}` : 'tries by itself once a cube is on show'}`
    : 'opened as a page: Enter VR is pressed once (the desktop then sends cubes into the session)'];
  parts.push(XE.canOffer(navigator.xr) ? `offerSession: ${entry.offerStopped ? `stopped (${entry.offerOutcome})` : entry.offered ? 'offered (the browser shows its Enter VR)' : entry.offerOutcome || 'not offered yet'}`
    : 'offerSession: not in this browser');
  return parts.join('; ');
}
function maybeAutoEnter() {
  if (!XE.shouldAutoEnter({ launched: entry.launched, tried: entry.tried, vrSupported: xrSupport.vr, active: single.active,
    inXR: !!xr || R.xrActive, cubeReady: !!state.cube || layers.hasScene(), busy: entering || !!(benchPrep && benchPrep.busy) })) return;
  entry.tried = true;
  shell.log('info', 'auto-enter', 'launched as an app: entering VR without a press');
  measure.event('auto-enter', { try: true });
  enterXR('immersive-vr', false).then(() => { entry.outcome = 'entered'; }).catch((err) => {
    const refused = XE.isActivationRefusal(err);
    entry.outcome = refused ? 'refused without a press: Enter VR is the way' : `failed: ${String(err.message || err).slice(0, 80)}`;
    shell.log('info', 'auto-enter-refused', `${err.name || 'Error'}: ${String(err.message || err).slice(0, 120)}`);
    $('xr-status').textContent = `${$('xr-status').textContent.replace(/ · press Enter VR$/, '')} · press Enter VR`;
  });
}
function maybeOffer() {
  if (entry.offerStopped || !XE.shouldOffer({ offerable: XE.canOffer(navigator.xr), vrSupported: xrSupport.vr, active: single.active,
    inXR: !!xr || R.xrActive || entering, offered: entry.offered })) return;
  // The capacity the cubes are written into in VR, now, on the 2-D page
  // (issue #229/#235): the offered session may start at any time.
  ensureCapacity(state.cube, { page: true });
  let offer;
  try { offer = navigator.xr.offerSession('immersive-vr', xrSessionInit(!query.has('nolayers'))); } catch (err) {
    entry.offerStopped = true;
    entry.offerOutcome = `${err.name || 'Error'}: ${String(err.message || err).slice(0, 60)}`;
    shell.log('info', 'xr-offer-refused', entry.offerOutcome);
    return;
  }
  entry.offered = true;
  entry.offers++;
  entry.offerOutcome = 'offered';
  shell.log('info', 'xr-offer', 'the browser\'s own Enter VR is offered');
  Promise.resolve(offer).then((session) => {
    entry.offered = false;
    entry.offerOutcome = 'accepted';
    shell.log('info', 'xr-offer-accepted', 'entered with the browser\'s Enter VR');
    return enterXR('immersive-vr', false, session).catch((e) => setStatus(`XR: ${e.message}`));
  }, (err) => {
    entry.offered = false;
    entry.offerOutcome = `${err && err.name ? err.name : 'ended'}`;
    // Withdrawn because the page entered by itself: offered again after that
    // session; any other refusal stops the offers (no loop of offers).
    if (!(xr || R.xrActive || entering)) entry.offerStopped = true;
    shell.log('info', 'xr-offer-ended', `${entry.offerOutcome}: ${String(err && err.message ? err.message : err).slice(0, 100)}`);
  });
}

$('enter-vr').addEventListener('click', () => enterXR('immersive-vr', false).catch((e) => setStatus(`XR: ${e.message}`)));
$('enter-ar').addEventListener('click', () => enterXR('immersive-ar', false).catch((e) => setStatus(`XR: ${e.message}`)));
// The headset benchmark: its cubes are prepared on the 2-D page first (one
// press), then the second press enters VR (issue #229: no 3-D texture can be
// allocated during the session).
$('bench-xr').addEventListener('click', () => {
  if (!benchPrep || (!benchPrep.busy && !benchReady())) { releaseBench(true); prepareBench().catch((e) => setStatus(`Benchmark: ${e.message || e}`)); return; }
  if (benchPrep.busy) return;
  enterXR('immersive-vr', true).catch((e) => setStatus(`XR: ${e.message}`));
});

/** The session's WebGL layer. Always with an alpha channel: Quest Browser 154
 *  gives an opaque layer (alpha: false) an incomplete framebuffer, so every
 *  draw fails and the headset shows black. In VR the page clears to opaque
 *  black, so the alpha channel changes nothing on screen; in passthrough it
 *  is what lets the room show through. */
function makeXRLayer(session, scale) {
  return new XRWebGLLayer(session, gl, { framebufferScaleFactor: scale, antialias: false, alpha: true });
}

/** The session's features: the floor, hands, composition layers (#226). */
function xrSessionInit(wantLayers) {
  return { optionalFeatures: ['local-floor', 'hand-tracking', ...(wantLayers ? ['layers'] : [])] };
}

/** A session being requested (requestSession not answered yet). */
let entering = false;

/** `given`: a session the browser already made (offerSession, #237). */
async function enterXR(mode, bench, given = null) {
  const refuse = (msg) => {
    if (given) {
      // A session the browser made that this page cannot use: ended, and no
      // 3-D texture is allocated until it has (issue #229).
      if (!R.xrActive) {
        R.xrActive = true;
        given.addEventListener('end', () => { R.xrActive = false; setTimeout(() => afterXREnd(false), 0); });
      }
      given.end().catch(() => {});
    }
    throw new Error(msg);
  };
  if (!single.active) refuse('this page is paused: the page in use is in another window');
  const startGen = xrStartGen;
  // Composition layers for the text (issue #226), unless ?nolayers.
  const wantLayers = !query.has('nolayers');
  if (bench && !benchReady()) refuse('the benchmark\'s cubes are not prepared: press Prepare the headset benchmark first');
  if (R.xrActive || entering) refuse('a VR session is already running');
  // The capacity the cubes are written into in VR (issue #235): set now, on
  // the 2-D page, for the Max side and Texture chosen (no 3-D allocation in
  // VR). An offered session's was set when it was offered.
  if (!given) ensureCapacity(state.cube, { page: true });
  let session = given;
  if (!session) {
    entering = true;
    try { session = await navigator.xr.requestSession(mode, xrSessionInit(wantLayers)); } finally { entering = false; }
  }
  // From here until the session ends no 3-D texture is allocated (issue
  // #229): loads wait for the end (installCube), the benchmark's cubes were
  // prepared before. Registered first, so it is cleared before any other
  // 'end' listener runs; what waited is done after them.
  R.xrActive = true;
  session.addEventListener('end', () => { R.xrActive = false; setTimeout(() => afterXREnd(!!bench), 0); });
  try {
    await startSession(session, mode, bench, startGen, wantLayers);
  } catch (err) {
    // A session the page could not set up is ended, not left running unowned
    // (it would keep R.xrActive set, and every load waiting, for good).
    // The flag stays set until the session's end event clears it (a load landing meanwhile still waits).
    if (!xr) session.end().catch(() => {});
    throw err;
  }
}

/** The rest of enterXR, once the session exists. */
async function startSession(session, mode, bench, startGen, wantLayers) {
  await gl.makeXRCompatible();
  const fbScale = readNumber('fbscale');
  const layer = makeXRLayer(session, fbScale);
  if ('fixedFoveation' in layer) layer.fixedFoveation = readNumber('foveation');
  session.updateRenderState({ depthNear: 0.05, depthFar: 50 });
  let ref;
  try { ref = await session.requestReferenceSpace('local-floor'); } catch { ref = await session.requestReferenceSpace('local'); }
  // With layers, the projection layer is the first of the session's `layers`
  // (a session with layers has no baseLayer); without, the base layer as before.
  let quads = null, layersNote = 'in the scene (?nolayers)';
  if (wantLayers && layersAvailable(session)) {
    try {
      session.updateRenderState({ layers: [layer] });
      quads = new QuadLayers({
        binding: new XRWebGLBinding(session, gl), session, gl, space: ref,
        makeTransform: (p, q) => new XRRigidTransform({ x: p[0], y: p[1], z: p[2] }, { x: q[0], y: q[1], z: q[2], w: q[3] }),
        flipY: query.get('layerflip') !== '0',
        onFail: (why) => { shell.log('warning', 'xr-layers-off', `drawn in the scene from now on: ${why}`); measure.event('xr-layers', { on: false, why: String(why).slice(0, 160) }); },
      });
      quads.setBase(layer);
      layersNote = 'quad layers';
    } catch (err) {
      quads = null;
      layersNote = `in the scene (layers refused: ${err.message || err})`;
      session.updateRenderState({ baseLayer: layer });
    }
  } else {
    if (wantLayers) layersNote = 'in the scene (no composition layers in this browser)';
    session.updateRenderState({ baseLayer: layer });
  }
  let targetHz = 0;
  if (session.supportedFrameRates && session.updateTargetFrameRate) {
    const want = readNumber('rate');
    const rates = Array.from(session.supportedFrameRates);
    const pick = rates.reduce((a, b) => (Math.abs(b - want) < Math.abs(a - want) ? b : a), rates[0]);
    // Bounded: a browser that never settles the request must not keep the
    // session from drawing its first frame (the headset would stay black).
    try {
      await Promise.race([session.updateTargetFrameRate(pick),
        new Promise((resolve) => setTimeout(resolve, 1500))]);
    } catch { /* keep the default */ }
  }
  targetHz = session.frameRate || readNumber('rate');
  // A newer page took over while the session was being made: it is not used.
  if (!single.active || startGen !== xrStartGen) {
    session.end().catch(() => {});
    throw new Error('a newer VR page was opened meanwhile: this one is paused');
  }
  xr = { session, ref, layer, mode, fbScale, fbChosen: fbScale, targetHz, placed: false, last: 0, hands: {}, grab: null,
    buttons: {}, bench: bench ? makeBench() : null, place: null, endedBy: '', quads, layersNote };
  Object.assign(ui.menu, { open: false, pinned: false, armed: false, wasHeld: false });
  ui.menuModel = null;
  ui.menuDrag = null;
  stats.buf.length = stats.cpu.length = stats.gpu.length = 0;
  governor.max = state.spv; governor.value = state.spv;
  eyeGovernor.reset(fbScale);
  shell.xrStarted();
  shell.clearNotice('xr-ended');
  const policy = qualityPolicy({ targetHz, spvMin: governor.min, spvChosen: state.spv, eyeChosen: fbScale });
  shell.log('info', 'xr-start', `${mode}${bench ? ' (benchmark)' : ''}, ${targetHz} Hz, eye buffer ${layer.framebufferWidth}×${layer.framebufferHeight}; text ${layersNote}`);
  shell.log('info', 'quality-policy', `${policy}; governor ${state.governorOn ? 'on' : 'off'}`);
  measure.event('xr-start', { mode: mode === 'immersive-ar' ? 'ar' : 'vr', bench: !!bench, display_hz: targetHz,
    eye: [layer.framebufferWidth, layer.framebufferHeight], scale: fbScale,
    foveation: 'fixedFoveation' in layer ? layer.fixedFoveation : null, rates: session.supportedFrameRates ? Array.from(session.supportedFrameRates) : null,
    policy, layers: layersNote });
  measure.xrStarted();
  // The controls guide is shown on the 2-D page before entering VR (issue
  // #226); in VR only when asked for (More → Guide), beside the cube.
  session.addEventListener('end', () => {
    if (xr && xr.quads) xr.quads.destroy();
    const b = xr && xr.bench;
    // A benchmark left before its end: its settings and the cube shown before it come back.
    if (b && !b.finished) finishBench(b);
    const endedBy = xr ? xr.endedBy : '';
    const summary = `${fpsLine()}; ${currentQualityNote() || 'quality as chosen'}`;
    const dragged = Object.values(wins).filter((w) => w.drag);
    if (dragged.length) {
      settings.windows = { ...settings.windows, ...Object.fromEntries(dragged.map((w) => [w.id, { ...w.offset }])) };
      persist();
    }
    xr = null;
    measure.event('xr-end', { by: endedBy || 'the system' });
    measure.xrEnded();
    if (endedBy) {
      shell.log('info', 'xr-end', `by ${endedBy}; ${summary}`);
    } else {
      // Not asked for by the page: the system menu, the headset taken off, the
      // browser. The 2-D page (which the headset shows next) says so.
      shell.log('warning', 'xr-ended', `unexpectedly; ${summary}`);
      if (!gl.isContextLost()) shell.notify('xr-ended');
    }
    // A press held on the menu or a window when the session ended is released.
    menu.pointer.update(null, false); menu.pointer.update(null, false);
    for (const w of Object.values(wins)) { w.pointer.update({ bar: null, content: null }, false); w.drag = null; }
    ui.hit = null; ui.clipDrag = null; ui.sliding = null;
    Object.assign(ui.menu, { open: false, pinned: false, armed: false, wasHeld: false });
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
      // button 0 the pinch, button 1 the closed hand, button 4 the palm up (hands.js).
      handKeys.add(src.handedness);
      const h = handTracker.update(frame, src, xr.ref, performance.now(), xr.headPos, xr.headRight);
      if (h) hands[src.handedness] = h;
      continue;
    }
    if (!src.gripSpace || !src.gamepad) continue;
    const gp = frame.getPose(src.gripSpace, xr.ref);
    if (!gp) continue;
    const m = gp.transform.matrix;
    const hand = { m: new Float32Array(m), pos: [m[12], m[13], m[14]], g: src.gamepad, ray: null, src };
    const rp = src.targetRaySpace && frame.getPose(src.targetRaySpace, xr.ref);
    if (rp) {
      const r = rp.transform.matrix;
      hand.ray = { o: [r[12], r[13], r[14]], d: [-r[8], -r[9], -r[10]] };
    }
    hands[src.handedness] = hand;
  }
  xr.hands = hands;
  handTracker.keep(handKeys);
  // Fixed roles (issue #201): the primary hand points and acts, the secondary
  // holds the menu; both grab. A change of hands forgets the buttons held.
  const roles = C.roles(settings.leftHanded);
  const Ph = hands[roles.primary], Sh = hands[roles.secondary];
  const btn = (h, k) => { const b = h && h.g.buttons[C.BUTTON_INDEX[k]]; return !!(b && b.pressed); };
  if (!xr.roles || xr.roles.primary !== roles.primary) {
    // What the hands hold at the swap is not a new press in their new roles.
    xr.buttons = { pTrig: btn(Ph, 'trigger'), pLower: btn(Ph, 'lower'), pUpper: btn(Ph, 'upper'), sUpper: btn(Sh, 'upper') };
    xr.roles = roles;
    ui.clipDrag = null;
  }
  const L = btn(hands.left, 'grip'), Rr = btn(hands.right, 'grip');
  if (xr.bench) { xr.grab = null; state.moving = false; ui.hit = null; return; }

  // One meaning per control in this mode (controls.js): the same map draws the labels.
  const map = C.controlMap(mapOpts());
  const act = (role, control) => C.actionFor(map, role, control);
  // The menu: held open by the secondary hand's lower button (or its palm up).
  const was = ui.menu.open;
  const { opened, ...open } = HM.menuOpenState(ui.menu, act('secondary', 'lower') === 'menu' && btn(Sh, 'lower'));
  Object.assign(ui.menu, open);
  if (ui.menu.open && !was) buzz('secondary', 'press');
  updateMenuPose(opened);

  // The message card first (epic #166), then the menu and the windows: a ray
  // on them, or a press that began on them, is theirs, not the cube's.
  const pTrig = btn(Ph, 'trigger');
  const pRay = Ph ? Ph.ray : null;
  const onCard = shell.cardPointer(pRay, pTrig);
  const onUi = onCard ? (uiPointer(null, pTrig, xr.headPos), true) : uiPointer(pRay, pTrig, xr.headPos);
  const boxRay = pRay && state.cube && !onUi ? selection.worldRayToBox(state.model, pRay.o, pRay.d) : null;
  if (CUT.nohover) {
    // Debug (issue #229): no hover picks; one left over is cleared once. A
    // click still picks, snaps and selects below.
    if (selection.sel.hover) selSync.hover(selection, null);
  } else {
    if (onUi && selection.sel.hover) selSync.hover(selection, null);
    if (boxRay && (xr.hoverFrame = (xr.hoverFrame || 0) + 1) % 2 === 0) { selSync.hover(selection, boxRay); cutCounts.hoverPicks++; }
  }
  // The trigger on the cube, by the mode. The pick runs on the reduced cube in
  // this frame; what it asks of the backend never holds a frame back.
  if (pressedEdge('pTrig', pTrig) && boxRay) primaryAct(act('primary', 'trigger'), boxRay);
  if (ui.clipDrag) {
    // A face of the clipping box follows the controller while the trigger is held.
    if (pTrig && pRay && state.cube && act('primary', 'trigger') === 'clip-drag') {
      const tip = selection.worldRayToBox(state.model, pRay.o, pRay.d).o;
      const d = ui.clipDrag;
      state.clip = C.moveFace(state.clip, d.face, d.v0 + tip[d.face.axis] - d.start);
      decorationsChanged();
    } else {
      ui.clipDrag = null;
    }
  }

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
  const [px, py] = Ph ? stick(Ph.g).map(C.dead) : [0, 0];
  const [sx, sy] = Sh ? stick(Sh.g).map(C.dead) : [0, 0];
  // One undo step per push of a stick (from rest to rest).
  const continuous = [[act('primary', 'stickY'), py], [act('primary', 'stickX'), px], [act('secondary', 'stickY'), sy]]
    .filter(([a, v]) => a && v);
  if (continuous.length && !xr.stickMoving) pushUndo();
  xr.stickMoving = continuous.length > 0;
  for (const [a, v] of continuous) stickAction(a, v, dtS);
  if (!xr.flick) xr.flick = new C.Flick();
  // A tool step wants a deliberate sideways push, held briefly, never while
  // that hand holds the grip (moving the cube) or a selection is busy.
  const stepAction = act('secondary', 'stickX');
  const toolStep = stepAction === 'next-tool';
  const [rsx, rsy] = Sh ? stick(Sh.g) : [0, 0];   // raw: rest is judged without the dead zone
  stickStep(stepAction, xr.flick.update(rsx, {
    cross: rsy, dtMs: dtS * 1000,
    holdMs: toolStep ? FLICK_TOOL_HOLD_MS : 0, ratio: FLICK_SIDEWAYS_RATIO,
    blocked: btn(Sh, 'grip') || (toolStep && !!toolsBusy().busy),
  }));

  if (pressedEdge('pLower', btn(Ph, 'lower')) && act('primary', 'lower') === 'next-mode') {
    setMode(C.nextMode(ui.mode));
    buzz('primary', 'press');
  }
  if (pressedEdge('pUpper', btn(Ph, 'upper')) && act('primary', 'upper') === 'undo') {
    if (undo()) buzz('primary', 'press');
  }
  if (pressedEdge('sUpper', btn(Sh, 'upper')) && act('secondary', 'upper') === 'reset-view') {
    resetView();
    buzz('secondary', 'press');
  }
  // A pulse when the tools' work ends, two when it fails.
  const now = toolsBusy();
  const kind = C.selectionPulse(ui.busy, now);
  if (kind) buzz('primary', kind);
  // And a toast (issue #256): the selection's numbers, or what ended.
  if (kind) toast(WR.resultToast(kind, ui.busy.selBusy ? selectionSummary() : null, now.error, ui.busy.busy), 'result');
  ui.busy = now;
  if (xr.slabDirty && (performance.now() - (xr.slabBuilt || 0) > 100)) {
    rebuildDecorations(); xr.slabDirty = false; xr.slabBuilt = performance.now();
  }
}

function controllerSegments() {
  const segs = [];
  const primary = C.roles(settings.leftHanded).primary;
  for (const [hand, h] of Object.entries(xr.hands)) {
    const m = h.m;
    const p = (x, y, z) => M.transformPoint(m, [x, y, z]);
    const col = hand === primary ? [1, 0.75, 0.4, 1] : [0.5, 0.8, 1, 1];
    // The controllers and hands themselves are solid models (inputMesh, issue
    // #226); a tracked hand keeps its pinch line, the thumb tip to the index tip.
    if (h.isHand) {
      const j = h.joints;
      if (j['thumb-tip'] && j['index-finger-tip'] && h.gesture.pinchDist < 0.05) {
        segs.push({ a: j['thumb-tip'].p, b: j['index-finger-tip'].p, color: h.gesture.pinch ? [1, 0.85, 0.3, 1] : col });
      }
    } else {
      segs.push({ a: p(0, 0, -0.06), b: p(0, 0, -0.1), color: col });     // the barrel's direction
    }
    if (hand !== primary || !h.ray) continue;
    if (shell.cardHit) {
      segs.push({ a: h.ray.o, b: shell.cardHit.point, color: [1, 1, 1, 0.7] });
    } else if (ui.hit) {
      // On the menu or a window: the ray to the point it points at, and a small cross there.
      const q = ui.hit.point, pm = ui.hit.model;
      segs.push({ a: h.ray.o, b: q, color: [1, 1, 1, 0.6] });
      const ax = M.normalize(M.sub(M.transformPoint(pm, [1, 0, 0]), M.position(pm)));
      const ay = M.normalize(M.sub(M.transformPoint(pm, [0, 1, 0]), M.position(pm)));
      const az = M.normalize(M.sub(M.transformPoint(pm, [0, 0, 1]), M.position(pm)));
      const lift = M.add(q, M.scale3(az, 0.002));   // just in front of it
      segs.push({ a: M.add(lift, M.scale3(ax, -0.008)), b: M.add(lift, M.scale3(ax, 0.008)), color: [1, 0.85, 0.3, 1] });
      segs.push({ a: M.add(lift, M.scale3(ay, -0.008)), b: M.add(lift, M.scale3(ay, 0.008)), color: [1, 0.85, 0.3, 1] });
    } else if (state.cube) {
      // The ray, to the point the trigger would pick (or 1.5 m).
      const hv = selection.sel.hover;
      let end = M.add(h.ray.o, M.scale3(M.normalize(h.ray.d), 1.5));
      if (hv) end = boxToWorld(hv.p);
      segs.push({ a: h.ray.o, b: end, color: [1, 1, 1, 0.35] });
    }
  }
  return segs;
}

/** The controllers and tracked hands as solid models (issue #226): one
 *  world-space triangle list for this frame. Primary orange, secondary blue.
 *  A controller whose real model is ready (controller-models.js) is left to
 *  that model, posed here into modelDraws for R.drawModels. */
const meshScratch = [];
const modelDraws = [];
let modelDrawCount = 0;
function inputMesh() {
  meshScratch.length = 0;
  modelDrawCount = 0;
  const primary = C.roles(settings.leftHanded).primary;
  for (const [hand, h] of Object.entries(xr.hands)) {
    const col = hand === primary ? [1, 0.62, 0.25, 1] : [0.35, 0.68, 1, 1];
    if (h.isHand) handMesh(meshScratch, h.joints, col, { pinch: !!h.gesture.pinch, grip: !!h.gesture.grip });
    else if (h.src && (h.model = controllerModels.pose(h.src, h.m, h.g))) {
      const e = h.model;
      if (modelDraws.length <= modelDrawCount) modelDraws.push({ handle: null, rig: null });
      const d = modelDraws[modelDrawCount++];
      d.handle = e.handle; d.rig = e.rig;
    } else {
      const b = (k) => { const x = h.g.buttons[C.BUTTON_INDEX[k]]; return !!(x && x.pressed); };
      controllerMesh(meshScratch, h.m, col, { pressed: { trigger: b('trigger'), grip: b('grip') }, handedness: hand });
    }
  }
  if (meshArray.length < meshScratch.length) meshArray = new Float32Array(Math.max(meshScratch.length, 2 * meshArray.length));
  meshArray.set(meshScratch);
  return meshArray.subarray(0, meshScratch.length);
}
let meshArray = new Float32Array(65536);

/** The numbers (frame rate, latency; with ?debug or in the benchmark, issue
 *  #256) above the secondary hand; next to the cube without a hand. */
function hudModel() {
  const r = C.roles(settings.leftHanded);
  const h = xr.hands[r.secondary];
  if (h) {
    let m = M.translation(0, 0.085, 0.01);
    m = M.multiply(m, M.rotation([1, 0, 0], -0.6));
    return M.multiply(h.m, m);
  }
  const pl = xr.place;
  const right = [Math.cos(pl.yaw), 0, -Math.sin(pl.yaw)];
  const p = M.add(pl.p, M.add(M.scale3(right, -0.5), [0, 0.0, 0]));
  return M.multiply(M.translation(...p), M.rotation([0, 1, 0], pl.yaw + 0.5));
}

/** The flat surfaces of this frame, back to front: [{id, model, handle, size}]. */
function overlayList(card, winList, hm, cards, menuM, menuTex) {
  const out = [];
  const guide = card && card.level === 'guide';
  if (guide) out.push({ id: 'card', model: card.model, handle: card.tex, size: card.size });
  for (const w of winList) {
    out.push({ id: 'win-' + w.id, model: w.model, handle: w.tex, size: w.size });
    out.push({ id: 'bar-' + w.id, model: W.barModel(w.model, w.size), handle: barTexture(w), size: W.barSize(w.size[0]) });
  }
  if (hm) out.push({ id: 'hud', model: hm, handle: hudTex, size: [0.16, 0.15] });
  for (const k of cards) out.push({ id: k.id, model: k.model, handle: k.tex, size: k.size });
  if (menuM) out.push({ id: 'menu', model: menuM, handle: menuTex, size: HM.MENU_SIZE });
  if (card && !guide) out.push({ id: 'card', model: card.model, handle: card.tex, size: card.size });
  return out;
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
  xr.pose = pose;
  const hp = pose.transform.position;
  xr.headPos = [hp.x, hp.y, hp.z];
  { const hm = pose.transform.matrix; xr.headRight = M.normalize([hm[0], 0, hm[2]]); }
  handleInput(frame, dtS);
  if (xr.bench) {
    benchStep(xr.bench, time, interval);
    if (!xr) return;                  // the benchmark ended the session (its end may be synchronous)
  }

  const layer = xr.layer;
  gl.bindFramebuffer(gl.FRAMEBUFFER, layer.framebuffer);
  gl.clearColor(0, 0, 0, xr.mode === 'immersive-ar' ? 0 : 1);
  gl.depthMask(true);         // a draw may have left depth writes off: the clear needs them
  gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
  R.beginFrame();
  R.setLines(controllerSegments(), R.tmpLines);
  R.setMesh(inputMesh());
  // The numbers four times a second: on the 2-D page always, in the headset
  // only with ?debug or in the benchmark (issue #256): no canvas drawn nor
  // uploaded otherwise.
  const hudOn = (DEBUG_HUD || !!xr.bench) && !CUT.nohud;
  if (!CUT.nohud && time - hudLast > 250) {
    hudLast = time;
    const lines = hudLines(xr.targetHz);
    if (hudOn) {
      drawHudCanvas(lines);
      R.updatePanel(hudTex, hudCanvas);
      cutCounts.hudUploads++;
    }
    $('hud-text').textContent = lines.join('\n');
  }
  // The page's labels (DOM) four times a second at most, HUD shown or not.
  if (xr.labelsDirty && time - (xr.labelsAt || 0) > 250) { xr.labelsAt = time; updateLabels(); xr.labelsDirty = false; }
  // The numbers sit where the menu opens: hidden while it is open.
  const hm = hudOn && !ui.menu.open ? hudModel() : null;
  if (hm) cutCounts.hudQuads++;
  syncSelection();
  const winList = xr.bench ? [] : shownWindows();
  const menuTex = menuShown() ? menuTexture(time) : null;
  const menuM = menuTex ? menuWorld() : null;
  const card = xr.bench ? null : shell.cardForFrame(pose.transform.matrix, { onToast: toast });
  const cards = handCards(time, dtS);
  // Every flat surface with text, back to front (issue #226): the guide
  // lowest (never in front of the menu), the windows, the numbers, the
  // readout, the labels, the menu, a message card on top. As composition
  // layers where the browser has them (sharp at any eye-buffer scale), else
  // drawn in the scene below in the same order.
  const overlays = overlayList(card, winList, hm, cards, menuM, menuTex);
  const asLayers = !!(xr.quads && xr.quads.frame(frame, overlays));
  xr.overlaysAsLayers = asLayers;
  // No refinement during the benchmark, nor in passthrough: the backend's frame
  // has no alpha (it is composited over black), so over the room it would
  // either darken it where the headset draws emission the full cube has not,
  // or glow where only the full cube has emission. The headset's own
  // rendering is shown there (docs/user-guide/vr/reference.md).
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
    if (!asLayers) for (const o of overlays) R.drawPanel(v, o.model, o.handle, o.size);
    R.drawModels(v, modelDraws, modelDrawCount);   // the real controller models
    R.drawMesh(v);            // the page's own controllers and the hands (models.js)
    R.drawWorldLines(v);      // and their rays, over the panels drawn in the scene
  }
  R.endFrame();
  const cpu = performance.now() - t0;
  stats.push(interval, cpu, R.gpuMs);
  if (xr.bench) xr.bench.record(interval, cpu, R.gpuMs);
  else measure.frame(interval, cpu, R.gpuMs);
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
    const layer = makeXRLayer(xr.session, scale);
    if ('fixedFoveation' in layer) layer.fixedFoveation = 'fixedFoveation' in old ? old.fixedFoveation : readNumber('foveation');
    // With layers the projection layer is replaced in the `layers` array (the
    // quads kept), else as the base layer.
    if (xr.quads) xr.quads.setBase(layer);
    else xr.session.updateRenderState({ baseLayer: layer });
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
// leaving VR, on the page. The cubes are made and put on the GPU on the 2-D
// page before the session (prepareBench), never in VR (issue #229); in VR the
// benchmark only switches between them (R.useVolume).
const BENCH_SIZES = [128, 256, 384, 512];
// ?debug&benchms=N: N ms measured a configuration (the checks' short run).
const BENCH_MEASURE_MS = query.has('debug') && Number(query.get('benchms')) > 0 ? Number(query.get('benchms')) : 3000;
const BENCH_SETTLE_MS = Math.min(750, BENCH_MEASURE_MS / 4);

/** The benchmark's cubes on the GPU: {busy, cubes: [{n, slot, cube, info}], bytes, skipped}, or null. */
let benchPrep = null;

function benchReady() {
  return !!(benchPrep && !benchPrep.busy && benchPrep.cubes.length && benchPrep.cubes.every((e) => !e.slot.lost && !e.slot.released));
}

function setBenchButton() {
  const b = $('bench-xr');
  b.textContent = benchReady() ? 'Start the headset benchmark' : benchPrep && benchPrep.busy ? 'Preparing the benchmark…' : 'Prepare the headset benchmark';
}

/**
 * Makes the benchmark's synthetic cubes and puts each on the GPU now, on the
 * 2-D page: as many of BENCH_SIZES as fit this device's texture share
 * together (quality.textureLimitBytes; 210 MB for all four, which an 8 GB
 * headset's 256 MiB holds). Their voxels are not kept on the page, only the
 * threshold and colour range each starts with.
 */
async function prepareBench() {
  if (benchPrep) return;
  if (R.xrActive) throw new Error('leave VR first');
  // The device's share, less what the cube on show keeps on the GPU beside them.
  // (The capacity of issue #235 and any volume of its own: g.cubeBytes.)
  const g = R.gpuStats();
  const limit = cubeLimitBytes() - g.cubeBytes;
  const sizes = [];
  let bytes = 0;
  for (const n of BENCH_SIZES) {
    const need = n ** 3 + Math.ceil(n / 8) ** 3 * 4;
    if (n > R.maxTex3D || bytes + need > limit) continue;
    sizes.push(n);
    bytes += need;
  }
  const skipped = BENCH_SIZES.filter((n) => !sizes.includes(n));
  benchPrep = { busy: true, cubes: [], bytes, skipped };
  setBenchButton();
  const t0 = performance.now();
  try {
    for (const n of sizes) {
      setStatus(`Preparing the benchmark's cubes: ${n}³…`);
      await new Promise((r) => setTimeout(r, 0));          // the status is drawn
      if (R.xrActive || benchPrep.cancelled) throw new Error(R.xrActive ? 'a VR session started while the benchmark was being prepared' : 'another cube was loaded while the benchmark was being prepared: prepare it again');
      const g0 = performance.now();
      const data = V.syntheticCube(n);
      const info = V.syntheticInfo(n);
      const cube = { data, dims: [n, n, n], encoding: 'uint8', lo: 0, hi: 1, factors: [1, 1, 1], mode: 'synthetic',
        reducedThreshold: 0.25, bunit: '', fullShape: [n, n, n] };
      const generateMs = performance.now() - g0;
      const src = thresholdSourceFor(settings, { synthetic: true, debug: DEBUG_HUD });
      cube.norms = thresholdNorms(cube, info, src);
      const slot = R.prepareVolume(cube, 8, { mask: false });
      cube.data = null;                                     // on the GPU now; not kept on the page
      benchPrep.cubes.push({ n, slot, cube, info, generateMs });
    }
    if (benchPrep.cancelled) throw new Error('another cube was loaded while the benchmark was being prepared: prepare it again');
  } catch (err) {
    benchPrep.busy = false;
    releaseBench(true);
    throw err;
  }
  benchPrep.busy = false;
  setBenchButton();
  setStatus(`Benchmark ready: ${sizes.map((n) => `${n}³`).join(', ')} on the GPU (${(bytes / 1e6).toFixed(0)} MB, `
    + `${((performance.now() - t0) / 1000).toFixed(1)} s)${skipped.length ? `; ${skipped.map((n) => `${n}³`).join(', ')} left out: over this device's texture share` : ''}.\n`
    + 'Press Start the headset benchmark to enter VR.');
}

/**
 * The benchmark's cubes given back (after its session, or a failed
 * preparation). When one of them is still on show (no cube was loaded
 * before the benchmark), an ordinary synthetic cube is made in its place,
 * with its voxels, so the page can pick and read it: the same size, or the
 * largest smaller one that fits this device with its selection mask (a 512³
 * does not on a headset). The others are given back first.
 */
function releaseBench(quiet = false) {
  if (!benchPrep || benchPrep.busy) return;
  const prep = benchPrep;
  benchPrep = null;                  // first: the replacement below installs a cube (installCube)
  const shown = prep.cubes.find((e) => e.slot === R.slot || e.cube === state.cube);
  for (const e of prep.cubes) if (e !== shown) R.releaseVolume(e.slot);
  if (shown && !R.xrActive) {
    // The replacement is allocated while the shown slot is still held (setVolume
    // releases it after): both must fit the device's share together.
    // A replacement that fits the capacity (issue #235) needs no new storage.
    const held = R.slot === shown.slot ? shown.slot.textureBytes + shown.slot.macroBytes : 0;
    const fits = (n) => {
      const b = cubeBudget({ dims: [n, n, n], encoding: 'uint8' }, { maxTex3D: R.maxTex3D,
        headset: isHeadsetBrowser(navigator.userAgent), deviceMemoryGB: navigator.deviceMemory, capacity: R.cap });
      return b.ok && (b.inCapacity || b.gpuBytes + held + b.capacityBytes <= b.limitBytes);
    };
    const n = BENCH_SIZES.filter((s) => s <= shown.n && fits(s)).pop() || BENCH_SIZES[0];
    const said = (err) => { if (!quiet) setStatus(`Failed: ${err.message || err}`); };
    try { loadSynthetic(n).catch(said); } catch (err) { said(err); }
  }
  for (const e of prep.cubes) R.releaseVolume(e.slot);
  setBenchButton();
}

function benchPlan(includeLoaded, sizes = BENCH_SIZES) {
  const plan = [];
  for (const n of sizes) for (const spv of [0.5, 1, 2]) plan.push({ n, spv });
  if (includeLoaded) for (const spv of [0.5, 1, 2]) plan.push({ n: 0, spv });
  return plan;
}

function makeBench() {
  const saved = state.cube && R.slot ? { info: state.info, cube: state.cube, open: state.open, openPath: state.openPath, slot: R.slot } : null;
  const b = {
    plan: benchPlan(!!saved, benchPrep.cubes.map((e) => e.n)), i: -1, t0: 0, saved, results: [], status: 'benchmark starting',
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

/** One of the prepared cubes on show, in VR: no allocation. */
function showBenchCube(n) {
  const e = benchPrep.cubes.find((x) => x.n === n);
  loads.begin('');              // a newer load than any backend load under way
  state.load = { generateMs: e.generateMs, mb: n ** 3 / 1e6 };
  installCube(e.info, e.cube, null, '', e.slot);
}

function benchStep(b, time) {
  const cfg = b.plan[b.i];
  // A new configuration starts on the frame AFTER its cube was put on show.
  if (b.t0 === null) b.t0 = time;
  const elapsed = time - b.t0;
  if (b.i < 0 || elapsed > BENCH_SETTLE_MS + BENCH_MEASURE_MS) {
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
    if (next.n && next.n !== b.currentN) { showBenchCube(next.n); b.currentN = next.n; }
    if (!next.n && b.saved) {
      R.useVolume(b.saved.slot);
      state.cube = b.saved.cube; state.info = b.saved.info; applyExtent(); resetThreshold(); b.currentN = 0;
    }
    state.spv = next.spv;
    b.t0 = null; b.intervals = []; b.gpu = []; b.cpu = []; b.measuring = false;
    b.status = `benchmark ${b.i + 1}/${b.plan.length}: preparing`;
    return;
  }
  b.measuring = (time - b.t0) > BENCH_SETTLE_MS;
  const c = b.plan[b.i];
  b.status = `benchmark ${b.i + 1}/${b.plan.length}: ${c.n ? `${c.n}^3` : 'loaded cube'} at ${c.spv} samples/voxel`;
  state.model = placedModel(xr.place, (time / 1000) * 0.35);
}

function finishBench(b) {
  b.finished = true;
  state.spv = b.savedParams.spv;
  state.governorOn = b.savedParams.governorOn;
  state.mode = b.savedParams.mode;
  state.skip = b.savedParams.skip;
  if (b.saved && !b.saved.slot.lost && !b.saved.slot.released) {
    R.useVolume(b.saved.slot);
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
    loadSynthetic(n, { sync: true });
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
  window.__vrMeasure = { measure, start: (opts) => startOrCancelRun(opts) };
  window.__vrLink = linkState;
  window.__vrState = state;
  window.__vrGL = gl;
  // Issue #226: the XR session's state (fake-XR checks), the renderer's GPU
  // textures, the one-page state.
  window.__vrXR = () => (xr ? { overlaysAsLayers: !!xr.overlaysAsLayers, layersNote: xr.layersNote, fbScale: xr.fbScale,
    targetHz: xr.targetHz, quads: xr.quads ? { active: xr.quads.active, shown: xr.quads.shown, pool: xr.quads.pool.size, copies: xr.quads.copies } : null,
    meshCount: R.meshCount, labelCount: R.labelCount, modelDraws: R.modelDraws,
    controllerModels: Object.fromEntries(Object.entries(controllerModels.status)
      .map(([k, e]) => [k, { status: e.status, profileId: e.profileId, error: e.error }])) } : null);
  window.__vrControllerModels = controllerModels;
  // Issue #256: the wrist cards' opacity and gesture, the toast on show.
  window.__vrWrist = () => ({ secondary: { alpha: wristCards.secondary.alpha, on: wristCards.secondary.gesture.on },
    primary: { alpha: wristCards.primary.alpha, on: wristCards.primary.gesture.on }, toast: toasts.current() });
  // A real controller model's node (its world position now), for the checks.
  window.__vrControllerNode = (hand, name) => {
    const e = controllerModels.status[hand];
    if (!e || !e.rig) return null;
    const i = e.rig.model.nodes.findIndex((n) => n.name === name);
    if (i < 0) return null;
    const w = e.rig.world[i];
    return { p: [w[12], w[13], w[14]], m: Array.from(w), drawn: !!e.rig.drawn[i] };
  };
  window.__vrGpu = () => R.gpuStats();
  // Issue #246: a cube put on show as a load puts it (the switch timing, validation/vr_switch_block.mjs).
  window.__vrInstall = (info, cube) => installCube(info, cube, null, '');
  // Issue #229: the 3-D allocation guard (calls, refusals), the session flag,
  // a cube waiting for the session's end, the benchmark's preparation.
  window.__vrTex3D = () => ({ calls: { ...R.tex3dGuard.calls }, refused: R.tex3dGuard.refused, xrActive: R.xrActive,
    staged: stagedCube ? { dims: stagedCube.cube.dims, factors: stagedCube.cube.factors } : null,
    capacity: R.cap ? { dims: R.cap.dims.slice(), encoding: R.cap.encoding, bytes: R.cap.totalBytes, onShow: !!(R.slot && R.slot.cap === R.cap), bank: R.slot ? R.slot.bank ?? null : null } : null,
    bench: benchPrep ? { busy: benchPrep.busy, ready: benchReady(), sizes: benchPrep.cubes.map((e) => e.n), bytes: benchPrep.bytes } : null });
  window.__vrSingle = single;
  // Issue #237: the headset hub's controller and how Enter VR is reached.
  window.__vrHub = hub;
  window.__vrEntry = () => ({ ...entry, line: entryLine() });
  window.__vrCuts = () => ({ active: activeCuts.slice(), counts: { ...cutCounts } });
  window.__vrSetEyeScale = (s) => { if (xr) setEyeBufferScale(s); };
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
  /**
   * Issue #235: the cube on show drawn from a corner of the capacity, with a
   * brighter, larger cube's voxels left beyond it in the bank, against the
   * same cube in storage of its own size; composite and maximum intensity,
   * the whole box and a clipping box at one face. Outside VR. Returns the
   * largest channel difference, the pixels that differ by more than 2 and
   * the pixels drawn; the cube is put back into the capacity after.
   */
  window.__vrCapacityCompare = () => {
    const c = state.cube;
    if (!c || !c.data || R.xrActive || !R.fitsCapacity(c)) return { skipped: 'no cube with voxels in the capacity, or in VR' };
    const big = R.cap.dims.map((v, i) => Math.min(v, c.dims[i] + 24));
    const bright = c.encoding === 'uint8' ? new Uint8Array(big[0] * big[1] * big[2]).fill(255)
      : new Uint16Array(big[0] * big[1] * big[2]).fill(0x3c00);        // 1.0 as float16
    const w = canvas.width, h = canvas.height;
    const views = [
      { p: {} }, { p: { mode: 1 } },
      { p: { boxMin: [0, 0, 0], boxMax: [1, 1, 0.5] } }, { p: { boxMin: [0.5, 0, 0], boxMax: [1, 1, 1], mode: 1 } },
    ];
    const shoot = () => views.map(({ p }) => {
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.viewport(0, 0, w, h);
      gl.clearColor(0, 0, 0, 1);
      gl.clear(gl.COLOR_BUFFER_BIT);
      const model = M.multiply(M.translation(0, 0, -1.4), M.multiply(M.rotation([0.3, 1, 0.2], 0.7), M.scaling(1)));
      R.drawVolume({ proj: M.perspective(60 * Math.PI / 180, w / h, 0.05, 50), view: M.identity() }, model, volParams({ spv: 1, maxSteps: 4096, ...p }));
      const px = new Uint8Array(w * h * 4);
      gl.readPixels(0, 0, w, h, gl.RGBA, gl.UNSIGNED_BYTE, px);
      return px;
    });
    // Both banks hold the bright cube; the cube on show is then written over one of them.
    R.showVolume({ data: bright, dims: big, encoding: c.encoding });
    R.showVolume({ data: bright, dims: big, encoding: c.encoding });
    R.showVolume(c);
    const inCap = R.slot.cap === R.cap;
    const a = shoot();
    R.setVolume(c);                            // storage of its own size
    const own = R.slot.cap === null;
    const b = shoot();
    R.showVolume(c);                           // back into the capacity
    let maxDiff = 0, differ = 0, drawn = 0;
    for (let k = 0; k < a.length; k++) {
      for (let i = 0; i < a[k].length; i += 4) {
        const d = Math.max(Math.abs(a[k][i] - b[k][i]), Math.abs(a[k][i + 1] - b[k][i + 1]), Math.abs(a[k][i + 2] - b[k][i + 2]));
        if (d > maxDiff) maxDiff = d;
        if (d > 2) differ++;
        if (b[k][i] + b[k][i + 1] + b[k][i + 2] > 6) drawn++;
      }
    }
    return { inCap, own, maxDiff, differ, drawn, pixels: views.length * w * h, cube: c.dims, capacity: R.cap.dims, bright: big, lost: gl.isContextLost() };
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
  // The hand menu in the desktop view (?panel3d, issue #201): a widget's
  // centre as a page pixel (to drive it with real mouse events), and a press
  // through the same ray code the controllers use; its redraw cost.
  const quadPagePixel = (model, size, px, x, y) => {
    const v = lastDesktopView || desktopView();
    const local = [(x / px[0] - 0.5) * size[0], (0.5 - y / px[1]) * size[1], 0];
    const c = M.transformPoint(M.multiply(v.proj, M.multiply(v.view, model)), local);
    const rect = canvas.getBoundingClientRect();
    return [rect.left + (c[0] + 1) / 2 * rect.width, rect.top + (1 - c[1]) / 2 * rect.height];
  };
  const widgetCentre = (id) => {
    const w = menuLayout().find((x) => x.id === id);
    return w ? [w.rect[0] + w.rect[2] / 2, w.rect[1] + w.rect[3] / 2] : null;
  };
  window.__vrMenu = {
    ui, menu, wins, settings: () => JSON.parse(JSON.stringify(settings)), view: menuView,
    layout: () => menuLayout().map((w) => ({ id: w.id, kind: w.kind, rect: w.rect, disabled: w.disabled || '' })),
    /** A pointer action applied as the menu would ({type, id, value}): for checks. */
    act: (a) => { menuLayout(); applyMenu({ ...a, widget: HM.widgetById(menu.pointer.layout, a.id) }); ui.sliding = null; },
    controlMap: () => C.controlMap(mapOpts()),
    /** The page pixel of widget `id`'s centre (or of canvas pixel [x, y]) in the desktop view. */
    pagePixel(id, at = null) {
      const c = at || widgetCentre(id);
      return c ? quadPagePixel(menuWorld(), menuSize(), HM.MENU_PX, c[0], c[1]) : null;
    },
    /** The page pixel of a window's bar widget ('drag' or 'close'), or null when it is not shown. */
    windowPixel(id, widget = 'close') {
      const w = wins[id];
      if (!w.model) return null;
      const b = W.barLayout().find((x) => x.id === widget);
      return quadPagePixel(W.barModel(w.model, w.size), W.barSize(w.size[0]), W.BAR_PX, b.rect[0] + b.rect[2] / 2, b.rect[1] + b.rect[3] / 2);
    },
    /** Times n forced redraws of the menu's current page: drawMenu (CPU), the upload, both with a GPU sync. */
    bench(n = 60) {
      const ctx2 = menu.canvas.getContext('2d');
      const draw = [], upload = [], synced = [];
      const px = new Uint8Array(4);
      if (!menu.tex) menu.tex = R.createPanel();
      menuLayout();
      for (let i = 0; i < n; i++) {
        const view = menuView(menu.ctx);
        view.values = { ...view.values, thr: (i % 50) / 50 };      // a slider moving: a real change each time
        gl.readPixels(0, 0, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, px);
        const t0 = performance.now();
        HM.drawMenu(ctx2, ui.page, menu.spec, view, { hover: 'thr', active: 'thr' }, menu.ctx);
        const t1 = performance.now();
        R.updatePanel(menu.tex, menu.canvas);
        const t2 = performance.now();
        R.drawPanel(lastDesktopView || desktopView(), menuWorld(), menu.tex, menuSize());
        gl.readPixels(0, 0, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, px);
        const t3 = performance.now();
        draw.push(t1 - t0); upload.push(t2 - t1); synced.push(t3 - t0);
      }
      menu.key = '';
      const q = (a, f) => { const s = [...a].sort((x, y) => x - y); return s[Math.min(s.length - 1, Math.floor(f * s.length))]; };
      const sum = (a) => ({ p50: q(a, 0.5), p95: q(a, 0.95), max: Math.max(...a) });
      return { n, page: ui.page, draw: sum(draw), upload: sum(upload), drawUploadSynced: sum(synced), px: HM.MENU_PX };
    },
    /** The readout's text at box point p, and its cost (n computations). */
    readout(p, n = 1000) {
      const t0 = performance.now();
      let lines = null;
      for (let i = 0; i < n; i++) lines = readoutAt(p);
      return { lines, usPerCall: (1000 * (performance.now() - t0)) / n };
    },
  };
  // Kept for the lifecycle run: the settings and the Look page's values.
  window.__vrPanel = { settings: () => JSON.parse(JSON.stringify(settings)), view: panelView };
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
// Restored while the session that lost it has not ended yet: the rebuild
// (which allocates the cube's 3-D textures) waits for its end (issue #229).
let restoreAfterXR = false;
canvas.addEventListener('webglcontextrestored', () => {
  clearTimeout(contextTimer);
  if (R.xrActive) {
    restoreAfterXR = true;
    shell.log('info', 'webgl-context-restored', 'rebuilt when the VR session ends');
    return;
  }
  rebuildGraphics();
});

function rebuildGraphics() {
  const t0 = performance.now();
  try {
    R.restore();
    if (R.capacityError) shell.log('warning', 'capacity-failed', `after the context was restored: ${R.capacityError}`);
    releaseBench(true);              // the benchmark's cubes were on the lost context
    layers.restore();
    refiner.draw = new RefineDraw(gl);
    refiner.weight = 0;
    menu.key = '';
    for (const w of Object.values(wins)) w.barKey = '';
    for (const k of [readout.live, readout.pin]) k.key = '';
    for (const k of Object.values(wristCards)) { k.key = ''; k.level = -1; }
    toastCard.id = -1;
    hudLast = 0;
    selSync.mark();
    rebuildDecorations();
    shell.clearNotice('context-lost');
    shell.clearNotice('context-failed');
    shell.notify('context-restored', { wasInVR: contextLostInVR });
    shell.log('info', 'webgl-context-restored', `rebuilt in ${(performance.now() - t0).toFixed(0)} ms`);
    installStaged();                 // a cube that waited for a session ended by the loss
  } catch (err) {
    shell.clearNotice('context-lost');
    shell.notify('context-failed', { detail: String(err.message || err) });
    shell.log('error', 'webgl-context-restore-failed', String(err.message || err));
  }
  contextLostInVR = false;
  startDesktop();
}

// ── Guide and diagnostics on the page (epic #166) ───────────────────────────
$('show-guide').addEventListener('click', () => {
  const box = $('guide-text');
  box.textContent = shell.guideText();
  box.hidden = !box.hidden;
});
// The first-run guide is on this page, before entering VR (issue #226): in
// the headset it covered the menu and the labels. Got it: not shown again.
$('guide-ok').addEventListener('click', () => {
  $('guide-text').hidden = true;
  $('guide-ok').hidden = true;
  if (!settings.guideSeen) { settings.guideSeen = true; persist(); }
});
function showGuideOnPage() {
  if (!shouldShowGuide(settings, { bench: query.has('bench') })) return;
  $('guide-text').textContent = shell.guideText();
  $('guide-text').hidden = false;
  $('guide-ok').hidden = false;
}

/** Where the guide's card goes in the headset (notice.js guideCardModel):
 *  beside the cube, on the primary hand's side (the menu is in the other). */
function guidePlacement() {
  const side = C.roles(settings.leftHanded).primary === 'right' ? 1 : -1;
  if (!state.info) return { side };
  const half = state.extent.map((v) => v / 2);
  return { side, cube: { centre: M.transformPoint(state.model, [0, 0, 0]), radius: Math.hypot(...half) * M.uniformScale(state.model) } };
}
$('copy-diag').addEventListener('click', async () => {
  const text = shell.diagnostics();
  $('diag-out').hidden = false;
  $('diag-out').textContent = text;
  try { await navigator.clipboard.writeText(text); setStatus('Diagnostics copied: paste them into the support request.'); }
  catch { setStatus('Diagnostics shown below the buttons: select and copy them (the clipboard needs https or localhost).'); }
});

// ── Start ───────────────────────────────────────────────────────────────────
shell.start();
ensureCapacity(null);   // before any VR session (issue #235)
// What this page is about to show (issue #237): a page opened with a cube
// to load claims as one with content, so an older page with a cube gives
// way to it; an empty page (a tab restored, an empty loader) never makes it.
const again = query.has('synthetic') || query.has('bench') ? '' : takeReloadPath();
single.content = !!(query.has('synthetic') || (again && tokenField.value.trim()) || pairing.dataset || pairing.path);
single.cube = HUB.fileName(again || pairing.path || '');
single.claim();         // older VR pages of this origin stand down (issue #226)
if (single.active) hub.start();   // the headset hub: hello, state, the desktop's commands (#237)
checkXR();
updateLabels();
showGuideOnPage();
if (query.has('record')) measure.startRecord('address');
updateMeasureDom();
if (query.has('bench')) {
  desktopBench().catch((e) => { document.title = `BENCH FAILED ${e.message}`; console.error(e); });
} else {
  startDesktop();
  if (query.has('synthetic')) loadSyntheticSaying(Number(query.get('synthetic')) || 256);
  else if (again && tokenField.value.trim()) {
    // Reloaded after a lost graphics context: the cube that was on show.
    $('path').value = again;
    withLoading(loadFromBackend).catch((err) => { if (!err.superseded) setStatus(`Failed: ${err.message || err}`); });
  } else if (pairing.dataset || pairing.path) {
    withLoading(loadPaired).catch((err) => {
      if (!err.superseded) setStatus(`The desktop's cube could not be opened: ${err.message || err}`);
    });
  }
}

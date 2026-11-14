// Per-viewer conveniences of the VR page (issue #172), kept in this browser's
// localStorage: colour map, threshold source, refinement settings, the last
// selection tool, whether the controls guide was seen, and the controls'
// preferences of issue #201 (left-handed, the labels by the controllers,
// haptics) and where the result windows were moved to. Nothing here is a result or a
// secret (the token and the pairing are never stored here). Every access is
// guarded: storage may be missing, full or refused (private mode, a headset
// browser's settings), and the page then works with the defaults.
// Pure (the storage is passed in): unit-tested in backend/tests/vr/vr-panel.test.mjs.

export const SETTINGS_KEY = 'visivo-vr-settings';
export const SETTINGS_VERSION = 1;

export const DEFAULT_SETTINGS = {
  colormap: 'viridis',
  thrSource: 'cube',             // 'cube' (the full cube's, as on the desktop) | 'reduced' (for this binning)
  thrSourceChosen: false,        // true once chosen (page or ?debug menu); honoured with ?debug only (#257)
  refine: { enabled: true, scale: 1, quality: 95, depth: true },
  tool: 'lasso',                 // the Kinematic Lasso first (#257: lasso a galaxy, then Isolate, in two presses)
  guideSeen: false,              // the first-run controls guide was shown (epic #166)
  leftHanded: false,             // the left hand points and acts, the right holds the menu (#201)
  labels: true,                  // the button legend (#201), on the primary wrist since #256
  haptics: true,                 // pulses on a press, a result, an error (#201)
  windows: {},                   // {result|moment|pv: {right, up, fwd, yaw}}: offsets from the cube's first placement
};

export const WINDOW_IDS = ['result', 'moment', 'pv'];

const finite = (v, lo, hi) => typeof v === 'number' && Number.isFinite(v) && v >= lo && v <= hi;

/** How far a window may be kept from the cube's first placement (metres, radians). */
export const PANEL_LIMITS = { right: 3, up: 2, fwd: 3, yaw: 2 * Math.PI };

/** A window offset brought within PANEL_LIMITS (a drag may end farther away),
 *  so what is saved is always what sanitizeSettings keeps. */
export function clampPanelOffset(off) {
  const out = {};
  for (const k of ['right', 'up', 'fwd', 'yaw']) {
    const v = Number.isFinite(off && off[k]) ? off[k] : 0;
    out[k] = Math.max(-PANEL_LIMITS[k], Math.min(PANEL_LIMITS[k], v));
  }
  return out;
}

/**
 * The threshold source for a cube just loaded: what the desktop uses, the
 * full cube's threshold ('cube': reduced_info's default_threshold, the value
 * the desktop's cube window starts from, vtkWindowCube's initial threshold),
 * so the headset shows the emission the desktop shows, in the same BUNIT.
 * (With coarse binning the threshold for the binning also sits deep in the
 * full cube's noise, which the refined frames then show as fog: on an 11 GiB
 * cube binned 12x12x4, 405 KB frames and pose->frame 236 ms instead of 141.)
 * 'reduced' (for this binning) only for a synthetic cube, which has no full
 * cube. A stored choice counts only with ?debug (`debug`), where the menu
 * shows the setting (#257): a hidden setting never changes the view.
 */
export function thresholdSourceFor(settings, { synthetic = false, debug = false } = {}) {
  if (debug && settings && settings.thrSourceChosen && (settings.thrSource === 'reduced' || settings.thrSource === 'cube')) {
    return settings.thrSource;
  }
  return synthetic ? 'reduced' : 'cube';
}

/** A stored object made safe: unknown keys dropped, wrong values replaced by defaults. */
export function sanitizeSettings(raw, { colormaps = null, tools = null } = {}) {
  const d = DEFAULT_SETTINGS;
  const s = raw && typeof raw === 'object' ? raw : {};
  const out = { colormap: d.colormap, thrSource: d.thrSource, thrSourceChosen: false, refine: { ...d.refine }, tool: d.tool, guideSeen: d.guideSeen,
    leftHanded: d.leftHanded, labels: d.labels, haptics: d.haptics, windows: {} };
  if (typeof s.colormap === 'string' && (!colormaps || colormaps.includes(s.colormap))) out.colormap = s.colormap;
  if (s.thrSource === 'reduced' || s.thrSource === 'cube') out.thrSource = s.thrSource;
  out.thrSourceChosen = s.thrSourceChosen === true;
  const r = s.refine && typeof s.refine === 'object' ? s.refine : {};
  if (typeof r.enabled === 'boolean') out.refine.enabled = r.enabled;
  if ([0.4, 0.6, 0.8, 1].includes(r.scale)) out.refine.scale = r.scale;
  if ([70, 85, 95].includes(r.quality)) out.refine.quality = r.quality;
  if (typeof r.depth === 'boolean') out.refine.depth = r.depth;
  const lim = PANEL_LIMITS;
  const okOffset = (p) => p && typeof p === 'object' && finite(p.right, -lim.right, lim.right) && finite(p.up, -lim.up, lim.up)
      && finite(p.fwd, -lim.fwd, lim.fwd) && finite(p.yaw, -lim.yaw, lim.yaw);
  const w = s.windows && typeof s.windows === 'object' ? s.windows : {};
  for (const id of WINDOW_IDS) {
    const p = w[id];
    if (okOffset(p)) out.windows[id] = { right: p.right, up: p.up, fwd: p.fwd, yaw: p.yaw };
  }
  for (const k of ['leftHanded', 'labels', 'haptics']) if (typeof s[k] === 'boolean') out[k] = s[k];
  if (typeof s.tool === 'string' && (!tools || tools.includes(s.tool))) out.tool = s.tool;
  else if (tools && tools.length && !tools.includes(out.tool)) out.tool = tools[0];
  if (typeof s.guideSeen === 'boolean') out.guideSeen = s.guideSeen;
  return out;
}

/** The settings from `storage` (localStorage or a stand-in), or the defaults. */
export function loadSettings(storage, opts) {
  let raw = null;
  try {
    const text = storage && storage.getItem(SETTINGS_KEY);
    const obj = text ? JSON.parse(text) : null;
    raw = obj && obj.v === SETTINGS_VERSION ? obj.s : null;
  } catch { raw = null; }
  return sanitizeSettings(raw, opts);
}

/** Saves `settings`; false when the storage refused (the page carries on). */
export function saveSettings(storage, settings) {
  try {
    if (!storage) return false;
    storage.setItem(SETTINGS_KEY, JSON.stringify({ v: SETTINGS_VERSION, s: sanitizeSettings(settings) }));
    return true;
  } catch { return false; }
}

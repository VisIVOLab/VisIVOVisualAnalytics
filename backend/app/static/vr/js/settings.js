// Per-viewer conveniences of the VR page (issue #172), kept in this browser's
// localStorage: colour map, threshold source, refinement settings, the in-VR
// panel's position, the last selection tool and whether the controls guide
// was seen. Nothing here is a result or a
// secret (the token and the pairing are never stored here). Every access is
// guarded: storage may be missing, full or refused (private mode, a headset
// browser's settings), and the page then works with the defaults.
// Pure (the storage is passed in): unit-tested in backend/tests/vr/vr-panel.test.mjs.

export const SETTINGS_KEY = 'visivo-vr-settings';
export const SETTINGS_VERSION = 1;

export const DEFAULT_SETTINGS = {
  colormap: 'viridis',
  thrSource: 'reduced',          // 'reduced' (for this binning) | 'cube' (the full cube's, as on the desktop)
  refine: { enabled: true, scale: 1, quality: 95, depth: true },
  panel: null,                   // {right, up, fwd, yaw}: offset from the cube's first placement (metres, radians)
  tool: 'probe',
  guideSeen: false,              // the first-run controls guide was shown (epic #166)
};

const finite = (v, lo, hi) => typeof v === 'number' && Number.isFinite(v) && v >= lo && v <= hi;

/** How far the panel may be kept from the cube's first placement (metres, radians). */
export const PANEL_LIMITS = { right: 3, up: 2, fwd: 3, yaw: 2 * Math.PI };

/** A panel offset brought within PANEL_LIMITS (a drag may end farther away),
 *  so what is saved is always what sanitizeSettings keeps. */
export function clampPanelOffset(off) {
  const out = {};
  for (const k of ['right', 'up', 'fwd', 'yaw']) {
    const v = Number.isFinite(off && off[k]) ? off[k] : 0;
    out[k] = Math.max(-PANEL_LIMITS[k], Math.min(PANEL_LIMITS[k], v));
  }
  return out;
}

/** A stored object made safe: unknown keys dropped, wrong values replaced by defaults. */
export function sanitizeSettings(raw, { colormaps = null, tools = null } = {}) {
  const d = DEFAULT_SETTINGS;
  const s = raw && typeof raw === 'object' ? raw : {};
  const out = { colormap: d.colormap, thrSource: d.thrSource, refine: { ...d.refine }, panel: null, tool: d.tool, guideSeen: d.guideSeen };
  if (typeof s.colormap === 'string' && (!colormaps || colormaps.includes(s.colormap))) out.colormap = s.colormap;
  if (s.thrSource === 'reduced' || s.thrSource === 'cube') out.thrSource = s.thrSource;
  const r = s.refine && typeof s.refine === 'object' ? s.refine : {};
  if (typeof r.enabled === 'boolean') out.refine.enabled = r.enabled;
  if ([0.4, 0.6, 0.8, 1].includes(r.scale)) out.refine.scale = r.scale;
  if ([70, 85, 95].includes(r.quality)) out.refine.quality = r.quality;
  if (typeof r.depth === 'boolean') out.refine.depth = r.depth;
  const p = s.panel;
  const lim = PANEL_LIMITS;
  if (p && typeof p === 'object' && finite(p.right, -lim.right, lim.right) && finite(p.up, -lim.up, lim.up)
      && finite(p.fwd, -lim.fwd, lim.fwd) && finite(p.yaw, -lim.yaw, lim.yaw)) {
    out.panel = { right: p.right, up: p.up, fwd: p.fwd, yaw: p.yaw };
  }
  if (typeof s.tool === 'string' && (!tools || tools.includes(s.tool))) out.tool = s.tool;
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

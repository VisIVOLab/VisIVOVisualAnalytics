// The analysis panel in the headset (issue #191): lasso seeds, Isolate and
// Remove, the moment of the selection, the PV path. A second quad hung under
// the control panel (vr-panel.js), moved and hidden with it, used with the
// same ray, trigger or pinch through vr-panel's PanelPointer. Pure layout and
// placement, tested by node (backend/tests/vr/tools-pv.test.mjs).

import * as M from './mat4.js';
import { PANEL_SIZE } from './vr-panel.js';

export const TOOLS_PX = [1024, 480];
/** Metres, at the control panel's 2133 px/m. */
export const TOOLS_SIZE = [TOOLS_PX[0] / 2133, TOOLS_PX[1] / 2133];
const GAP = 0.012;

const WH = 56, X0 = 240, RIGHT = 1004;
const ROWS = [24, 96, 168, 240];
export const ROW_LABELS = ['Lasso seeds', 'Selection', 'Moment', 'PV path'];

export function toolsLayout() {
  const w = [];
  const span = (i, n, k, gap = 8) => {
    const cw = (RIGHT - X0 - gap * (n - 1)) / n;
    return [X0 + k * (cw + gap), ROWS[i], cw, WH];
  };
  [['new', 'new'], ['add', 'add'], ['carve', 'carve']].forEach(([v, label], k) => {
    w.push({ id: `seed:${v}`, kind: 'button', rect: span(0, 3, k), label, group: 'seed', value: v });
  });
  w.push({ id: 'isolate', kind: 'button', rect: span(1, 3, 0), label: 'Isolate' });
  w.push({ id: 'remove', kind: 'button', rect: span(1, 3, 1), label: 'Remove' });
  w.push({ id: 'show-applied', kind: 'button', rect: span(1, 3, 2), label: 'Show that cube' });
  [0, 1, 2].forEach((o) => w.push({ id: `moment:${o}`, kind: 'button', rect: span(2, 4, o), label: `M${o}`, group: 'moment', value: o }));
  w.push({ id: 'moment-hide', kind: 'button', rect: span(2, 4, 3), label: 'Hide' });
  w.push({ id: 'pv-draw', kind: 'button', rect: span(3, 6, 0), label: 'Draw' });
  w.push({ id: 'pv-undo', kind: 'button', rect: span(3, 6, 1), label: 'Undo' });
  w.push({ id: 'pv-new', kind: 'button', rect: span(3, 6, 2), label: 'New' });
  w.push({ id: 'pv-narrow', kind: 'button', rect: span(3, 6, 3), label: 'w −' });
  w.push({ id: 'pv-wide', kind: 'button', rect: span(3, 6, 4), label: 'w +' });
  w.push({ id: 'pv-hide', kind: 'button', rect: span(3, 6, 5), label: 'Hide' });
  return w;
}

/** The panel hangs under the control panel whose pose is `controlModel`. */
export function toolsModel(controlModel) {
  return M.multiply(controlModel, M.translation(0, -(PANEL_SIZE[1] / 2 + GAP + TOOLS_SIZE[1] / 2), 0));
}

/**
 * Where a product plane hangs. In the room (`desktop` false), from `anchor`
 * (where the cube was first placed, 0.75 m in front of the viewer): above the
 * cube's sides, the moment on the right over the results panel, the PV
 * diagram on the left over the control panel, turned toward where the viewer
 * stood, their lower edges clear of both panels. In the desktop view, left of
 * the cube (the control panel is on the right there).
 */
export function productModel(anchor, which, desktop = false) {
  const ay = anchor.yaw;
  const right = [Math.cos(ay), 0, -Math.sin(ay)];
  const fwd = [-Math.sin(ay), 0, -Math.cos(ay)];
  const s = which === 'moment' ? 1 : -1;
  const at = desktop
    ? { right: -1.12, up: which === 'moment' ? 0.22 : -0.22, fwd: 0.1, yaw: 0.55 }
    : { right: 0.42 * s, up: 0.48, fwd: -0.05, yaw: -0.35 * s };
  const p = M.add(anchor.p, M.add(M.scale3(right, at.right), M.add([0, at.up, 0], M.scale3(fwd, at.fwd))));
  return M.multiply(M.translation(...p), M.rotation([0, 1, 0], ay + at.yaw));
}

const C = {
  bg: 'rgba(12, 16, 24, 0.92)', fg: '#e6e9ef', muted: '#9aa3b2', btn: '#1d2433', btnHover: '#2b3650',
  on: '#2f7fd8', onFg: '#ffffff', warn: '#ffd27a', err: '#ff8a8a', edge: '#4a5670', armed: '#d2731a',
};

function roundRect(c, x, y, w, h, r) {
  c.beginPath();
  c.moveTo(x + r, y);
  c.arcTo(x + w, y, x + w, y + h, r);
  c.arcTo(x + w, y + h, x, y + h, r);
  c.arcTo(x, y + h, x, y, r);
  c.arcTo(x, y, x + w, y, r);
  c.closePath();
}

function fit(c, text, maxW) {
  if (c.measureText(text).width <= maxW) return text;
  let t = text;
  while (t.length > 1 && c.measureText(`${t}…`).width > maxW) t = t.slice(0, -1);
  return `${t}…`;
}

/** view: {seedMode, moment (order shown or null), pvArmed, canApply, canShow,
 *  pvWidth, status: [lines], error}; ui: {hover}. */
export function drawTools(c, layout, view, ui = {}) {
  const [W, H] = TOOLS_PX;
  c.clearRect(0, 0, W, H);
  c.fillStyle = C.bg;
  roundRect(c, 0, 0, W, H, 18);
  c.fill();
  c.textBaseline = 'middle';
  ROW_LABELS.forEach((label, i) => {
    c.fillStyle = C.fg;
    c.font = '28px -apple-system, system-ui, sans-serif';
    c.fillText(label, 20, ROWS[i] + WH / 2);
  });
  for (const w of layout) {
    const [x, y, ww, hh] = w.rect;
    const hov = ui.hover === w.id;
    const on = (w.group === 'seed' && view.seedMode === w.value) || (w.group === 'moment' && view.moment === w.value);
    const armed = w.id === 'pv-draw' && view.pvArmed;
    const disabled = ((w.id === 'isolate' || w.id === 'remove' || w.group === 'moment') && !view.canApply)
      || (w.id === 'show-applied' && !view.canShow);
    c.fillStyle = armed ? C.armed : on ? C.on : hov && !disabled ? C.btnHover : C.btn;
    roundRect(c, x, y, ww, hh, 10);
    c.fill();
    if (hov) { c.strokeStyle = C.edge; c.lineWidth = 2; c.stroke(); }
    c.fillStyle = disabled ? '#5b6475' : on || armed ? C.onFg : C.fg;
    c.font = '26px -apple-system, system-ui, sans-serif';
    c.textAlign = 'center';
    const label = w.id === 'pv-draw' && view.pvArmed ? 'Drawing' : w.label;
    c.fillText(fit(c, label, ww - 12), x + ww / 2, y + hh / 2 + 1);
    c.textAlign = 'left';
  }
  c.font = '22px ui-monospace, Menlo, monospace';
  let y = ROWS[3] + WH + 34;
  c.fillStyle = C.muted;
  c.fillText(`slit width ${view.pvWidth || 1} px`, 20, y - 4 + 0);
  y += 30;
  // The error first, so it is never cut off by the other lines.
  const status = view.error ? [view.error, ...(view.status || []).filter((l) => l !== view.error)] : (view.status || []);
  for (const l of status.slice(0, 4)) {
    c.fillStyle = l === view.error ? C.err : C.warn;
    c.fillText(fit(c, l, W - 40), 20, y);
    y += 28;
  }
}

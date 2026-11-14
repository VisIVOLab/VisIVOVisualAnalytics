// The in-VR control panel (issue #172): a canvas drawn to a texture, used with
// the controller's ray and trigger (or the mouse in a desktop browser). Since
// issue #201 it is the Look page of the hand menu (hand-menu.js), drawn there
// with `embedded`; the ray-quad hit and the pointer serve the menu and the
// result windows (windows.js) too.
// The layout, the ray-quad hit and the pointer logic are pure and unit-tested
// (backend/tests/vr/vr-panel.test.mjs); drawPanel needs only a 2-D context.

import * as M from './mat4.js';

export const PANEL_PX = [1024, 832];
/** Metres: 2133 px/m, so the 22-32 px text is 1.0-1.5 cm high (about 0.8-1.1° at 0.75 m). */
export const PANEL_SIZE = [0.48, 0.39];

const WH = 56, LABEL_W = 230, X0 = 240, RIGHT = 1004;
// The rows' tops; under the channel range (row 5) a line of text: its velocities.
const ROWS = [140, 212, 284, 356, 428, 500, 604, 676];
const SLAB_TEXT_Y = 580;

/** The panel's widgets: {id, kind, rect: [x, y, w, h], row, ...}. Fixed: it does
 *  not depend on the values shown, so a hit is decided by geometry alone. */
export function panelLayout() {
  const w = [];
  const row = (i) => ROWS[i];
  // The title bar drags the panel; its right end recalls the controls guide (#166).
  w.push({ id: 'drag', kind: 'drag', rect: [0, 0, PANEL_PX[0] - 96, 72] });
  w.push({ id: 'guide', kind: 'button', rect: [PANEL_PX[0] - 88, 8, 72, WH], label: '?' });
  // Row 0: colour map, render mode.
  w.push({ id: 'cmap-prev', kind: 'button', rect: [X0, row(0), 64, WH], label: '◀' });
  w.push({ id: 'cmap-next', kind: 'button', rect: [X0 + 264, row(0), 64, WH], label: '▶' });
  w.push({ id: 'mode:0', kind: 'button', rect: [620, row(0), 184, WH], label: 'emission', group: 'mode', value: 0 });
  w.push({ id: 'mode:1', kind: 'button', rect: [812, row(0), 192, WH], label: 'max intensity', group: 'mode', value: 1 });
  // Row 1: threshold; row 2: its source.
  w.push({ id: 'thr', kind: 'slider', rect: [X0, row(1), 544, WH] });
  w.push({ id: 'thr-reset', kind: 'button', rect: [800, row(1), RIGHT - 800, WH], label: 'Reset' });
  w.push({ id: 'thrsrc:reduced', kind: 'button', rect: [X0, row(2), 372, WH], label: 'for this binning', group: 'thrsrc', value: 'reduced' });
  w.push({ id: 'thrsrc:cube', kind: 'button', rect: [X0 + 380, row(2), RIGHT - X0 - 380, WH], label: 'full cube (desktop)', group: 'thrsrc', value: 'cube' });
  // Rows 3, 4: colour max, opacity.
  w.push({ id: 'cmax', kind: 'slider', rect: [X0, row(3), RIGHT - X0, WH] });
  w.push({ id: 'opacity', kind: 'slider', rect: [X0, row(4), RIGHT - X0, WH] });
  // Row 5: the channel range (the velocity slab).
  w.push({ id: 'slab', kind: 'range', rect: [X0, row(5), 544, WH] });
  w.push({ id: 'slab-all', kind: 'button', rect: [800, row(5), RIGHT - 800, WH], label: 'All channels' });
  // Row 6: refinement.
  w.push({ id: 'refine', kind: 'toggle', rect: [X0, row(6), 160, WH] });
  // Row 7: the selection tool.
  const tools = [['probe', 'spectrum'], ['lasso', 'lasso'], ['box', 'box']];
  tools.forEach(([t, label], i) => {
    w.push({ id: `tool:${t}`, kind: 'button', rect: [X0 + i * 196, row(7), 188, WH], label, group: 'tool', value: t });
  });
  w.push({ id: 'sel-clear', kind: 'button', rect: [X0 + 3 * 196, row(7), RIGHT - X0 - 3 * 196, WH], label: 'Clear' });
  return w;
}

export const ROW_LABELS = ['Colour map', 'Threshold', 'from', 'Colour max', 'Opacity', 'Channels', 'Refine', 'Tool'];

const inRect = (r, x, y) => x >= r[0] && x < r[0] + r[2] && y >= r[1] && y < r[1] + r[3];
const clamp01 = (v) => Math.max(0, Math.min(1, v));
const TRACK_PAD = 18;

/** The slider fraction (0..1) of canvas x on widget `w`'s track. */
export function trackFraction(w, x) {
  const x0 = w.rect[0] + TRACK_PAD, len = w.rect[2] - 2 * TRACK_PAD;
  return clamp01((x - x0) / len);
}

/** The widget under canvas point (x, y), or null. */
export function hitWidget(layout, x, y) {
  for (const w of layout) if (inRect(w.rect, x, y)) return w;
  return null;
}

/**
 * Where a world ray meets the panel: `model` places the quad (centred on its
 * origin, in its xy plane, facing +z), `size` its sides in metres. Returns
 * {x, y} in canvas pixels (y down), `t` along the ray (in units of |d|) and
 * the world point, or null when the ray misses it or points away.
 */
export function rayPanel(model, size, o, d, px = PANEL_PX) {
  const inv = M.invert(model);
  const lo = M.transformPoint(inv, o);
  const l1 = M.transformPoint(inv, [o[0] + d[0], o[1] + d[1], o[2] + d[2]]);
  const ld = M.sub(l1, lo);
  if (Math.abs(ld[2]) < 1e-9) return null;
  const t = -lo[2] / ld[2];
  if (!(t > 0)) return null;
  const u = (lo[0] + t * ld[0]) / size[0] + 0.5;
  const v = 0.5 - (lo[1] + t * ld[1]) / size[1];
  if (u < 0 || u > 1 || v < 0 || v > 1) return null;
  return { x: u * px[0], y: v * px[1], t, point: [o[0] + t * d[0], o[1] + t * d[1], o[2] + t * d[2]] };
}

/**
 * Is the panel hit at `hitT` (rayPanel's t, in units of |d|) behind the cube?
 * The cube is the box [boxMin, boxMax] (box coordinates 0..1, the clipping
 * box drawn) of a model of sides `extent` centred on its origin, placed by
 * `cubeModel`. True when the ray enters that box in front of the panel (entry
 * t > 0 and before the panel), so the cube, not the panel, takes the ray; a
 * ray starting inside the box leaves it to the panel. t is the same in both spaces
 * (an affine map keeps the ray's parameter).
 */
export function panelBehindCube(hitT, cubeModel, extent, o, d, boxMin = [0, 0, 0], boxMax = [1, 1, 1]) {
  const inv = M.invert(cubeModel);
  const mo = M.transformPoint(inv, o);
  const md = M.sub(M.transformPoint(inv, [o[0] + d[0], o[1] + d[1], o[2] + d[2]]), mo);
  const lo = [0, 1, 2].map((i) => (boxMin[i] - 0.5) * extent[i]);
  const hi = [0, 1, 2].map((i) => (boxMax[i] - 0.5) * extent[i]);
  const span = M.rayBox(mo, md, lo, hi);
  // Only a box the ray ENTERS in front of the panel hides it: with the
  // controller inside the box (a cube scaled up around you) the panel wins,
  // since that is where it is needed most.
  if (!span || span[0] <= 0) return false;
  return span[0] < hitT;
}

/**
 * The pointer on the panel: hover, press, drag with capture. `update(hit,
 * pressed)` once per frame (hit: rayPanel's answer or null), returns the
 * actions to apply: {type: 'press', id, widget} for a button, toggle or swatch;
 * {type: 'slide', id, value} for a slider (0..1); {type: 'range', id, value:
 * [lo, hi]} for the channel range; {type: 'drag-start' | 'drag' | 'drag-end',
 * hit}. A press that starts on the panel belongs to it until released, even
 * when the ray leaves it (a slider keeps following), and `owns` says so: the
 * selection tool must then not fire.
 * `rangeValue(id)` gives the current [lo, hi] of a range widget.
 */
export class PanelPointer {
  constructor(layout, { rangeValue = () => [0, 1], minGap = 0.01 } = {}) {
    this.layout = layout;
    this.rangeValue = rangeValue;
    this.minGap = minGap;
    this.hover = null;      // id under the ray
    this.active = null;     // the widget captured by a press
    this.handle = 0;        // range: 0 = low end, 1 = high end
    this.owns = false;      // the current press started on the panel
    this.down = false;
  }

  update(hit, pressed) {
    const out = [];
    const w = hit ? hitWidget(this.layout, hit.x, hit.y) : null;
    this.hover = w ? w.id : null;
    const edgeDown = pressed && !this.down;
    const edgeUp = !pressed && this.down;
    this.down = pressed;
    if (edgeDown) {
      this.owns = !!hit;
      this.active = null;
      if (w) {
        if (w.kind === 'button' || w.kind === 'toggle' || w.kind === 'swatch') {
          out.push({ type: 'press', id: w.id, widget: w });
        } else if (w.kind === 'slider') {
          this.active = w;
          out.push({ type: 'slide', id: w.id, value: trackFraction(w, hit.x) });
        } else if (w.kind === 'range') {
          this.active = w;
          const f = trackFraction(w, hit.x);
          const [lo, hi] = this.rangeValue(w.id);
          // The nearer end moves; between equal distances, the side the press is on.
          this.handle = Math.abs(f - lo) < Math.abs(f - hi) ? 0 : Math.abs(f - lo) > Math.abs(f - hi) ? 1 : (f < lo ? 0 : 1);
          out.push({ type: 'range', id: w.id, value: this._range(f, lo, hi) });
        } else if (w.kind === 'drag') {
          this.active = w;
          out.push({ type: 'drag-start', hit });
        }
      }
    } else if (pressed && this.active) {
      const a = this.active;
      if (a.kind === 'drag') {
        out.push({ type: 'drag' });
      } else if (hit) {
        if (a.kind === 'slider') out.push({ type: 'slide', id: a.id, value: trackFraction(a, hit.x) });
        else if (a.kind === 'range') {
          const [lo, hi] = this.rangeValue(a.id);
          out.push({ type: 'range', id: a.id, value: this._range(trackFraction(a, hit.x), lo, hi) });
        }
      }
    } else if (edgeUp) {
      if (this.active && this.active.kind === 'drag') out.push({ type: 'drag-end' });
      this.active = null;
      // `owns` stays true for this frame so the release is not a selection either.
    } else if (!pressed) {
      this.owns = false;
    }
    return out;
  }

  _range(f, lo, hi) {
    if (this.handle === 0) return [Math.min(f, hi - this.minGap), hi].map(clamp01);
    return [lo, Math.max(f, lo + this.minGap)].map(clamp01);
  }

  /** Does the panel take the ray now (hovered or captured)? */
  takesRay() { return this.hover !== null || this.active !== null || (this.down && this.owns); }
}

/**
 * The panel's pose: at offset {right, up, fwd, yaw} from an anchor {p, yaw}
 * (where the cube was first placed: right/forward in its frame), facing the
 * viewer the anchor faced, turned by `yaw`.
 */
export function panelModel(anchor, off) {
  const ay = anchor.yaw;
  const right = [Math.cos(ay), 0, -Math.sin(ay)];
  const fwd = [-Math.sin(ay), 0, -Math.cos(ay)];      // from the viewer toward the cube
  const p = M.add(anchor.p, M.add(M.scale3(right, off.right), M.add([0, off.up, 0], M.scale3(fwd, off.fwd))));
  return M.multiply(M.translation(...p), M.rotation([0, 1, 0], ay + off.yaw));
}

/** The offset that puts the panel's centre at world `p`, facing world point `eye`. */
export function offsetFor(anchor, p, eye) {
  const ay = anchor.yaw;
  const right = [Math.cos(ay), 0, -Math.sin(ay)];
  const fwd = [-Math.sin(ay), 0, -Math.cos(ay)];
  const r = M.sub(p, anchor.p);
  const toEye = M.sub(eye, p);
  // The quad faces +z: its yaw is the direction from the panel to the eye.
  let yaw = Math.atan2(toEye[0], toEye[2]) - ay;
  yaw = Math.atan2(Math.sin(yaw), Math.cos(yaw));
  return { right: M.dot(r, right), up: r[1], fwd: M.dot(r, fwd), yaw };
}

/** Left of the cube, a little nearer, turned toward where the viewer stood:
 *  about 0.78 m away for a cube placed 0.75 m in front. */
export const DEFAULT_OFFSET = { right: -0.62, up: -0.02, fwd: -0.28, yaw: 0.92 };

// ── Drawing ─────────────────────────────────────────────────────────────────

const C = {
  bg: 'rgba(12, 16, 24, 0.92)', head: 'rgba(30, 38, 54, 0.96)', fg: '#e6e9ef', muted: '#9aa3b2',
  btn: '#1d2433', btnHover: '#2b3650', on: '#2f7fd8', onFg: '#ffffff', track: '#3a4357', fill: '#4fa3ff',
  warn: '#ffd27a', err: '#ff8a8a', edge: '#4a5670',
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

/**
 * Draws the panel. `view`: {title, info: [lines], cmap, mode, thr: {f, text},
 * thrSource, cmax: {f, text}, opacity: {f, text}, slab: {lo, hi, text},
 * refine: {on, text}, tool, status, hint}; `ui`: {hover, active}.
 */
export function drawPanel(c, layout, view, ui = {}, { embedded = false } = {}) {
  const [W, H] = PANEL_PX;
  c.clearRect(0, 0, W, H);
  c.fillStyle = C.bg;
  roundRect(c, 0, 0, W, H, 18);
  c.fill();
  c.fillStyle = ui.hover === 'drag' || (ui.active && ui.active === 'drag') ? '#34415e' : C.head;
  roundRect(c, 0, 0, W, 72, 18);
  c.fill();
  c.textBaseline = 'middle';
  c.fillStyle = C.fg;
  c.font = 'bold 32px -apple-system, system-ui, sans-serif';
  c.fillText(fit(c, view.title || 'VisIVO VR', W - 330), 20, 37);
  c.font = '22px -apple-system, system-ui, sans-serif';
  c.fillStyle = C.muted;
  // In the hand menu (issue #201) the panel is a page: nothing to drag.
  if (!embedded) {
    c.textAlign = 'right';
    c.fillText('⠿ drag to move', W - 108, 37);
    c.textAlign = 'left';
  }
  c.font = '24px ui-monospace, Menlo, monospace';
  (view.info || []).slice(0, 2).forEach((l, i) => { c.fillStyle = i ? C.muted : C.fg; c.fillText(fit(c, l, W - 40), 20, 92 + i * 28); });

  ROW_LABELS.forEach((label, i) => {
    const y = ROWS[i];
    c.fillStyle = C.fg;
    c.font = '28px -apple-system, system-ui, sans-serif';
    c.fillText(label, 20, y + (i === 2 ? WH / 2 : 18));
  });
  const valueUnder = (i, text) => {
    c.fillStyle = C.muted;
    c.font = '22px ui-monospace, Menlo, monospace';
    c.fillText(fit(c, text || '', LABEL_W - 24), 20, ROWS[i] + 46);
  };
  valueUnder(1, view.thr && view.thr.text);
  valueUnder(3, view.cmax && view.cmax.text);
  valueUnder(4, view.opacity && view.opacity.text);

  for (const w of layout) {
    const [x, y, ww, hh] = w.rect;
    const hov = ui.hover === w.id;
    if (w.kind === 'button') {
      const on = (w.group === 'mode' && view.mode === w.value) || (w.group === 'thrsrc' && view.thrSource === w.value)
        || (w.group === 'tool' && view.tool === w.value);
      c.fillStyle = on ? C.on : hov ? C.btnHover : C.btn;
      roundRect(c, x, y, ww, hh, 10);
      c.fill();
      if (hov) { c.strokeStyle = C.edge; c.lineWidth = 2; c.stroke(); }
      c.fillStyle = on ? C.onFg : C.fg;
      c.font = '26px -apple-system, system-ui, sans-serif';
      c.textAlign = 'center';
      c.fillText(fit(c, w.label, ww - 16), x + ww / 2, y + hh / 2 + 1);
      c.textAlign = 'left';
    } else if (w.kind === 'slider' || w.kind === 'range') {
      const x0 = x + TRACK_PAD, len = ww - 2 * TRACK_PAD, cy = y + hh / 2;
      c.fillStyle = hov || (ui.active === w.id) ? '#202838' : 'rgba(0,0,0,0)';
      roundRect(c, x, y, ww, hh, 10);
      c.fill();
      c.fillStyle = C.track;
      roundRect(c, x0, cy - 5, len, 10, 5);
      c.fill();
      let a = 0, b = 0;
      if (w.kind === 'slider') { const v = view[w.id]; b = clamp01(v ? v.f : 0); }
      else { a = clamp01(view.slab ? view.slab.lo : 0); b = clamp01(view.slab ? view.slab.hi : 1); }
      c.fillStyle = C.fill;
      roundRect(c, x0 + a * len, cy - 5, Math.max(2, (b - a) * len), 10, 5);
      c.fill();
      const knob = (f) => {
        c.beginPath();
        c.arc(x0 + f * len, cy, 16, 0, 2 * Math.PI);
        c.fillStyle = '#e6e9ef';
        c.fill();
      };
      if (w.kind === 'range') knob(a);
      knob(b);
    } else if (w.kind === 'toggle') {
      const on = !!(view.refine && view.refine.on);
      c.fillStyle = on ? C.on : hov ? C.btnHover : C.btn;
      roundRect(c, x, y, ww, hh, hh / 2);
      c.fill();
      c.beginPath();
      c.arc(on ? x + ww - hh / 2 : x + hh / 2, y + hh / 2, hh / 2 - 6, 0, 2 * Math.PI);
      c.fillStyle = '#ffffff';
      c.fill();
      c.fillStyle = on ? C.onFg : C.fg;
      c.font = '24px -apple-system, system-ui, sans-serif';
      c.textAlign = 'center';
      c.fillText(on ? 'on' : 'off', on ? x + (ww - hh) / 2 : x + hh + (ww - hh) / 2 - 6, y + hh / 2 + 1);
      c.textAlign = 'left';
    }
  }
  // Values to the right of their widgets.
  c.font = '26px -apple-system, system-ui, sans-serif';
  c.fillStyle = C.fg;
  c.textAlign = 'center';
  c.fillText(fit(c, view.cmap || '', 190), X0 + 64 + 100, ROWS[0] + WH / 2 + 1);
  c.textAlign = 'left';
  c.font = '22px ui-monospace, Menlo, monospace';
  c.fillStyle = C.muted;
  c.fillText(fit(c, (view.slab && view.slab.text) || '', LABEL_W - 24), 20, ROWS[5] + 46);
  c.font = '24px ui-monospace, Menlo, monospace';
  c.fillStyle = C.fg;
  c.fillText(fit(c, (view.slab && view.slab.world) || '', RIGHT - X0), X0, SLAB_TEXT_Y);
  c.fillStyle = view.refine && view.refine.warn ? C.warn : C.muted;
  c.fillText(fit(c, (view.refine && view.refine.text) || '', RIGHT - X0 - 180), X0 + 180, ROWS[6] + WH / 2);

  const fy = ROWS[7] + WH + 30;
  c.font = '22px -apple-system, system-ui, sans-serif';
  if (view.status) { c.fillStyle = view.statusError ? C.err : C.warn; c.fillText(fit(c, view.status, W - 40), 20, fy); }
  c.fillStyle = C.muted;
  c.fillText(fit(c, view.hint || '', W - 40), 20, fy + 30);
}

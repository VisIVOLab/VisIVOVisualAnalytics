// Result windows in the headset (issue #201): the selection's results
// (spectrum, statistics), the moment and the PV diagram, each a quad with a
// title bar above it (windowRects), named after what it shows (windowTitle,
// issue #267). The primary ray and trigger on the bar move the window
// (drag) or close it (×); a new result opens it again. Where each one was
// left is kept for the next sessions (settings.js `windows`).
//
// Also the choice of which of several surfaces (the hand menu, the windows)
// takes the ray this frame. Pure: tested by node (backend/tests/vr/windows.test.mjs).

import * as M from './mat4.js';
import { PanelPointer, rayPanel, panelModel } from './vr-panel.js';
import { TOOL_NAMES } from './select-ui.js';
import { fitText } from './text-fit.js';

export const BAR_PX = [768, 64];
export const BAR_GAP = 0.004;

/** The windows: their title, content size (metres) and default place, as
 *  offsets from where the cube was first put ({right, up, fwd, yaw}, as
 *  vr-panel's panelModel); `desktop` the place in the page's 3-D view. */
export const WINDOWS = {
  result: { title: 'Selection', size: [0.34, 0.248],
    place: { right: 0.55, up: 0.12, fwd: 0, yaw: -0.5 }, desktop: { right: 0.95, up: -0.42, fwd: -0.3, yaw: -0.45 } },
  moment: { title: 'Moment', size: null,
    place: { right: 0.42, up: 0.48, fwd: -0.05, yaw: -0.35 }, desktop: { right: -1.12, up: 0.22, fwd: 0.1, yaw: 0.55 } },
  pv: { title: 'PV diagram', size: null,
    place: { right: -0.42, up: 0.48, fwd: -0.05, yaw: 0.35 }, desktop: { right: -1.12, up: -0.22, fwd: 0.1, yaw: 0.55 } },
};

/**
 * The name on a window's bar (issue #267): what it shows, not a generic
 * "Selection". `state`: {tool} (the selection's tool, whose result the
 * Selection window draws), {momentOrder} (the moment on show, or null).
 */
export function windowTitle(id, state = {}) {
  if (id === 'result') return TOOL_NAMES[state.tool] || WINDOWS.result.title;
  if (id === 'moment' && Number.isInteger(state.momentOrder)) return `Moment ${state.momentOrder}`;
  return WINDOWS[id] ? WINDOWS[id].title : String(id);
}

/**
 * A window's parts in its own plane (metres, y up, origin at the content's
 * centre), as [x0, y0, x1, y1]: the content of `size`, the bar of the same
 * width above it, BAR_GAP apart. The bar never covers the content.
 */
export function windowRects(size) {
  const [w, h] = size;
  const bh = barSize(w)[1];
  const top = h / 2 + BAR_GAP;
  return { content: [-w / 2, -h / 2, w / 2, h / 2], bar: [-w / 2, top, w / 2, top + bh] };
}

/** The bar's widgets: drag (most of it) and close (×, at its right end). */
export function barLayout() {
  const [W, H] = BAR_PX;
  return [
    { id: 'drag', kind: 'drag', rect: [0, 0, W - 84, H] },
    { id: 'close', kind: 'button', rect: [W - 76, 4, 72, H - 8], label: '×' },
  ];
}

/** The bar's size in metres for a window `width` wide. */
export function barSize(width) { return [width, width * BAR_PX[1] / BAR_PX[0]]; }

/** The bar's pose: above the content quad `model` of size `size`. */
export function barModel(model, size) {
  const b = windowRects(size).bar;
  return M.multiply(model, M.translation(0, (b[1] + b[3]) / 2, 0));
}

/** The content's pose from the offset `off` from `anchor` ({p, yaw}). */
export function windowModel(anchor, off) { return panelModel(anchor, off); }

/**
 * Which surface takes the ray: `hits` [{t} | null] per surface, `captured`
 * the index of a surface holding a press (it keeps the ray), or -1. Returns
 * the index of the nearest hit (or the captured one), -1 for none.
 */
export function pickSurface(hits, captured = -1) {
  if (captured >= 0) return captured;
  let best = -1;
  hits.forEach((h, i) => { if (h && (best < 0 || h.t < hits[best].t)) best = i; });
  return best;
}

/**
 * One window's pointer: the ray on its bar (drag, close) and on its content
 * (which takes the ray, so the selection does not fire through it).
 */
export class WindowPointer {
  constructor() { this.bar = new PanelPointer(barLayout()); this.contentDown = false; this.onContent = false; }
  /** hits: {bar, content} (rayPanel answers or null). Returns the bar's actions. */
  update(hits, pressed) {
    const acts = this.bar.update(hits.bar, pressed);
    this.onContent = !!hits.content;
    if (pressed && !this.down && hits.content) this.contentDown = true;
    if (!pressed) this.contentDown = false;
    this.down = pressed;
    return acts;
  }
  /** The ray is on the window (hovering its content included), or a press began on it. */
  takesRay() { return this.bar.takesRay() || this.bar.owns || this.contentDown || this.onContent; }
  get captured() { return !!this.bar.active || this.contentDown; }
}

/** The ray's hits on a window: {bar, content, t (nearest), point}. */
export function windowHits(model, size, ray) {
  if (!ray) return { bar: null, content: null, t: null };
  const content = rayPanel(model, size, ray.o, ray.d, [1000, 1000]);
  const bar = rayPanel(barModel(model, size), barSize(size[0]), ray.o, ray.d, BAR_PX);
  const near = [bar, content].filter(Boolean).sort((a, b) => a.t - b.t)[0] || null;
  return { bar, content, t: near ? near.t : null, point: near ? near.point : null };
}

// ── Drawing ─────────────────────────────────────────────────────────────────

function roundRect(c, x, y, w, h, r) {
  c.beginPath();
  c.moveTo(x + r, y);
  c.arcTo(x + w, y, x + w, y + h, r);
  c.arcTo(x + w, y + h, x, y + h, r);
  c.arcTo(x, y + h, x, y, r);
  c.arcTo(x, y, x + w, y, r);
  c.closePath();
}

const BAR_TITLE_FONT = 'bold 30px -apple-system, system-ui, sans-serif';
const BAR_HINT_FONT = '22px -apple-system, system-ui, sans-serif';
export const BAR_HINT = '⠿ trigger here to move';

/**
 * The bar's text: the title from x = 18, the drag hint right-aligned at
 * W - 100 (left of the close button). `measureTitle` / `measureHint` give
 * widths in the two fonts. The hint is left out when the title needs its
 * room; a title wider than the whole drag area is cut with an ellipsis.
 * Returns {title, hint (the text or ''), titleMax, hintRight}.
 */
export function barText(title, measureTitle, measureHint) {
  const [W] = BAR_PX;
  const left = 18, hintRight = W - 100, gap = 24;
  const hintLeft = hintRight - measureHint(BAR_HINT);
  if (left + measureTitle(String(title ?? '')) + gap <= hintLeft) {
    return { title: String(title ?? ''), hint: BAR_HINT, titleMax: hintLeft - gap - left, hintRight };
  }
  const titleMax = hintRight - left;
  return { title: fitText(measureTitle, title, titleMax), hint: '', titleMax, hintRight };
}

/** The title bar: `title`, a drag hint, the close button; ui: {hover}. */
export function drawBar(c, title, ui = {}) {
  const [W, H] = BAR_PX;
  c.clearRect(0, 0, W, H);
  c.fillStyle = ui.hover === 'drag' || ui.active === 'drag' ? 'rgba(52, 65, 94, 0.96)' : 'rgba(30, 38, 54, 0.96)';
  roundRect(c, 0, 0, W, H, 12);
  c.fill();
  c.textBaseline = 'middle';
  const width = (font) => (s) => { c.font = font; return c.measureText(s).width; };
  const t = barText(title, width(BAR_TITLE_FONT), width(BAR_HINT_FONT));
  c.fillStyle = '#e6e9ef';
  c.font = BAR_TITLE_FONT;
  c.textAlign = 'left';
  c.fillText(t.title, 18, H / 2 + 1);
  if (t.hint) {
    c.font = BAR_HINT_FONT;
    c.fillStyle = '#9aa3b2';
    c.textAlign = 'right';
    c.fillText(t.hint, t.hintRight, H / 2 + 1);
  }
  const [x, y, w, h] = barLayout()[1].rect;
  c.fillStyle = ui.hover === 'close' ? '#8a3a3a' : '#3a2a30';
  roundRect(c, x, y, w, h, 10);
  c.fill();
  c.fillStyle = '#ffffff';
  c.font = 'bold 36px -apple-system, system-ui, sans-serif';
  c.textAlign = 'center';
  c.fillText('×', x + w / 2, y + h / 2 + 2);
  c.textAlign = 'left';
}

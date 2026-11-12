// Messages that must reach the user wherever they are (epic #166): on the 2-D
// page as a banner, and in the headset as a card in front of the viewer, with
// buttons used by the primary controller's ray and trigger (or the mouse, in
// the desktop view with ?panel3d). The lifecycle's notices (lifecycle.js) and
// the first-run controls guide (onboarding.js) are shown here.
//
// The layout, the text wrapping, the card's placement and the choice of
// which notice is on show are pure and unit-tested
// (backend/tests/vr/notice.test.mjs); drawCard needs only a 2-D context.

import * as M from './mat4.js';

export const CARD_PX = [1120, 640];
/** Metres: 2000 px/m, so the 26-36 px text is 1.3-1.8 cm high (about 1-1.4° at 0.8 m). */
export const CARD_SIZE = [0.56, 0.32];

const PAD = 36, TITLE_Y = 56, BODY_Y = 120, LINE_H = 38, BTN_H = 76, BTN_GAP = 20;
export const BODY_FONT = '28px -apple-system, system-ui, sans-serif';
export const MAX_ACTIONS = 4;

/** Lower value = shown first. A guide never hides an error. */
const PRIORITY = { error: 0, warning: 1, info: 2, guide: 3 };

/**
 * The card's buttons, right-aligned in its bottom row: widgets {id:
 * 'act:<action id>', kind: 'button', rect, label, action}. Up to MAX_ACTIONS.
 */
export function cardLayout(notice) {
  const acts = ((notice && notice.actions) || []).slice(0, MAX_ACTIONS);
  const [W, H] = CARD_PX;
  const y = H - PAD - BTN_H;
  const w = Math.min(300, Math.floor((W - 2 * PAD - (acts.length - 1) * BTN_GAP) / Math.max(1, acts.length)));
  let x = W - PAD - acts.length * w - (acts.length - 1) * BTN_GAP;
  return acts.map((a) => {
    const widget = { id: `act:${a.id}`, kind: 'button', rect: [x, y, w, BTN_H], label: a.label, action: a.id };
    x += w + BTN_GAP;
    return widget;
  });
}

/** Lines of at most `maxW` (as `measure(text)` reports), words kept whole
 *  (a single word longer than a line is cut). */
export function wrapText(text, maxW, measure) {
  const out = [];
  for (const para of String(text || '').split('\n')) {
    let line = '';
    for (const word of para.split(/\s+/).filter(Boolean)) {
      const next = line ? `${line} ${word}` : word;
      if (measure(next) <= maxW) { line = next; continue; }
      if (line) out.push(line);
      let w = word;
      while (measure(w) > maxW && w.length > 1) {
        let k = w.length - 1;
        while (k > 1 && measure(w.slice(0, k)) > maxW) k--;
        out.push(w.slice(0, k));
        w = w.slice(k);
      }
      line = w;
    }
    out.push(line);
  }
  return out;
}

/** How many body lines fit above the buttons. */
export function bodyCapacity() {
  return Math.floor((CARD_PX[1] - PAD - BTN_H - 16 - BODY_Y) / LINE_H) + 1;
}

/**
 * The card's pose: `dist` metres in front of the head, a little below the
 * eyes, upright and facing the viewer. `head` is the viewer's transform
 * (column-major 4×4, as XRRigidTransform.matrix).
 */
export function cardModel(head, dist = 0.8, drop = 0.06) {
  const pos = [head[12], head[13], head[14]];
  let fwd = [-head[8], 0, -head[10]];
  if (Math.hypot(fwd[0], fwd[2]) < 1e-6) fwd = [0, 0, -1];   // looking straight up or down
  fwd = M.normalize(fwd);
  const p = M.add(pos, M.add(M.scale3(fwd, dist), [0, -drop, 0]));
  const yaw = Math.atan2(-fwd[0], -fwd[2]);
  return M.multiply(M.translation(...p), M.rotation([0, 1, 0], yaw));
}

/** The card moved aside: `yaw` radians to the viewer's left of the gaze's
 *  heading, lower, a little nearer. */
export function parkedCardModel(head, yaw = 0.7, dist = 0.75, drop = 0.32) {
  return cardModel(M.multiply(head, M.rotation([0, 1, 0], yaw)), dist, drop);
}

/** Should the card be put in front of the viewer again? When it is more
 *  than `maxDeg` from the gaze's heading, or farther than `maxDist`. */
export function cardNeedsMove(cardM, head, maxDeg = 35, maxDist = 1.6) {
  if (!cardM) return true;
  const pos = [head[12], head[13], head[14]];
  const c = M.position(cardM);
  const to = M.sub(c, pos);
  const flat = [to[0], 0, to[2]];
  const dist = Math.hypot(...to);
  if (dist > maxDist || Math.hypot(flat[0], flat[2]) < 1e-6) return true;
  let fwd = [-head[8], 0, -head[10]];
  if (Math.hypot(fwd[0], fwd[2]) < 1e-6) return false;
  fwd = M.normalize(fwd);
  const cos = M.dot(M.normalize(flat), fwd);
  return Math.acos(Math.max(-1, Math.min(1, cos))) * 180 / Math.PI > maxDeg;
}

/**
 * The notices waiting to be shown; the most important one is on show. A
 * notice with the same id replaces the old one; `autoHideMs` notices go by
 * themselves; a dismissed non-sticky notice of a given id is not shown again
 * until `clear(id)` (the condition ended) or it is shown with `force`.
 */
export class NoticeCenter {
  constructor(now = () => Date.now()) {
    this.now = now;
    this.items = new Map();
    this.dismissed = new Set();
    this.hidden = new Set();        // folded into a badge until cleared
    this.version = 0;
  }

  show(notice, { force = false } = {}) {
    if (!notice) return;
    if (!force && this.dismissed.has(notice.id)) return;
    if (force) this.dismissed.delete(notice.id);
    const prev = this.items.get(notice.id);
    const same = prev && JSON.stringify(prev.notice) === JSON.stringify(notice);
    this.items.set(notice.id, { notice, at: prev && same ? prev.at : this.now() });
    if (!same) this.version++;
  }

  clear(id) {
    this.dismissed.delete(id);
    this.hidden.delete(id);
    if (this.items.delete(id)) this.version++;
  }

  /** The user closed it: gone, and not brought back by the same condition. */
  dismiss(id) {
    this.dismissed.add(id);
    if (this.items.delete(id)) this.version++;
  }

  has(id) { return this.items.has(id); }

  /** Off the card, into badge(), until clear(id) (its condition changed). A
   *  show() of the same id meanwhile (the next failed check) keeps it hidden. */
  hide(id) {
    if (!this.items.has(id)) return;
    this.hidden.add(id);
    this.version++;
  }

  /** Back on the card: true when there was one hidden. */
  unhideAll() {
    const any = [...this.hidden].some((id) => this.items.has(id));
    this.hidden.clear();
    if (any) this.version++;
    return any;
  }

  /** The short line for the hidden notices still standing (the most important first), or ''. */
  badge() {
    const ids = [...this.hidden].filter((id) => this.items.has(id));
    if (!ids.length) return '';
    const ns = ids.map((id) => this.items.get(id).notice).sort((a, b) => (PRIORITY[a.level] ?? 2) - (PRIORITY[b.level] ?? 2));
    return `⚠ ${ns[0].badge || ns[0].title} — Help → Guide shows it`;
  }

  /** The notice on show, or null (expired ones are dropped first). */
  current() {
    const t = this.now();
    for (const [id, it] of this.items) {
      if (it.notice.autoHideMs && t - it.at > it.notice.autoHideMs) { this.items.delete(id); this.version++; }
    }
    let best = null;
    for (const [id, it] of this.items) {
      if (this.hidden.has(id)) continue;
      const pr = PRIORITY[it.notice.level] ?? 2;
      if (!best || pr < best.pr || (pr === best.pr && it.at > best.at)) best = { pr, at: it.at, notice: it.notice };
    }
    return best ? best.notice : null;
  }
}

// ── Drawing ─────────────────────────────────────────────────────────────────

const COLORS = {
  error: { edge: '#ff8a8a', head: 'rgba(70, 24, 28, 0.97)' },
  warning: { edge: '#ffd27a', head: 'rgba(66, 48, 18, 0.97)' },
  info: { edge: '#4fa3ff', head: 'rgba(22, 40, 66, 0.97)' },
  guide: { edge: '#4fa3ff', head: 'rgba(22, 40, 66, 0.97)' },
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

/** Draws `notice` with its buttons (`layout` = cardLayout(notice)); `ui` = {hover}. */
export function drawCard(c, notice, layout, ui = {}) {
  const [W, H] = CARD_PX;
  const col = COLORS[notice.level] || COLORS.info;
  c.clearRect(0, 0, W, H);
  c.fillStyle = 'rgba(12, 16, 24, 0.96)';
  roundRect(c, 0, 0, W, H, 22);
  c.fill();
  c.fillStyle = col.head;
  roundRect(c, 0, 0, W, 96, 22);
  c.fill();
  c.strokeStyle = col.edge;
  c.lineWidth = 4;
  roundRect(c, 2, 2, W - 4, H - 4, 20);
  c.stroke();
  c.textBaseline = 'middle';
  c.textAlign = 'left';
  c.fillStyle = '#ffffff';
  c.font = 'bold 36px -apple-system, system-ui, sans-serif';
  c.fillText(notice.title || '', PAD, TITLE_Y - 8);
  c.font = BODY_FONT;
  const measure = (t) => c.measureText(t).width;
  const lines = [];
  for (const l of notice.lines || []) lines.push(...wrapText(l, W - 2 * PAD, measure));
  const cap = bodyCapacity();
  const shown = lines.length > cap ? [...lines.slice(0, cap - 1), `${lines[cap - 1]}…`] : lines;
  c.fillStyle = '#e6e9ef';
  shown.forEach((l, i) => c.fillText(l, PAD, BODY_Y + i * LINE_H));
  for (const w of layout) {
    const [x, y, ww, hh] = w.rect;
    const hov = ui.hover === w.id;
    c.fillStyle = hov ? '#3a5a8a' : '#1d2433';
    roundRect(c, x, y, ww, hh, 14);
    c.fill();
    c.strokeStyle = hov ? '#ffffff' : '#4a5670';
    c.lineWidth = 2;
    c.stroke();
    c.fillStyle = '#ffffff';
    c.font = '30px -apple-system, system-ui, sans-serif';
    c.textAlign = 'center';
    c.fillText(w.label, x + ww / 2, y + hh / 2 + 1);
    c.textAlign = 'left';
  }
  if (notice.footer) {
    c.fillStyle = '#9aa3b2';
    c.font = '22px -apple-system, system-ui, sans-serif';
    c.fillText(notice.footer, PAD, H - PAD - BTN_H / 2);
  }
}

/** The 2-D page's banner: the same text and buttons. `onAction(id)`. */
export function renderDom(doc, notice, onAction) {
  const box = doc.getElementById('notice');
  if (!box) return;
  if (!notice) { box.hidden = true; return; }
  box.hidden = false;
  box.className = notice.level === 'guide' ? 'info' : notice.level;
  doc.getElementById('notice-title').textContent = notice.title || '';
  doc.getElementById('notice-body').textContent = (notice.lines || []).join('\n');
  const acts = doc.getElementById('notice-actions');
  acts.textContent = '';
  for (const a of (notice.actions || []).slice(0, MAX_ACTIONS)) {
    const b = doc.createElement('button');
    b.textContent = a.label;
    b.addEventListener('click', () => onAction(a.id));
    acts.appendChild(b);
  }
}

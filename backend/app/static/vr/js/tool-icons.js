// The tools' icons (issue #257): our own vector drawings on a 2-D canvas, no
// icon set. One function per tool draws it in a square cell of side s at
// (x, y), in the colour given; used by the tool palette (tool-palette.js) and
// by the hand menu's tool buttons, so both places look alike. Pure drawing:
// node tests run it on a 2-D context stand-in.

const TAU = 2 * Math.PI;

function stroke(c, color, w) {
  c.strokeStyle = color;
  c.lineWidth = w;
  c.lineCap = 'round';
  c.lineJoin = 'round';
}

/** Explore: an eye. */
function explore(c, x, y, s, color) {
  const cx = x + s / 2, cy = y + s / 2, w = s * 0.42, h = s * 0.24;
  stroke(c, color, s * 0.06);
  c.beginPath();
  c.moveTo(cx - w, cy);
  c.quadraticCurveTo(cx, cy - 2 * h, cx + w, cy);
  c.quadraticCurveTo(cx, cy + 2 * h, cx - w, cy);
  c.closePath();
  c.stroke();
  c.beginPath();
  c.arc(cx, cy, s * 0.12, 0, TAU);
  c.fillStyle = color;
  c.fill();
}

/** Kinematic Lasso: a loose loop around a blob, its rope trailing to a pointer. */
function lasso(c, x, y, s, color) {
  const cx = x + s * 0.46, cy = y + s * 0.4;
  stroke(c, color, s * 0.06);
  c.beginPath();
  c.ellipse(cx, cy, s * 0.32, s * 0.22, -0.25, 0.35, TAU - 0.05);
  c.stroke();
  c.beginPath();
  c.moveTo(cx + s * 0.27, cy + s * 0.14);
  c.quadraticCurveTo(cx + s * 0.1, cy + s * 0.36, cx + s * 0.3, cy + s * 0.5);
  c.stroke();
  c.beginPath();
  c.arc(cx - s * 0.04, cy, s * 0.1, 0, TAU);
  c.fillStyle = color;
  c.fill();
}

/** Spectrum probe: a crosshair over a small spectrum. */
function probe(c, x, y, s, color) {
  stroke(c, color, s * 0.055);
  const cx = x + s * 0.32, cy = y + s * 0.32, r = s * 0.16;
  c.beginPath();
  c.arc(cx, cy, r, 0, TAU);
  c.moveTo(cx - r * 1.6, cy); c.lineTo(cx - r * 0.5, cy);
  c.moveTo(cx + r * 0.5, cy); c.lineTo(cx + r * 1.6, cy);
  c.moveTo(cx, cy - r * 1.6); c.lineTo(cx, cy - r * 0.5);
  c.moveTo(cx, cy + r * 0.5); c.lineTo(cx, cy + r * 1.6);
  c.stroke();
  // A line profile: flat, a peak, flat.
  c.beginPath();
  const x0 = x + s * 0.1, x1 = x + s * 0.92, yb = y + s * 0.86;
  c.moveTo(x0, yb);
  c.lineTo(x + s * 0.5, yb);
  c.lineTo(x + s * 0.62, y + s * 0.56);
  c.lineTo(x + s * 0.74, yb);
  c.lineTo(x1, yb);
  c.stroke();
}

/** Box region: a dashed rectangle with solid corners. */
function box(c, x, y, s, color) {
  const a = x + s * 0.18, b = y + s * 0.22, w = s * 0.64, h = s * 0.56, k = s * 0.14;
  stroke(c, color, s * 0.05);
  if (typeof c.setLineDash === 'function') c.setLineDash([s * 0.07, s * 0.07]);
  c.strokeRect(a, b, w, h);
  if (typeof c.setLineDash === 'function') c.setLineDash([]);
  stroke(c, color, s * 0.08);
  c.beginPath();
  for (const [px, py, dx, dy] of [[a, b, 1, 1], [a + w, b, -1, 1], [a, b + h, 1, -1], [a + w, b + h, -1, -1]]) {
    c.moveTo(px + dx * k, py); c.lineTo(px, py); c.lineTo(px, py + dy * k);
  }
  c.stroke();
}

/** Velocity range: two brackets on a velocity axis, with a band between. */
function slab(c, x, y, s, color) {
  const yb = y + s * 0.78, a = x + s * 0.32, b = x + s * 0.68;
  c.fillStyle = color;
  c.globalAlpha = 0.35;
  c.fillRect(a, y + s * 0.2, b - a, yb - y - s * 0.2);
  c.globalAlpha = 1;
  stroke(c, color, s * 0.06);
  c.beginPath();
  c.moveTo(x + s * 0.08, yb); c.lineTo(x + s * 0.92, yb);
  c.moveTo(a, y + s * 0.16); c.lineTo(a, yb + s * 0.06);
  c.moveTo(b, y + s * 0.16); c.lineTo(b, yb + s * 0.06);
  c.stroke();
  c.font = `bold ${Math.round(s * 0.2)}px -apple-system, system-ui, sans-serif`;
  c.textAlign = 'center';
  c.textBaseline = 'middle';
  c.fillText('v', x + s / 2, y + s * 0.9);
  c.textAlign = 'left';
}

/** Spatial crop: a cube with one face pushed in. */
function crop(c, x, y, s, color) {
  const d = s * 0.16, a = x + s * 0.16, b = y + s * 0.3, w = s * 0.5;
  stroke(c, color, s * 0.05);
  c.beginPath();
  c.rect(a, b, w, w);                                       // front face
  c.moveTo(a, b); c.lineTo(a + d, b - d); c.lineTo(a + w + d, b - d); c.lineTo(a + w, b);
  c.moveTo(a + w + d, b - d); c.lineTo(a + w + d, b + w - d); c.lineTo(a + w, b + w);
  c.stroke();
  // The crop's face, inside, and its handle.
  stroke(c, color, s * 0.07);
  c.beginPath();
  c.moveTo(a + w * 0.62, b + w * 0.08); c.lineTo(a + w * 0.62, b + w * 0.92);
  c.moveTo(a + w * 0.62, b + w / 2); c.lineTo(a + w * 1.25, b + w / 2);
  c.stroke();
}

/** Moment: a map with nested contours. */
function moment(c, x, y, s, color) {
  stroke(c, color, s * 0.045);
  c.strokeRect(x + s * 0.12, y + s * 0.12, s * 0.76, s * 0.76);
  for (const [r, a] of [[0.3, 0.5], [0.2, 0.75], [0.1, 1]]) {
    c.globalAlpha = a;
    c.beginPath();
    c.ellipse(x + s * 0.5, y + s * 0.52, s * r * 1.2, s * r, 0.4, 0, TAU);
    c.stroke();
  }
  c.globalAlpha = 1;
}

/** PV slice: a path with posts over the cube's face, and its diagram beside. */
function pv(c, x, y, s, color) {
  stroke(c, color, s * 0.055);
  const pts = [[0.14, 0.7], [0.36, 0.34], [0.6, 0.58], [0.86, 0.26]];
  c.beginPath();
  pts.forEach(([px, py], i) => (i ? c.lineTo(x + px * s, y + py * s) : c.moveTo(x + px * s, y + py * s)));
  c.stroke();
  c.fillStyle = color;
  for (const [px, py] of pts) { c.beginPath(); c.arc(x + px * s, y + py * s, s * 0.06, 0, TAU); c.fill(); }
  stroke(c, color, s * 0.035);
  c.beginPath();
  c.moveTo(x + s * 0.1, y + s * 0.88); c.lineTo(x + s * 0.9, y + s * 0.88);
  c.stroke();
}

export const ICONS = { explore, lasso, probe, box, slab, crop, moment, pv };

/** Draws icon `id` in the square (x, y, s); false when there is none. */
export function drawIcon(c, id, x, y, s, color = '#e6e9ef') {
  const f = ICONS[id];
  if (!f) return false;
  c.save();
  f(c, x, y, s, color);
  c.restore();
  return true;
}

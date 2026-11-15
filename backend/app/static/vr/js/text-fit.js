// Text that stays inside its panel (issue #267): long lines wrap at spaces,
// a title too wide for its place is cut with an ellipsis. `measure(text)`
// is the width of `text` in the font the caller draws with (a 2-D canvas's
// measureText(text).width), so these stay pure: tested by node
// (backend/tests/vr/windows.test.mjs).

const ELLIPSIS = '…';

/** The longest start of `word` (at least one character) narrower than `maxWidth`. */
function cutWord(measure, word, maxWidth) {
  let n = 1;
  while (n < word.length && measure(word.slice(0, n + 1)) <= maxWidth) n++;
  return n;
}

/**
 * `text` as lines no wider than `maxWidth`: broken at spaces, a single word
 * wider than the line broken where it reaches the edge. With `maxLines`, at
 * most that many lines (none for 0), the last ending in an ellipsis when
 * text was left out. Spaces at a break are dropped; an empty text gives one
 * empty line. The one exception to the width: a single character wider than
 * the whole line is still put on a line of its own (a glyph is not split).
 */
export function wrapText(measure, text, maxWidth, maxLines = Infinity) {
  if (!(maxLines > 0)) return [];
  const words = String(text ?? '').split(/\s+/).filter(Boolean);
  const lines = [];
  let cur = '';
  for (let word of words) {
    const joined = cur ? `${cur} ${word}` : word;
    if (measure(joined) <= maxWidth) { cur = joined; continue; }
    if (cur) { lines.push(cur); cur = ''; }
    while (measure(word) > maxWidth && word.length > 1) {
      const n = cutWord(measure, word, maxWidth);
      lines.push(word.slice(0, n));
      word = word.slice(n);
    }
    cur = word;
  }
  if (cur || !lines.length) lines.push(cur);
  if (lines.length <= maxLines) return lines;
  const kept = lines.slice(0, maxLines);
  kept[kept.length - 1] = fitText(measure, `${kept[kept.length - 1]}${ELLIPSIS}`, maxWidth);
  return kept;
}

/** `text` cut to fit `maxWidth`, ending in an ellipsis when it was cut;
 *  empty when not even the ellipsis fits. */
export function fitText(measure, text, maxWidth) {
  const s = String(text ?? '');
  if (measure(s) <= maxWidth) return s;
  let lo = 0, hi = s.length;
  // The longest start that fits with the ellipsis after it.
  while (lo < hi) {
    const mid = Math.ceil((lo + hi) / 2);
    if (measure(s.slice(0, mid).trimEnd() + ELLIPSIS) <= maxWidth) lo = mid; else hi = mid - 1;
  }
  if (lo > 0) return s.slice(0, lo).trimEnd() + ELLIPSIS;
  return measure(ELLIPSIS) <= maxWidth ? ELLIPSIS : '';
}

// ── Shrinking a little before cutting (issue #276) ──────────────────────────
// Every text drawn on the VR page's canvases goes through these: it wraps
// inside its box, or shrinks a little (to 85 % of its size at most), and only
// then is cut with an ellipsis. `measureIn(font, text)` is the width of
// `text` in CSS font `font` (measureWith(ctx) for a 2-D context).

/** The sizes a text may shrink to, as fractions of its own. */
export const SHRINK = [1, 0.92, 0.85];

/** CSS font `font` with its pixel size times `f` ("bold 28px X" → "bold 25.8px X"). */
export function scaleFont(font, f) {
  if (f === 1) return font;
  return String(font).replace(/(\d+(?:\.\d+)?)px/, (_, n) => `${Math.round(Number(n) * f * 10) / 10}px`);
}

/** measureIn for a 2-D context: sets its font, measures. */
export function measureWith(c) {
  return (font, text) => { c.font = font; return c.measureText(text).width; };
}

/**
 * One line of `text` in at most `maxWidth`: {text, font, cut}. As it is when
 * it fits, else at the first smaller size of `steps` that fits, else cut with
 * an ellipsis at the smallest.
 */
export function fitLine(measureIn, font, text, maxWidth, steps = SHRINK) {
  const s = String(text ?? '');
  for (const f of steps) {
    const ft = scaleFont(font, f);
    if (measureIn(ft, s) <= maxWidth) return { text: s, font: ft, cut: false };
  }
  const ft = scaleFont(font, steps[steps.length - 1]);
  const t = fitText((x) => measureIn(ft, x), s, maxWidth);
  return { text: t, font: ft, cut: t !== s };
}

/**
 * Paragraphs (a string, or an array of them; '\n' also breaks) wrapped to
 * `maxWidth` in at most `maxLines` lines: {lines, font, cut}, at the first
 * size of `steps` where everything fits, else at the smallest with the last
 * line cut with an ellipsis.
 */
export function wrapFit(measureIn, font, paras, maxWidth, maxLines = Infinity, steps = SHRINK) {
  const list = (Array.isArray(paras) ? paras : [paras]).flatMap((p) => String(p ?? '').split('\n'));
  const wrapAt = (ft) => list.flatMap((p) => wrapText((x) => measureIn(ft, x), p, maxWidth));
  for (const f of steps) {
    const ft = scaleFont(font, f);
    const lines = wrapAt(ft);
    if (lines.length <= maxLines) return { lines, font: ft, cut: false };
  }
  const ft = scaleFont(font, steps[steps.length - 1]);
  const all = wrapAt(ft);
  if (!(maxLines > 0)) return { lines: [], font: ft, cut: all.some(Boolean) };
  const lines = all.slice(0, maxLines);
  lines[lines.length - 1] = fitText((x) => measureIn(ft, x), `${lines[lines.length - 1]}${ELLIPSIS}`, maxWidth);
  return { lines, font: ft, cut: true };
}

/**
 * Draws one line of `text` at (x, y) in at most `maxWidth` (the context's
 * textAlign and textBaseline apply): shrunk a little or cut, as fitLine.
 * Returns what was drawn.
 */
export function drawFit(c, text, x, y, maxWidth, font, steps = SHRINK) {
  const r = fitLine(measureWith(c), font, text, maxWidth, steps);
  c.font = r.font;
  c.fillText(r.text, x, y);
  return r;
}

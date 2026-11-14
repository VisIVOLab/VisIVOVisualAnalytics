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

// The text fitting every canvas of the VR page goes through (issue #276):
// a text shrinks a little before it is cut, paragraphs wrap and shrink, the
// font's size is scaled in place (node --test). The drawn result, with the
// browser's own fonts, is checked by validation/vr_text_fit.mjs.
import test from 'node:test';
import assert from 'node:assert/strict';
import * as T from '../../app/static/vr/js/text-fit.js';

/** A font whose characters are `size * 0.5` px wide (size from the CSS font). */
const measureIn = (font, text) => {
  const size = Number(/(\d+(?:\.\d+)?)px/.exec(font)[1]);
  return [...String(text)].length * size * 0.5;
};

test('scaleFont scales the pixel size only, and leaves a font at scale 1 as it is', () => {
  assert.equal(T.scaleFont('bold 28px -apple-system, system-ui, sans-serif', 0.85), 'bold 23.8px -apple-system, system-ui, sans-serif');
  assert.equal(T.scaleFont('600 40px ui-monospace, Menlo, monospace', 0.92), '600 36.8px ui-monospace, Menlo, monospace');
  assert.equal(T.scaleFont('22px X', 1), '22px X');
});

test('fitLine: as it is when it fits, else a little smaller, else cut with an ellipsis at the smallest size', () => {
  // 10 characters at 20 px = 100 px.
  assert.deepEqual(T.fitLine(measureIn, '20px X', 'abcdefghij', 100), { text: 'abcdefghij', font: '20px X', cut: false });
  // 92 % (18.4 px) is 92 px.
  assert.deepEqual(T.fitLine(measureIn, '20px X', 'abcdefghij', 95), { text: 'abcdefghij', font: '18.4px X', cut: false });
  // 85 % (17 px) is 85 px.
  assert.deepEqual(T.fitLine(measureIn, '20px X', 'abcdefghij', 86), { text: 'abcdefghij', font: '17px X', cut: false });
  // Narrower still: cut at 17 px (8.5 px a character): 60 px holds 7 characters, the ellipsis one of them.
  const r = T.fitLine(measureIn, '20px X', 'abcdefghij', 60);
  assert.equal(r.font, '17px X');
  assert.equal(r.cut, true);
  assert.equal(r.text, 'abcdef…');
  assert.ok(measureIn(r.font, r.text) <= 60);
  // Own steps (a large font that may shrink further).
  assert.equal(T.fitLine(measureIn, '40px X', 'abcdefghij', 150, [1, 0.75]).font, '30px X');
});

test('wrapFit: paragraphs wrapped into the lines given, shrunk when they do not fit, the last line cut as a last resort', () => {
  const text = ['one two three four', 'five six'];
  // 20 px: 10 px a character; 100 px holds "one two" (7), "three four" (10).
  let r = T.wrapFit(measureIn, '20px X', text, 100, 4);
  assert.deepEqual(r, { lines: ['one two', 'three four', 'five six'], font: '20px X', cut: false });
  // Two lines only: at 17 px (8.5 a character) 100 px holds 11 characters: still three lines; cut.
  r = T.wrapFit(measureIn, '20px X', text, 100, 2);
  assert.equal(r.cut, true);
  assert.equal(r.lines.length, 2);
  assert.ok(r.lines[1].endsWith('…'));
  assert.ok(r.lines.every((l) => measureIn(r.font, l) <= 100));
  // Shrinking makes it fit: 15 characters at 20 px = 150 px, at 17 px = 127.5 px.
  r = T.wrapFit(measureIn, '20px X', 'aaaaaaa bbbbbbb', 130, 1);
  assert.deepEqual(r, { lines: ['aaaaaaa bbbbbbb'], font: '17px X', cut: false });
  // '\n' breaks too; an empty text is one empty line.
  assert.deepEqual(T.wrapFit(measureIn, '20px X', 'a\nb', 100, 3).lines, ['a', 'b']);
  assert.deepEqual(T.wrapFit(measureIn, '20px X', '', 100, 3).lines, ['']);
  assert.deepEqual(T.wrapFit(measureIn, '20px X', 'a b', 100, 0).lines, []);
});

test('drawFit draws what fitLine chose, in its font', () => {
  const calls = [];
  const c = { font: '', measureText(t) { return { width: measureIn(this.font, t) }; }, fillText(t, x, y) { calls.push([t, x, y, this.font]); } };
  const r = T.drawFit(c, 'abcdefghij', 5, 7, 90, '20px X');
  assert.deepEqual(calls, [['abcdefghij', 5, 7, '17px X']]);
  assert.equal(r.font, '17px X');
});

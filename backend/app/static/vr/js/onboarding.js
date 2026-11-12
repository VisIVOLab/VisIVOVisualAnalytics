// The first-run controls guide (epic #166): shown on the headset's message
// card (notice.js) at the first entry into VR, dismissed with Close, recalled
// from the panel's "?" button or the page's "Controls guide" button. Whether
// it was seen is kept in the page's settings (settings.js, guideSeen).
//
// Pure: unit-tested in backend/tests/vr/notice.test.mjs. The controls listed
// here are the ones main.js handleInput() binds; the test checks the two stay
// in step by the button names.

export const GUIDE_PAGES = [
  {
    title: 'Move the cube',
    lines: [
      'Grip (either hand): hold the cube and move or turn it with your hand.',
      'Both grips: pull apart or push together to scale it, twist to turn it.',
      'Put the cube down by letting go: it stays where you leave it.',
      'The box, its ticks and labels are in world coordinates: R.A., Dec. and velocity (km/s).',
    ],
  },
  {
    title: 'Threshold, opacity, channels',
    lines: [
      'Right stick up / down: threshold. Right stick left / right: opacity.',
      'Left stick up / down: move the velocity slab along the spectral axis; left / right: widen or narrow it.',
      'A: next colour map.   B: all channels again (resets the clipping box).',
      'X: emission or maximum intensity.',
    ],
  },
  {
    title: 'Tools and the panel',
    lines: [
      'Right trigger: use the tool where the right ray points (spectrum, Kinematic Lasso, box region); its result is computed on the full cube.',
      'Left trigger: next tool.',
      'The panel (left of the cube): point the right ray at it and pull the trigger; drag its title bar to move it. Left stick click: hide or show it.',
      'Y: the numbers (frame rate, latency) on the left controller.   Right stick click: hold the frame rate on or off.',
      'The "?" on the panel shows this guide again.',
    ],
  },
];

/** The card for page `i` (0-based) of the guide. */
export function guideNotice(i) {
  const n = GUIDE_PAGES.length;
  const k = Math.max(0, Math.min(n - 1, i | 0));
  const p = GUIDE_PAGES[k];
  const actions = [];
  if (k > 0) actions.push({ id: 'guide-back', label: 'Back' });
  if (k < n - 1) actions.push({ id: 'guide-next', label: 'Next' });
  actions.push({ id: 'guide-close', label: k < n - 1 ? 'Skip' : 'Close' });
  return {
    id: 'guide', level: 'guide', title: `${p.title}  (${k + 1}/${n})`, lines: p.lines, actions,
    footer: 'Controls guide: "?" on the panel shows it again.', page: k,
  };
}

/** The guide as plain text, for the 2-D page. */
export function guideText() {
  return GUIDE_PAGES.map((p) => `${p.title}\n${p.lines.map((l) => `• ${l}`).join('\n')}`).join('\n\n');
}

/** Show the guide at this entry into VR? Only the first time, never in the benchmark. */
export function shouldShowGuide(settings, { bench = false } = {}) {
  return !bench && !(settings && settings.guideSeen === true);
}

/** The guide's buttons: the page to go to, or null when it closes. */
export function guideStep(page, action) {
  if (action === 'guide-next') return Math.min(GUIDE_PAGES.length - 1, page + 1);
  if (action === 'guide-back') return Math.max(0, page - 1);
  return null;
}

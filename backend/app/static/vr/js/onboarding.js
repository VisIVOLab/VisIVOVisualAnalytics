// The first-run controls guide (epic #166, rebuilt for issue #201): shown on
// the headset's message card (notice.js) at the first entry into VR,
// dismissed with Close, recalled from the menu's Help → Guide or the page's
// "Controls guide" button. Whether it was seen is kept in the page's settings
// (settings.js, guideSeen). It names the buttons of the hands as they are
// set (left-handed or not), from the same control map (controls.js) the
// input handler and the labels use.
//
// Pure: unit-tested in backend/tests/vr/notice.test.mjs and controls.test.mjs.

import { roles, buttonNames } from './controls.js';

/** The guide's pages for the hands as set. */
export function guidePages({ leftHanded = false } = {}) {
  const r = roles(leftHanded);
  const P = buttonNames(r.primary), S = buttonNames(r.secondary);
  return [
    {
      title: 'Your two hands',
      lines: [
        `Your ${r.primary} hand points and acts: its trigger uses what its ray points at.`,
        `Your ${r.secondary} hand holds the menu. Either grip grabs the cube; both grips move, turn and scale it.`,
        `Turn a wrist towards your eyes, as to read a watch: the ${r.primary} one says what the buttons do now, the ${r.secondary} one shows the cube and the selection's numbers. Left-handed? Menu → Settings.`,
      ],
    },
    {
      title: 'The menu',
      lines: [
        `Hold ${S.lower} (${r.secondary} hand): the menu opens by your wrist. Let go: it closes.`,
        `Point at it with the ${r.primary} ray and pull the trigger. Tabs: Mode, Look, Analysis, Layers, Settings, Help.`,
        'Look has the colour map, threshold, opacity and channels; Help shows the controls of the mode you are in.',
      ],
    },
    {
      title: 'Three modes',
      lines: [
        `${P.lower}: next mode. The mode is on your wrists and next to the cursor; a change is told low in the view.`,
        'Explore: stick ↕ threshold, ↔ opacity. Select: the trigger takes a spectrum, a lasso seed or a box corner.',
        'Clip: the stick moves and widens the velocity slab; in the box tool, hold the trigger to drag a face.',
        `${r.secondary} stick ←/→: the next tool of the mode.   ${P.upper}: undo.   ${S.upper}: reset the view.`,
      ],
    },
    {
      title: 'Hands without controllers',
      lines: [
        'Put the controllers down: a pinch is the trigger, a fist is the grip.',
        `Turn your ${r.secondary} palm up to open the menu; pinch with the other hand to press.`,
        'Results open in windows: point at a title bar and hold the trigger to move one, × closes it.',
      ],
    },
  ];
}

/** The pages for a right-handed user (kept for callers that list them). */
export const GUIDE_PAGES = guidePages();

/** The card for page `i` (0-based) of the guide. */
export function guideNotice(i, opts = {}) {
  const pages = guidePages(opts);
  const n = pages.length;
  const k = Math.max(0, Math.min(n - 1, i | 0));
  const p = pages[k];
  const actions = [];
  if (k > 0) actions.push({ id: 'guide-back', label: 'Back' });
  if (k < n - 1) actions.push({ id: 'guide-next', label: 'Next' });
  actions.push({ id: 'guide-close', label: k < n - 1 ? 'Skip' : 'Close' });
  return {
    id: 'guide', level: 'guide', title: `${p.title}  (${k + 1}/${n})`, lines: p.lines, actions,
    footer: 'Controls guide: the menu\'s Help → Guide shows it again.', page: k,
  };
}

/** The guide as plain text, for the 2-D page. */
export function guideText(opts = {}) {
  return guidePages(opts).map((p) => `${p.title}\n${p.lines.map((l) => `• ${l}`).join('\n')}`).join('\n\n');
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

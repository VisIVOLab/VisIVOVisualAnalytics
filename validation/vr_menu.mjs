// The hand menu driven with the mouse (issues #201, #257). Run by
// vr_lifecycle.mjs with --regressions (as in CI) or --menu, against its
// backend and its synthetic cube, in the page's desktop view with
// ?debug&panel3d: every press is a real mouse event at the widget's place on
// the screen, going through the same ray code the headset's controllers use.
// Checks the five tabs and the modes they set, a colour map swatch and the
// threshold slider with the footer's Undo, More (handedness, the technical
// page under ?debug), Range (the velocity range dragged, a click on the cube
// centring it, Full band), Select (a spectrum in its result window, ×
// closing it; the selection actions hidden without a selection; a lasso
// seed mode), Measure (Pin spectrum, a moment of the whole field with no
// selection); measures the menu's redraw cost and the readout's per-call cost.

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

export async function runMenuChecks({ c, check, waitFor, results = {} }) {
  const menu = 'window.__vrMenu';
  const st = 'window.__vrState';
  async function mouse(type, x, y, buttons = 0) {
    await c.send('Input.dispatchMouseEvent', { type, x, y, button: type === 'mouseMoved' && !buttons ? 'none' : 'left',
      buttons, clickCount: type === 'mouseMoved' ? 0 : 1 });
  }
  async function clickAt(x, y) {
    await mouse('mouseMoved', x, y);
    await mouse('mousePressed', x, y, 1);
    await mouse('mouseReleased', x, y, 0);
    await sleep(60);
  }
  async function press(id) {
    const at = await c.evaluate(`${menu}.pagePixel(${JSON.stringify(id)})`);
    if (!at) throw new Error(`no widget ${id} on the menu's page ${await c.evaluate(`${menu}.ui.page`)}`);
    await clickAt(at[0], at[1]);
  }
  /** A drag along a slider or range widget, from fraction f0 to f1 of its track. */
  async function drag(id, f0, f1, steps = 5) {
    const pts = await c.evaluate(`(() => { const w = ${menu}.layout().find((x) => x.id === ${JSON.stringify(id)}); if (!w) return null;
      const y = w.rect[1] + w.rect[3] / 2, x = (f) => w.rect[0] + 18 + (w.rect[2] - 36) * f;
      return { a: ${menu}.pagePixel(null, [x(${f0}), y]), b: ${menu}.pagePixel(null, [x(${f1}), y]) }; })()`);
    if (!pts) throw new Error(`no widget ${id} on the menu's page`);
    await mouse('mouseMoved', pts.a[0], pts.a[1]);
    await mouse('mousePressed', pts.a[0], pts.a[1], 1);
    for (let i = 1; i <= steps; i++) await mouse('mouseMoved', pts.a[0] + (pts.b[0] - pts.a[0]) * i / steps, pts.a[1], 1);
    await mouse('mouseReleased', pts.b[0], pts.b[1], 0);
    await sleep(60);
  }
  const ui = (expr) => c.evaluate(`(() => { const ui = ${menu}.ui; return ${expr}; })()`);
  const ids = () => c.evaluate(`${menu}.layout().map((w) => w.id)`);

  await waitFor(c, `!!${menu} && ${menu}.layout().length > 0`, 10000);
  const tabs = await c.evaluate(`${menu}.layout().filter((w) => w.id.startsWith('tab:')).map((w) => w.id)`);
  check('menu: drawn in the desktop view with its five tabs (Display, Range, Select, Measure, More)',
    tabs.join(',') === 'tab:display,tab:range,tab:select,tab:measure,tab:more', tabs.join(','));
  const footer = await ids();
  check('menu: the footer (Undo, Reset view, Keep open) and the title bar', ['undo', 'reset-view', 'menu-pin', 'menu-drag'].every((id) => footer.includes(id)));

  // A tab sets the mode; Select's tools and Range's trigger tools.
  await press('tab:select');
  await press('tool:box');
  const sel = await c.evaluate(`({ mode: ${menu}.ui.mode, tool: window.__vrSelection.sel.tool, page: document.getElementById('ui-mode').value })`);
  check('menu: the Select tab sets Select mode, Box region by mouse', sel.mode === 'select' && sel.tool === 'box' && sel.page === 'select', JSON.stringify(sel));
  check('menu: no selection, no Isolate / Remove / Show result cube', !(await ids()).some((id) => ['isolate', 'remove', 'show-applied'].includes(id)));
  await press('tab:range');
  check('menu: the Range tab sets Range (clip) mode', await ui(`ui.mode === 'clip'`));
  await press('sub:clip:box');
  check('menu: the trigger tool Spatial crop', await ui(`ui.mode === 'clip' && ui.clipTool === 'box'`));
  await press('tab:display');
  check('menu: the Display tab sets Explore mode', await ui(`ui.mode === 'explore'`));

  // Display: a colour map swatch, the threshold slider dragged, both undone from the footer.
  const cmap0 = await c.evaluate(`${st}.colormap`);
  const other = await c.evaluate(`${menu}.layout().filter((w) => w.id.startsWith('cmap:')).map((w) => w.id.slice(5)).find((n) => n !== ${st}.colormap)`);
  await press(`cmap:${other}`);
  const cmap1 = await c.evaluate(`${st}.colormap`);
  const thr0 = await c.evaluate(`${st}.thrNorm`);
  await drag('thr', 0.02, 0.6);
  const after = await c.evaluate(`({ thr: ${st}.thrNorm, undo: ${menu}.ui.undo.size })`);
  check('menu: a colour map swatch and the threshold slider follow the mouse', cmap1 === other && cmap1 !== cmap0 && after.thr > thr0 * 1.5,
    JSON.stringify({ cmap0, cmap1, ...after, thr0 }));
  await press('undo');
  const u1 = await c.evaluate(`({ thr: ${st}.thrNorm, cmap: ${st}.colormap })`);
  await press('undo');
  const u2 = await c.evaluate(`({ thr: ${st}.thrNorm, cmap: ${st}.colormap })`);
  check('menu: the footer\'s Undo takes back the drag (one step) and then the colour map', Math.abs(u1.thr - thr0) < 1e-9
    && u1.cmap === cmap1 && u2.cmap === cmap0, JSON.stringify({ u1, u2 }));

  // The catalogue toggle on Display, and its undo (a catalogue answer stood in).
  const cat = await c.evaluate(`(() => {
    const L = window.__vrLayers.state, m = window.__vrMenu;
    L.overlay = { valid: true, name: 'catalogue-check', sources: [], polylines: [], n_sources: 0, warnings: [] };
    const greyed = m.layout().find((w) => w.id === 'ct-show').disabled;
    m.act({ type: 'press', id: 'ct-show' });
    const off = L.s.catShow;
    m.act({ type: 'press', id: 'undo' });
    const back = L.s.catShow;
    L.overlay = null;
    return { greyed, off, back };
  })()`);
  check('menu: Display → Catalogue Overlay toggles, and Undo puts it back', !cat.greyed && cat.off === false && cat.back === true, JSON.stringify(cat));

  // More: handedness, kept in this browser and shown on the page; the technical page (?debug).
  await press('tab:more');
  await press('hand:left');
  const lh = await c.evaluate(`({ s: ${menu}.settings().leftHanded, box: document.getElementById('left-handed').checked,
    menu: ${menu}.controlMap().roles.secondary, stored: JSON.parse(localStorage.getItem('visivo-vr-settings')).s.leftHanded })`);
  check('menu: More → Left-handed swaps the roles and is kept', lh.s && lh.box && lh.menu === 'right' && lh.stored, JSON.stringify(lh));
  await press('hand:right');
  check('menu: and back', !(await c.evaluate(`${menu}.settings().leftHanded`)));
  const more = await ids();
  check('menu: More holds no technical setting, only the way to them under ?debug',
    more.includes('debug-page') && !more.some((id) => /^set:(governor|refine|record)|^thrsrc:/.test(id)), more.filter((id) => !id.startsWith('tab:')).join(','));
  await press('debug-page');
  const dbg = await c.evaluate(`({ page: ${menu}.ui.page, ids: ${menu}.layout().map((w) => w.id), src: document.getElementById('thr-source').value })`);
  check('menu: the technical page (?debug): governor, refinement, recording, threshold source; the threshold from the full cube',
    dbg.page === 'debug' && ['set:governor', 'set:refine', 'set:refineDepth', 'set:refineScale', 'set:record', 'thrsrc:cube'].every((id) => dbg.ids.includes(id)),
    JSON.stringify({ page: dbg.page, src: dbg.src }));
  await press('debug-back');

  // Range: the velocity range dragged; then a click on the cube centres it on the emission.
  await press('tab:range');
  await press('sub:clip:slab');
  await drag('slab', 1, 0.2, 4);
  const narrowed = await c.evaluate(`[${st}.clip.min[2], ${st}.clip.max[2]]`);
  // The source sits at the cube's centre (validation's synthetic cube): the ray through it.
  const centre = await c.evaluate(`window.__vrProject([0.5, 0.47, 0.5])`);
  await clickAt(centre[0], centre[1]);
  const slab = await c.evaluate(`[${st}.clip.min[2], ${st}.clip.max[2]]`);
  const c1 = (slab[0] + slab[1]) / 2, w0 = narrowed[1] - narrowed[0], w1 = slab[1] - slab[0];
  check('menu: the velocity range drags; in Range a click centres it on the emission, same width',
    narrowed[1] < 0.35 && c1 > 0.35 && c1 < 0.65 && Math.abs(w1 - w0) < 0.02, JSON.stringify({ narrowed, slab }));
  await drag('crop:0', 0, 0.3, 3);
  const crop = await c.evaluate(`[${st}.clip.min[0], ${st}.clip.max[0]]`);
  check('menu: the spatial crop on x drags', crop[0] > 0.2 && crop[1] === 1, JSON.stringify(crop));
  await press('crop-reset');
  await press('slab-all');
  check('menu: Reset crop and Full band', await c.evaluate(`${st}.clip.min.every((v) => v === 0) && ${st}.clip.max.every((v) => v === 1)`));

  // Select, Spectrum probe: a click on the cube opens the result window; × closes it.
  await press('tab:select');
  await press('tool:probe');
  await clickAt(centre[0], centre[1]);
  const spec = await waitFor(c, `!!window.__vrSelection.sel.spectrum || !!window.__vrSelection.sel.error`, 30000);
  await sleep(100);
  const shown = await c.evaluate(`({ spectrum: !!window.__vrSelection.sel.spectrum, err: window.__vrSelection.sel.error, win: !!${menu}.wins.result.model })`);
  check('menu: in Select a click takes the spectrum, in a result window', spec.value && shown.spectrum && shown.win, JSON.stringify(shown));
  const x = await c.evaluate(`${menu}.windowPixel('result', 'close')`);
  if (x) await clickAt(x[0], x[1]);
  await sleep(100);
  check('menu: × closes the result window', x && !(await c.evaluate(`${menu}.wins.result.model`)), JSON.stringify(x));
  await press('seed:add');
  check('menu: Lasso: Add picks the Kinematic Lasso and its seed mode',
    (await c.evaluate(`[window.__vrSelection.sel.tool, window.__vrSelection.sel.seedMode].join()`)) === 'lasso,add');
  await press('seed:new');
  await press('tool:probe');

  // Measure: the spectrum pinned; a moment with no selection is of the whole field.
  await press('tab:measure');
  check('menu: the Measure tab keeps the trigger harmless (Explore mode)', await ui(`ui.mode === 'explore'`));
  await press('spec-pin');
  check('menu: Pin spectrum', await c.evaluate('window.__vrSelection.hasPin()'));
  await press('spec-pin');
  await press('moment:0');
  const mom = await waitFor(c, `!!(window.__vrAnalysis.moment.state.result || window.__vrAnalysis.moment.state.error)`, 60000);
  const m = await c.evaluate(`(() => { const s = window.__vrAnalysis.moment.state; return { whole: !!(s.result && s.result.whole), err: s.error,
    req: s.result && s.result.req, win: !!${menu}.wins.moment.model }; })()`);
  check('menu: Intensity (M0) with no selection: the whole field over the velocity range, automatic mask, in its window',
    mom.value && m.whole && m.req && m.req.mask_enabled && m.req.threshold_auto && m.win, JSON.stringify(m));
  await press('moment-hide');

  // Costs: the menu's redraw (each page) and the readout's text.
  const bench = {};
  for (const page of ['display', 'range', 'select', 'measure', 'more']) {
    await c.evaluate(`${menu}.ui.page = ${JSON.stringify(page)}`);
    bench[page] = await c.evaluate(`${menu}.bench(40)`);
  }
  const ro = await c.evaluate(`${menu}.readout([0.5, 0.47, 0.5], 2000)`);
  results.menu = { bench, readout: ro };
  const worst = Math.max(...Object.values(bench).map((b) => b.draw.p50));
  check('menu: a page redraws in under 4 ms (median, CPU)', worst < 4,
    Object.entries(bench).map(([k, b]) => `${k} ${b.draw.p50.toFixed(2)}+${b.upload.p50.toFixed(2)} ms`).join(', '));
  check('readout: under 50 µs a call', ro.usPerCall < 50, `${ro.usPerCall.toFixed(1)} µs: ${ro.lines.join(' | ')}`);
  await c.evaluate(`${menu}.ui.page = 'display'`);
  await press('tab:display');
}

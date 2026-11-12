// The hand menu driven with the mouse (issue #201). Run by vr_lifecycle.mjs
// with --regressions (as in CI) or --menu, against its backend and its
// synthetic cube, in the page's desktop view with ?panel3d: every press is a
// real mouse event at the widget's place on the screen, going through the
// same ray code the headset's controllers use. Checks the tabs, the modes and
// tools, a Look slider and its undo, the settings (left-handed), the Help
// page, Clip's slab moved by a click on the cube, a Select spectrum opening
// its result window and × closing it; measures the menu's redraw cost and the
// readout's per-call cost.

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
  const ui = (expr) => c.evaluate(`(() => { const ui = ${menu}.ui; return ${expr}; })()`);

  await waitFor(c, `!!${menu} && ${menu}.layout().length > 0`, 10000);
  check('menu: drawn in the desktop view with its six tabs',
    (await c.evaluate(`${menu}.layout().filter((w) => w.id.startsWith('tab:')).length`)) === 6);

  // Modes and tools from the Mode page.
  await press('tab:mode');
  await press('mode:select');
  await press('sub:select:box');
  const sel = await c.evaluate(`({ mode: ${menu}.ui.mode, tool: window.__vrSelection.sel.tool, page: document.getElementById('ui-mode').value })`);
  check('menu: Select mode and its box tool by mouse', sel.mode === 'select' && sel.tool === 'box' && sel.page === 'select', JSON.stringify(sel));
  await press('sub:clip:box');
  check('menu: a Clip tool switches to Clip', await ui(`ui.mode === 'clip' && ui.clipTool === 'box'`));

  // The Look page: a colour map step, a slider dragged, both undone.
  await press('tab:look');
  const cmap0 = await c.evaluate(`${st}.colormap`);
  await press('cmap-next');
  const cmap1 = await c.evaluate(`${st}.colormap`);
  const thr = await c.evaluate(`(() => { const w = ${menu}.layout().find((x) => x.id === 'thr'); const y = w.rect[1] + w.rect[3] / 2;
    return { a: ${menu}.pagePixel(null, [w.rect[0] + 30, y]), b: ${menu}.pagePixel(null, [w.rect[0] + w.rect[2] * 0.6, y]), thr: ${st}.thrNorm }; })()`);
  await mouse('mouseMoved', thr.a[0], thr.a[1]);
  await mouse('mousePressed', thr.a[0], thr.a[1], 1);
  for (let i = 1; i <= 5; i++) await mouse('mouseMoved', thr.a[0] + (thr.b[0] - thr.a[0]) * i / 5, thr.a[1], 1);
  await mouse('mouseReleased', thr.b[0], thr.b[1], 0);
  await sleep(60);
  const after = await c.evaluate(`({ thr: ${st}.thrNorm, undo: ${menu}.ui.undo.size })`);
  check('menu: the colour map steps and the threshold slider follows a drag', cmap1 !== cmap0 && after.thr > thr.thr * 1.5,
    JSON.stringify({ cmap0, cmap1, ...after, thr0: thr.thr }));
  await press('tab:mode');
  await press('undo');
  const u1 = await c.evaluate(`({ thr: ${st}.thrNorm, cmap: ${st}.colormap })`);
  await press('undo');
  const u2 = await c.evaluate(`({ thr: ${st}.thrNorm, cmap: ${st}.colormap })`);
  check('menu: Undo takes back the drag (one step) and then the colour map', Math.abs(u1.thr - thr.thr) < 1e-9
    && u1.cmap === cmap1 && u2.cmap === cmap0, JSON.stringify({ u1, u2 }));

  // Settings: left-handed, kept in this browser and shown on the page.
  await press('tab:settings');
  await press('set:leftHanded');
  const lh = await c.evaluate(`({ s: ${menu}.settings().leftHanded, box: document.getElementById('left-handed').checked,
    menu: ${menu}.controlMap().roles.secondary, stored: JSON.parse(localStorage.getItem('visivo-vr-settings')).s.leftHanded })`);
  check('menu: Settings → left-handed swaps the roles and is kept', lh.s && lh.box && lh.menu === 'right' && lh.stored, JSON.stringify(lh));
  await press('set:leftHanded');
  check('menu: and back', !(await c.evaluate(`${menu}.settings().leftHanded`)));

  // Help: the control map of another mode.
  await press('tab:help');
  await press('help-mode:clip');
  const help = await c.evaluate(`${menu}.view().help`);
  check('menu: Help shows the chosen mode\'s controls', help.helpMode === 'clip'
    && help.helpLines.some((l) => /slab position/.test(Array.isArray(l) ? l[0] : l)), JSON.stringify(help.helpLines.slice(0, 2)).slice(0, 160));

  // Clip, slab: narrow it on the Look page's channel range, then a click on the cube centres it on the emission.
  await press('tab:look');
  const rg = await c.evaluate(`(() => { const w = ${menu}.layout().find((x) => x.id === 'slab'); const y = w.rect[1] + w.rect[3] / 2;
    return { a: ${menu}.pagePixel(null, [w.rect[0] + w.rect[2] - 20, y]), b: ${menu}.pagePixel(null, [w.rect[0] + 18 + (w.rect[2] - 36) * 0.2, y]) }; })()`);
  await mouse('mouseMoved', rg.a[0], rg.a[1]);
  await mouse('mousePressed', rg.a[0], rg.a[1], 1);
  for (let i = 1; i <= 4; i++) await mouse('mouseMoved', rg.a[0] + (rg.b[0] - rg.a[0]) * i / 4, rg.a[1], 1);
  await mouse('mouseReleased', rg.b[0], rg.b[1], 0);
  await sleep(60);
  const narrowed = await c.evaluate(`[${st}.clip.min[2], ${st}.clip.max[2]]`);
  await press('tab:mode');
  await press('sub:clip:slab');
  // The source sits at the cube's centre (validation's synthetic cube): the ray through it.
  const centre = await c.evaluate(`window.__vrProject([0.5, 0.47, 0.5])`);
  await clickAt(centre[0], centre[1]);
  const slab = await c.evaluate(`[${st}.clip.min[2], ${st}.clip.max[2]]`);
  const c1 = (slab[0] + slab[1]) / 2, w0 = narrowed[1] - narrowed[0], w1 = slab[1] - slab[0];
  check('menu: the channel range drags; in Clip a click centres the slab on the emission, same width',
    narrowed[1] < 0.35 && c1 > 0.35 && c1 < 0.65 && Math.abs(w1 - w0) < 0.02, JSON.stringify({ narrowed, slab }));
  await press('clip-all');
  check('menu: All channels', await c.evaluate(`${st}.clip.min[2] === 0 && ${st}.clip.max[2] === 1`));

  // Select, spectrum: a click on the cube opens the result window; × closes it.
  await press('sub:select:probe');
  await clickAt(centre[0], centre[1]);
  const spec = await waitFor(c, `!!window.__vrSelection.sel.spectrum || !!window.__vrSelection.sel.error`, 30000);
  await sleep(100);
  const shown = await c.evaluate(`({ spectrum: !!window.__vrSelection.sel.spectrum, err: window.__vrSelection.sel.error, win: !!${menu}.wins.result.model })`);
  check('menu: in Select a click takes the spectrum, in a result window', spec.value && shown.spectrum && shown.win, JSON.stringify(shown));
  const x = await c.evaluate(`${menu}.windowPixel('result', 'close')`);
  if (x) await clickAt(x[0], x[1]);
  await sleep(100);
  check('menu: × closes the result window', x && !(await c.evaluate(`${menu}.wins.result.model`)), JSON.stringify(x));

  // The Analysis page: a lasso seed mode.
  await press('tab:analysis');
  await press('seed:add');
  check('menu: Analysis → add seed', (await c.evaluate(`window.__vrSelection.sel.seedMode`)) === 'add');
  await press('seed:new');

  // Costs: the menu's redraw (each page) and the readout's text.
  const bench = {};
  for (const page of ['mode', 'look', 'analysis', 'settings', 'help']) {
    await c.evaluate(`${menu}.ui.page = ${JSON.stringify(page)}`);
    bench[page] = await c.evaluate(`${menu}.bench(40)`);
  }
  const ro = await c.evaluate(`${menu}.readout([0.5, 0.47, 0.5], 2000)`);
  results.menu = { bench, readout: ro };
  const worst = Math.max(...Object.values(bench).map((b) => b.draw.p50));
  check('menu: a page redraws in under 4 ms (median, CPU)', worst < 4,
    Object.entries(bench).map(([k, b]) => `${k} ${b.draw.p50.toFixed(2)}+${b.upload.p50.toFixed(2)} ms`).join(', '));
  check('readout: under 50 µs a call', ro.usPerCall < 50, `${ro.usPerCall.toFixed(1)} µs: ${ro.lines.join(' | ')}`);
  await press('tab:mode');
  await press('mode:explore');
}

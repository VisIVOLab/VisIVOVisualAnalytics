// The debug cuts of the VR page (issue #229), run by vr_lifecycle.mjs
// --regressions (and --xrfake alone) through the WebXR stand-in (XR_FAKE).
//
// On the Quest the first Kinematic Lasso loses the WebGL context while the
// selection is shown and the view or the controller moves. ?debug&nohover,
// &nohud, &noreadout, &noselpanel, &noseloutline and &novolume each leave
// one per-frame path out, so that each run on the headset is "reload, click".
// Here, for no cut, each cut alone and all of them together: the page loads
// with no error, a lasso made with the primary trigger in VR completes
// (mask, statistics, recipe), the controller then moves over the selection
// for a while with frames still coming and no context loss, and the
// diagnostics name the cuts. With no cut: the results panel is not painted
// again while the controller holds still (the redraw loop of #229).

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

export const CUTS = ['nohover', 'nohud', 'noreadout', 'noselpanel', 'noseloutline', 'novolume'];

export async function runCutChecks({ c, check, waitFor, base, token, cube }) {
  const say = (m) => console.log(`     cuts: ${m}`);
  const variants = [[], ...CUTS.map((n) => [n]), CUTS];
  for (const cuts of variants) {
    const label = cuts.length === CUTS.length ? 'all cuts' : cuts.length ? cuts[0] : 'no cut';
    const errs0 = c.pageErrors.length;
    const url = `${base}/vr/?debug&refine=0&xrfake${cuts.map((n) => `&${n}`).join('')}#token=${encodeURIComponent(token)}`;
    await c.send('Page.navigate', { url });
    const up = await waitFor(c, `!!window.__vrShell && window.__vrShell.life.state === 'ok'`, 30000);
    if (!up.value) { check(`cuts ${label}: page up`, false); continue; }
    await c.evaluate(`(() => { const $ = (id) => document.getElementById(id); $('path').value = ${JSON.stringify(cube)}; $('load').click(); })()`);
    const ok = await waitFor(c, `!!(window.__vrState.open && window.__vrState.cube && /upload/.test(document.getElementById('load-status').textContent))`, 60000);
    if (!ok.value) { check(`cuts ${label}: cube loaded`, false); continue; }
    // VR, Select mode, the Kinematic Lasso, the primary (right) trigger on the emission.
    await c.evaluate(`(async () => { const b = document.getElementById('enter-vr');
      for (let i = 0; i < 50 && b.disabled; i++) await new Promise((r) => setTimeout(r, 50)); b.click(); })()`);
    const running = await waitFor(c, 'window.__xrfake.frames > 20 && !!window.__vrXR()', 15000);
    if (!running.value) { check(`cuts ${label}: VR session draws frames`, false); continue; }
    await c.evaluate(`(() => { window.__vrMenu.ui.mode = 'select'; window.__vrSelection.setTool('lasso'); })()`);
    const f0 = await c.evaluate('window.__xrfake.frames');
    await c.evaluate('window.__xrfake.buttons.right[0].pressed = true');
    await waitFor(c, `window.__xrfake.frames > ${f0 + 2}`, 5000);
    await c.evaluate('window.__xrfake.buttons.right[0].pressed = false');
    const done = await waitFor(c, '!!(window.__vrSelection.sel.lasso || window.__vrSelection.sel.error) && !window.__vrSelection.sel.busy', 30000);
    const res = await c.evaluate(`(() => { const s = window.__vrSelection.sel; return { lasso: !!s.lasso, count: s.lasso && s.lasso.count,
      stats: !!(s.lasso && s.lasso.stats), recipes: s.recipes.length, error: s.error, maskBytes: window.__vrGpu().maskBytes }; })()`);
    check(`cuts ${label}: the trigger's lasso completes (mask, statistics, recipe)`,
      done.value && res.lasso && res.stats && res.recipes === 1 && res.maskBytes > 0,
      JSON.stringify(res));
    // Still: the panel is not painted again (no cut: the redraw loop of #229).
    await sleep(300);
    const p0 = await c.evaluate('window.__vrSelection.panelCounts()');
    const s0 = await c.evaluate('window.__xrfake.frames');
    await sleep(1500);
    const p1 = await c.evaluate('window.__vrSelection.panelCounts()');
    const s1 = await c.evaluate('window.__xrfake.frames');
    say(`${label}: still for ${s1 - s0} frames: ${p1.paints - p0.paints} panel paints, ${p1.uploads - p0.uploads} panel uploads (${p1.paints} / ${p1.uploads} in all)`);
    check(`cuts ${label}: a still controller repaints no panel`, p1.paints === p0.paints && p1.uploads === p0.uploads,
      `${p1.paints - p0.paints} paints, ${p1.uploads - p0.uploads} uploads over ${s1 - s0} frames`);
    if (cuts.includes('noselpanel')) check(`cuts ${label}: no panel painted at all`, p1.paints === 0 && p1.uploads === 0, JSON.stringify(p1));
    // Moving: the aim sweeps over the selection for 2 s (or 10 frames under software GL).
    const m0 = await c.evaluate(`(() => { const S = window.__xrfake; const a0 = [...S.aimRight]; let k = 0;
      window.__cutSweep = setInterval(() => { k++; S.aimRight = [a0[0] + 0.06 * Math.sin(k / 3), a0[1] + 0.04 * Math.cos(k / 4), a0[2]]; }, 16);
      window.__cutAim0 = a0; return S.frames; })()`);
    let m1 = m0;
    for (const t0 = Date.now(); Date.now() - t0 < 6000 && (m1 < m0 + 10 || Date.now() - t0 < 2000);) {
      await sleep(250);
      m1 = await c.evaluate('window.__xrfake.frames');
    }
    await c.evaluate('clearInterval(window.__cutSweep); window.__xrfake.aimRight = window.__cutAim0');
    const after = await c.evaluate(`({ lost: window.__vrGL.isContextLost(), lasso: !!window.__vrSelection.sel.lasso,
      hover: !!window.__vrSelection.sel.hover, diag: window.__vrShell.diagnostics(), hud: document.getElementById('hud-text').textContent,
      cut: window.__vrCuts() })`);
    // Each cut leaves its own path out (zero), the control run does it all (non-zero).
    const n = after.cut.counts;
    say(`${label}: ${JSON.stringify(n)}`);
    const paths = { novolume: ['volumeDraws'], noseloutline: ['selLineDraws', 'selLineSegments'], nohud: ['hudUploads', 'hudQuads'],
      noreadout: ['readoutCards'], nohover: ['hoverPicks'] };
    for (const [name, keys] of Object.entries(paths)) {
      const off = cuts.includes(name);
      if (!off && name === 'noreadout' && cuts.includes('nohover')) continue;   // no hover: no live readout to show
      check(`cuts ${label}: ${keys.join(', ')} ${off ? 'none' : 'happen'}`, keys.every((k) => (off ? n[k] === 0 : n[k] > 0)),
        keys.map((k) => `${k} ${n[k]}`).join(', '));
    }
    check(`cuts ${label}: frames keep coming while the controller moves over the selection`, m1 >= m0 + 10 && !after.lost && after.lasso,
      `${m1 - m0} frames, context ${after.lost ? 'LOST' : 'kept'}, lasso ${after.lasso ? 'kept' : 'gone'}`);
    if (cuts.includes('nohover')) check(`cuts ${label}: no hover`, !after.hover);
    const cutLine = after.diag.split('\n').find((l) => l.startsWith('debug cuts:'));
    check(`cuts ${label}: the diagnostics name the cuts`, cuts.length ? cutLine === `debug cuts: ${cuts.join(' ')}` : !cutLine, cutLine || '(none)');
    if (cuts.length && !cuts.includes('nohud')) {
      check(`cuts ${label}: the HUD's first line names the cuts`, after.hud.split('\n')[0] === `debug cuts: ${cuts.join(' ')}`, after.hud.split('\n')[0]);
    }
    await c.evaluate('window.__vrShell.act("exit-vr")');
    await waitFor(c, '!window.__vrXR()', 5000);
    const errs = await c.evaluate(`window.__vrShell.diag.items.filter((e) => e.level === 'error'
      && !(e.event === 'request-failed' && e.message.includes('/v1/render/'))).map((e) => e.event + ': ' + e.message)`);
    const pe = c.pageErrors.slice(errs0);
    check(`cuts ${label}: no page error`, !errs.length && !pe.length, [...errs, ...pe].join(' / '));
  }
  // The switches without ?debug do nothing: the page's numbers are written and
  // name no cut (no debug hooks on such a page: read from the page itself).
  const url = `${base}/vr/?refine=0&xrfake${CUTS.map((n) => `&${n}`).join('')}#token=${encodeURIComponent(token)}`;
  await c.send('Page.navigate', { url });
  const up = await waitFor(c, `document.readyState === 'complete' && !!document.getElementById('enter-vr')`, 30000);
  await c.evaluate(`(() => { const $ = (id) => document.getElementById(id); $('path').value = ${JSON.stringify(cube)}; $('load').click(); })()`);
  const loaded = await waitFor(c, `/upload/.test(document.getElementById('load-status').textContent)`, 60000);
  await c.evaluate(`(async () => { const b = document.getElementById('enter-vr');
    for (let i = 0; i < 100 && b.disabled; i++) await new Promise((r) => setTimeout(r, 50)); b.click(); })()`);
  const hud = await waitFor(c, `window.__xrfake.frames > 20 && /^FPS /.test(document.getElementById('hud-text').textContent)`, 15000);
  const first = await c.evaluate(`document.getElementById('hud-text').textContent.split('\\n')[0]`);
  check('cuts without ?debug: the switches do nothing (the page numbers written, no cut named)',
    up.value && loaded.value && hud.value && /^FPS /.test(first), first);
  // Back to the page the checks after these expect (the one before them): ?debug
  // for the hooks, the WebXR stand-in for Enter VR, the cube loaded.
  await c.send('Page.navigate', { url: `${base}/vr/?debug&refine=0&xrfake=nolayers#token=${encodeURIComponent(token)}` });
  await waitFor(c, `!!window.__vrShell && window.__vrShell.life.state === 'ok'`, 30000);
  await c.evaluate(`(() => { const $ = (id) => document.getElementById(id); $('path').value = ${JSON.stringify(cube)}; $('load').click(); })()`);
  await waitFor(c, `!!(window.__vrState.open && window.__vrState.cube && /upload/.test(document.getElementById('load-status').textContent))`, 60000);
}

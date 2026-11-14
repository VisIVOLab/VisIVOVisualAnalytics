// No 3-D texture allocated during a VR session (issue #229), run by
// vr_lifecycle.mjs --regressions (and --xrfake alone) through the WebXR
// stand-in (XR_FAKE).
//
// On a Quest 3 a texImage3D issued while the immersive session runs makes
// the GPU process's command decoder fail (kTexImage3D, OnParseError): the
// context is lost and the GPU process restarts. The page now allocates every
// 3-D texture on the 2-D page. This drives, in one VR session, three Kinematic
// Lassos, an Isolate shown in the headset, a cube of another dataset, a load
// at another binning and ten more switches, and counts, at
// WebGL2RenderingContext.prototype (installed before the page's scripts:
// independent of the page's own guard), every 3-D allocation that reached the
// GPU while the session ran: there must be none. Since issue #235 those cubes
// are shown at once, written into the fixed capacity allocated before the
// session; only a cube larger than it waits for the session's end. The image
// of a cube in a corner of the capacity is compared with the same cube in
// storage of its own size. Then the headset benchmark: prepared on the 2-D
// page, run in VR with no allocation either.

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/** Installed with Page.addScriptToEvaluateOnNewDocument: texture calls that
 *  reached the GPU, split by whether the page's VR session was running. */
export const TEX3D_PROBE = `(() => {
  if (window.__tex3dProbe || typeof WebGL2RenderingContext === 'undefined') return;
  const P = WebGL2RenderingContext.prototype;
  const n = { inXR: {}, outXR: {} };
  const live = () => { try { return !!(window.__vrTex3D && window.__vrTex3D().xrActive); } catch { return false; } };
  for (const name of ['texImage3D', 'texStorage3D', 'texSubImage3D', 'texImage2D', 'texStorage2D']) {
    const f = P[name];
    if (!f) continue;
    P[name] = function (...a) {
      const k = live() ? 'inXR' : 'outXR';
      n[k][name] = (n[k][name] || 0) + 1;
      return f.apply(this, a);
    };
  }
  window.__tex3dProbe = () => JSON.parse(JSON.stringify(n));
})();`;

const allocsIn = (p) => (p.inXR.texImage3D || 0) + (p.inXR.texStorage3D || 0);

export async function runTex3DChecks({ c, check, waitFor, base, token, cube, other }) {
  const say = (m) => console.log(`     tex3d: ${m}`);
  const have = await c.evaluate('typeof window.__tex3dProbe === "function"');
  if (!have) { check('tex3d: the probe is installed', false); return; }
  const url = `${base}/vr/?debug&refine=0&xrfake&benchms=200#token=${encodeURIComponent(token)}`;
  // 8-bit textures throughout (the capacity's texture follows the page's choice and the cubes').
  await c.send('Page.navigate', { url });
  const up = await waitFor(c, `!!window.__vrShell && window.__vrShell.life.state === 'ok' && typeof window.__tex3dProbe === 'function'`, 30000);
  if (!up.value) { check('tex3d: page up', false); return; }
  const errs0 = c.pageErrors.length;
  await c.evaluate(`(() => { const $ = (id) => document.getElementById(id); $('encoding').value = 'uint8'; $('fxy').value = '1'; $('path').value = ${JSON.stringify(cube)}; $('load').click(); })()`);
  const ok = await waitFor(c, `!!(window.__vrState.open && window.__vrState.cube && /upload/.test(document.getElementById('load-status').textContent))`, 60000);
  if (!ok.value) { check('tex3d: cube loaded', false); return; }
  const factors0 = await c.evaluate('window.__vrState.cube.factors.join()');
  const p0 = await c.evaluate('window.__tex3dProbe()');
  // The placeholder mask and the capacity (two banks of cube and skip grid, one mask): the cube went into it.
  const capIn = await c.evaluate('window.__vrTex3D().capacity');
  check('tex3d: the cube\'s textures are immutable storage, no texImage3D at all; the cube is in the capacity',
    !(p0.outXR.texImage3D) && p0.outXR.texStorage3D === 6 && !!capIn && capIn.onShow,
    JSON.stringify({ calls: p0.outXR, capacity: capIn }));

  // ── One VR session: lassos, an Isolate shown, a load at another binning ──
  await c.evaluate(`(async () => { const b = document.getElementById('enter-vr');
    for (let i = 0; i < 50 && b.disabled; i++) await new Promise((r) => setTimeout(r, 50)); b.click(); })()`);
  const running = await waitFor(c, 'window.__xrfake.frames > 20 && !!window.__vrXR() && window.__vrTex3D().xrActive', 15000);
  if (!running.value) { check('tex3d: VR session draws frames', false); return; }
  await c.evaluate(`(() => { window.__vrMenu.ui.mode = 'select'; window.__vrSelection.setTool('lasso'); })()`);
  let lassos = 0;
  const aims = [[0, 1.48, -0.75], [0.01, 1.48, -0.75], [-0.01, 1.47, -0.75]];
  for (let i = 0; i < 3; i++) {
    await c.evaluate(`(() => { window.__t3prev = window.__vrSelection.sel.lasso; window.__xrfake.aimRight = ${JSON.stringify(aims[i])}; })()`);
    await sleep(150);
    const f0 = await c.evaluate('window.__xrfake.frames');
    await c.evaluate('window.__xrfake.buttons.right[0].pressed = true');
    await waitFor(c, `window.__xrfake.frames > ${f0 + 2}`, 5000);
    await c.evaluate('window.__xrfake.buttons.right[0].pressed = false');
    const done = await waitFor(c, `(() => { const s = window.__vrSelection.sel; return !s.busy && ((s.lasso && s.lasso !== window.__t3prev) || !!s.error); })()`, 30000);
    const r = await c.evaluate(`(() => { const s = window.__vrSelection.sel; return { lasso: !!s.lasso && s.lasso !== window.__t3prev, count: s.lasso && s.lasso.count, error: s.error }; })()`);
    if (done.value && r.lasso) lassos++;
    else say(`lasso ${i + 1}: ${JSON.stringify(r)}`);
    await sleep(300);
  }
  check('tex3d: three lassos in VR complete', lassos === 3, `${lassos} of 3`);
  // Issue #235: every cube that fits the capacity is shown at once in VR,
  // written into the bank not on show with texSubImage3D only.
  const cap0 = await c.evaluate('window.__vrTex3D().capacity');
  check('tex3d: the capacity is allocated before the session', !!cap0 && cap0.onShow && cap0.dims.every((v) => v >= 64),
    JSON.stringify(cap0));
  const switched = [];
  const shownNow = async (label, expr) => {
    const t0 = Date.now();
    const ok = await waitFor(c, expr, 60000);
    const r = await c.evaluate(`({ t: window.__vrTex3D(), notice: (window.__vrShell.notices.current() || {}).id, dims: window.__vrState.cube.dims.join('x'),
      factors: window.__vrState.cube.factors.join(), path: window.__vrState.openPath, frames: window.__xrfake.frames })`);
    const f1 = r.frames;
    await sleep(400);
    const f2 = await c.evaluate('window.__xrfake.frames');
    switched.push(`${label} ${r.dims} in ${Date.now() - t0} ms`);
    return { ok: ok.value && !r.t.staged && r.notice !== 'cube-waiting' && r.t.xrActive && f2 > f1, r };
  };
  // Isolate, then show its cube in the headset: at once.
  await c.evaluate(`window.__vrAnalysis.press('isolate')`);
  const iso = await waitFor(c, `!!window.__vrAnalysis.lasso.state.applied && !window.__vrAnalysis.lasso.state.busy`, 60000);
  await c.evaluate(`window.__vrAnalysis.press('show-applied')`);
  const s1 = await shownNow('isolate', `window.__vrState.loading === 0 && window.__vrState.openPath !== ${JSON.stringify(cube)} && !!window.__vrState.openPath`);
  check('tex3d: Show that cube after an Isolate in VR shows it at once (no waiting notice)', iso.value && s1.ok, JSON.stringify({ path: s1.r.path.split('/').pop(), dims: s1.r.dims, staged: s1.r.t.staged, notice: s1.r.notice }));
  // A cube from another dataset (another file), loaded from the page's fields as from the desktop.
  await c.evaluate(`(() => { const $ = (id) => document.getElementById(id); $('fxy').value = '1'; $('path').value = ${JSON.stringify(other)}; $('load').click(); })()`);
  const s2 = await shownNow('other dataset', `window.__vrState.loading === 0 && window.__vrState.openPath === ${JSON.stringify(other)}`);
  check('tex3d: a cube from another dataset loaded in VR is shown at once', s2.ok, JSON.stringify({ dims: s2.r.dims, bank: s2.r.t.capacity && s2.r.t.capacity.bank }));
  // A cube larger than the capacity still waits for the end of the session, with its notice.
  const bigSide = cap0.dims[0] < 384 ? 384 : null;
  if (bigSide) {
    await c.evaluate(`(() => { document.getElementById('synthetic-size').value = '${bigSide}'; document.getElementById('synthetic').click(); })()`);
    const staged = await waitFor(c, `!!window.__vrTex3D().staged`, 30000);
    const st = await c.evaluate(`({ t: window.__vrTex3D(), notice: (window.__vrShell.notices.current() || {}).id, path: window.__vrState.openPath,
      lines: ((window.__vrShell.notices.current() || {}).lines || []).join(' ') })`);
    check('tex3d: a cube larger than the capacity waits for the end of the session, with a notice saying why',
      staged.value && st.notice === 'cube-waiting' && st.path === other && /larger than the room kept/.test(st.lines),
      JSON.stringify({ staged: st.t.staged, notice: st.notice, why: st.lines.slice(0, 120) }));
  }
  // A load at another binning: at once too (and newer than the waiting cube, which is dropped).
  await c.evaluate(`(() => { const $ = (id) => document.getElementById(id); $('fxy').value = '2'; $('path').value = ${JSON.stringify(cube)}; $('load').click(); })()`);
  const s3 = await shownNow('binning 2', `window.__vrState.loading === 0 && window.__vrState.cube.factors[0] === 2 && window.__vrState.openPath === ${JSON.stringify(cube)}`);
  check('tex3d: a load at another binning in VR is shown at once', s3.ok, JSON.stringify({ factors: s3.r.factors, dims: s3.r.dims, staged: s3.r.t.staged }));
  // Ten more switches in the same session (the Quest check of #185, here on the fake).
  let tenOk = 0;
  for (let i = 0; i < 10; i++) {
    const f = i % 2 ? '1' : '2';
    await c.evaluate(`(() => { const $ = (id) => document.getElementById(id); $('fxy').value = '${f}'; $('path').value = ${JSON.stringify(i % 3 === 2 ? other : cube)}; $('load').click(); })()`);
    const s = await shownNow(`switch ${i + 1}`, `window.__vrState.loading === 0 && window.__vrState.cube.factors[0] === ${f} && window.__vrState.openPath === ${JSON.stringify(i % 3 === 2 ? other : cube)}`);
    if (s.ok) tenOk++;
  }
  check('tex3d: ten more cube switches in one session, each shown at once', tenOk === 10, `${tenOk} of 10`);
  // Back to the first cube at binning 2 (what the checks below expect).
  await c.evaluate(`(() => { const $ = (id) => document.getElementById(id); $('fxy').value = '2'; $('path').value = ${JSON.stringify(cube)}; $('load').click(); })()`);
  await shownNow('binning 2 again', `window.__vrState.loading === 0 && window.__vrState.cube.factors[0] === 2 && window.__vrState.openPath === ${JSON.stringify(cube)}`);
  say(`switches in VR (as polled): ${switched.join('; ')}`);
  const written = await c.evaluate(`window.__vrShell.diag.items.filter((e) => e.event === 'cube-switched-in-vr').map((e) => e.message)`);
  say(`written into the capacity in VR: ${written.join('; ')}`);
  // The controller sweeps over the cube; frames keep coming, the context is kept.
  const m0 = await c.evaluate(`(() => { const S = window.__xrfake; const a0 = [...S.aimRight]; let k = 0;
    window.__t3sweep = setInterval(() => { k++; S.aimRight = [a0[0] + 0.06 * Math.sin(k / 3), a0[1] + 0.04 * Math.cos(k / 4), a0[2]]; }, 16);
    return S.frames; })()`);
  let m1 = m0;
  for (const t0 = Date.now(); Date.now() - t0 < 6000 && (m1 < m0 + 10 || Date.now() - t0 < 1500);) { await sleep(250); m1 = await c.evaluate('window.__xrfake.frames'); }
  await c.evaluate('clearInterval(window.__t3sweep)');
  const inVR = await c.evaluate(`({ p: window.__tex3dProbe(), t: window.__vrTex3D(), lost: window.__vrGL.isContextLost(), factors: window.__vrState.cube.factors.join() })`);
  say(`in the session: ${JSON.stringify(inVR.p.inXR)}; guard ${JSON.stringify(inVR.t.calls)} refused ${inVR.t.refused}`);
  check('tex3d: no texImage3D or texStorage3D reached the GPU during the session', allocsIn(inVR.p) === 0 && inVR.t.refused === 0,
    `texImage3D ${inVR.p.inXR.texImage3D || 0}, texStorage3D ${inVR.p.inXR.texStorage3D || 0}, refused by the guard ${inVR.t.refused}`);
  check('tex3d: the masks and the cubes went in as sub-images during the session', (inVR.p.inXR.texSubImage3D || 0) >= 3 + 2 * 13, `${inVR.p.inXR.texSubImage3D || 0} texSubImage3D`);
  check('tex3d: frames kept coming, the context kept, the last cube on show', m1 >= m0 + 10 && !inVR.lost && inVR.factors.startsWith('2,2'),
    `${m1 - m0} frames, context ${inVR.lost ? 'LOST' : 'kept'}, factors ${inVR.factors}`);
  // Leave VR: nothing waits any more (the oversized cube was superseded by a newer load).
  await c.evaluate('window.__vrShell.act("exit-vr")');
  const out = await waitFor(c, `!window.__vrXR() && !window.__vrTex3D().xrActive && !window.__vrTex3D().staged && window.__vrState.cube.factors[0] === 2`, 15000);
  const after = await c.evaluate(`({ p: window.__tex3dProbe(), notice: (window.__vrShell.notices.current() || {}).id })`);
  check('tex3d: after the session the cube shown in VR stays, no notice left', out.value && after.notice !== 'cube-waiting' && allocsIn(after.p) === 0,
    JSON.stringify({ notice: after.notice }));
  // The oversized cube once more, in a short session: shown when it ends, allocated on the 2-D page.
  if (bigSide) {
    await c.evaluate(`(async () => { const b = document.getElementById('enter-vr');
      for (let i = 0; i < 50 && b.disabled; i++) await new Promise((r) => setTimeout(r, 50)); b.click(); })()`);
    const inSession = await waitFor(c, 'window.__xrfake.frames > 5 && window.__vrTex3D().xrActive', 15000);
    const pS = await c.evaluate('window.__tex3dProbe()');
    const before = await c.evaluate('window.__vrState.cube.dims.join("x")');
    await c.evaluate(`document.getElementById('synthetic').click()`);
    const waits = await waitFor(c, `!!window.__vrTex3D().staged`, 30000);
    const kept = await c.evaluate(`({ dims: window.__vrState.cube.dims.join('x'), xr: window.__vrTex3D().xrActive })`);
    check('tex3d: in a new session the cube larger than the capacity waits, the cube on show stays',
      inSession.value && waits.value && kept.xr && kept.dims === before, JSON.stringify({ inSession: inSession.value, staged: waits.value, before, kept }));
    if (inSession.value && waits.value) {
      await c.evaluate('window.__vrShell.act("exit-vr")');
      const shown = await waitFor(c, `!window.__vrTex3D().xrActive && !window.__vrTex3D().staged && window.__vrState.cube.dims[0] === ${bigSide}`, 30000);
      const pA = await c.evaluate(`({ p: window.__tex3dProbe(), cap: window.__vrTex3D().capacity })`);
      check('tex3d: the cube larger than the capacity is shown when the session ends, allocated on the 2-D page',
        shown.value && (pA.p.outXR.texStorage3D || 0) > (pS.outXR.texStorage3D || 0) && allocsIn(pA.p) === allocsIn(pS),
        `capacity after: ${pA.cap ? pA.cap.dims.join('x') : 'none'}; texStorage3D outside VR ${pS.outXR.texStorage3D || 0} -> ${pA.p.outXR.texStorage3D || 0}`);
    } else {
      await c.evaluate('window.__vrShell.act("exit-vr")');
      await waitFor(c, '!window.__vrTex3D().xrActive', 15000);
    }
    await c.evaluate(`(() => { const $ = (id) => document.getElementById(id); $('fxy').value = '2'; $('path').value = ${JSON.stringify(cube)}; $('load').click(); })()`);
    await waitFor(c, `window.__vrState.loading === 0 && window.__vrState.cube.factors[0] === 2 && window.__vrState.openPath === ${JSON.stringify(cube)}`, 60000);
  }
  // The image of a cube in a corner of the capacity (a brighter cube's voxels
  // beyond it) against the same cube in storage of its own size.
  await c.evaluate(`(() => { const $ = (id) => document.getElementById(id); $('fxy').value = '1'; $('path').value = ${JSON.stringify(other)}; $('load').click(); })()`);
  await waitFor(c, `window.__vrState.loading === 0 && window.__vrState.openPath === ${JSON.stringify(other)} && window.__vrState.cube.factors[0] === 1`, 60000);
  const cmp = await c.evaluate('window.__vrCapacityCompare()');
  say(`image comparison: ${JSON.stringify(cmp)}`);
  check('tex3d: a cube smaller than the capacity draws as with storage of its own size (no seams, no stale voxels)',
    cmp.inCap && cmp.own && cmp.drawn > 2000 && cmp.differ === 0 && cmp.maxDiff <= 2 && !cmp.lost,
    `max channel difference ${cmp.maxDiff}, ${cmp.differ} pixels over 2 of ${cmp.drawn} drawn; cube ${cmp.cube} in ${cmp.capacity}, bright ${cmp.bright}`);
  await c.evaluate(`(() => { const $ = (id) => document.getElementById(id); $('fxy').value = '2'; $('path').value = ${JSON.stringify(cube)}; $('load').click(); })()`);
  await waitFor(c, `window.__vrState.loading === 0 && window.__vrState.cube.factors[0] === 2 && window.__vrState.openPath === ${JSON.stringify(cube)}`, 60000);

  // ── The headset benchmark: prepared on the page, run in VR ───────────────
  const pB = await c.evaluate('window.__tex3dProbe()');
  await c.evaluate(`document.getElementById('bench-xr').click()`);
  const ready = await waitFor(c, `!!(window.__vrTex3D().bench && window.__vrTex3D().bench.ready)`, 120000, 250);
  const prep = await c.evaluate(`({ t: window.__vrTex3D(), label: document.getElementById('bench-xr').textContent, gpu: window.__vrGpu() })`);
  check('tex3d: the benchmark\'s cubes are prepared on the page, then the button enters VR', ready.value && prep.label === 'Start the headset benchmark',
    `${JSON.stringify(prep.t.bench)}; ${prep.gpu.otherVolumes} volumes beside the cube, ${(prep.gpu.otherVolumeBytes / 1e6).toFixed(0)} MB`);
  await c.evaluate(`document.getElementById('bench-xr').click()`);
  const benchOn = await waitFor(c, `window.__vrTex3D().xrActive`, 15000);
  const benchDone = await waitFor(c, `!window.__vrTex3D().xrActive && Array.isArray(window.__vrBench) && !window.__vrTex3D().bench`, 180000, 250);
  const rows = await c.evaluate('(window.__vrBench || []).map((r) => r.cube + " " + r.spv)');
  const pB2 = await c.evaluate(`({ p: window.__tex3dProbe(), gpu: window.__vrGpu(), cube: window.__vrState.cube.factors.join(), lost: window.__vrGL.isContextLost() })`);
  const sizes = (prep.t.bench && prep.t.bench.sizes) || [];
  check('tex3d: the benchmark runs every prepared cube in VR with no 3-D allocation, then gives them back',
    benchOn.value && benchDone.value && rows.length === 3 * sizes.length + 3 && allocsIn(pB2.p) === allocsIn(pB) && !pB2.lost
      && pB2.gpu.otherVolumes === 0 && pB2.cube.startsWith('2,2'),
    `${rows.length} rows (${sizes.join(', ')} and the loaded cube); in-VR allocations ${allocsIn(pB2.p) - allocsIn(pB)}; ${pB2.gpu.otherVolumes} volumes kept; cube ${pB2.cube}`);
  // Left before its end: the cube shown before it comes back, its cubes are given back.
  await c.evaluate(`document.getElementById('bench-xr').click()`);
  const ready2 = await waitFor(c, `!!(window.__vrTex3D().bench && window.__vrTex3D().bench.ready)`, 120000, 250);
  await c.evaluate(`document.getElementById('bench-xr').click()`);
  await waitFor(c, `window.__vrTex3D().xrActive && window.__xrfake.frames > 0`, 15000);
  await sleep(600);
  await c.evaluate('window.__vrShell.act("exit-vr")');
  const out2 = await waitFor(c, `!window.__vrTex3D().xrActive && !window.__vrTex3D().bench`, 15000);
  const pB3 = await c.evaluate(`({ p: window.__tex3dProbe(), gpu: window.__vrGpu(), cube: window.__vrState.cube.factors.join(), spv: window.__vrState.spv })`);
  check('tex3d: a benchmark left early gives its cubes back and the loaded cube returns',
    ready2.value && out2.value && pB3.gpu.otherVolumes === 0 && pB3.cube.startsWith('2,2') && allocsIn(pB3.p) === allocsIn(pB),
    `${pB3.gpu.otherVolumes} volumes kept; cube ${pB3.cube}; in-VR allocations ${allocsIn(pB3.p) - allocsIn(pB)}`);
  say(`texture calls in VR over the whole run: ${JSON.stringify(pB3.p.inXR)}`);
  // A cube put on show while the preparation runs: it is cancelled, nothing kept.
  // Once at least one of its cubes is on the GPU (polled in the page between its steps).
  const mid = await c.evaluate(`new Promise((resolve) => {
    document.getElementById('synthetic-size').value = '128';
    document.getElementById('bench-xr').click();
    const t0 = Date.now();
    const iv = setInterval(() => {
      const b = window.__vrTex3D().bench;
      if (b && b.busy && b.sizes.length >= 1) {
        clearInterval(iv);
        const before = window.__vrGpu().otherVolumes;
        document.getElementById('synthetic').click();
        resolve({ before, after: window.__vrGpu().otherVolumes, made: b.sizes.length });
      } else if (!b || !b.busy || Date.now() - t0 > 60000) { clearInterval(iv); resolve({ missed: true }); }
    }, 0);
  })`, 70000);
  const cancelled = await waitFor(c, `!window.__vrTex3D().bench && /prepare it again/.test(document.getElementById('load-status').textContent)`, 60000);
  const g4 = await c.evaluate('window.__vrGpu()');
  check('tex3d: a cube shown during the benchmark\'s preparation cancels it and gives its cubes back at once',
    !mid.missed && mid.before >= 1 && mid.after === 0 && cancelled.value && g4.otherVolumes === 0,
    `${JSON.stringify(mid)}; ${g4.otherVolumes} volumes kept`);
  // Prepared, then another cube loaded: the preparation (sized beside the old cube) is given back.
  await c.evaluate(`document.getElementById('bench-xr').click()`);
  const ready3 = await waitFor(c, `!!(window.__vrTex3D().bench && window.__vrTex3D().bench.ready)`, 120000, 250);
  await c.evaluate(`(() => { const $ = (id) => document.getElementById(id); $('fxy').value = '1'; $('path').value = ${JSON.stringify(cube)}; $('load').click(); })()`);
  const dropped = await waitFor(c, `window.__vrState.loading === 0 && window.__vrState.cube.factors[0] === 1 && !window.__vrTex3D().bench`, 60000);
  const g3 = await c.evaluate(`({ gpu: window.__vrGpu(), label: document.getElementById('bench-xr').textContent })`);
  check('tex3d: a cube loaded after the benchmark was prepared gives the preparation back',
    ready3.value && dropped.value && g3.gpu.otherVolumes === 0 && g3.label === 'Prepare the headset benchmark',
    `${g3.gpu.otherVolumes} volumes kept; button "${g3.label}"`);
  const errs = await c.evaluate(`window.__vrShell.diag.items.filter((e) => e.level === 'error'
    && !(e.event === 'request-failed' && e.message.includes('/v1/render/'))).map((e) => e.event + ': ' + e.message)`);
  const pe = c.pageErrors.slice(errs0);
  check('tex3d: no page error (no tex3d-in-xr either)', !errs.length && !pe.length, [...errs, ...pe].join(' / '));
  // Back to the page the checks after these expect (as vr_cuts.mjs leaves it).
  await c.send('Page.navigate', { url: `${base}/vr/?debug&refine=0&xrfake=nolayers#token=${encodeURIComponent(token)}` });
  await waitFor(c, `!!window.__vrShell && window.__vrShell.life.state === 'ok'`, 30000);
  await c.evaluate(`(() => { const $ = (id) => document.getElementById(id); $('path').value = ${JSON.stringify(cube)}; $('load').click(); })()`);
  await waitFor(c, `!!(window.__vrState.open && window.__vrState.cube && /upload/.test(document.getElementById('load-status').textContent))`, 60000);
}

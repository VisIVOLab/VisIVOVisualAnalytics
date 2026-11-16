// The crop in more detail (issue #328), run by vr_lifecycle.mjs --crop-detail
// (and --xrfake, --regressions) through the WebXR stand-in (XR_FAKE) with the
// texture probe (TEX3D_PROBE). A cube binned 4×4×4 on the page; a crop of a
// quarter of each axis; once it settles the page must show the crop's region
// at level 0 (four times finer), written into the capacity's spare bank: no
// 3-D allocation during the VR session; the picture changes; clearing the
// crop and moving it out of the region bring the coarse cube back at once.
// Screenshots of the coarse and the detailed crop go to the run's directory.

import { writeFileSync } from 'node:fs';
import { join } from 'node:path';

const allocsIn = (p) => (p.inXR.texImage3D || 0) + (p.inXR.texStorage3D || 0);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

export async function runCropDetailChecks({ c, check, waitFor, base, token, cube, out, results = {} }) {
  const L = 'crop detail';
  await c.send('Page.navigate', { url: `${base}/vr/?debug&refine=0&xrfake&n=detail#token=${encodeURIComponent(token)}` });
  const up = await waitFor(c, `!!window.__vrShell && window.__vrShell.life.state === 'ok' && location.search.includes('n=detail')
    && typeof window.__tex3dProbe === 'function'`, 30000);
  if (!up.value) { check(`${L}: page up with the texture probe`, false); return results; }
  await c.evaluate(`(() => { const $ = (id) => document.getElementById(id); $('encoding').value = 'uint8'; $('fxy').value = '4'; $('fz').value = '4';
    $('path').value = ${JSON.stringify(cube)}; $('load').click(); })()`);
  const loaded = await waitFor(c, `!!(window.__vrState.cube && window.__vrState.loading === 0 && /upload/.test(document.getElementById('load-status').textContent))`, 60000);
  const info = await c.evaluate(`({ factors: window.__vrState.cube.factors, dims: window.__vrState.cube.dims, full: window.__vrState.info.full_shape,
    cap: window.__vrTex3D().capacity })`);
  check(`${L}: the cube loaded binned 4×4×4 into the capacity`, !!loaded.value && info.factors.join() === '4,4,4' && info.cap && info.cap.onShow, JSON.stringify(info));
  if (!loaded.value) return results;
  const shot = async (name) => {
    const r = await c.send('Page.captureScreenshot', { format: 'png' });
    const buf = Buffer.from(r.data, 'base64');
    if (out) writeFileSync(join(out, `crop_detail_${name}.png`), buf);
    return buf;
  };
  // The coarse crop first (detail off: the same crop as the coarse cube shows it).
  await c.evaluate(`(() => { window.__vrDetail.off = true; const s = window.__vrState; s.clip = { min: [0.25, 0.25, 0.25], max: [0.5, 0.5, 0.5] }; })()`);
  await sleep(600);
  const coarse = await shot('coarse');
  const coarseFrac = await c.evaluate('window.__vrDrawnFraction()');
  // Detail on: due once the crop has stayed still.
  const t0 = Date.now();
  await c.evaluate('window.__vrDetail.off = false');
  const shown = await waitFor(c, '!!window.__vrDetail.shown', 15000);
  const d = await c.evaluate(`(() => { const d = window.__vrDetail; return { last: d.last, region: d.shown && d.shown.region, onShow: window.__vrTex3D().capacity }; })()`);
  results.desktop = { ms: Date.now() - t0, last: d.last };
  check(`${L}: the settled crop is shown from level 0 (four times finer), its ${d.region ? d.region.dims.join('×') : '?'} voxels in the spare bank`,
    !!shown.value && d.region && d.region.level === 0 && d.region.step === 1 && d.onShow && d.onShow.onShow,
    `${shown.ms} ms after it settled; ${JSON.stringify(d.last)}`);
  await sleep(400);
  const fine = await shot('detail');
  const fineFrac = await c.evaluate('window.__vrDrawnFraction()');
  check(`${L}: the picture is the crop's, now in more detail (different pixels, about as much drawn)`,
    !coarse.equals(fine) && fineFrac.fraction > 0.3 * coarseFrac.fraction && fineFrac.fraction > 0.001,
    `drawn ${(100 * coarseFrac.fraction).toFixed(2)} % coarse, ${(100 * fineFrac.fraction).toFixed(2)} % detailed; PNG ${coarse.length} / ${fine.length} bytes`);

  // Cleared: the coarse cube at once.
  await c.evaluate(`(() => { window.__vrState.clip = { min: [0, 0, 0], max: [1, 1, 1] }; })()`);
  const back = await waitFor(c, '!window.__vrDetail.shown && !window.__vrTex3D().region', 3000);
  check(`${L}: clearing the crop shows the coarse cube again`, !!back.value, `${back.ms} ms`);

  // In VR: the same crop, no 3-D allocation; moved out of the region: dropped.
  await c.evaluate(`(async () => { const b = document.getElementById('enter-vr');
    for (let i = 0; i < 50 && b.disabled; i++) await new Promise((r) => setTimeout(r, 50)); b.click(); })()`);
  const running = await waitFor(c, 'window.__xrfake.frames > 20 && !!window.__vrXR() && window.__vrTex3D().xrActive', 15000);
  if (!running.value) { check(`${L}: VR session draws frames`, false); return results; }
  const p0 = await c.evaluate('window.__tex3dProbe()');
  await c.evaluate(`(() => { window.__vrState.clip = { min: [0.5, 0.25, 0.25], max: [0.75, 0.5, 0.5] }; })()`);
  const inVR = await waitFor(c, '!!window.__vrDetail.shown', 15000);
  const f0 = await c.evaluate('window.__xrfake.frames');
  await sleep(1000);
  const f1 = await c.evaluate('window.__xrfake.frames');
  const p1 = await c.evaluate('window.__tex3dProbe()');
  const last = await c.evaluate('window.__vrDetail.last');
  results.vr = last;
  check(`${L}: in VR the crop's detail is shown with no 3-D texture allocated, frames still drawn`,
    !!inVR.value && allocsIn(p1) === allocsIn(p0) && allocsIn(p1) === 0 && f1 - f0 > 10,
    `in-VR allocations ${allocsIn(p1) - allocsIn(p0)}, texSubImage3D ${(p1.inXR.texSubImage3D || 0) - (p0.inXR.texSubImage3D || 0)}; ${f1 - f0} frames in 1 s; ${JSON.stringify(last)}`);
  // Dragged out of the region: the coarse cube at once, then the new region once it settles.
  await c.evaluate(`(() => { window.__vrState.clip = { min: [0, 0.25, 0.25], max: [0.25, 0.5, 0.5] }; })()`);
  const dropped = await waitFor(c, '!window.__vrDetail.shown || window.__vrDetail.shown.region.box.x0 === 0', 3000);
  const again = await waitFor(c, '!!window.__vrDetail.shown && window.__vrDetail.shown.region.box.x0 === 0', 15000);
  const p2 = await c.evaluate('window.__tex3dProbe()');
  check(`${L}: a crop moved out of the region drops it, and its own region follows (still no allocation)`,
    !!dropped.value && !!again.value && allocsIn(p2) === 0, `${dropped.ms} ms / ${again.ms} ms`);
  const errs = await c.evaluate(`window.__vrShell.diag.items.filter((e) => e.level === 'error').map((e) => e.event + ': ' + e.message)`);
  check(`${L}: no page error`, !errs.length && !c.pageErrors.length, [...errs, ...c.pageErrors].join(' / '));
  await c.evaluate('window.__vrShell.act("exit-vr")');
  await waitFor(c, '!window.__vrXR()', 10000);
  return results;
}

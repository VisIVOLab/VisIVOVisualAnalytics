// The headset's view on the desktop (issue #281), run by vr_lifecycle.mjs
// --regressions (and --xrfake alone) through the WebXR stand-in (XR_FAKE),
// with composition layers.
//
// - Watching starts with the desktop's route: the page gets the `watch`
//   command, shows its badge, and frames come (JPEG, the size the page says).
// - The menu is a quad layer, so not in the projection layer: it must be in
//   the frames all the same (drawn into the copy). A frame with the menu open
//   differs from one without it where the menu is.
// - The capture's cost on the page's thread, per frame (issue and read back),
//   is reported; no 3-D allocation in VR; stopping takes the badge away.
//
// Frames are decoded in the page (createImageBitmap) for the pixel checks:
// node has no JPEG decoder.

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

export async function runSpectatorChecks({ c, check, waitFor, base, token, cube }) {
  const say = (m) => console.log(`     spectator: ${m}`);
  await c.send('Page.navigate', { url: `${base}/vr/?debug&refine=0&xrfake&spect=1#token=${encodeURIComponent(token)}` });
  const up = await waitFor(c, `!!window.__vrShell && window.__vrShell.life.state === 'ok' && !!window.__vrSpectator`, 30000);
  if (!up.value) { check('spectator: page up', false); return {}; }
  await c.evaluate(`(() => { const $ = (id) => document.getElementById(id); $('encoding').value = 'uint8'; $('fxy').value = '1'; $('path').value = ${JSON.stringify(cube)}; $('load').click(); })()`);
  const loaded = await waitFor(c, `!!(window.__vrState.open && window.__vrState.cube && window.__vrState.loading === 0)`, 60000);
  if (!loaded.value) { check('spectator: cube loaded', false); return {}; }
  await c.evaluate(`(async () => { const b = document.getElementById('enter-vr');
    for (let i = 0; i < 50 && b.disabled; i++) await new Promise((r) => setTimeout(r, 50)); b.click(); })()`);
  const running = await waitFor(c, 'window.__xrfake.frames > 20 && !!window.__vrXR() && window.__vrXR().overlaysAsLayers', 15000);
  if (!running.value) { check('spectator: VR session with layers draws frames', false); return {}; }
  const session = await c.evaluate('window.__vrRefine.refiner.backend().session');
  const client = await c.evaluate('window.__vrHub.st.client');
  await waitFor(c, 'window.__vrHub.st.helloSession !== ""', 10000);
  const headers = { 'X-Visivo-Token': token, 'X-Visivo-Session': session };
  const watch = (body) => fetch(`${base}/v1/vr/headset/spectator`, { method: 'POST', headers: { ...headers, 'Content-Type': 'application/json' },
    body: JSON.stringify({ client, ...body }) });
  const read = async (after, wait = 3) => {
    const r = await fetch(`${base}/v1/vr/headset/spectator/frame?client=${encodeURIComponent(client)}&after=${after}&wait=${wait}`, { headers });
    const buf = r.status === 200 ? Buffer.from(await r.arrayBuffer()) : null;
    return { status: r.status, buf, seq: Number(r.headers.get('x-visivo-frame-seq') || 0),
      w: Number(r.headers.get('x-visivo-frame-width') || 0), h: Number(r.headers.get('x-visivo-frame-height') || 0),
      cost: r.headers.get('x-visivo-frame-cost-ms') };
  };
  // Decoded in the page: luminance per pixel; with `ref`, the mean |difference| and where it is.
  const decode = (buf) => c.evaluate(`(async () => {
    const bin = atob(${JSON.stringify(buf.toString('base64'))});
    const u = new Uint8Array(bin.length); for (let i = 0; i < bin.length; i++) u[i] = bin.charCodeAt(i);
    const bmp = await createImageBitmap(new Blob([u], { type: 'image/jpeg' }));
    const cv = new OffscreenCanvas(bmp.width, bmp.height); const g = cv.getContext('2d'); g.drawImage(bmp, 0, 0);
    const d = g.getImageData(0, 0, bmp.width, bmp.height).data;
    const lum = []; let lit = 0;
    for (let i = 0; i < d.length; i += 4) { const l = (d[i] + d[i + 1] + d[i + 2]) / 3; lum.push(l); if (l > 24) lit++; }
    return { w: bmp.width, h: bmp.height, lit: lit / lum.length, lum };
  })()`);

  const p0 = await c.evaluate('window.__tex3dProbe ? window.__tex3dProbe() : null');
  const before = await c.evaluate('window.__vrStats(window.__vrXR().targetHz || 72)');
  const r0 = await watch({ on: true, fps: 15, width: 384 });
  const on = await waitFor(c, 'window.__vrSpectator.watching && window.__vrSpectator.stats().stored >= 3', 15000);
  const st = await c.evaluate('window.__vrSpectator.stats()');
  const shown = await c.evaluate('(window.__vrXR().quads || {}).shown || []');
  check('spectator: the desktop\'s watch reaches the page, which shows its badge (a quad) and sends frames',
    r0.status === 200 && !!on.value && shown.includes('spectator-badge'),
    JSON.stringify({ status: r0.status, ms: on.ms, shown, stored: st.stored, size: st.size }));
  const f1 = await read(0);
  const isJpeg = !!f1.buf && f1.buf[0] === 0xff && f1.buf[1] === 0xd8;
  const img1 = isJpeg ? await decode(f1.buf) : null;
  check('spectator: a frame is a JPEG of the left eye, the width asked for, with the cube in it',
    f1.status === 200 && isJpeg && f1.w === 384 && !!img1 && img1.w === 384 && img1.h === f1.h && img1.lit > 0.005,
    JSON.stringify({ status: f1.status, size: [f1.w, f1.h], bytes: f1.buf && f1.buf.length, lit: img1 && +img1.lit.toFixed(4), cost: f1.cost }));
  // The menu (a quad layer) opened: it must show in the frames.
  await c.evaluate('window.__xrfake.buttons.left[4].pressed = true');
  await waitFor(c, '((window.__vrXR().quads || {}).shown || []).includes("menu")', 5000);
  await sleep(400);
  const f2 = await read(f1.seq);
  const f3 = await read(f2.seq);       // one captured with the menu surely drawn
  await c.evaluate('window.__xrfake.buttons.left[4].pressed = false');
  const img3 = f3.buf ? await decode(f3.buf) : null;
  let diff = 0, changed = 0;
  if (img1 && img3 && img1.lum.length === img3.lum.length) {
    for (let i = 0; i < img1.lum.length; i++) { const d = Math.abs(img1.lum[i] - img3.lum[i]); diff += d; if (d > 40) changed++; }
    diff /= img1.lum.length;
    changed /= img1.lum.length;
  }
  check('spectator: the menu, a composition layer, is drawn into the frames (a large part of the view changes)',
    changed > 0.05, JSON.stringify({ meanAbsDiff: +diff.toFixed(2), changedFraction: +changed.toFixed(3), seqs: [f1.seq, f3.seq] }));
  await sleep(2000);
  const s2 = await c.evaluate('window.__vrSpectator.stats()');
  const during = await c.evaluate('window.__vrStats(window.__vrXR().targetHz || 72)');
  say(`cost on the page's thread (ms mean/max): issue ${s2.issueMs.mean?.toFixed(3)}/${s2.issueMs.max?.toFixed(3)}, read ${s2.readMs.mean?.toFixed(3)}/${s2.readMs.max?.toFixed(3)}; `
    + `encode ${s2.encodeMs.mean?.toFixed(1)} ms (${s2.encoder}), post ${s2.postMs.mean?.toFixed(1)} ms, ${s2.rate.toFixed(1)} fps, ${(s2.bytes / 1024).toFixed(1)} KB; `
    + `frame CPU ${before.cpu?.mean?.toFixed(2)} -> ${during.cpu?.mean?.toFixed(2)} ms`);
  check('spectator: the JPEG is made in a worker, and the capture never waits for the GPU (read back a frame or more later)',
    s2.encoder === 'worker' && s2.fenceFrames.mean >= 1, JSON.stringify({ encoder: s2.encoder, fenceFrames: s2.fenceFrames }));
  const p1 = await c.evaluate('window.__tex3dProbe ? window.__tex3dProbe() : null');
  const allocs = (p) => (p ? (p.inXR.texImage3D || 0) + (p.inXR.texStorage3D || 0) : 0);
  check('spectator: no 3-D allocation in VR while watched (#229)', allocs(p1) === allocs(p0), JSON.stringify(p1 && p1.inXR));
  // Stopped: the page stops, the badge goes, the frame read says not watched.
  const r1 = await watch({ on: false });
  const off = await waitFor(c, '!window.__vrSpectator.watching && !((window.__vrXR().quads || {}).shown || []).includes("spectator-badge")', 10000);
  const after = await read(0, 0);
  check('spectator: stopping turns the capture and the badge off; the frame read answers 409',
    r1.status === 200 && !!off.value && after.status === 409, JSON.stringify({ status: r1.status, read: after.status }));
  await c.evaluate('window.__vrShell.act("exit-vr")');
  await waitFor(c, '!window.__vrXR()', 10000);
  return { stats: s2, frameCpu: { before: before.cpu, during: during.cpu } };
}

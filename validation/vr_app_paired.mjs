// The VisIVO VR headset app's pairing flow (issue #383), as far as a desktop
// Chrome can show it: the app's scanner opens the public loader with
// ?launch=immersive#pair=<code>; the loader pairs and its page enters VR by
// itself (no press: the app's browser grants a session without one); leaving
// VR closes the loader's window, which brings back the app's scanner; a code
// that cannot be used, or none, is said in VR by the loader itself, and
// leaving that notice closes the window too. Run by vr_lifecycle.mjs
// --app-paired, with the WebXR stand-in (XR_FAKE, ?xrfake) and window.close
// counted (CLOSE_SPY) in every frame. What only the Quest shows (the
// browser's immersive app, the camera, Horizon OS's switch from the panel)
// is on the device check list.

/** window.close counted (window.__closed), before the page's scripts. */
export const CLOSE_SPY = `(() => {
  window.__closed = 0;
  window.close = () => { window.__closed++; };
})()`;

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/** The loader's iframe (the VR page) once it is there: its frame id, or null. */
async function appFrame(c, ms = 30000) {
  const t0 = Date.now();
  while (Date.now() - t0 < ms) {
    const tree = await c.send('Page.getFrameTree');
    const child = (tree.frameTree.childFrames || [])[0];
    if (child && /\/app\/[a-z0-9]+\/vr\//.test(child.frame.url)) return { id: child.frame.id, url: child.frame.url };
    await sleep(100);
  }
  return null;
}

export async function runAppPairedChecks({ c, check, waitFor, loader, service, code, results }) {
  const top = { top: true };
  const topWait = async (expr, ms = 20000) => {
    const t0 = Date.now();
    for (;;) {
      let v = null;
      try { v = await c.evaluate(expr, 5000, top); } catch { v = null; }
      if (v) return { value: v, ms: Date.now() - t0 };
      if (Date.now() - t0 > ms) return { value: null, ms: Date.now() - t0, timeout: true };
      await sleep(100);
    }
  };
  const base = `${loader}/?launch=immersive&xrfake&debug&refine=0&pair_service=${encodeURIComponent(service)}`;

  // ── The scanner's launch: paired, in VR by itself ──────────────────────────
  const t0 = Date.now();
  await c.send('Page.navigate', { url: `${base}#pair=${code}` });
  const frame = await appFrame(c);
  check('app: the loader paired and opened the page', !!frame, `${Date.now() - t0} ms`);
  if (!frame) return;
  check('app: the page is told it runs in the headset app (launch=immersive)', /[?&]launch=immersive(&|#|$)/.test(frame.url), frame.url.replace(/#.*/, ''));
  c.target.frameId = frame.id;
  const inVR = await waitFor(c, '!!(window.__vrXR && window.__vrXR())', 20000);
  check('app: the page entered VR by itself, with no press', !!inVR.value, `${Date.now() - t0} ms from the launch`);
  results.timings.appLaunchToVrMs = Date.now() - t0;
  const fake = await c.evaluate('({ requests: window.__xrfake.requests, live: window.__xrfake.live() })');
  check('app: one session, asked for by the page', fake.requests === 1 && fake.live === 1, JSON.stringify(fake));
  const topFake = await c.evaluate('({ requests: window.__xrfake.requests })', 5000, top);
  check('app: the loader asked for no session of its own', topFake.requests === 0, JSON.stringify(topFake));
  // The way out says Exit VR (back to the scanner), not Quit.
  const exit = await waitFor(c, `(() => { const n = window.__vrShell.notices.current(); const a = n && (n.actions || []).find((x) => x.id === 'exit-vr'); return a ? a.label : ''; })()`, 10000);
  check('app: the way out of VR reads Exit VR (the scanner comes back), not Quit', exit.value === 'Exit VR', String(exit.value));
  // Leaving VR: the loader's window is closed (the scanner comes back).
  await c.evaluate('window.__xrfake.lastSession.end()');
  const closed = await topWait('window.__closed > 0', 5000);
  check('app: leaving VR closes the loader\'s window (back to the scanner)', !!closed.value, `${closed.ms} ms`);
  c.target.frameId = null;

  // ── A code that cannot be used: said in VR by the loader ───────────────────
  const t1 = Date.now();
  await c.send('Page.navigate', { url: `${base}&n=2#pair=BADBAD` });
  const asked = await topWait('!!(window.__xrfake && window.__xrfake.live() === 1)', 30000);
  check('app: an unknown code: the loader shows it in VR (its own session)', !!asked.value, `${Date.now() - t1} ms`);
  const said = await c.evaluate(`({ status: document.getElementById('status').textContent, closed: window.__closed,
    frame: !document.getElementById('app').hidden, frames: window.__xrfake.frames })`, 5000, top);
  check('app: the notice is drawn and names the reason; no page opened', /not known/.test(said.status) && !said.frame && said.frames > 0
    && said.closed === 0, JSON.stringify(said));
  await c.evaluate('window.__xrfake.lastSession._fire("select")', 5000, top);
  const closed2 = await topWait('window.__closed > 0 && window.__xrfake.live() === 0', 5000);
  check('app: a trigger or a pinch ends the notice and closes the window (back to the scanner)', !!closed2.value, `${closed2.ms} ms`);

  // ── No code at all ─────────────────────────────────────────────────────────
  // A fresh tab, as each launch of the app's browser part is: no resume code
  // of the first pairing kept in this tab (that one would resume instead).
  await c.evaluate('sessionStorage.clear()', 5000, top);
  await c.send('Page.navigate', { url: `${base}&n=3` });
  const none = await topWait('!!(window.__xrfake && window.__xrfake.live() === 1) && /without a pairing code/.test(document.getElementById("status").textContent)', 15000);
  check('app: opened without a code: said in VR too, never a 2-D form nobody sees', !!none.value, `${none.ms} ms`);
  if (none.value) await c.evaluate('window.__xrfake.lastSession._fire("select")', 5000, top);

  // ── The same loader in a browser tab: the form, as before ─────────────────
  await c.send('Page.navigate', { url: `${loader}/?xrfake&pair_service=${encodeURIComponent(service)}` });
  const form = await topWait(`!document.getElementById('pair-form').hidden && !document.getElementById('home').hidden`, 10000);
  const tabFake = await c.evaluate('window.__xrfake.requests', 5000, top);
  check('app: in a browser tab the loader shows its code form and asks for no session', !!form.value && tabFake === 0, `${tabFake} requests`);
}

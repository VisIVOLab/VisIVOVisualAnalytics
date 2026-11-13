// The headset path of the VR page on the desktop (issue #226), run by
// vr_lifecycle.mjs --regressions (and --xrfake alone). No headset here: a
// stand-in for WebXR (XR_FAKE, installed before the page's scripts with
// ?xrfake in the address) gives the page a session of two views, two
// controllers whose buttons the checks press, WebXR composition layers
// (XRWebGLBinding quad layers backed by real WebGL textures) and the frame
// loop. It catches what only runs in VR: the overlays as layers and their
// fallback, the controller models, the menu opened by the secondary hand,
// the readout under the primary ray, a smaller eye buffer, and the one
// active page across two tabs. What it cannot tell is how the Quest's
// compositor shows them: that is on the Quest check list.

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

export const XR_FAKE = `(() => {
  if (!/[?&]xrfake/.test(location.search)) return;
  const noLayers = /[?&]xrfake=nolayers/.test(location.search);
  const ident = () => new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]);
  const translate = (x, y, z) => { const m = ident(); m[12] = x; m[13] = y; m[14] = z; return m; };
  const invertRigid = (m) => {
    const r = ident();
    for (let i = 0; i < 3; i++) for (let j = 0; j < 3; j++) r[4 * i + j] = m[4 * j + i];
    for (let i = 0; i < 3; i++) r[12 + i] = -(r[i] * m[12] + r[4 + i] * m[13] + r[8 + i] * m[14]);
    return r;
  };
  /** A pose at p whose -z points at q. */
  const lookAt = (p, q) => {
    let z = [p[0] - q[0], p[1] - q[1], p[2] - q[2]];
    const n = Math.hypot(...z); z = z.map((v) => v / n);
    let x = [z[2], 0, -z[0]]; const nx = Math.hypot(...x) || 1; x = x.map((v) => v / nx);
    const y = [z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]];
    return new Float32Array([...x, 0, ...y, 0, ...z, 0, ...p, 1]);
  };
  const persp = (fovy, aspect, near, far) => {
    const f = 1 / Math.tan(fovy / 2), nf = 1 / (near - far);
    return new Float32Array([f / aspect, 0, 0, 0, 0, f, 0, 0, 0, 0, (far + near) * nf, -1, 0, 0, 2 * far * near * nf, 0]);
  };
  const S = window.__xrfake = { frames: 0, renderStates: [], quadsMade: 0, quadsLive: 0, copies: 0, ended: 0,
    buttons: { left: Array.from({ length: 7 }, () => ({ pressed: false, touched: false, value: 0 })),
               right: Array.from({ length: 7 }, () => ({ pressed: false, touched: false, value: 0 })) },
    aimRight: [0, 1.48, -0.75], rate: 72 };
  const head = [0, 1.6, 0];
  class FakeLayer {
    constructor(session, gl, opts = {}) {
      const s = opts.framebufferScaleFactor || 1;
      this.framebuffer = null;
      this.framebufferWidth = Math.round(1680 * s) * 2; this.framebufferHeight = Math.round(1760 * s);
      this.fixedFoveation = 1;
      this._gl = gl;
    }
    getViewport(view) {
      const c = this._gl.canvas, w = c.width / 2, h = c.height;
      return view.eye === 'left' ? { x: 0, y: 0, width: w, height: h } : { x: w, y: 0, width: w, height: h };
    }
  }
  window.XRWebGLLayer = FakeLayer;
  window.XRRigidTransform = class { constructor(p, o) { this.position = p; this.orientation = o; } };
  if (!noLayers) {
    window.XRWebGLBinding = class {
      constructor(session, gl) { this.gl = gl; }
      createQuadLayer(init) {
        const gl = this.gl;
        const tex = gl.createTexture();
        gl.bindTexture(gl.TEXTURE_2D, tex);
        gl.texStorage2D(gl.TEXTURE_2D, 1, gl.RGBA8, init.viewPixelWidth, init.viewPixelHeight);
        S.quadsMade++; S.quadsLive++;
        return { init, tex, needsRedraw: false, width: init.width, height: init.height, transform: null,
          destroy() { gl.deleteTexture(tex); S.quadsLive--; } };
      }
      getSubImage(layer) {
        // As the spec: only a layer of the active array (set in an earlier frame).
        if (!activeLayers.includes(layer)) throw new TypeError('getSubImage: the layer is not in the session active layers');
        S.copies++;
        return { colorTexture: layer.tex, viewport: { x: 0, y: 0, width: layer.init.viewPixelWidth, height: layer.init.viewPixelHeight } };
      }
    };
  } else {
    delete window.XRWebGLBinding;
  }
  const listeners = {};
  let activeLayers = [], pendingLayers = null;
  const gamepad = (hand) => ({ buttons: S.buttons[hand], axes: [0, 0, 0, 0] });
  const sources = ['left', 'right'].map((hand) => ({ handedness: hand, gripSpace: { hand, kind: 'grip' },
    targetRaySpace: { hand, kind: 'ray' }, gamepad: gamepad(hand), profiles: [] }));
  let enabled = [];
  const session = {
    get enabledFeatures() { return enabled; },
    inputSources: sources,
    supportedFrameRates: new Float32Array([72, 80, 90, 120]),
    frameRate: 72,
    async updateTargetFrameRate(r) { this.frameRate = r; S.rate = r; },
    updateRenderState(s) {
      S.renderStates.push({ base: !!s.baseLayer, layers: s.layers ? s.layers.map((l) => (l instanceof FakeLayer ? 'projection' : 'quad')) : null });
      if (s.layers && !s.layers.every((l) => l instanceof FakeLayer || l.tex)) throw new Error('not a layer');
      // Applied at the next frame, as in a browser.
      pendingLayers = s.layers || (s.baseLayer ? [s.baseLayer] : pendingLayers);
    },
    async requestReferenceSpace() { return { kind: 'ref' }; },
    requestAnimationFrame(cb) { return requestAnimationFrame((t) => { if (!this.ended) { if (pendingLayers) { activeLayers = pendingLayers; pendingLayers = null; } S.frames++; cb(t, frame); } }); },
    async end() { if (this.ended) return; this.ended = true; S.ended++; (listeners.end || []).forEach((f) => f({})); },
    addEventListener(type, fn) { (listeners[type] = listeners[type] || []).push(fn); },
  };
  const headM = translate(...head);
  const viewFor = (eye) => {
    const m = translate(head[0] + (eye === 'left' ? -0.032 : 0.032), head[1], head[2]);
    return { eye, projectionMatrix: persp(1.6, 0.95, 0.05, 50), transform: { matrix: m, inverse: { matrix: invertRigid(m) } } };
  };
  const frame = {
    session,
    getViewerPose() {
      return { transform: { matrix: headM, position: { x: head[0], y: head[1], z: head[2] }, inverse: { matrix: invertRigid(headM) } },
        views: [viewFor('left'), viewFor('right')] };
    },
    getPose(space) {
      const p = space.hand === 'left' ? [-0.2, 1.3, -0.35] : [0.18, 1.35, -0.3];
      const q = space.hand === 'left' ? [-0.2, 1.3, -1.2] : S.aimRight;
      return { transform: { matrix: lookAt(p, q) } };
    },
    getJointPose() { return null; },
  };
  const xr = {
    async isSessionSupported(mode) { return mode === 'immersive-vr' || mode === 'immersive-ar'; },
    async requestSession(mode, opts) {
      // A slow headset: the session answers after S.delayMs (a takeover check).
      if (S.delayMs) await new Promise((r) => setTimeout(r, S.delayMs));
      session.ended = false;
      enabled = ['local-floor', ...((opts && opts.optionalFeatures) || []).filter((f) => f !== 'layers' || !noLayers)];
      return session;
    },
  };
  Object.defineProperty(navigator, 'xr', { value: xr, configurable: true });
  // The native one waits for a real XR device.
  WebGL2RenderingContext.prototype.makeXRCompatible = async function () {};
})();`;

/** The XR checks on the page `c` (opened with ?xrfake or ?xrfake=nolayers). */
export async function runXRFakeChecks({ c, check, waitFor, label = 'layers', expectLayers = true }) {
  const say = (m) => console.log(`     xrfake ${label}: ${m}`);
  const enter = await c.evaluate(`(async () => {
    const b = document.getElementById('enter-vr');
    for (let i = 0; i < 50 && b.disabled; i++) await new Promise((r) => setTimeout(r, 50));
    if (b.disabled) return 'Enter VR disabled';
    b.click();
    return 'clicked';
  })()`);
  const running = await waitFor(c, 'window.__xrfake.frames > 40 && !!window.__vrXR()', 15000);
  const why = running.value ? '' : await c.evaluate(`JSON.stringify({ status: document.getElementById('load-status').textContent.slice(-160),
    frames: window.__xrfake && window.__xrfake.frames, states: window.__xrfake && window.__xrfake.renderStates })`);
  check(`xrfake ${label}: Enter VR starts the session and draws frames`, enter === 'clicked' && running.value, `${enter}; ${running.ms} ms ${why}`);
  if (!running.value) return;
  const st = await c.evaluate('window.__vrXR()');
  const rs = await c.evaluate('window.__xrfake.renderStates');
  say(JSON.stringify({ st, renderStates: rs.slice(0, 4) }));
  if (expectLayers) {
    check(`xrfake ${label}: the projection layer is first of the layers array, overlays are quads`,
      st.overlaysAsLayers && st.quads && st.quads.active && rs.some((s) => s.layers && s.layers[0] === 'projection' && s.layers.length > 1),
      JSON.stringify(st.quads));
  } else {
    check(`xrfake ${label}: without layers, the base layer and the overlays in the scene`,
      !st.overlaysAsLayers && !st.quads && rs.some((s) => s.base) && !rs.some((s) => s.layers), st.layersNote);
  }
  check(`xrfake ${label}: controllers drawn as models`, st.meshCount > 300, `${st.meshCount} vertices`);
  // The menu: the secondary (left) hand's lower button held.
  await c.evaluate('window.__xrfake.buttons.left[4].pressed = true');
  await sleep(400);
  const withMenu = await c.evaluate('window.__vrXR()');
  if (expectLayers) check(`xrfake ${label}: the menu opens as a quad`, (withMenu.quads.shown || []).includes('menu'), JSON.stringify(withMenu.quads.shown));
  else check(`xrfake ${label}: the menu opens (in the scene)`, await c.evaluate('window.__vrMenu.ui.menu.open'));
  await c.evaluate('window.__xrfake.buttons.left[4].pressed = false');
  await sleep(300);
  // The readout: the primary ray on the cube's emission.
  const hover = await waitFor(c, '!!window.__vrSelection.sel.hover', 4000);
  const ro = await c.evaluate('window.__vrXR()');
  if (expectLayers) {
    check(`xrfake ${label}: the readout under the primary ray, as a quad`, hover.value && (ro.quads.shown || []).includes('readout-live'),
      JSON.stringify(ro.quads.shown));
  } else {
    check(`xrfake ${label}: the primary ray finds the emission`, hover.value);
  }
  // A smaller eye buffer during the session: the projection layer replaced, quads kept.
  const before = (await c.evaluate('window.__xrfake.renderStates')).length;
  await c.evaluate('window.__vrSetEyeScale(0.85)');
  await sleep(300);
  const after = await c.evaluate('({ rs: window.__xrfake.renderStates, xr: window.__vrXR() })');
  const last = after.rs[after.rs.length - 1];
  check(`xrfake ${label}: a smaller eye buffer keeps drawing`, after.xr && after.xr.fbScale === 0.85 && after.rs.length > before
    && (expectLayers ? last.layers && last.layers[0] === 'projection' && after.xr.quads.active : last.base), JSON.stringify(last));
  // Frames keep coming: a count, not a rate. Under software GL (CI's
  // SwiftShader) a stereo frame takes about 100 ms, so wait up to 5 s for 5.
  const frames0 = await c.evaluate('window.__xrfake.frames');
  let frames1 = frames0;
  for (const t0 = Date.now(); Date.now() - t0 < 5000 && frames1 < frames0 + 5;) {
    await sleep(250);
    frames1 = await c.evaluate('window.__xrfake.frames');
  }
  check(`xrfake ${label}: frames keep coming`, frames1 >= frames0 + 5, `${frames1 - frames0} frames`);
  // The page's own cost of a stereo frame on this Mac (CPU; GPU where the timer query exists).
  const cost = await c.evaluate(`(() => { const s = window.__vrStats(72); return { n: s.n, cpu: s.cpu && s.cpu.mean, cpuP95: s.cpu && s.cpu.p95, gpu: s.gpu && s.gpu.mean, fps: s.fps }; })()`);
  say(`frame cost: CPU ${cost.cpu ? cost.cpu.toFixed(2) : 'n/a'} ms mean (p95 ${cost.cpuP95 ? cost.cpuP95.toFixed(2) : 'n/a'}), GPU ${cost.gpu ? cost.gpu.toFixed(2) : 'n/a'} ms, ${cost.n} frames`);
  // Leave VR: the quads go.
  await c.evaluate('window.__vrShell.act("exit-vr")');
  await waitFor(c, '!window.__vrXR()', 5000);
  const fake = await c.evaluate('window.__xrfake');
  if (expectLayers) check(`xrfake ${label}: leaving VR destroys the quads`, fake.quadsLive === 0 && fake.quadsMade > 0, `${fake.quadsMade} made, ${fake.copies} copies`);
  const errs = await c.evaluate(`window.__vrShell.diag.items.filter((e) => e.level === 'error').map((e) => e.event + ': ' + e.message)`);
  check(`xrfake ${label}: no page error in VR`, !errs.length && !c.pageErrors.length, [...errs, ...c.pageErrors].join(' / '));
}

/**
 * A newer page claims while this one's session is still being made (a slow
 * headset): the session must be ended, not shown on the paused page. Then
 * this page takes over again. On a page opened with ?xrfake.
 */
export async function runDelayedTakeoverCheck({ c, check, waitFor }) {
  const r = await c.evaluate(`(async () => {
    const S = window.__xrfake; S.delayMs = 800; const ended0 = S.ended;
    document.getElementById('enter-vr').click();
    await new Promise((res) => setTimeout(res, 200));
    // Another page of this origin announces itself (a channel of its own).
    const ch = new BroadcastChannel('visivo-vr-page');
    ch.postMessage({ type: 'claim', id: 'zz-other-page', t: Date.now() + 1 });
    ch.close();
    await new Promise((res) => setTimeout(res, 1500));
    S.delayMs = 0;
    return { xr: !!window.__vrXR(), active: window.__vrSingle.active, ended: S.ended - ended0 };
  })()`);
  check('one page: a session made while a newer page claims is ended, not shown', !r.xr && !r.active && r.ended === 1, JSON.stringify(r));
  await c.evaluate(`document.getElementById('take-over').click()`);
  await waitFor(c, 'window.__vrSingle.active', 3000);
}

/** After a lost context, Reload page reopens the cube that was on show. */
export async function runReloadCheck({ c, check, waitFor, path }) {
  await c.evaluate('setTimeout(() => window.__vrShell.act("reload"), 50), true');
  await sleep(1500);
  const back = await waitFor(c, `!!(window.__vrState && window.__vrState.open && window.__vrState.openPath === ${JSON.stringify(path)})`, 30000);
  check('Reload page reopens the cube on show by its path', back.value, `${back.ms} ms`);
}

/**
 * One active page: a second tab on the same address makes the first stand
 * down (Enter VR off, the notice), Use this window takes over again.
 */
export async function runSinglePageChecks({ c, check, waitFor, cdpPort, url, connect }) {
  const t = await (await fetch(`http://127.0.0.1:${cdpPort}/json/new?${encodeURIComponent(url)}`, { method: 'PUT' })).json();
  const c2 = connect(t.webSocketDebuggerUrl);
  try {
    await c2.send('Runtime.enable');
    const up = await waitFor(c2, '!!window.__vrSingle && window.__vrSingle.active', 20000);
    const first = await waitFor(c, '!window.__vrSingle.active', 5000);
    const st = await c.evaluate(`({ vr: document.getElementById('enter-vr').disabled, banner: !document.getElementById('stood-down').hidden,
      notice: (window.__vrShell.notices.current() || {}).id })`);
    check('one page: a newer tab makes this one stand down (Enter VR off, "Open in another window")', up.value && first.value
      && st.vr && st.banner && st.notice === 'stood-down', JSON.stringify(st));
    await c.evaluate(`document.getElementById('take-over').click()`);
    const back = await waitFor(c, 'window.__vrSingle.active', 5000);
    const other = await waitFor(c2, '!window.__vrSingle.active', 5000);
    const st2 = await c.evaluate(`({ vr: document.getElementById('enter-vr').disabled, banner: !document.getElementById('stood-down').hidden })`);
    check('one page: Use this window takes over, the other tab stands down', back.value && other.value && !st2.vr && !st2.banner, JSON.stringify(st2));
  } finally {
    try { c2.close(); } catch { /* closed */ }
    await fetch(`http://127.0.0.1:${cdpPort}/json/close/${t.id}`).catch(() => {});
  }
}

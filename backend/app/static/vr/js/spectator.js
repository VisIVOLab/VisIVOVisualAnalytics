// The headset's view on the desktop (issue #281): while the desktop watches,
// the page sends it a small copy of what the user sees, a few times a second.
//
// - Off by default. The desktop's `watch` command (the hub's command poll,
//   headset.js) turns it on with a frame rate and a width, or off; the
//   backend's answer to a frame (`watched: false`) also turns it off, when the
//   desktop went away without saying so.
// - What is captured: the left eye of the XR frame (the whole canvas on the
//   2-D page), cropped to the middle of the view (CROP of each side, about
//   the field a flat window shows well) around the eye's line of sight, scaled
//   down to the width asked for. The cube, the controllers, the hands and the
//   rays are in the projection layer. The menu, the wrist cards, the windows
//   and the message card are composition layers when the browser has them
//   (xr-layers.js): the compositor draws them over the projection layer, so
//   they are NOT in it. They are drawn into the copy here, over the scene, in
//   the same order and at the same place (the panels the ray hits), as the
//   compositor draws them in the headset. Without layers they are in the
//   scene already, and so is the "watched" badge.
// - Cost: the XR frame never waits. The frame that captures does one scaled
//   blit into a small framebuffer, draws the panels, and reads the pixels
//   into a pixel-pack buffer (async: readPixels into a PBO, then a fence);
//   once the fence has passed, the bytes are copied out (getBufferSubData,
//   no GPU stall) in a task of their own right after that frame. Inside the
//   frame the copy first waits for the GPU process to work through the
//   frame's own commands: on a Quest 3, 2.5 ms for a 512 px copy, and still
//   about 2 ms for each 64 KB piece of it, against 1.8 ms after the frame
//   (5.5 ms for 960 px), when the XR frame does not wait for it. The JPEG
//   is made in a worker (OffscreenCanvas), then posted: one capture in
//   flight at a time, so a slow network or encoder drops frames, never
//   queues them.
// - No 3-D texture is made here (issue #229): a renderbuffer and a buffer,
//   made at the first capture of a watch and freed when it ends.
// - While watched, the headset shows a small badge (upper left of the view)
//   and the 2-D page a line at its top.
//
// Pure helpers are exported for node's tests (backend/tests/vr/spectator.test.mjs);
// createSpectator(ctx) is the controller.

import { apiUrl } from './base-path.js';
import { pageFetch } from './transport.js';

export const DEFAULT_FPS = 15;
export const MAX_FPS = 30;
export const DEFAULT_WIDTH = 512;
export const MIN_WIDTH = 160;
export const MAX_WIDTH = 960;
/** The part of the eye's view kept (of its width and of its height). */
export const CROP = 0.8;
/** JPEG quality of the frames. */
export const QUALITY = 0.72;
/** A frame post at most (ms): later, it is dropped and the next one goes. */
export const POST_TIMEOUT_MS = 5000;
/** Failed posts in a row after which the page pauses for PAUSE_MS (still
 *  watched: a link that drops for a minute, as adb over USB can, must not end
 *  the watch; the backend's answer ends it when the desktop has gone). */
export const MAX_FAILURES = 5;
export const PAUSE_MS = 5000;
/** The badge's place in the head's frame (metres) and size. */
export const BADGE_OFFSET = [-0.13, 0.1, -0.65];
export const BADGE_SIZE = [0.085, 0.0213];
const BADGE_PX = [320, 80];

const clampInt = (v, lo, hi, dflt) => {
  const n = Math.round(Number(v));
  return Number.isFinite(n) ? Math.min(hi, Math.max(lo, n)) : dflt;
};

/** A `watch` command as the page applies it: {on, fps, width}, bounded. */
export function watchParams(cmd) {
  const c = cmd || {};
  return { on: c.on === true, fps: clampInt(c.fps, 1, MAX_FPS, DEFAULT_FPS),
    width: clampInt(c.width, MIN_WIDTH, MAX_WIDTH, DEFAULT_WIDTH) };
}

/** A `watch` command the page can act on. */
export function watchUsable(cmd) {
  return !!cmd && Number.isInteger(cmd.seq) && cmd.seq > 0 && cmd.type === 'watch' && typeof cmd.on === 'boolean';
}

/**
 * What one capture copies, in GL pixels (origin bottom left):
 * - src [x0, y0, x1, y1]: the crop of the viewport `vp` ({x, y, width,
 *   height}), `crop` of each side, centred on the eye's line of sight (where
 *   view-space -z lands: ndc (-proj[8], -proj[9]) for a WebXR projection,
 *   off the middle on a headset's asymmetric frustum), kept inside `vp`;
 * - w, h: the copy's size, `width` wide at most (never wider than the crop),
 *   the crop's aspect kept, both even;
 * - panelViewport [x, y, w, h]: the viewport that puts the whole eye's view
 *   on the copy so that the crop fills it (the panels are drawn with it and
 *   the eye's own projection: what falls outside is clipped).
 */
export function captureRect(vp, proj, crop = CROP, width = DEFAULT_WIDTH) {
  const W = Math.max(1, Math.floor(vp.width)), H = Math.max(1, Math.floor(vp.height));
  const c = Math.min(1, Math.max(0.1, Number(crop) || 1));
  const cw = Math.max(2, Math.min(W, Math.round(W * c))), ch = Math.max(2, Math.min(H, Math.round(H * c)));
  const ax = proj ? -proj[8] : 0, ay = proj ? -proj[9] : 0;
  const cx = vp.x + ((Math.max(-1, Math.min(1, ax || 0)) + 1) / 2) * W;
  const cy = vp.y + ((Math.max(-1, Math.min(1, ay || 0)) + 1) / 2) * H;
  const x0 = Math.round(Math.min(vp.x + W - cw, Math.max(vp.x, cx - cw / 2)));
  const y0 = Math.round(Math.min(vp.y + H - ch, Math.max(vp.y, cy - ch / 2)));
  let w = Math.min(cw, Math.max(2, Math.round(Number(width) || DEFAULT_WIDTH)));
  w -= w % 2;
  let h = Math.max(2, Math.round((w * ch) / cw));
  h -= h % 2;
  const kx = w / cw, ky = h / ch;
  return { src: [x0, y0, x0 + cw, y0 + ch], w, h,
    panelViewport: [Math.round((vp.x - x0) * kx), Math.round((vp.y - y0) * ky), Math.round(W * kx), Math.round(H * ky)] };
}

/** The rows of an RGBA image of `w`×`h` upside down (GL's bottom-up rows to
 *  an image's top-down), from `src` into `dst` (both Uint8 arrays). */
export function flipRows(src, w, h, dst) {
  const row = w * 4;
  for (let y = 0; y < h; y++) dst.set(src.subarray((h - 1 - y) * row, (h - y) * row), y * row);
  return dst;
}

/** When the next capture is due at `fps` (frames of the display come every
 *  few ms: a capture at the first frame at or after 1/fps, with 2 ms slack). */
export class Pacer {
  constructor(fps = DEFAULT_FPS) { this.fps = fps; this.last = -Infinity; }
  due(now) { return now - this.last >= 1000 / this.fps - 2; }
  mark(now) { this.last = now; }
}

/** The last `n` values: mean and max (the HUD's numbers). */
export class Ring {
  constructor(n = 60) { this.n = n; this.v = []; }
  push(x) { if (Number.isFinite(x)) { this.v.push(x); if (this.v.length > this.n) this.v.shift(); } }
  get mean() { return this.v.length ? this.v.reduce((a, b) => a + b, 0) / this.v.length : null; }
  get max() { return this.v.length ? Math.max(...this.v) : null; }
  clear() { this.v.length = 0; }
}

/** The frame post's query. */
export function frameQuery(client, eye, costMs) {
  const q = new URLSearchParams({ client: String(client), eye: eye === 'right' || eye === 'mono' ? eye : 'left' });
  if (Number.isFinite(costMs)) q.set('cost_ms', Math.min(1000, Math.max(0, costMs)).toFixed(3));
  return q;
}

/** The frame rate measured from the times (ms) of the frames sent, over the last second or two. */
export function rateOf(times, now) {
  const recent = times.filter((t) => now - t <= 2000);
  if (recent.length < 2) return 0;
  return (1000 * (recent.length - 1)) / Math.max(1, recent[recent.length - 1] - recent[0]);
}

/** The badge's model: `head` (the viewer's 4×4, column-major) times a
 *  translation by BADGE_OFFSET, so it stays at the upper left of the view. */
export function badgeModel(head, offset = BADGE_OFFSET) {
  const m = Float32Array.from(head);
  const [x, y, z] = offset;
  for (let r = 0; r < 3; r++) m[12 + r] = head[r] * x + head[4 + r] * y + head[8 + r] * z + head[12 + r];
  return m;
}

/** The worker's encoder (a module worker with an OffscreenCanvas), or the
 *  page's own canvas when the browser has no such worker. encode(buf, w, h,
 *  quality) -> Promise<{jpeg: ArrayBuffer, buf (handed back), ms}>. */
export function createEncoder({ makeWorker, makeCanvas } = {}) {
  let worker = null, broken = false, nextId = 1;
  const waiting = new Map();
  try {
    worker = makeWorker ? makeWorker() : null;
  } catch { worker = null; }
  if (worker) {
    worker.onmessage = (e) => {
      const m = e.data || {};
      const p = waiting.get(m.id);
      if (!p) return;
      waiting.delete(m.id);
      if (m.unsupported) {
        // No OffscreenCanvas able to make a JPEG in a worker: the page's canvas from now on.
        broken = true;
        onPage(m.buf, m.w, m.h, m.quality).then(p.resolve, p.reject);
      } else if (m.error) p.reject(Object.assign(new Error(m.error), { buf: m.buf }));
      else p.resolve({ jpeg: m.jpeg, buf: m.buf, ms: m.ms, where: 'worker' });
    };
    worker.onerror = () => { broken = true; for (const p of waiting.values()) p.reject(new Error('the encoder worker failed')); waiting.clear(); };
  }
  let canvas = null;
  async function onPage(buf, w, h, quality) {
    const t0 = performance.now();
    if (!canvas) canvas = makeCanvas ? makeCanvas(w, h) : null;
    if (!canvas) throw new Error('no canvas to encode the frame');
    if (canvas.width !== w || canvas.height !== h) { canvas.width = w; canvas.height = h; }
    const img = new ImageData(w, h);
    flipRows(new Uint8Array(buf), w, h, new Uint8Array(img.data.buffer));
    canvas.getContext('2d').putImageData(img, 0, 0);
    const blob = canvas.convertToBlob ? await canvas.convertToBlob({ type: 'image/jpeg', quality })
      : await new Promise((res) => canvas.toBlob(res, 'image/jpeg', quality));
    if (!blob) throw new Error('the frame was not encoded');
    return { jpeg: await blob.arrayBuffer(), buf, ms: performance.now() - t0, where: 'page' };
  }
  return {
    get where() { return worker && !broken ? 'worker' : 'page'; },
    encode(buf, w, h, quality) {
      if (!worker || broken) return onPage(buf, w, h, quality);
      const id = nextId++;
      return new Promise((resolve, reject) => {
        waiting.set(id, { resolve, reject });
        worker.postMessage({ id, buf, w, h, quality }, [buf]);
      });
    },
    close() { try { if (worker) worker.terminate(); } catch { /* gone */ } worker = null; },
  };
}

/**
 * The page's controller. ctx: {
 *   gl, renderer (drawPanel, createPanel, updatePanel, releasePanel),
 *   auth() -> {token, session} | null, client() -> the hub client id,
 *   log(level, event, message), makeWorker?, makeCanvas?, document?,
 *   fetch?, now? (tests) }.
 * Per rendered frame the page calls frame(...) once, after the scene; with
 * the XR frame's head pose it may add badge(head) to its overlays.
 */
export function createSpectator(ctx) {
  const gl = ctx.gl;
  const R = ctx.renderer;
  const doFetch = ctx.fetch || ((...a) => pageFetch(...a));
  const now = ctx.now || (() => performance.now());
  const log = (level, event, message) => { try { ctx.log && ctx.log(level, event, message); } catch { /* the log's own */ } };
  const st = {
    watching: false, fps: DEFAULT_FPS, width: DEFAULT_WIDTH,
    stage: '',                   // '' idle, 'gpu' (fence pending), 'read' (the copy out of the PBO), 'encode', 'post'
    sent: 0, stored: 0, dropped: 0, failures: 0, lastError: '', why: '',
    size: [0, 0], bytes: 0, eye: 'left', checked: false, pausedUntil: 0, pauses: 0,
  };
  // Where the bytes are copied out of the PBO once the fence has passed (the
  // copy waits for the GPU process to reach it in its queue): 'idle', a task
  // of its own right after the frame; 'start', at the start of the next
  // frame (frameStart()); 'frame', inside frame(). ?debug&specread= picks one.
  const readAt = ['idle', 'start', 'frame'].includes(ctx.readAt) ? ctx.readAt : 'idle';
  const issueMs = new Ring(), readMs = new Ring(), encodeMs = new Ring(), postMs = new Ring(), gpuWaitFrames = new Ring();
  const sentTimes = [];
  const pacer = new Pacer();
  let res = null;                // {fbo, rb, pbo, w, h, bytes}
  let fence = null, fenceFrames = 0, pending = null;   // pending: {w, h, eye, issue}
  const pool = [];               // ArrayBuffers handed back by the encoder
  let encoder = null;
  let badge = null;              // {handle, canvas}
  let domBadge = null;
  let gen = 0;                   // bumped by each start/stop: work of an earlier watch is dropped

  function ensureResources(w, h) {
    if (res && res.w === w && res.h === h) return res;
    freeResources();
    const fbo = gl.createFramebuffer(), rb = gl.createRenderbuffer(), pbo = gl.createBuffer();
    gl.bindRenderbuffer(gl.RENDERBUFFER, rb);
    gl.renderbufferStorage(gl.RENDERBUFFER, gl.RGBA8, w, h);
    gl.bindRenderbuffer(gl.RENDERBUFFER, null);
    // Bound as the draw framebuffer only: the frame's own FRAMEBUFFER binding
    // (its READ side) is put back by issue().
    gl.bindFramebuffer(gl.DRAW_FRAMEBUFFER, fbo);
    gl.framebufferRenderbuffer(gl.DRAW_FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.RENDERBUFFER, rb);
    gl.bindBuffer(gl.PIXEL_PACK_BUFFER, pbo);
    gl.bufferData(gl.PIXEL_PACK_BUFFER, w * h * 4, gl.STREAM_READ);
    gl.bindBuffer(gl.PIXEL_PACK_BUFFER, null);
    res = { fbo, rb, pbo, w, h, bytes: w * h * 4 };
    return res;
  }

  function freeResources() {
    if (fence) { try { gl.deleteSync(fence); } catch { /* lost */ } fence = null; }
    if (!res) return;
    try { gl.deleteFramebuffer(res.fbo); gl.deleteRenderbuffer(res.rb); gl.deleteBuffer(res.pbo); } catch { /* lost */ }
    res = null;
    pool.length = 0;
  }

  function showBadge(on) {
    const doc = ctx.document || (typeof document !== 'undefined' ? document : null);
    if (doc && doc.body) {
      if (on && !domBadge) {
        domBadge = doc.createElement('div');
        domBadge.id = 'spectator-badge';
        domBadge.textContent = '● Watched from the desktop';
        domBadge.setAttribute('role', 'status');
        domBadge.style.cssText = 'position:fixed;top:6px;right:8px;z-index:50;padding:2px 8px;border-radius:10px;'
          + 'background:rgba(160,20,20,0.85);color:#fff;font:12px system-ui,sans-serif;pointer-events:none';
        doc.body.appendChild(domBadge);
      } else if (!on && domBadge) {
        domBadge.remove();
        domBadge = null;
      }
    }
    if (!on && badge) { try { R.releasePanel(badge.handle); } catch { /* lost */ } badge = null; }
  }

  function badgePanel() {
    if (badge) return badge;
    const doc = ctx.document || (typeof document !== 'undefined' ? document : null);
    if (!doc || !R.createPanel) return null;
    const c = doc.createElement('canvas');
    [c.width, c.height] = BADGE_PX;
    const g = c.getContext('2d');
    g.fillStyle = 'rgba(30,30,30,0.78)';
    g.beginPath();
    if (g.roundRect) g.roundRect(0, 0, c.width, c.height, 36); else g.rect(0, 0, c.width, c.height);
    g.fill();
    g.fillStyle = '#e53935';
    g.beginPath();
    g.arc(40, 40, 18, 0, 2 * Math.PI);
    g.fill();
    g.fillStyle = '#ffffff';
    g.font = '600 30px system-ui, sans-serif';
    g.textBaseline = 'middle';
    g.fillText('Desktop watching', 72, 42);
    const handle = R.createPanel();
    R.updatePanel(handle, c);
    badge = { handle, canvas: c };
    return badge;
  }

  function start(p) {
    const was = st.watching;
    st.watching = true;
    st.fps = p.fps;
    st.width = p.width;
    pacer.fps = p.fps;
    st.failures = 0;
    st.pausedUntil = 0;
    st.why = '';
    if (!was) {
      gen++;
      st.checked = false;
      if (!encoder) encoder = createEncoder({ makeWorker: ctx.makeWorker, makeCanvas: ctx.makeCanvas });
      showBadge(true);
      log('info', 'spectator-on', `the desktop watches this headset: ${p.fps} fps, ${p.width} px wide`);
    }
  }

  /** Watching ends (the desktop's command, its absence, or failures). */
  function stop(why = 'the desktop stopped watching') {
    if (!st.watching) return;
    st.watching = false;
    st.why = why;
    gen++;
    pending = null;
    st.stage = '';
    freeResources();
    showBadge(false);
    for (const r of [issueMs, readMs, encodeMs, postMs, gpuWaitFrames]) r.clear();
    sentTimes.length = 0;
    log('info', 'spectator-off', why);
  }

  /** The desktop's `watch` command. */
  function command(cmd) {
    const p = watchParams(cmd);
    if (p.on) start(p); else stop();
  }

  /** The badge as an overlay of this frame ({id, model, handle, size}), or null when not watched. */
  function badgeOverlay(head) {
    if (!st.watching || !head) return null;
    const b = badgePanel();
    return b ? { id: 'spectator-badge', model: badgeModel(head), handle: b.handle, size: BADGE_SIZE } : null;
  }

  /** Issues one capture: blit, panels, readPixels into the PBO, fence. */
  function issue(src) {
    const rect = captureRect(src.vp, src.proj, src.crop == null ? CROP : src.crop, st.width);
    const r = ensureResources(rect.w, rect.h);
    gl.bindFramebuffer(gl.READ_FRAMEBUFFER, src.fbo);
    gl.bindFramebuffer(gl.DRAW_FRAMEBUFFER, r.fbo);
    const [x0, y0, x1, y1] = rect.src;
    gl.blitFramebuffer(x0, y0, x1, y1, 0, 0, r.w, r.h, gl.COLOR_BUFFER_BIT, gl.LINEAR);
    gl.bindFramebuffer(gl.FRAMEBUFFER, r.fbo);
    if (src.overlays && src.overlays.length && src.view) {
      // The composition layers, as the compositor draws them: over the scene, back to front.
      gl.viewport(...rect.panelViewport);
      for (const o of src.overlays) if (o && o.handle && o.handle.tex) R.drawPanel(src.view, o.model, o.handle, o.size);
    }
    gl.bindBuffer(gl.PIXEL_PACK_BUFFER, r.pbo);
    gl.pixelStorei(gl.PACK_ALIGNMENT, 4);
    gl.readPixels(0, 0, r.w, r.h, gl.RGBA, gl.UNSIGNED_BYTE, 0);
    gl.bindBuffer(gl.PIXEL_PACK_BUFFER, null);
    fence = gl.fenceSync(gl.SYNC_GPU_COMMANDS_COMPLETE, 0);
    gl.flush();
    fenceFrames = 0;
    // What the frame was drawing into, as it was.
    gl.bindFramebuffer(gl.FRAMEBUFFER, src.fbo);
    if (src.restoreViewport) gl.viewport(...src.restoreViewport);
    pending = { w: r.w, h: r.h, eye: src.eye || 'left' };
    st.stage = 'gpu';
    if (!st.checked) {
      // Once per watch: a blit the browser refuses (an unexpected multisampled
      // source) would otherwise send black frames for ever.
      st.checked = true;
      const err = gl.getError();
      if (err !== gl.NO_ERROR) {
        stop(`the capture failed (GL error 0x${err.toString(16)}): not watched`);
        log('warning', 'spectator-capture-failed', `GL error 0x${err.toString(16)} on the first capture`);
      }
    }
  }

  /** The fence has passed: the bytes are to be read (where: readAt). */
  function startRead() {
    gl.deleteSync(fence);
    fence = null;
    st.stage = 'read';
    if (readAt === 'idle') {
      const p = pending, myGen = gen;
      setTimeout(() => {
        if (myGen !== gen || pending !== p || st.stage !== 'read') return;
        try { readAll(); } catch (e) { stop(`the capture failed: ${e && e.message ? e.message : e}`); }
      }, 0);
    } else if (readAt === 'frame') {
      readAll();
    }
  }

  /** The bytes out of the PBO (no wait for the GPU: the fence has passed), to the encoder. */
  function readAll() {
    const p = pending;
    const bytes = p.w * p.h * 4;
    let buf = pool.pop();
    if (!buf || buf.byteLength !== bytes) buf = new ArrayBuffer(bytes);
    const t = now();
    gl.bindBuffer(gl.PIXEL_PACK_BUFFER, res.pbo);
    gl.getBufferSubData(gl.PIXEL_PACK_BUFFER, 0, new Uint8Array(buf));
    gl.bindBuffer(gl.PIXEL_PACK_BUFFER, null);
    p.read = now() - t;
    readMs.push(p.read);
    st.stage = 'encode';
    encodeAndPost(buf, p, gen);
  }

  /** At the start of a rendered frame, before anything is drawn (readAt 'start'). */
  function frameStart() {
    if (!st.watching || st.stage !== 'read' || readAt !== 'start') return;
    try { readAll(); } catch (e) { stop(`the capture failed: ${e && e.message ? e.message : e}`); }
  }

  async function encodeAndPost(buf, p, myGen) {
    try {
      const t0 = now();
      const out = await encoder.encode(buf, p.w, p.h, QUALITY);
      if (out.buf && out.buf.byteLength === p.w * p.h * 4 && pool.length < 2) pool.push(out.buf);
      if (myGen !== gen) return;
      encodeMs.push(out.ms != null ? out.ms : now() - t0);
      st.stage = 'post';
      await post(out.jpeg, p, myGen);
    } catch (e) {
      if (e && e.buf && pool.length < 2) pool.push(e.buf);
      if (myGen !== gen) return;
      fail(e && e.message ? e.message : String(e));
    } finally {
      if (myGen === gen) { st.stage = ''; pending = null; }
    }
  }

  function fail(msg) {
    st.failures++;
    st.lastError = String(msg).slice(0, 160);
    if (st.failures === 1) log('warning', 'spectator-frame-failed', st.lastError);
    if (st.failures >= MAX_FAILURES) {
      st.failures = 0;
      st.pauses++;
      st.pausedUntil = now() + PAUSE_MS;
      log('warning', 'spectator-paused', `${MAX_FAILURES} frames in a row not sent (${st.lastError}): next try in ${PAUSE_MS / 1000} s`);
    }
  }

  async function post(jpeg, p, myGen) {
    const a = ctx.auth && ctx.auth();
    if (!a) { stop('no backend session'); return; }
    const cost = Math.max(p.issue || 0, p.read || 0);
    const q = frameQuery(ctx.client(), p.eye, cost);
    const ac = typeof AbortController !== 'undefined' ? new AbortController() : null;
    const timer = setTimeout(() => { if (ac) ac.abort(); }, POST_TIMEOUT_MS);
    const t0 = now();
    let resp;
    try {
      resp = await doFetch(apiUrl(`/v1/vr/headset/spectator/frame?${q}`), {
        method: 'POST', body: jpeg, signal: ac ? ac.signal : undefined,
        headers: { 'X-Visivo-Token': a.token, 'X-Visivo-Session': a.session, 'Content-Type': 'image/jpeg' } });
    } finally {
      clearTimeout(timer);
    }
    if (myGen !== gen) return;
    postMs.push(now() - t0);
    st.sent++;
    st.bytes = jpeg.byteLength;
    if (resp.status === 404) { stop('the hub does not know this page (it says hello again)'); return; }
    if (resp.status === 429) { st.dropped++; return; }
    if (resp.status !== 200) { fail(`frame post: ${resp.status}`); return; }
    const ack = await resp.json();
    if (myGen !== gen) return;
    st.failures = 0;
    if (!ack.watched) { stop('nobody watches this headset any more'); return; }
    if (ack.stored) { st.stored++; sentTimes.push(now()); if (sentTimes.length > 64) sentTimes.shift(); } else st.dropped++;
  }

  /**
   * Once per rendered frame, after the scene and before its end. src: {
   *   fbo (the framebuffer drawn into: the XR layer's, or null for the canvas),
   *   vp {x, y, width, height} (the eye's viewport), proj, view ({proj, view}
   *   for the panels), eye ('left' | 'mono'), overlays (the composition layers
   *   of this frame, to draw into the copy; null when they are in the scene),
   *   crop (1 for the 2-D page), restoreViewport? }.
   * Costs nothing when not watched; never waits for the GPU.
   */
  function frame(src) {
    if (!st.watching || !gl) return;
    const t0 = now();
    try {
      if (st.stage === 'gpu' && fence) {
        fenceFrames++;
        const s = gl.clientWaitSync(fence, 0, 0);
        if (s === gl.ALREADY_SIGNALED || s === gl.CONDITION_SATISFIED) {
          gpuWaitFrames.push(fenceFrames);
          startRead();
        } else if (s === gl.WAIT_FAILED) {
          fail('the capture fence failed');
          gl.deleteSync(fence);
          fence = null;
          st.stage = '';
          pending = null;
        }
        return;
      }
      if (st.stage || t0 < st.pausedUntil || !pacer.due(t0)) return;
      pacer.mark(t0);
      // The first capture of a watch is checked for a GL error (issue()):
      // errors left by earlier work are read off first, so they are not its own.
      if (!st.checked) for (let i = 0; i < 4 && gl.getError() !== gl.NO_ERROR; i++) { /* drained */ }
      issue(src);
      if (pending) { pending.issue = now() - t0; issueMs.push(pending.issue); }
      st.eye = src.eye || 'left';
    } catch (e) {
      stop(`the capture failed: ${e && e.message ? e.message : e}`);
    }
  }

  function hudLines() {
    if (!st.watching) return st.why && st.sent ? [`spectator off: ${st.why}`.slice(0, 70)] : [];
    const f = (r) => (r.mean == null ? 'n/a' : `${r.mean.toFixed(2)}/${r.max.toFixed(2)}`);
    const rate = rateOf(sentTimes, now());
    return [
      `spectator ${rate.toFixed(1)}/${st.fps} fps ${pending ? `${pending.w}×${pending.h}` : res ? `${res.w}×${res.h}` : ''} ${(st.bytes / 1024).toFixed(0)} KB, ${st.stored} sent, ${st.dropped} dropped`,
      `capture ms mean/max: issue ${f(issueMs)} read ${f(readMs)} (${readAt}, fence ${gpuWaitFrames.mean == null ? 'n/a' : gpuWaitFrames.mean.toFixed(1)} frames)`,
      `encode ${encodeMs.mean == null ? 'n/a' : encodeMs.mean.toFixed(1)} ms (${encoder ? encoder.where : 'n/a'}), post ${postMs.mean == null ? 'n/a' : postMs.mean.toFixed(1)} ms${st.lastError ? `; ${st.lastError.slice(0, 40)}` : ''}`,
    ];
  }

  /** For the checks and the measurements (?debug). */
  function stats() {
    const r = (x) => ({ mean: x.mean, max: x.max, n: x.v.length });
    return { watching: st.watching, fps: st.fps, width: st.width, stage: st.stage, sent: st.sent, stored: st.stored,
      dropped: st.dropped, failures: st.failures, pauses: st.pauses, lastError: st.lastError, why: st.why, bytes: st.bytes,
      size: res ? [res.w, res.h] : null, rate: rateOf(sentTimes, now()), encoder: encoder ? encoder.where : '',
      readAt, issueMs: r(issueMs), readMs: r(readMs),
      encodeMs: r(encodeMs), postMs: r(postMs), fenceFrames: r(gpuWaitFrames) };
  }

  return { st, command, stop, frame, frameStart, badge: badgeOverlay, hudLines, stats, get watching() { return st.watching; } };
}

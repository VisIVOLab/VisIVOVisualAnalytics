// Refinement (issue #170): the page's own render session on the backend, the
// poses sent to it while the view is still or slow, the stereo frames that
// come back, and their blending into the headset's rendering. The headset's
// own rendering is always drawn first: nothing here holds a frame back, and
// every network step is asynchronous.

import * as M from './mat4.js';
import * as F from './refine.js';
import { RefineDraw } from './refine-gl.js';
import { RtcStereo } from './refine-webrtc.js';
import { apiUrl, wsUrl } from './base-path.js';
import { showLimits, reprojectionHud, frameReprojection } from './depth-mode.js';
import { LinkPolicy } from './link-policy.js';


export const REFINE_DEFAULTS = {
  scale: 1,            // remote eye size / the eye buffer (XR) or canvas (desktop)
  quality: 95,         // JPEG
  depthScale: 4,
  sampleDistance: 0.5, // voxels
  maxOutstanding: 2,
  maxEyePx: 2560,
  transport: 'auto',   // 'auto' | 'webrtc' | 'jpeg': the colour as H.264 video (issue #189)
  adaptLink: true,     // the window (and, if `link` asks, the eye size) from the measured round trip
  link: {},            // LinkPolicy options (link-policy.js LINK_DEFAULTS)

};

export class Refiner {
  /**
   * `gl`; `backend()`: the page's Backend (token, VisIVO session) or null.
   */
  constructor(gl, backend, opts = {}) {
    this.gl = gl;
    this.backend = backend;
    const { draw: _draw, ...settings } = opts;
    this.o = { ...REFINE_DEFAULTS, ...settings };
    this.draw = opts.draw || new RefineDraw(gl);   // opts.draw: a stand-in, for tests without GL
    this.rtc = new RtcStereo({ send: (c) => this._send(c), onFrame: (f) => this._onVideoFrame(f),
      onFailed: () => this._videoLost(), mode: this.o.transport });
    this.motion = new F.MotionDetector();
    this.book = new F.PoseBook();
    this.enabled = false;
    this.datasetId = null;
    this.sessionId = null;
    this.ws = null;
    this.streaming = false;
    this.status = 'off';
    this.error = '';
    this.unavailable = '';
    this.version = 0;          // of the look (transfer function, crop): a frame of another is dropped
    this.key = '';
    this.sentKey = null;
    this.keySentAt = 0;
    this.lastSentA = null;
    this.weight = 0;
    this.useDepth = true;
    this.incoming = null;
    this.decoding = 0;
    this.newestDecoded = 0;
    this.stats = {
      arrivals: new F.Rolling(120), latency: new F.Rolling(120), network: new F.Rolling(120),
      stopFirst: new F.Rolling(50), stopFull: new F.Rolling(50), upload: new F.Rolling(60),
      decode: new F.Rolling(60), lastServer: null, lastKB: 0, lastStopFirst: NaN, lastStopFull: NaN,
      sent: 0, received: 0, dropped: 0,
    };
    this.stop = { at: null, first: false };
    this.lastT = 0;
    this._keepalive = null;
    this.gen = 0;              // bumped when the session is closed: late events of the old one are ignored
    this.onEvent = null;       // optional hook: (name, data) for measurements
    this.paused = false;       // measurements: no automatic requests
    this.frozen = false;       // measurements: the frame on show kept as it is (no fade, no forgetting)
    this.waiters = new Map();  // pose id -> resolve, for one-off frames (measurements)
    this.suspended = '';       // why the page draws no refined frame now (passthrough), or ''
    this._retry = null;
    // A far backend (issue #187): the window and the eye size follow the
    // measured round trip.
    this.link = new LinkPolicy(this.o.link);
    this.linkState = this.link.decide();
    this.failures = 0;         // connection attempts failed in a row: the retry backs off (#166)
    this.retryAt = 0;          // when the next attempt is due (performance.now()), 0 = none
    this.onLifecycle = null;   // optional hook: (name, detail) for the page's diagnostics
  }

  /** The wait before the next attempt: 2, 4, 8, 16, 30, 30… s. */
  _backoffMs() {
    this.failures++;
    return Math.min(30000, 2000 * 2 ** (this.failures - 1));
  }

  // ── Lifecycle ─────────────────────────────────────────────────────────────

  /**
   * The cube on show changed: its dataset id (null for a synthetic one), and
   * whether the backend would draw it out of core (reduced_info's
   * render_out_of_core): such a session gives no stereo frames yet, and asking
   * for one would start building the cube's brick pyramid, so none is asked.
   */
  async setDataset(datasetId, { outOfCore = false } = {}) {
    // The new cube first, so an attempt still running for the old one sees
    // the change when it ends (its gen and dataset are both stale).
    const closing = this._closeSession();
    this.datasetId = datasetId || null;
    this.unavailable = !datasetId ? 'no backend cube (synthetic)'
      : outOfCore ? 'the backend draws this cube out of core: no refined frames yet' : '';
    this.error = '';
    this.sentKey = null;
    await closing;
    this._connect();
  }

  /** Off: the session is closed (its render slot freed); on: a new one. */
  setEnabled(on) {
    if (on === this.enabled) return;
    this.enabled = on;
    if (on) {
      this.lastSentA = null;
      this.error = '';
      this._connect();
    } else {
      this._closeSession();
    }
  }

  /** After the page comes back from the back-forward cache (pagehide closed the session). */
  resume() { this._connect(); }

  /**
   * Why no refined frame can be shown now (passthrough, the benchmark), or ''
   * to resume. Suspended, the render session is closed, so it holds no
   * render slot (nor GPU memory) on the backend; resuming creates a new one.
   * Cheap to call every frame with the same reason.
   */
  setSuspended(reason) {
    reason = reason || '';
    if (reason === this.suspended) return;
    const was = this.suspended;
    this.suspended = reason;
    if (reason && !was) {
      this._closeSession();
    } else if (!reason) {
      this.lastSentA = null;
      this._connect();
    }
  }

  _wanted() { return this.enabled && !!this.datasetId && !this.unavailable && !this.suspended; }

  _retryLater(ms) {
    clearTimeout(this._retry);
    this.retryAt = performance.now() + ms;
    this._retry = setTimeout(() => { this._retry = null; this.retryAt = 0; this._connect(); }, ms);
  }

  /** Try now (the backend is back): the wait is cut short, the count kept. */
  retrySoon() {
    if (this._retry && this._wanted()) { clearTimeout(this._retry); this._retry = null; this.retryAt = 0; this._connect(); }
  }

  /** Gives refinement up for this cube: the session is closed, nothing retried. */
  _giveUp(reason) {
    this.unavailable = reason;
    this._closeSession();
  }

  async _createSession(gen) {
    const b = this.backend();
    if (!b) throw new Error('no backend');
    let body = { dataset_id: this.datasetId, queue: true };
    for (;;) {
      const r = await b._json('POST', '/v1/render/create_session', body);
      if (r.valid && r.session_id) return r.session_id;
      if (r.queued && r.queue_id) {
        this.status = `queued (${r.position} of ${r.queue_length})`;
        body = { ...body, queue_id: r.queue_id };
      } else if (r.preparing) {
        // Out of core (a backend that does not say so in reduced_info): a
        // pyramid build has started, but its session could give no stereo
        // frames; not followed.
        const e = new Error('the backend draws this cube out of core: no refined frames yet');
        e.giveUp = true;
        throw e;
      } else {
        throw new Error(r.error || 'no render session');
      }
      await new Promise((res) => setTimeout(res, 1000 * (r.poll_after_seconds || 2)));
      if (!this._wanted() || gen !== this.gen || body.dataset_id !== this.datasetId) {
        if (body.queue_id) {
          b._json('DELETE', `/v1/render/queue/${body.queue_id}`).catch(() => {});
        }
        throw new Error('cancelled');
      }
    }
  }

  async _connect() {
    if (!this._wanted() || this.ws || this._connecting) return;
    clearTimeout(this._retry);
    this._retry = null;
    this._connecting = true;
    const datasetId = this.datasetId, gen = this.gen;
    try {
      this.status = 'connecting';
      if (!this.sessionId) {
        const sid = await this._createSession(gen);
        if (gen !== this.gen || datasetId !== this.datasetId || !this._wanted()) {
          // The cube changed, or refinement was turned off, meanwhile.
          this._deleteSession(sid);
          return;
        }
        this.sessionId = sid;
      }
      const b = this.backend();
      // The token as a subprotocol ("visivo-token.<token>", the backend answers
      // "visivo.v1"), so it is in no access log; in the address only when it
      // has characters a subprotocol may not carry.
      const base = wsUrl(`/v1/render/${this.sessionId}/ws`);   // with the page's prefix (#187)
      const ws = F.subprotocolSafe(b.token)
        ? new WebSocket(base, ['visivo.v1', `visivo-token.${b.token}`])
        : new WebSocket(`${base}?token=${encodeURIComponent(b.token)}`);
      ws.binaryType = 'arraybuffer';
      this.ws = ws;
      let opened = false;
      // Every handler first: a socket that is no longer the refiner's (closed
      // for a new cube, replaced by a reconnection) must not touch the state
      // of the one that replaced it, whatever order the events arrive in.
      ws.addEventListener('open', () => {
        if (ws !== this.ws) return;
        opened = true;
        if (this.failures && this.onLifecycle) this.onLifecycle('refine-reconnected', `after ${this.failures} failed attempt(s)`);
        this.failures = 0;
        this.status = 'waiting';
        ws.send(JSON.stringify({ type: 'stream', window: this._window(), max_fps: 72 }));
        this.rtc.connected();

        this.sentKey = null;
        this.lastSentA = null;
        clearInterval(this._keepalive);
        this._keepalive = setInterval(() => this._send({ type: 'keepalive' }), 60000);
      });
      ws.addEventListener('message', (e) => { if (ws === this.ws) this._onMessage(e.data); });
      ws.addEventListener('close', (e) => {
        if (ws !== this.ws) return;
        this.ws = null;
        clearInterval(this._keepalive);
        this.streaming = false;
        this.book.clear();
        if (this.ws === null) this.rtc.close();
        if (e.code === 1013) this.error = 'the render session is in use by another client';
        // A handshake refused: the session is gone (idle timeout) or not ours.
        if (!opened && gen === this.gen) this.sessionId = null;
        if (gen === this.gen && this._wanted() && this.datasetId === datasetId) {
          // The frame on show was rendered for a stream that is gone: the
          // headset's own rendering is shown until a new one arrives.
          this.draw.forget();
          this.weight = 0;
          this.status = 'reconnecting';
          const ms = this._backoffMs();
          if (this.onLifecycle) this.onLifecycle('refine-ws-closed', `code ${e.code}${opened ? '' : ' (handshake refused)'}, retry in ${(ms / 1000).toFixed(0)} s`);
          this._retryLater(ms);
        }
      });
    } catch (err) {
      // An attempt made stale meanwhile (a cube change, off, suspended) says
      // nothing about the session on show now.
      if (gen !== this.gen) return;
      this.error = String(err.message || err);
      if (err.giveUp || F.renderUnavailable(this.error)) {
        // No render sessions on this backend, or none this cube can use. (A
        // dataset the backend no longer holds is not that: the page reopens
        // it, setDataset() then connects for the new id; until then, retries.)
        this._giveUp(err.giveUp ? this.error : `no render sessions on this backend (${this.error})`);
      } else if (this._wanted() && datasetId === this.datasetId && this.error !== 'cancelled') {
        this.status = 'unavailable';
        this._retryLater(this._backoffMs());
      }
    } finally {
      this._connecting = false;
      // A cube change or a re-enable that came while this attempt was running
      // found it busy and returned: connect for the cube on show now.
      if ((gen !== this.gen || datasetId !== this.datasetId) && this._wanted() && !this.ws) this._connect();
    }
  }

  async _closeSession({ unloading = false } = {}) {
    this.gen++;
    clearTimeout(this._retry);
    this._retry = null;
    this.retryAt = 0;
    clearInterval(this._keepalive);
    const ws = this.ws, sid = this.sessionId;
    this.ws = null;
    this.sessionId = null;
    this.streaming = false;
    this.book.clear();
    this.rtc.close();
    this.draw.forget();
    this.weight = 0;
    // A new session is measured afresh (it may be another backend's).
    this.link.reset();
    this.linkState = this.link.decide();
    // A frame decoded for the session closed here is not shown afterwards.
    if (this.incoming) { if (this.incoming.bitmap) this.incoming.bitmap.close(); this.incoming = null; }
    if (ws) { try { ws.close(); } catch { /* closed */ } }
    await this._deleteSession(sid, { keepalive: unloading });
  }

  /**
   * Frees the render slot. `keepalive` only when the page is going away (the
   * request must outlive it): a keepalive request goes through the browser
   * process, and to a backend that is going down it was seen to stall a
   * headless page; otherwise a plain request, which the backend's idle
   * timeout backs up if it fails.
   */
  async _deleteSession(sid, { keepalive = false } = {}) {
    const b = this.backend();
    if (!sid || !b) return;
    try {
      await fetch(apiUrl(`/v1/render/${sid}`), { method: 'DELETE', headers: b.headers(false), keepalive });
    } catch { /* the idle timeout closes it */ }
  }

  /** On page unload: the session is closed without waiting. */
  closeNow() { this._closeSession({ unloading: true }); }

  _send(cmd) {
    if (this.ws && this.ws.readyState === WebSocket.OPEN) {
      this.ws.send(JSON.stringify(cmd));
      return true;
    }
    return false;
  }

  // ── Frames from the backend ───────────────────────────────────────────────

  _onMessage(data) {
    if (typeof data === 'string') {
      let m;
      try { m = JSON.parse(data); } catch { return; }
      if (m.stream === 'on') { this.streaming = true; return; }
      if (typeof m.type === 'string' && m.type.startsWith('xr_rtc_')) { this.rtc.onMessage(m); return; }
      if (m.type === 'xr_error') {
        if (Number.isInteger(m.pose_id)) this.book.take(m.pose_id);
        this.error = m.error || 'stereo frame refused';
        if (stereoRefused(m)) {
          // This session can give no stereo frames: its slot is freed.
          this._giveUp(this.error);
        }
      }
      return;
    }
    this._send({ type: 'frame_ack' });
    const tRecv = performance.now();
    if (!F.isVXR1(data)) return;           // the mono view's first frame
    let parsed;
    try { parsed = F.parseVXR1(data); } catch (err) { this.error = String(err.message || err); return; }
    const h = parsed.header;
    if (this.waiters.has(h.pose_id)) {
      // A one-off frame (a reference for a measurement): handed over, not shown.
      const resolve = this.waiters.get(h.pose_id);
      this.waiters.delete(h.pose_id);
      this.book.take(h.pose_id);
      resolve({ header: h, color: parsed.color.slice(), depth: parsed.depth ? parsed.depth.slice() : null });
      return;
    }
    const rec = this.book.take(h.pose_id);
    this.stats.received++;
    if (!this._sameRequest(rec)) { this.stats.dropped++; return; }

    this.stats.lastServer = h.timing_ms || null;
    if (this.o.adaptLink) {
      this.link.push(tRecv - rec.tSent, h.timing_ms);
      this.linkState = this.link.decide();
      if (this.linkState.changed) this._send({ type: 'stream', window: this._window(), max_fps: 72 });
    }
    this.stats.lastKB = (parsed.color.byteLength + (parsed.depth ? parsed.depth.byteLength : 0)) / 1024;
    if (h.color && h.color.format === 'h264-webrtc') {
      // The colour comes as video (refine-webrtc.js): this header and its
      // depth wait there for the frame of their pose id. A header the video
      // no longer takes (given up meanwhile) leaves its pose unshown.
      if (!this.rtc.onHeader(parsed, rec, tRecv)) this._videoLost();
      return;
    }
    this.stats.arrivals.push(tRecv);
    this.stats.network.push(tRecv - rec.tSent);
    // Network time of a JPEG frame (what it took beside the backend's own
    // work): it decides whether the video is worth it.
    this.rtc.observeJpeg(tRecv - rec.tSent - ((h.timing_ms && h.timing_ms.total) || 0));
    this.decoding++;
    this._decode(parsed, rec, tRecv).catch((err) => { this.error = `decode: ${err.message || err}`; })
      .finally(() => { this.decoding--; });
  }

  /**
   * The video was given up (back to JPEG), or a video header came after it
   * was: the poses whose headers waited for their video frame were taken
   * from the book when the headers arrived, so a JPEG of the same pose would
   * be dropped, and a still view sends no new pose by itself. Forgetting the
   * last pose sent makes the next still frame ask again, now as a JPEG.
   */
  _videoLost() {
    this.lastSentA = null;
  }

  /** A frame of the video, paired with its header (refine-webrtc.js). */
  _onVideoFrame({ bitmap, vScale, header, depthBytes, rec, tRecv }) {
    const tVideo = performance.now();
    this.stats.arrivals.push(tVideo);
    this.stats.network.push(tVideo - rec.tSent);
    this.decoding++;
    this._decode({ header, color: null, depth: depthBytes }, rec, tRecv, bitmap, vScale)
      .catch((err) => { this.error = `decode: ${err.message || err}`; })
      .finally(() => { this.decoding--; });
  }

  /** Was `rec` asked for the look, cube and session on show now? */
  _sameRequest(rec) {
    return !!rec && rec.version === this.version && rec.gen === this.gen
      && rec.datasetId === this.datasetId && rec.sessionId === this.sessionId;
  }

  /** The frame's JPEG (or its video frame, `bitmap`, whose top `vScale` is
   *  the eyes) and its depth decoded, then shown. */
  async _decode(parsed, rec, tRecv, bitmap0 = null, vScale = 1) {

    const h = parsed.header;
    const [bm, dp] = await Promise.allSettled([
      bitmap0 || createImageBitmap(new Blob([parsed.color], { type: 'image/jpeg' }),
        { premultiplyAlpha: 'none', colorSpaceConversion: 'none' }),
      parsed.depth && h.depth
        ? F.inflate(parsed.depth).then((raw) => F.decodeDepth(raw, h.depth.width, h.depth.height, h.depth.z_near, h.depth.z_far))
        : Promise.resolve(null),
    ]);
    if (bm.status !== 'fulfilled' || dp.status !== 'fulfilled') {
      if (bm.status === 'fulfilled') bm.value.close();      // never leak a decoded image
      throw (bm.status === 'rejected' ? bm.reason : dp.reason);
    }
    const bitmap = bm.value, depth = dp.value;
    const tDone = performance.now();
    // Checked again after the decode: the cube, the session or the look may
    // have changed while it ran.
    if (rec.id < this.newestDecoded || !this._sameRequest(rec)) { bitmap.close(); this.stats.dropped++; return; }
    this.newestDecoded = rec.id;
    if (this.incoming && this.incoming.bitmap) this.incoming.bitmap.close();
    this.incoming = { bitmap, depth, header: h, rec, tRecv, tDone, vScale };
    this.stats.decode.push(tDone - tRecv);
    this.stats.latency.push(tDone - rec.tSent);
  }

  // ── Every displayed frame ─────────────────────────────────────────────────

  /**
   * Called once per displayed frame, before drawing. `ctx`: {t (ms), eyes:
   * [{eye, view, proj, width, height}] (two; the same twice in a desktop
   * browser), aCentre (view * model of the head), model, zStretch, look:
   * {tf, crop} (session commands from refine.js), lookKey, background (the
   * colour the page clears to: the backend composites over it), and for a
   * point table (issue #190) look.commands and poseExtra (data_to_model)}.
   */
  frame(ctx) {
    if (this.frozen) return;
    const t = ctx.t;
    const dt = this.lastT ? Math.min(100, t - this.lastT) : 0;
    this.lastT = t;
    const was = this.motion.state;
    const state = this.motion.push(t, ctx.aCentre);
    if (was === 'moving' && state !== 'moving') this.stop = { at: t, first: false, full: false };

    // The look: a change makes every frame in flight or on show stale.
    if (ctx.lookKey !== this.key) {
      this.key = ctx.lookKey;
      this.version++;
      this.book.clear();
      this.lastSentA = null;
      if (this.incoming) { if (this.incoming.bitmap) this.incoming.bitmap.close(); this.incoming = null; }
    }
    const active = this.enabled && this.ws && this.streaming && !this.unavailable;
    if (active) this.rtc.tick(t);
    if (active && this.sentKey !== this.key && t - this.keySentAt > 100) {
      // At most ten looks a second while a slider moves; the last always goes.
      // A clipping box the backend would refuse is not sent: no pose either
      // (its frames would show the wrong box), and the panel says why.
      this.keySentAt = t;
      if (ctx.look.cropError) {
        this.error = `clipping box not sent: ${ctx.look.cropError}`;
      } else {
        // A cube's look is its transfer function and crop; a point table's
        // (issue #190) a list of the point session's commands.
        for (const cmd of ctx.look.commands || [ctx.look.tf, ctx.look.crop]) this._send(cmd);
        this.sentKey = this.key;
        if (this.error.startsWith('clipping box')) this.error = '';
      }
    }
    if (this.incoming) {
      const up = this.draw.upload(this.incoming);
      this.stats.upload.push(up);
      this.incoming = null;
    }

    // Ask for a frame of this pose: still or slow, the look sent, few in flight.
    if (active && !this.paused && state !== 'moving' && this.sentKey === this.key
        && F.poseChanged(this.lastSentA, ctx.aCentre, 0.2, 0.0015)
        && this.book.outstanding(t) < this._window()) {
      this._sendPose(ctx, t);
    }

    const shown = this.draw.shown;
    const usable = !!(shown && this.enabled && shown.rec.version === this.version
      && F.frameUsable(shown.rec.aCentre, ctx.aCentre, state, showLimits(shown.header, F.SHOW_DEFAULTS)));
    this.weight = F.fadeStep(this.weight, usable, dt);
    if (!usable && this.weight <= 0 && shown) {
      // Faded out: forgotten, and the next still pose is asked for again even
      // if it is the one this frame was for.
      this.draw.forget();
      this.lastSentA = null;
    }
    if (this.weight > 0 && this.stop.at !== null && shown && shown.rec.tSent >= this.stop.at) {
      if (!this.stop.first) {
        this.stop.first = true;
        this.stats.lastStopFirst = t - this.stop.at;
        this.stats.stopFirst.push(this.stats.lastStopFirst);
        if (this.onEvent) this.onEvent('refined-first', { ms: this.stats.lastStopFirst });
      }
      if (this.weight >= 1 && !this.stop.full) {
        this.stop.full = true;
        this.stats.lastStopFull = t - this.stop.at;
        this.stats.stopFull.push(this.stats.lastStopFull);
        if (this.onEvent) this.onEvent('refined-full', { ms: this.stats.lastStopFull });
      }
    }
    if (!this.enabled) this.status = 'off';
    else if (this.unavailable) this.status = 'unavailable';
    else if (this.ws && this.streaming) this.status = this.weight > 0 ? 'refined' : 'waiting';
  }

  /** Poses in flight, and the backend's ack window. */
  _window() {
    return this.o.adaptLink ? Math.max(this.o.maxOutstanding, this.linkState.window) : this.o.maxOutstanding;
  }

  _eyeSize(eyes) {
    const w = Math.max(...eyes.map((e) => e.width)), h = Math.max(...eyes.map((e) => e.height));
    const cap = Math.min(this.o.maxEyePx, this.o.adaptLink ? this.linkState.maxEyePx : Infinity);
    const s = Math.min(this.o.scale, cap / w, cap / h);
    return [Math.max(16, Math.round(w * s)), Math.max(16, Math.round(h * s))];
  }

  _sendPose(ctx, t, extra = {}, size = null) {
    const [width, height] = size || this._eyeSize(ctx.eyes);
    // Left first, as the backend lays the frame out (eye index 0 = left).
    const eyes = [...ctx.eyes].sort((a, b) => (a.eye === 'right') - (b.eye === 'right'))
      .map((e) => ({ eye: e.eye, view: Float32Array.from(e.view), proj: Float32Array.from(e.proj) }));
    const id = this.book.add({
      tSent: t, version: this.version, eyes, model: Float32Array.from(ctx.model),
      aCentre: Float32Array.from(ctx.aCentre),
      // Which session asked: a frame decoded after a cube change or a new
      // session (gen bumped by _closeSession) is never shown.
      gen: this.gen, datasetId: this.datasetId, sessionId: this.sessionId,
    });
    const ok = this._send({
      type: 'xr_pose', pose_id: id, t_client: Date.now() / 1000,
      model: Array.from(ctx.model), z_stretch: ctx.zStretch,
      views: eyes.map((e) => ({ eye: e.eye, view: Array.from(e.view), projection: Array.from(e.proj), width, height })),
      depth: this.o.depthScale ? { scale: this.o.depthScale } : false,
      quality: this.o.quality, sample_distance: this.o.sampleDistance,
      background: ctx.background || [0, 0, 0], ...(ctx.poseExtra || {}), ...extra,
    });
    if (ok) { this.stats.sent++; this.lastSentA = Float32Array.from(ctx.aCentre); }
    else this.book.take(id);
    return id;
  }

  /**
   * One frame for this pose with other settings (`extra`: quality,
   * sample_distance; `size`: [w, h] per eye), resolved with {header, color,
   * depth} and never shown: the reference of a quality comparison.
   */
  requestOnce(ctx, extra = {}, size = null, timeoutMs = 20000) {
    return new Promise((resolve, reject) => {
      if (!(this.ws && this.streaming) || this.sentKey !== this.key) { reject(new Error('not connected, or the look not sent yet')); return; }
      // As a JPEG even while the colour goes as video: handed over whole.
      const id = this._sendPose(ctx, performance.now(), { frame_color: 'jpeg', ...extra }, size);
      this.lastSentA = null;
      this.waiters.set(id, resolve);
      setTimeout(() => { if (this.waiters.delete(id)) reject(new Error('no frame')); }, timeoutMs);
    });
  }

  /** Will composite() draw this frame? (The page then leaves the selection
   *  out of its own volume and draws it once, over the refined frame.) */
  get showing() {
    return this.enabled && !this.suspended && this.weight > 0 && !!this.draw.shown;
  }

  /** Blends the refined frame into eye `eyeIndex` (after the local volume). */
  composite(view, model, eyeIndex) {
    if (!this.showing) return false;
    return this.draw.draw(view, model, eyeIndex, this.weight, this.useDepth);
  }

  // ── HUD ───────────────────────────────────────────────────────────────────

  hudLines(t = performance.now()) {
    const s = this.stats;
    const f0 = (v) => (Number.isFinite(v) ? v.toFixed(0) : '-');
    if (!this.enabled) return ['refine off'];
    if (this.suspended) return [`refine off ${this.suspended}`];
    if (this.unavailable) return [`refine unavailable: ${this.unavailable}`.slice(0, 60)];
    const shown = this.draw.shown;
    const age = shown ? t - shown.rec.tSent : NaN;
    const head = this.status === 'refined' ? `refined ${(100 * this.weight).toFixed(0)} %` : this.status;
    const lines = [
      `refine ${head}  ${this.motion.state}  age ${f0(age)} ms  remote ${s.arrivals.rate(t).toFixed(1)} fps`,
      `  pose->frame ${f0(s.latency.quantile(0.5))} ms p50 ${f0(s.latency.quantile(0.95))} p95`
        + `  stop->refined ${f0(s.lastStopFirst)} ms (full ${f0(s.lastStopFull)})`,
    ];
    const srv = s.lastServer;
    if (srv) lines.push(`  server ${f0(srv.render)} ms render  ${f0(srv.total)} total  ${s.lastKB.toFixed(0)} KB  upload ${s.upload.quantile(0.5).toFixed(1)} ms`);
    const via = this.rtc.hudLine();
    if (via) lines.push(via);
    if (this.o.adaptLink) lines.push(this.link.hudLine(this.linkState));
    if (shown) lines.push(...reprojectionHud(shown.header));

    if (this.error && this.status !== 'refined') lines.push(`  ${this.error}`.slice(0, 60));
    return lines;
  }

  /** Numbers for measurements (debug hooks). */
  summary(t = performance.now()) {
    const s = this.stats;
    const q = (r) => ({ n: r.length, p50: r.quantile(0.5), p95: r.quantile(0.95) });
    return {
      status: this.status, motion: this.motion.state, weight: this.weight,
      remoteFps: s.arrivals.rate(t), latency: q(s.latency), network: q(s.network), decode: q(s.decode),
      upload: q(s.upload), stopFirst: q(s.stopFirst), stopFull: q(s.stopFull),
      lastServer: s.lastServer, lastKB: s.lastKB, sent: s.sent, received: s.received, dropped: s.dropped,
      skipped: this.book.skipped, error: this.error, transport: this.rtc.summary(),
      reprojection: this.draw.shown ? frameReprojection(this.draw.shown.header) : null,
      link: { ...this.linkState, maxEyePx: Number.isFinite(this.linkState.maxEyePx) ? this.linkState.maxEyePx : null },

    };
  }
}

/**
 * Does this xr_error mean the session gives no stereo frames at all (its slot
 * is then freed)? The backend's code (issue #190): "out_of_core" or
 * "unsupported"; from a backend without codes, its texts for the same two.
 */
export function stereoRefused(m) {
  if (m && (m.code === 'out_of_core' || m.code === 'unsupported')) return true;
  if (m && m.code) return false;
  return /out of core|drawn for cubes only/i.test(String((m && m.error) || ''));
}

/** The page's eye views for a desktop browser: the mono camera as both eyes. */
export function desktopEyes(v, width, height) {
  return ['left', 'right'].map((eye) => ({ eye, view: v.view, proj: v.proj, width, height }));
}

export { M };

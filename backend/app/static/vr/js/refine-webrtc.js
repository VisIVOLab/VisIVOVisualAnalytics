// Refined stereo frames as WebRTC (H.264) video (issue #189). The backend's
// render session sends the two eyes side by side as one video frame, with a
// stripe under them that holds the frame's pose id; the frame's header and
// depth image still come over the WebSocket (a VXR1 message whose colour is
// "h264-webrtc", with no colour payload). The page reads the stripe of every
// decoded frame, pairs it with the header of that pose id, crops the stripe
// off and hands both to the refiner as a JPEG frame would be: the pairing is
// exact by construction, whatever the transport drops or repeats.
//
// The pure parts (the stripe, the pairing, when to switch) have no DOM and
// are tested by node (backend/tests/vr/refine-webrtc.test.mjs); RtcStereo is
// the browser side (RTCPeerConnection, MediaStreamTrackProcessor or a video
// element with requestVideoFrameCallback). Protocol and stripe:
// docs/backend-api/render.md, "Stereo frames as WebRTC video".

// ── The pose id stripe (backend: render_session/_stereo_webrtc.py) ──────────

export const STRIPE = {
  start: [1, 1, 0, 1], end: [0, 1, 0, 0], idBits: 32, crcBits: 8, cells: 48, minCell: 8,
  // Rows from the bottom of the video frame to the row read: inside the
  // stripe whether it has 16 rows (even eye height) or 17 (odd).
  readFromBottom: 8,
};

/** CRC-8, polynomial 0x07, initial value 0 (CRC-8/SMBUS). */
export function crc8(bytes) {
  let crc = 0;
  for (const b of bytes) {
    crc ^= b;
    for (let i = 0; i < 8; i++) crc = crc & 0x80 ? ((crc << 1) ^ 0x07) & 0xff : (crc << 1) & 0xff;
  }
  return crc;
}

const be32 = (v) => [(v >>> 24) & 0xff, (v >>> 16) & 0xff, (v >>> 8) & 0xff, v & 0xff];

/** Width of one cell of a frame this wide; 0 when it is too narrow. */
export function stripeCell(frameWidth) {
  const cell = Math.floor(frameWidth / STRIPE.cells);
  return cell >= STRIPE.minCell ? cell : 0;
}

/** The 48 cells for a pose id (its low 32 bits), as the backend draws them. */
export function stripeBits(poseId) {
  const low = Number(BigInt.asUintN(32, BigInt(poseId)));
  const crc = crc8(be32(low));
  const bits = [...STRIPE.start];
  for (let i = STRIPE.idBits - 1; i >= 0; i--) bits.push((low >>> i) & 1);
  for (let i = STRIPE.crcBits - 1; i >= 0; i--) bits.push((crc >> i) & 1);
  return [...bits, ...STRIPE.end];
}

/**
 * The pose id (low 32 bits) from one row of grey levels across the frame
 * (0..255, length = the frame's width), or null when it does not decode:
 * each cell is the mean of its middle half, thresholded at 128; the start
 * and end patterns and the CRC must match.
 */
export function readStripe(gray) {
  const cell = stripeCell(gray.length);
  if (!cell) return null;
  const lo = Math.floor(cell / 4), hi = cell - Math.floor(cell / 4);
  const bits = [];
  for (let i = 0; i < STRIPE.cells; i++) {
    let s = 0;
    for (let x = i * cell + lo; x < i * cell + hi; x++) s += gray[x];
    bits.push(s / (hi - lo) >= 128 ? 1 : 0);
  }
  const n0 = STRIPE.start.length, ne = STRIPE.end.length;
  if (!STRIPE.start.every((b, i) => bits[i] === b) || !STRIPE.end.every((b, i) => bits[STRIPE.cells - ne + i] === b)) return null;
  let id = 0;
  for (let i = 0; i < STRIPE.idBits; i++) id = id * 2 + bits[n0 + i];
  let crc = 0;
  for (let i = 0; i < STRIPE.crcBits; i++) crc = (crc << 1) | bits[n0 + STRIPE.idBits + i];
  return crc === crc8(be32(id)) ? id : null;
}

/** Grey levels of one RGBA row (an ImageData's data, one row tall). */
export function grayRow(rgba, width) {
  const g = new Float32Array(width);
  for (let x = 0; x < width; x++) g[x] = (rgba[4 * x] + rgba[4 * x + 1] + rgba[4 * x + 2]) / 3;
  return g;
}

/**
 * Grey levels (0..255) of the first row of a VideoFrame.copyTo() buffer:
 * `format` the frame's (VideoPixelFormat), `layout` the plane layouts
 * copyTo() resolved with. YUV formats give their Y plane (limited range,
 * 16..235, stretched to 0..255); RGB ones the mean of R, G and B. Null for
 * a format not known here.
 */
export function grayFromCopy(buf, format, layout, width) {
  const f = String(format || '').toUpperCase();
  const { offset } = layout[0];
  const g = new Float32Array(width);
  if (/^(I420|I420A|I422|I444|NV12|I420P10|I420AP10)$/.test(f)) {
    if (/P10$/.test(f)) return null;
    for (let x = 0; x < width; x++) g[x] = Math.max(0, Math.min(255, (buf[offset + x] - 16) * 255 / 219));
    return g;
  }
  if (/^(RGBA|RGBX|BGRA|BGRX)$/.test(f)) {
    for (let x = 0; x < width; x++) {
      const i = offset + 4 * x;
      g[x] = (buf[i] + buf[i + 1] + buf[i + 2]) / 3;
    }
    return g;
  }
  return null;
}

/** The low 32 bits of a pose id, the key a video frame is paired by. */
export const videoKey = (poseId) => Number(BigInt.asUintN(32, BigInt(poseId)));

// ── Pairing headers with video frames ───────────────────────────────────────

/**
 * Headers (from the WebSocket) and decoded video frames, paired by pose id.
 * Either may come first. A header waits for its frame until a frame of a
 * newer pose comes (a frame lost on the network, or dropped with a link
 * that went down, never comes); a frame waits for its
 * header for `holdMs` (only the newest is kept). A frame of a pose already
 * shown (a keyframe resend) is a repeat, not shown again. `close(item)` is
 * called on every frame that is not handed out (an ImageBitmap's close).
 */
export class FramePairer {
  constructor({ maxHeaders = 8, holdMs = 1000, close = (v) => v && v.close && v.close() } = {}) {
    this.maxHeaders = maxHeaders;
    this.holdMs = holdMs;
    this.close = close;
    this.headers = new Map();      // key -> entry, in arrival order
    this.video = null;             // {key, frame, t}
    this.lastKey = -1;             // the newest pose paired
    this.stats = { paired: 0, headersDropped: 0, videoDropped: 0, repeats: 0, unreadable: 0 };
  }

  /** A header entry ({key, ...}); returns a pair {entry, frame, t} or null. */
  addHeader(entry, t) {
    this.headers.set(entry.key, entry);
    while (this.headers.size > this.maxHeaders) {
      this.headers.delete(this.headers.keys().next().value);
      this.stats.headersDropped++;
    }
    if (this.video && this.video.key === entry.key) {
      const v = this.video;
      this.video = null;
      return this._pair(entry.key, v.frame, v.t);
    }
    this._expire(t);
    return null;
  }

  /** A decoded frame whose stripe read `key` (null: unreadable). */
  addVideo(key, frame, t) {
    if (key === null) { this.stats.unreadable++; this.close(frame); return null; }
    if (key === this.lastKey && !this.headers.has(key)) { this.stats.repeats++; this.close(frame); return null; }
    if (this.headers.has(key)) return this._pair(key, frame, t);
    if (this.video) { this.stats.videoDropped++; this.close(this.video.frame); }
    this.video = { key, frame, t };
    return null;
  }

  _pair(key, frame, t) {
    const entry = this.headers.get(key);
    // Older headers will get no frame: the backend has moved on.
    for (const k of [...this.headers.keys()]) {
      this.headers.delete(k);
      if (k === key) break;
      this.stats.headersDropped++;
    }
    this.lastKey = key;
    this.stats.paired++;
    return { entry, frame, t };
  }

  _expire(t) {
    if (this.video && t - this.video.t > this.holdMs) {
      this.stats.videoDropped++;
      this.close(this.video.frame);
      this.video = null;
    }
  }

  /** Waiting headers. */
  get waiting() { return this.headers.size; }

  /** When the oldest waiting header arrived (its `tRecv`), or null. */
  get oldestHeaderAt() {
    const first = this.headers.values().next();
    return first.done ? null : first.value.tRecv ?? null;
  }

  clear() {
    this.headers.clear();
    if (this.video) { this.close(this.video.frame); this.video = null; }
  }
}

// ── When to use the video ───────────────────────────────────────────────────

export const TRANSPORT_DEFAULTS = {
  slowMs: 40,          // auto: video when the JPEG frames' network time is above this (median)
  frames: 8,           // JPEG frames looked at before deciding
  retryMs: 60000,      // after a failure of the video, JPEG for this long
};

/**
 * Chooses between the JPEG frames of the WebSocket and the WebRTC video.
 * 'jpeg': never video; 'webrtc': video at once; 'auto': video when the link
 * is slow, measured on the JPEG frames: their network time (pose sent to
 * frame received, less the backend's own time) has a median above `slowMs`.
 * After the video fails (no WebRTC in the browser or the backend, no
 * connection, no frame decoded), JPEG for `retryMs` ('webrtc': until the
 * session is opened again).
 */
export class TransportChooser {
  constructor(mode = 'auto', o = {}) {
    this.mode = ['auto', 'webrtc', 'jpeg'].includes(mode) ? mode : 'auto';
    this.o = { ...TRANSPORT_DEFAULTS, ...o };
    this.network = [];
    this.failedAt = -Infinity;
    this.reason = '';
  }

  /** One JPEG frame: its network time in ms (and its size, for the record). */
  observe(networkMs) {
    if (!Number.isFinite(networkMs)) return;
    this.network.push(networkMs);
    if (this.network.length > 4 * this.o.frames) this.network.shift();
  }

  medianNetwork() {
    const v = this.network.slice(-this.o.frames).sort((a, b) => a - b);
    return v.length ? v[Math.floor(v.length / 2)] : NaN;
  }

  /** Should the video be (or stay) on now? */
  wantVideo(t) {
    if (this.mode === 'jpeg') return false;
    if (t - this.failedAt < (this.mode === 'webrtc' ? Infinity : this.o.retryMs)) return false;
    if (this.mode === 'webrtc') return true;
    return this.network.length >= this.o.frames && this.medianNetwork() > this.o.slowMs;
  }

  failed(t, reason) {
    this.failedAt = t;
    this.reason = reason;
    this.network = [];
  }

  /** A new session: measured and decided again. */
  reset() {
    this.network = [];
    if (this.mode === 'webrtc') this.failedAt = -Infinity;
  }
}

// ── The browser side ────────────────────────────────────────────────────────

const median = (v) => (v.length ? [...v].sort((a, b) => a - b)[v.length >> 1] : NaN);

/**
 * The video link of one render-session WebSocket. `send(cmd)`: a command on
 * the WebSocket; `onFrame({bitmap, header, depthBytes, rec, tRecv, tVideo})`:
 * a paired frame, its stripe cropped off; `now()`: ms.
 */
export class RtcStereo {
  /** Frames in a row whose stripe does not read (or of the wrong size) before the video is given up. */
  static MAX_BAD_IN_A_ROW = 10;
  /** Frames that threw while read or shown before the video is given up. */
  static MAX_ERRORS = 5;

  constructor({ send, onFrame, mode = 'auto', now = () => performance.now(), crf, maxKbps } = {}) {
    this.send = send;
    this.onFrame = onFrame;
    this.now = now;
    this.chooser = new TransportChooser(mode);
    this.settings = { crf, max_kbps: maxKbps };
    this.pairer = new FramePairer();
    this.pc = null;
    this.state = 'off';          // off | offering | connecting | video | failed
    this.gen = 0;
    this.lastVideoAt = 0;
    this.firstHeaderAt = 0;
    this.bitrate = { kbps: NaN, bytes: 0, t: 0, framesDecoded: 0, framesDropped: 0, decodeMs: NaN, jitterMs: NaN };
    this.stripeMs = [];          // reading a frame's stripe (ms)
    this.badInARow = 0;          // frames unreadable or of the wrong size, in a row
    this.errors = 0;             // frames that threw while read or shown
    this.lastError = '';
    this.lastPairedAt = 0;
    this.lagMs = [];
    this._stats = null;
    this.encoder = '';
  }

  get active() { return this.state === 'video'; }

  /** The WebSocket is (again) open and streaming: decide afresh. */
  connected() { this.close(); this.chooser.reset(); this.maybeStart(); }

  /** A JPEG frame arrived: its network time feeds the choice. */
  observeJpeg(networkMs) {
    this.chooser.observe(networkMs);
    this.maybeStart();
  }

  maybeStart() {
    if (this.state === 'off' && this.chooser.wantVideo(this.now())) this.start();
  }

  _fail(reason) {
    if (this.state === 'off' || this.state === 'failed') return;
    this.chooser.failed(this.now(), reason);
    this.close();
    this.state = 'failed';
    this.send({ type: 'xr_rtc_stop' });
  }

  async start() {
    const gen = ++this.gen;
    if (typeof RTCPeerConnection !== 'function') { this.state = 'offering'; this._fail('no WebRTC in this browser'); return; }
    this.state = 'offering';
    try {
      const pc = new RTCPeerConnection({ iceServers: [] });
      this.pc = pc;
      const tr = pc.addTransceiver('video', { direction: 'recvonly' });
      try {
        const caps = RTCRtpReceiver.getCapabilities && RTCRtpReceiver.getCapabilities('video');
        const h264 = caps ? caps.codecs.filter((c) => /h264/i.test(c.mimeType)) : [];
        if (h264.length && tr.setCodecPreferences) tr.setCodecPreferences(h264);
      } catch { /* the browser keeps its order; the backend answers H.264 */ }
      // Show a frame as soon as it is decoded: no playout delay.
      if ('jitterBufferTarget' in tr.receiver) tr.receiver.jitterBufferTarget = 0;
      else if ('playoutDelayHint' in tr.receiver) tr.receiver.playoutDelayHint = 0;
      pc.addEventListener('track', (e) => this._read(e.track, gen));
      pc.addEventListener('connectionstatechange', () => {
        if (gen !== this.gen) return;
        if (pc.connectionState === 'connected') { this.state = 'video'; this.lastVideoAt = this.now(); }
        else if (pc.connectionState === 'failed' || pc.connectionState === 'closed') this._fail(`WebRTC ${pc.connectionState}`);
      });
      await pc.setLocalDescription(await pc.createOffer());
      // Host candidates only (no STUN): gathering ends at once on a LAN.
      await new Promise((resolve) => {
        if (pc.iceGatheringState === 'complete') { resolve(); return; }
        const done = () => { if (pc.iceGatheringState === 'complete') resolve(); };
        pc.addEventListener('icegatheringstatechange', done);
        setTimeout(resolve, 1500);
      });
      if (gen !== this.gen) return;
      this.state = 'connecting';
      this.connectingSince = this.now();
      this.send({ type: 'xr_rtc_offer', sdp: pc.localDescription.sdp,
        ...Object.fromEntries(Object.entries(this.settings).filter(([, v]) => v !== undefined)) });
    } catch (err) {
      if (gen === this.gen) this._fail(`WebRTC: ${err.message || err}`);
    }
  }

  /** xr_rtc_answer / xr_rtc_error from the WebSocket. */
  async onMessage(m) {
    const gen = this.gen;
    if (m.type === 'xr_rtc_error') { this._fail(m.error || 'refused by the backend'); return; }
    if (m.type === 'xr_rtc_answer' && this.pc) {
      try {
        await this.pc.setRemoteDescription({ type: m.sdp_type || 'answer', sdp: m.sdp });
      } catch (err) {
        if (gen === this.gen) this._fail(`WebRTC answer: ${err.message || err}`);
      }
    }
  }

  /**
   * A VXR1 header of a video frame (its colour "h264-webrtc"): waits for
   * the frame with its pose id. `rec`: the pose (refine-client's PoseBook).
   * Ignored unless the link is up (a header racing the link's own state).
   */
  onHeader(parsed, rec, tRecv) {
    if (this.state !== 'video') return;
    const h = parsed.header;
    if (!this.firstHeaderAt) this.firstHeaderAt = tRecv;
    const entry = { key: h.video ? h.video.id : videoKey(h.pose_id), header: h,
      depthBytes: parsed.depth ? parsed.depth.slice() : null, rec, tRecv };
    const pair = this.pairer.addHeader(entry, tRecv);
    if (pair) this._deliver(pair).catch((err) => this._error(err));
  }

  /** A frame that could not be read or shown; repeated, the video is given up. */
  _bad(reason) {
    this.badInARow++;
    if (this.badInARow >= RtcStereo.MAX_BAD_IN_A_ROW) this._fail(`${RtcStereo.MAX_BAD_IN_A_ROW} video frames in a row ${reason}`);
  }

  _error(err) {
    this.errors++;
    this.lastError = String((err && err.message) || err);
    if (this.errors >= RtcStereo.MAX_ERRORS) this._fail(`video frames failed: ${this.lastError}`);
  }

  /**
   * One decoded frame (a VideoFrame, or an ImageBitmap of the video
   * element's frame) and its stripe's grey row: paired, or kept, or closed.
   */
  async _handle(frame, gray, gen) {
    if (gen !== this.gen) { frame.close(); return; }
    const t = this.now();
    this.lastVideoAt = t;
    const key = gray ? readStripe(gray) : null;
    if (key === null) this._bad('without a readable pose id');
    const pair = this.pairer.addVideo(key, frame, t);
    if (pair) await this._deliver(pair);
  }

  async _read(track, gen) {
    if (typeof MediaStreamTrackProcessor === 'function') {
      // Every decoded frame, in order, without a video element (which an
      // immersive session does not composite, so its callbacks may stop).
      // The VideoFrame itself goes to the texture; its stripe is read by
      // copying out two rows only.
      const reader = new MediaStreamTrackProcessor({ track }).readable.getReader();
      this._reader = reader;
      for (;;) {
        const { value: frame, done } = await reader.read().catch(() => ({ done: true }));
        if (done || gen !== this.gen) { if (frame) frame.close(); break; }
        try {
          const t0 = this.now();
          const gray = await this._stripeOfVideoFrame(frame);
          this.stripeMs.push(this.now() - t0);
          if (this.stripeMs.length > 60) this.stripeMs.shift();
          await this._handle(frame, gray, gen);
        } catch (err) {
          try { frame.close(); } catch { /* closed already */ }
          this._error(err);
        }
      }
      return;
    }
    const video = document.createElement('video');
    Object.assign(video, { muted: true, playsInline: true, autoplay: true });
    video.style.cssText = 'position:fixed;left:0;top:0;width:2px;height:2px;opacity:0;pointer-events:none';
    document.body.appendChild(video);
    this._video = video;
    video.srcObject = new MediaStream([track]);
    video.play().catch(() => {});
    if (typeof video.requestVideoFrameCallback !== 'function') { this._fail('no way to read the video frames'); return; }
    const onVideo = async () => {
      if (gen !== this.gen) return;
      let bitmap = null;
      try {
        bitmap = await createImageBitmap(video);
        await this._handle(bitmap, this._stripeOfBitmap(bitmap), gen);
      } catch (err) {
        if (bitmap) bitmap.close();
        this._error(err);
      } finally {
        if (gen === this.gen) video.requestVideoFrameCallback(onVideo);
      }
    };
    video.requestVideoFrameCallback(onVideo);
  }

  /** The stripe's grey row of a VideoFrame: two rows copied out, no image. */
  async _stripeOfVideoFrame(frame) {
    // The visible rectangle, not the coded size: a decoder rounds the coded
    // size up to whole macroblocks (1830 rows become 1840), and the stripe is
    // at the bottom of what is visible.
    const vr = frame.visibleRect || { x: 0, y: 0, width: frame.displayWidth, height: frame.displayHeight };
    this.lastFrame = { format: frame.format, coded: [frame.codedWidth, frame.codedHeight], visible: [vr.x, vr.y, vr.width, vr.height] };
    if (!this._copyFails && frame.format) {
      try {
        const y = (vr.y + vr.height - STRIPE.readFromBottom) & ~1;   // 4:2:0: even rows
        const rect = { x: vr.x & ~1, y, width: vr.width, height: 2 };
        const size = frame.allocationSize({ rect });
        if (!this._copyBuf || this._copyBuf.length < size) this._copyBuf = new Uint8Array(size);
        const layout = await frame.copyTo(this._copyBuf, { rect });
        const gray = grayFromCopy(this._copyBuf, frame.format, layout, vr.width);
        if (gray) return gray;
      } catch {
        // A hardware decoder's frame (MediaCodec on a headset) may refuse a
        // copy: drawn through an image from now on.
      }
      this._copyFails = true;
    }
    this.stripeFallback = true;
    return this._stripeOfBitmap(await createImageBitmap(frame), true);
  }

  /** The stripe's grey row of an ImageBitmap, through a one-row canvas. */
  _stripeOfBitmap(bitmap, close = false) {
    const w = bitmap.width;
    if (!this._canvas || this._canvas.width !== w) {
      this._canvas = typeof OffscreenCanvas === 'function' ? new OffscreenCanvas(w, 1)
        : Object.assign(document.createElement('canvas'), { width: w, height: 1 });
      this._ctx = this._canvas.getContext('2d', { willReadFrequently: true });
    }
    this._ctx.drawImage(bitmap, 0, bitmap.height - STRIPE.readFromBottom, w, 1, 0, 0, w, 1);
    if (close) bitmap.close();
    return grayRow(this._ctx.getImageData(0, 0, w, 1).data, w);
  }

  /**
   * A paired frame to the refiner, whole: the texture shows only the eyes'
   * rows (`vScale`, the eye height over the frame's), so nothing is copied.
   */
  async _deliver({ entry, frame, t }) {
    const h = entry.header;
    const eyeH = h.color.height;
    const fw = frame.displayWidth || frame.width, fh = frame.displayHeight || frame.height;
    if (fw !== h.color.width || fh !== eyeH + h.color.stripe.rows) {
      frame.close();
      this.pairer.stats.unreadable++;
      this._bad('of the wrong size');
      return;
    }
    this.badInARow = 0;
    this.lastPairedAt = this.now();
    // How much later than its header the frame was decoded (the video's own
    // delay beyond the WebSocket's: encode, RTP, jitter buffer, decode).
    this.lagMs.push(t - entry.tRecv);
    if (this.lagMs.length > 60) this.lagMs.shift();
    this.onFrame({ bitmap: frame, vScale: eyeH / fh, header: h, depthBytes: entry.depthBytes,
      rec: entry.rec, tRecv: entry.tRecv, tVideo: t });
  }

  /** Called every displayed frame: the watchdogs and the link's numbers. */
  tick(t) {
    if (this.state === 'connecting' && t - this.connectingSince > 5000) this._fail('WebRTC did not connect within 5 s');
    // Headers come but no frame pairs with them for 3 s: no H.264 decoder,
    // blocked UDP, or frames whose stripe does not read.
    const oldest = this.pairer.oldestHeaderAt;
    if (this.state === 'video' && oldest !== null && t - oldest > 3000 && this.lastPairedAt < oldest) {
      this._fail(this.lastVideoAt < oldest ? 'no video frame decoded for 3 s' : 'no video frame paired for 3 s');
    }
    if (this.state === 'off') this.maybeStart();
    if (this.state === 'failed' && this.chooser.wantVideo(t)) this.state = 'off';
    if (this.pc && this.state === 'video' && !this._stats && t - this.bitrate.t > 1000) {
      this._stats = this.pc.getStats().then((report) => {
        report.forEach((s) => {
          if (s.type === 'inbound-rtp' && s.kind === 'video') {
            const b = this.bitrate;
            if (b.t) b.kbps = (8 * (s.bytesReceived - b.bytes)) / (t - b.t);
            b.bytes = s.bytesReceived; b.t = t;
            b.framesDecoded = s.framesDecoded; b.framesDropped = s.framesDropped || 0;
            b.decodeMs = s.framesDecoded ? 1000 * s.totalDecodeTime / s.framesDecoded : NaN;
            b.jitterMs = s.jitterBufferEmittedCount ? 1000 * s.jitterBufferDelay / s.jitterBufferEmittedCount : NaN;
            b.codec = s.decoderImplementation || '';
          }
        });
      }).catch(() => {}).finally(() => { this._stats = null; });
    }
  }

  hudLine() {
    const f0 = (v) => (Number.isFinite(v) ? v.toFixed(0) : '-');
    if (this.state === 'video') {
      const b = this.bitrate;
      return `  via WebRTC ${(b.kbps / 1000).toFixed(1)} Mbit/s  decode ${f0(b.decodeMs)} ms  paired ${this.pairer.stats.paired}`;
    }
    if (this.chooser.mode === 'jpeg') return '';
    if (this.state === 'failed' || this.chooser.reason) return `  via JPEG (WebRTC: ${this.chooser.reason})`.slice(0, 60);
    return `  via JPEG  net ${f0(this.chooser.medianNetwork())} ms (WebRTC above ${this.chooser.o.slowMs})`;
  }

  summary() {
    return { state: this.state, mode: this.chooser.mode, reason: this.chooser.reason,
      network: this.chooser.medianNetwork(), pairer: { ...this.pairer.stats }, bitrate: { ...this.bitrate },
      stripeMs: median(this.stripeMs), videoLagMs: median(this.lagMs), badInARow: this.badInARow,
      errors: this.errors, lastError: this.lastError, stripeFallback: !!this.stripeFallback,
      lastFrame: this.lastFrame || null };
  }

  close() {
    this.gen++;
    if (this._reader) { this._reader.cancel().catch(() => {}); this._reader = null; }
    if (this._video) { this._video.srcObject = null; this._video.remove(); this._video = null; }
    if (this.pc) { try { this.pc.close(); } catch { /* closed */ } this.pc = null; }
    this.pairer.clear();
    this.firstHeaderAt = 0;
    this.badInARow = 0;
    this.errors = 0;
    if (this.state !== 'failed') this.state = 'off';
  }
}

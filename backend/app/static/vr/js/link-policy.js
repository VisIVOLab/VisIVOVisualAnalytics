// The refinement's link to a far backend (issue #187).
//
// Measured on PLEIADI (V100, EGL) through the SSH tunnel on 2026-11-11, with
// two frames in flight: 1024^2 eyes 9.1 stereo frames/s, pose->frame 175 ms
// p50 (server 115 ms); 512^2 eyes 16.4 frames/s, 122 ms (server 41 ms); about
// 80 ms of each is the tunnel's round trip. With a fixed window of two the
// rate is capped by round trip / window, and the GPU waits for the network.
//
// So, from what the page measures on every frame (pose sent -> frame
// received, minus the server's time from the frame header):
// - the window (poses in flight, and the backend's ack window) is the number
//   of frames that keeps the GPU busy over the round trip,
//   ceil((transport + work) / work), between MIN and MAX, where work is the
//   frame's render + depth + encode (its waits left out: a wider window makes
//   them longer, and counting them would shrink the window again);
// - a far link (transport above remoteEnterMs, until it falls below
//   remoteLeaveMs) may also cap each eye at remoteMaxEyePx. Off by default:
//   a smaller frame is rendered and sent sooner but, scaled up, it is worse
//   than the headset's own picture of the reduced cube, which is what it is
//   meant to improve on. Measured 2026-11-11 (HCG 44 at 3x3x1, headless
//   Chrome, a 1600 px canvas, an emulated 80 ms link), against the backend's
//   full-size reference: page alone PSNR 35.9 dB / SSIM 0.968 on the
//   emission, refined with the eye capped at 768 px 35.4 / 0.947, refined at
//   full size 43.9 / 0.983 (#170 found the same at 0.6x: SSIM 0.957).
// Pure: backend/tests/vr/link-policy.test.mjs.

export const LINK_DEFAULTS = {
  minWindow: 2,
  maxWindow: 6,          // the backend accepts 1..8
  remoteEnterMs: 30,     // transport p50 above this: a far backend
  remoteLeaveMs: 20,
  remoteMaxEyePx: Infinity,   // no cap (see above); 768 trades quality for rate
  minSamples: 5,         // frames before deciding anything
  hysteresis: 0.15,      // of the frames-in-flight ratio, around each window step
  samples: 40,
};

function median(v) {
  if (!v.length) return NaN;
  const s = [...v].sort((a, b) => a - b);
  return s[Math.floor(s.length / 2)];
}

export class LinkPolicy {
  constructor(o = {}) {
    this.o = { ...LINK_DEFAULTS, ...o };
    this.transport = [];
    this.server = [];
    this.remote = false;
    this.window = this.o.minWindow;
  }

  /** One frame: `networkMs` pose sent -> frame received (page clock), and
   *  its header's `timing_ms` ({total, render, depth, encode, ...}). */
  push(networkMs, timing) {
    const t = timing || {};
    const total = Number(t.total);
    const work = ['render', 'depth', 'encode'].reduce((a, k) => a + (Number(t[k]) || 0), 0);
    if (!Number.isFinite(networkMs) || !Number.isFinite(total) || total < 0 || !(work > 0)) return;
    for (const [arr, v] of [[this.transport, Math.max(0, networkMs - total)], [this.server, work]]) {
      arr.push(v);
      if (arr.length > this.o.samples) arr.shift();
    }
  }

  /** {window, maxEyePx (Infinity: no cap), remote, transportMs, serverMs
   *  (the work per frame), changed}: the decision for now. Nothing changes before minSamples frames. */
  decide() {
    const transportMs = median(this.transport), serverMs = median(this.server);
    const before = `${this.window}/${this.remote}`;
    if (this.transport.length >= this.o.minSamples) {
      if (!this.remote && transportMs > this.o.remoteEnterMs) this.remote = true;
      else if (this.remote && transportMs < this.o.remoteLeaveMs) this.remote = false;
      // Frames needed in flight, with a band of +-HYSTERESIS around each
      // step so a ratio near a whole number does not flap the window 3<->4.
      const ratio = (transportMs + serverMs) / Math.max(serverMs, 1);
      const need = Math.max(this.o.minWindow, Math.min(this.o.maxWindow, Math.ceil(ratio)));
      const band = this.o.hysteresis;
      if ((need > this.window && ratio > this.window + band)
          || (need < this.window && ratio < this.window - 1 - band)) this.window = need;
    }
    return {
      window: this.window,
      maxEyePx: this.remote ? this.o.remoteMaxEyePx : Infinity,
      remote: this.remote, transportMs, serverMs,
      changed: `${this.window}/${this.remote}` !== before,
    };
  }

  /** A new session: measured again from the start. */
  reset() {
    this.transport.length = 0;
    this.server.length = 0;
    this.remote = false;
    this.window = this.o.minWindow;
  }

  hudLine(d = this.decide()) {
    const f0 = (v) => (Number.isFinite(v) ? v.toFixed(0) : '-');
    return `  link ${d.remote ? 'far' : 'near'} rtt ${f0(d.transportMs)} ms  window ${d.window}`
      + `${Number.isFinite(d.maxEyePx) ? `  eye <= ${d.maxEyePx} px` : ''}`;
  }
}

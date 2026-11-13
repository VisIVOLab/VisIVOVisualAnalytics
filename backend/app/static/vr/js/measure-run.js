// The page's side of the measurement records and the guided run (issue
// #224): the recorder fed every frame by main.js, the record's header and
// samples built from the page's own state, the run's conditions applied to
// the refiner, its prompts on the message card and its pulses. The pure parts
// are measure.js and measure-protocol.js.
//
// `ctx` (from main.js): {backend() → Backend | null, page {build, api},
// backendInfo() → /v1/vr/info answer or null, gpu() → gpuInfo(), xr() → the
// XR state or null, canvas, wrist(targetHz) → FrameStats summary, refiner,
// governorState() / setGovernor(on), effectiveSpv(), state, cubeInfo(),
// lookKey(), mode() → {mode, tool}, uiFlags() → {menu, card, hud},
// notices: {show(n), clear(id)}, buzz(kind), log(level, event, message),
// paired(), hostname, headset, onChange()}.

import { MeasureRecorder, MeasureSender, recordId, linkKind, shortHash, refineFigures, videoFigures, pairFigures, newValues } from './measure.js';
import { MeasureProtocol, protocolPlan, planSeconds, CONDITIONS } from './measure-protocol.js';
import { linkFigures } from './transport.js';
import { stepInfo } from './volume.js';

/** A log message for the record: what scrubbing left of a path (its last
 *  component, after an ellipsis) goes too, so no file name is kept. */
export function recordMessage(message) {
  return String(message || '').replace(/…[\\/][^\s]*/g, '…').slice(0, 300);
}

/** Desktop auto motion (tests and desktop comparisons): yaw rate in °/s by what the run asks. */
export const AUTO_RATES = { still: 0, hold: 0, turn: 20, move: 60 };

export function createMeasure(ctx) {
  const sender = new MeasureSender({
    post: async ({ record, seq, lines }) => {
      const b = ctx.backend();
      if (!b) return { status: 0 };
      return b.measurements(record, seq, lines);
    },
  });
  let pfSeen = 0;
  let prev = null;           // the last sample's conditions, for the change events
  let headerHadBackend = false;
  let pausedAt = 0;
  const recorder = new MeasureRecorder({ sender, snapshot: () => snapshot() });
  const run = { protocol: null, restore: null, startedRecord: false, auto: false, applyGen: -1, condition: '',
    noticeId: '', yawSign: 1, signAt: 0, lastStep: null };

  function rnd8() {
    const a = new Uint8Array(4);
    (globalThis.crypto || { getRandomValues: (x) => x.map(() => Math.floor(Math.random() * 256)) }).getRandomValues(a);
    return Array.from(a, (b) => b.toString(16).padStart(2, '0')).join('');
  }

  function xrPart() {
    const x = ctx.xr();
    if (!x) return null;
    const L = x.layer;
    return { mode: x.mode === 'immersive-ar' ? 'ar' : 'vr', display_hz: x.targetHz, eye: [L.framebufferWidth, L.framebufferHeight],
      scale: x.fbScale, chosen_scale: x.fbChosen, foveation: 'fixedFoveation' in L ? L.fixedFoveation : null };
  }

  function cubePart() {
    const c = ctx.state.cube, info = ctx.cubeInfo();
    if (!c) return null;
    return {
      dataset: ctx.state.open ? ctx.state.open.dataset_id : null, synthetic: c.mode === 'synthetic',
      dims: c.dims, encoding: c.encoding, factors: c.factors, binning: c.mode, full_shape: c.fullShape || null,
      source: c.source || 'cube', pyramid_level: info && info.pyramid ? info.pyramid.level ?? null : null,
      out_of_core_level: Number.isInteger(ctx.refiner.outOfCoreLevel) ? ctx.refiner.outOfCoreLevel : null,
    };
  }

  function backendPart() {
    const i = ctx.backendInfo();
    if (!i) return null;
    return { version: i.backend_version || null, build: i.build || null, vr_api: i.vr_api ?? null };
  }

  function header() {
    const g = ctx.gpu();
    const r = ctx.refiner;
    const b = backendPart();
    headerHadBackend = !!(b && b.version);
    return {
      page: { build: ctx.page.build, api: ctx.page.api },
      backend: b,
      device: { user_agent: String(globalThis.navigator ? navigator.userAgent : '').slice(0, 300), gpu: g.renderer || null,
        max_tex3d: g.maxTex3D ?? null, max_texture: g.maxTexture ?? null, timer_query: !!g.timerQuery,
        device_memory_gb: g.deviceMemoryGB ?? null, headset: !!ctx.headset },
      link: linkKind({ paired: ctx.paired(), hostname: ctx.hostname }),
      xr: xrPart(),
      settings: { spv: ctx.state.spv, governor: ctx.state.governorOn, skip: ctx.state.skip,
        refine: { on: r.enabled, transport: r.o.transport, scale: r.o.scale, quality: r.o.quality, depth: r.useDepth } },
      cube: cubePart(),
    };
  }

  function snapshot() {
    const x = ctx.xr();
    const targetHz = x ? x.targetHz : 0;
    const r = ctx.refiner;
    const sum = r.summary();
    const lat = r.stats.latency;
    const pf = newValues(lat, lat.total, pfSeen);
    pfSeen = lat.total;
    const refine = refineFigures({ enabled: r.enabled, suspended: r.suspended, status: sum.status, motion: sum.motion,
      weight: sum.weight, transport: sum.transport, mode: r.o.transport, scale: r.o.scale, quality: r.o.quality,
      depth: r.useDepth, latency: sum.latency, remoteFps: sum.remoteFps, lastStopFirst: r.stats.lastStopFirst,
      lastStopFull: r.stats.lastStopFull, lastServer: sum.lastServer, lastKB: sum.lastKB, window: r._window() }, pf);
    const st = ctx.state;
    const c = st.cube;
    const spv = ctx.effectiveSpv();
    const display = x ? xrPart() : { mode: null, display_hz: 0, eye: [ctx.canvas.width, ctx.canvas.height], scale: null, chosen_scale: null, foveation: null };
    const cube = cubePart();
    const m = ctx.mode();
    // Changes of the conditions, as events of their own.
    const now = { via: refine.via, on: refine.on, transport: refine.mode, scale: refine.scale, quality: refine.quality,
      factors: cube ? cube.factors.join('x') : null, eye: display.eye.join('x'), hz: display.display_hz };
    if (prev) {
      for (const k of Object.keys(now)) {
        if (now[k] !== prev[k]) recorder.event('change', { what: k, from: prev[k], to: now[k] });
      }
    }
    prev = now;
    if (!headerHadBackend) {
      const b = backendPart();
      if (b && b.version) { headerHadBackend = true; recorder.event('backend', b); }
    }
    return {
      xr: x ? (x.mode === 'immersive-ar' ? 'ar' : 'vr') : null, mode: m.mode, tool: m.tool, targetHz,
      wrist: ctx.wrist(targetHz),
      display,
      render: { spv: Math.round(spv * 1000) / 1000, spv_chosen: st.spv, governor: st.governorOn, skip: st.skip,
        max_steps: c ? stepInfo(c.dims, spv).maxSteps : null },
      refine, video: videoFigures(sum.transport), pair: pairFigures(linkFigures()), cube,
      look: c ? shortHash(ctx.lookKey() || '') : null,
      ui: ctx.uiFlags(),
    };
  }

  // ── Recording ─────────────────────────────────────────────────────────────

  function startRecord(why = 'user') {
    if (recorder.on) return recorder.id;
    pfSeen = ctx.refiner.stats.latency.total;
    prev = null;
    const id = recordId(new Date(), rnd8());
    recorder.start(header(), id);
    recorder.event('record-start', { why });
    ctx.log('info', 'measure-record', `record ${id} started (${why})`);
    ctx.onChange();
    return id;
  }

  async function stopRecord(why = 'user') {
    if (!recorder.on) return;
    if (run.protocol && !run.protocol.done) cancel('the record was stopped');
    await recorder.stop(why);
    ctx.log('info', 'measure-record', `record ${recorder.id} stopped (${why}): ${recorder.samples} samples, ${sender.sentLines} lines stored, ${sender.dropped} dropped`);
    ctx.onChange();
  }

  // ── The guided run ────────────────────────────────────────────────────────

  /** Can the condition be measured yet? true, false (wait) or 'failed: …'. */
  function ready(condition) {
    const cond = CONDITIONS[condition];
    const r = ctx.refiner;
    if (!cond.refine) return true;
    if (r.unavailable) return `failed: refinement unavailable (${r.unavailable})`;
    if (r.gen !== run.applyGen || !r.ws || !r.streaming) return false;
    if (cond.transport === 'webrtc') {
      if (r.rtc.state === 'failed') return `failed: WebRTC video (${r.rtc.chooser.reason || 'failed'})`;
      return r.rtc.state === 'video';
    }
    return r.rtc.state !== 'video';
  }

  async function apply(condition) {
    const cond = CONDITIONS[condition];
    const r = ctx.refiner;
    run.condition = condition;
    r.o.scale = cond.scale || run.restore.scale;
    if (!cond.refine) {
      r.setEnabled(false);
      run.applyGen = r.gen;
      return;
    }
    // A fresh session for every refined condition: each starts the same way.
    if (!r.enabled) {
      r.o.transport = cond.transport;
      r.rtc.chooser.mode = cond.transport;
      r.setEnabled(true);
    } else {
      await r.setTransport(cond.transport);
    }
    run.applyGen = r.gen;
  }

  function onActions(actions) {
    for (const a of actions) {
      if (a.do === 'apply') {
        // The previous step's last sample closes before anything changes.
        recorder.cut();
        recorder.setProto(null);
        run.applyGen = -2;
        recorder.event('condition', { condition: a.condition, label: CONDITIONS[a.condition].label });
        apply(a.condition).catch((err) => recorder.event('condition-failed', { condition: a.condition, error: String(err.message || err).slice(0, 200) }));
      } else if (a.do === 'phase') {
        recorder.cut();               // no sample spans two steps
        recorder.setProto({ condition: a.step.condition, phase: a.step.phase, part: a.step.part });
        recorder.event('phase', { index: a.index, ms: a.step.ms });
        run.lastStep = a.step;
      } else if (a.do === 'prompt') {
        if (run.noticeId) ctx.notices.clear(run.noticeId);
        run.noticeId = a.notice.id;
        ctx.notices.show(a.notice);
      } else if (a.do === 'pulse') {
        ctx.buzz(a.kind === 'hold' ? 'press' : 'done');
        if (a.kind !== 'start') recorder.event('cue', { kind: a.kind });
      } else if (a.do === 'degraded') {
        recorder.event('condition-degraded', { condition: a.condition, phase: a.phase, part: a.part });
      } else if (a.do === 'skip') {
        recorder.event('condition-skipped', { condition: a.condition, why: a.why });
        ctx.log('warning', 'measure-skip', `${a.condition}: ${a.why}`);
      } else if (a.do === 'done') {
        finish(a.cancelled, a.why);
      }
    }
  }

  function runProtocol({ second = false, scale = 1, auto = false, conditions = null } = {}) {
    if (run.protocol && !run.protocol.done) return { ok: false, why: 'a run is under way' };
    if (!ctx.state.cube) return { ok: false, why: 'load a cube first' };
    const plan = protocolPlan({ second, scale, conditions });
    const r = ctx.refiner;
    run.restore = { enabled: r.enabled, transport: r.o.transport, scale: r.o.scale, governor: ctx.governorState() };
    run.startedRecord = !recorder.on;
    run.auto = !!auto;
    run.inVR = !!ctx.xr();
    if (!recorder.on) startRecord('protocol');
    // The governor is held off for the whole run: it would change the samples
    // per voxel and the eye buffer under one condition and keep them for the next.
    ctx.setGovernor(false);
    run.protocol = new MeasureProtocol(plan, { now: performance.now() });
    recorder.event('protocol-start', { conditions: [...new Set(plan.map((s) => s.condition))], steps: plan.length,
      seconds: planSeconds(plan), second, scale, auto: run.auto, governor_held_off: true });
    ctx.log('info', 'measure-run', `started: ${plan.length} steps, ${planSeconds(plan).toFixed(0)} s`);
    onActions(run.protocol.update(performance.now(), env()));
    ctx.onChange();
    return { ok: true, record: recorder.id, seconds: planSeconds(plan) };
  }

  function env() { return { ready, inVR: !!ctx.xr() }; }

  function cancel(why = 'cancelled') {
    if (!run.protocol || run.protocol.done) return;
    onActions(run.protocol.cancel(why));
  }

  function finish(cancelled, why) {
    recorder.cut();
    recorder.setProto(null);
    recorder.event('protocol-end', { cancelled, why, skipped: run.protocol ? run.protocol.skipped : [] });
    if (run.noticeId) { ctx.notices.clear(run.noticeId); run.noticeId = ''; }
    const r = ctx.refiner;
    const back = run.restore;
    if (back) {
      r.o.scale = back.scale;
      ctx.setGovernor(back.governor);
      if (back.enabled) {
        if (!r.enabled) { r.o.transport = back.transport; r.rtc.chooser.mode = back.transport; r.setEnabled(true); }
        else if (r.o.transport !== back.transport) r.setTransport(back.transport);
      } else {
        r.setEnabled(false);
        r.o.transport = back.transport; r.rtc.chooser.mode = back.transport;
      }
    }
    ctx.notices.show({ id: 'measure-done', level: 'info', autoHideMs: 8000,
      title: cancelled ? 'Measurement run cancelled' : 'Measurement run complete',
      lines: [cancelled ? `Stopped: ${why}. What was measured is in the record.` : 'Every condition was measured.',
        `Record ${recorder.id}${sender.file ? ` (${sender.file})` : ''}.`],
      actions: [{ id: 'dismiss', label: 'OK' }] });
    ctx.log('info', 'measure-run', `${cancelled ? 'cancelled' : 'complete'}: ${why}`);
    run.restore = null;
    const stopAfter = run.startedRecord;
    run.startedRecord = false;
    if (stopAfter) stopRecord('protocol-end');
    ctx.onChange();
  }

  // ── Every frame ───────────────────────────────────────────────────────────

  /** After the frame's stats.push: the same three numbers. */
  function frame(intervalMs, cpuMs, gpuMs) {
    if (!recorder.on) return;
    recorder.frame(intervalMs, cpuMs, gpuMs);
    if (run.protocol && !run.protocol.done) onActions(run.protocol.update(performance.now(), env()));
    recorder.tick();
    if (sender.paused) {
      const t = performance.now();
      if (!pausedAt) pausedAt = t;
      else if (t - pausedAt > 30000) { pausedAt = 0; sender.resume(); }
    } else pausedAt = 0;
  }

  /** The desktop camera's yaw change for this frame (auto motion), radians. */
  function autoYaw(dtMs) {
    if (!run.auto || !run.protocol || run.protocol.done) return 0;
    const t = performance.now();
    const m = run.protocol.motion(t);
    if (m === 'turn' && t - run.signAt > 3000) { run.yawSign = -run.yawSign; run.signAt = t; }
    if (m === 'move' && run.lastMotion !== 'move') run.yawSign = -run.yawSign;
    run.lastMotion = m;
    return run.yawSign * (AUTO_RATES[m] || 0) * Math.PI / 180 * Math.min(100, dtMs) / 1000;
  }

  return {
    recorder, sender, run,
    get recording() { return recorder.on; },
    get running() { return !!(run.protocol && !run.protocol.done); },
    startRecord, stopRecord,
    toggleRecord() { if (recorder.on) stopRecord('user'); else startRecord('user'); },
    runProtocol, cancel, frame, autoYaw,
    event(name, data) { recorder.event(name, data); },
    /** The cube changed or VR ended under a run started in VR: it stops. */
    cubeChanged() { if (this.running) cancel('the cube changed'); },
    xrEnded() { if (this.running && run.inVR) cancel('VR was left'); },
    xrStarted() { if (this.running) run.inVR = true; },
    /** One line for the page and the menu. */
    statusText() {
      if (!recorder.on) return sender.record ? `Last record ${sender.record}${sender.file ? `: ${sender.file}` : ''}, ${sender.sentLines} lines stored${sender.dropped ? `, ${sender.dropped} dropped` : ''}` : 'Not recording';
      const st = run.protocol && !run.protocol.done && run.protocol.step;
      return `Recording ${recorder.id}: ${recorder.samples} samples, ${recorder.events} events, ${sender.sentLines} lines stored`
        + `${sender.dropped ? `, ${sender.dropped} dropped` : ''}${sender.status !== 'recording' ? ` (${sender.status})` : ''}`
        + `${st ? `; run: ${st.condition} / ${st.phase} (${st.part})` : ''}`;
    },
  };
}

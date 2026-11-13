// The guided measurement run (issue #224): the loaded cube through a fixed
// sequence of conditions (the reduced cube alone; refinement over JPEG;
// refinement over WebRTC video; optionally both again at a smaller refined
// frame size), each through the same timed phases (hold still, turn slowly,
// turn and stop), with a prompt on the message card and pulses on the
// controllers. Every sample and event of the record carries the condition,
// the phase and whether it is the lead-in (the prompt being read) or the
// measured part, so the conditions can be compared with the same cube,
// binning and look.
//
// Within a phase nothing changes but what the viewer is asked to do: the
// condition is applied once, before its settle phase; the governor is held
// off for the whole run (it would lower the samples per voxel or the eye
// buffer under one condition and leave them lowered for the next); the
// card's text changes only at a phase's start.
//
// Pure: the page applies the conditions, shows the prompts and plays the
// pulses that update() asks for. Unit-tested in backend/tests/vr/measure.test.mjs.

export const CONDITIONS = {
  local: { label: 'reduced cube on the headset only', refine: false },
  jpeg: { label: 'refinement over JPEG', refine: true, transport: 'jpeg' },
  webrtc: { label: 'refinement over WebRTC video', refine: true, transport: 'webrtc' },
  'jpeg-small': { label: 'refinement over JPEG, refined frames at 0.6×', refine: true, transport: 'jpeg', scale: 0.6 },
  'webrtc-small': { label: 'refinement over WebRTC video, refined frames at 0.6×', refine: true, transport: 'webrtc', scale: 0.6 },
};

/** Seconds: the lead-in of each phase (the prompt in front of the viewer), then what is measured. */
export const PHASES = {
  settle: { lead: 5, measure: 0, title: 'Hold still', text: 'Switching the condition: hold still and look at the cube.', motion: 'still' },
  still: { lead: 3, measure: 15, title: 'Hold still', text: 'Look at the cube and keep your head still.', motion: 'still' },
  turn: { lead: 3, measure: 15, title: 'Turn your head slowly', text: 'Turn your head slowly left and right, keeping the cube in view.', motion: 'turn' },
  stops: { lead: 3, measure: 20, title: 'Turn, then stop', text: 'At each long pulse turn your head; at each short pulse stop and hold still.', motion: 'stops' },
};
export const PHASE_ORDER = ['settle', 'still', 'turn', 'stops'];
/** The turn-and-stop rhythm: ms moving, then ms holding still. */
export const STOP_CYCLE = { move: 2000, hold: 2000 };
/** How long a refined condition may take to connect (beyond the settle phase) before it is skipped. */
export const READY_TIMEOUT_MS = 20000;

/** The run's steps: [{condition, phase, part, ms}]. `scale` shortens every step (tests). */
export function protocolPlan({ second = false, scale = 1, conditions = null } = {}) {
  const list = conditions || ['local', 'jpeg', 'webrtc', ...(second ? ['jpeg-small', 'webrtc-small'] : [])];
  const steps = [];
  for (const condition of list) {
    if (!CONDITIONS[condition]) throw new Error(`unknown condition ${condition}`);
    for (const phase of PHASE_ORDER) {
      const p = PHASES[phase];
      if (p.lead) steps.push({ condition, phase, part: 'lead', ms: Math.round(1000 * p.lead * scale) });
      if (p.measure) steps.push({ condition, phase, part: 'measure', ms: Math.round(1000 * p.measure * scale) });
    }
  }
  return steps;
}

/** The whole run's length in seconds (without waits for a connection). */
export function planSeconds(plan) { return plan.reduce((a, s) => a + s.ms, 0) / 1000; }

/** The card's notice for a step (one per phase: shown at its lead-in, parked once measured). */
export function promptNotice(step, index, plan, { inVR = true } = {}) {
  const cond = CONDITIONS[step.condition];
  const p = PHASES[step.phase];
  const conds = [...new Set(plan.map((s) => s.condition))];
  const ci = conds.indexOf(step.condition) + 1;
  const measured = plan.filter((s) => s.condition === step.condition && s.phase === step.phase && s.part === 'measure')
    .reduce((a, s) => a + s.ms, 0) / 1000;
  return {
    id: `measure-${index}`, level: 'info', sticky: true,
    // In front of the viewer during the lead-in, then aside (parked) while measured.
    parkAfterMs: step.part === 'lead' ? step.ms : 0,
    title: `Measuring ${ci}/${conds.length}: ${p.title}`,
    lines: [
      p.text,
      `Condition: ${cond.label}.`,
      measured ? `${measured.toFixed(0)} s measured; a pulse marks the start${step.phase === 'stops' ? ', then each turn and stop' : ''}.`
        : 'Nothing is measured until the condition is set up.',
      inVR ? 'Cancel stops the run; what was measured stays in the record.' : 'Cancel stops the run.',
    ],
    actions: [{ id: 'measure-cancel', label: 'Cancel' }],
    badge: 'Measurement run',
  };
}

/**
 * The run as a state machine. update(now, env) → list of actions the page
 * carries out in order:
 *   {do: 'apply', condition}     set the condition (then env.ready tells when it is)
 *   {do: 'phase', step, index}   a new step: tag the record, cut the sample window
 *   {do: 'prompt', notice}       show this card (replacing the previous one)
 *   {do: 'pulse', kind}          'start' | 'move' | 'hold'
 *   {do: 'skip', condition, why} the condition could not be set up, or stopped holding: its (remaining) steps are skipped
 *   {do: 'degraded', condition, phase, part} the condition does not hold right now (told once per step)
 *   {do: 'done', cancelled, why}
 * `env`: {ready: (condition) => true | false | 'failed: …'}.
 */
export class MeasureProtocol {
  constructor(plan, { now = 0, readyTimeoutMs = READY_TIMEOUT_MS } = {}) {
    this.plan = plan;
    this.i = -1;
    this.stepAt = now;
    this.readyTimeoutMs = readyTimeoutMs;
    this.waiting = null;        // {condition, since}: applied, not ready yet
    this.done = false;
    this.cycle = null;          // the stops phase: {moving, at}
    this.skipped = [];
  }

  get step() { return this.plan[this.i] || null; }

  cancel(why = 'cancelled') {
    if (this.done) return [];
    this.done = true;
    return [{ do: 'done', cancelled: true, why }];
  }

  update(now, env = {}) {
    if (this.done) return [];
    const out = [];
    if (this.waiting) {
      const r = env.ready ? env.ready(this.waiting.condition) : true;
      if (r === true) {
        this.waiting = null;
        this.stepAt = now;
      } else if (typeof r === 'string' || now - this.waiting.since > this.readyTimeoutMs) {
        const why = typeof r === 'string' ? r : `not ready after ${(this.readyTimeoutMs / 1000).toFixed(0)} s`;
        const cond = this.waiting.condition;
        this.waiting = null;
        this.skipped.push(cond);
        out.push({ do: 'skip', condition: cond, why });
        while (this.plan[this.i + 1] && this.plan[this.i + 1].condition === cond) this.i++;
        return out.concat(this._next(now, env));
      } else {
        return out;
      }
    }
    if (this.i < 0 || now - this.stepAt >= this.step.ms) {
      out.push(...this._next(now, env));
      if (this.done || this.waiting) return out;
    }
    const s = this.step;
    // A refined condition that stops holding (WebRTC given up for JPEG, the
    // render session gone): its remaining steps are skipped, so nothing is
    // measured under another condition's name; a moment's gap (a socket
    // reconnecting) is told once per step.
    if (CONDITIONS[s.condition].refine && env.ready) {
      const r = env.ready(s.condition);
      if (typeof r === 'string') {
        this.skipped.push(s.condition);
        out.push({ do: 'skip', condition: s.condition, why: `lost during ${s.phase}: ${r}` });
        while (this.plan[this.i + 1] && this.plan[this.i + 1].condition === s.condition) this.i++;
        return out.concat(this._next(now, env));
      }
      if (r !== true && !this.degraded) {
        this.degraded = true;
        out.push({ do: 'degraded', condition: s.condition, phase: s.phase, part: s.part });
      }
    }
    if (s.part === 'measure' && s.phase === 'stops') {
      // The rhythm: a long pulse to turn, a short one to stop.
      const el = now - this.stepAt;
      const period = STOP_CYCLE.move + STOP_CYCLE.hold;
      const moving = (el % period) < STOP_CYCLE.move;
      if (!this.cycle || this.cycle.moving !== moving) {
        this.cycle = { moving, at: now };
        out.push({ do: 'pulse', kind: moving ? 'move' : 'hold' });
      }
    }
    return out;
  }

  /** What the viewer is asked to do now: 'still' | 'turn' | 'move' | 'hold' (desktop auto motion). */
  motion(now) {
    const s = this.step;
    if (!s || this.waiting) return 'still';
    const m = PHASES[s.phase].motion;
    if (m !== 'stops') return s.part === 'lead' ? 'still' : m;
    if (s.part === 'lead') return 'still';
    const el = now - this.stepAt;
    return (el % (STOP_CYCLE.move + STOP_CYCLE.hold)) < STOP_CYCLE.move ? 'move' : 'hold';
  }

  _next(now, env) {
    const out = [];
    const prev = this.step;
    this.i++;
    this.cycle = null;
    this.degraded = false;
    const s = this.step;
    if (!s) { this.done = true; out.push({ do: 'done', cancelled: false, why: 'complete' }); return out; }
    this.stepAt = now;
    if (!prev || prev.condition !== s.condition) {
      out.push({ do: 'apply', condition: s.condition });
      if (CONDITIONS[s.condition].refine) this.waiting = { condition: s.condition, since: now };
    }
    out.push({ do: 'phase', step: { ...s }, index: this.i });
    if (s.part === 'lead') out.push({ do: 'prompt', notice: promptNotice(s, this.i, this.plan, env) });
    if (s.part === 'measure' && s.phase !== 'stops') out.push({ do: 'pulse', kind: 'start' });
    // A refined condition's settle phase is timed from when it is connected
    // (update() checks it from the next call on, once the page has applied it).
    return out;
  }
}

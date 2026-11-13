// Measurement records and the guided run (issue #224), driven by
// vr_lifecycle.mjs --measure against its own backend (which writes the
// records to <out>/measurements) and the cube it loaded, directly or
// --paired through the public loader:
//   1. the recording's cost: the desktop page's frames with recording off and
//      on, alternated, and the time spent building each sample;
//   2. a record started from the page's switch, the guided run (shortened,
//      the desktop camera turned by the run's auto motion) through every
//      condition, the record read back from the backend's directory and
//      checked (header, units, phases, events, no token, no path), and the
//      report script run on it;
//   3. a second run cancelled from the message card's Cancel.
import { readFileSync, readdirSync, existsSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function readRecord(dir, id) {
  const file = join(dir, `vr-measure-${id}.jsonl`);
  if (!existsSync(file)) return null;
  return readFileSync(file, 'utf8').trim().split('\n').filter(Boolean).map((l) => JSON.parse(l));
}

export async function runMeasureChecks({ c, check, waitFor, results, out, python, secrets = [], paired = false, scale = 0.08, blockMs = 5000, blocks = 3 }) {
  const dir = join(out, 'measurements');
  const M = 'window.__vrMeasure.measure';
  results.measure = {};

  // ── 1. The cost of recording: off and on, alternated ─────────────────────
  const say = (m) => console.log(`     measure: ${m}`);
  say('recording off and on, alternated');
  // The recorder's snapshot answers at once (a stalled page would show here first).
  say(`snapshot: ${await c.evaluate(`(() => { const t0 = performance.now(); try { const s = window.__vrMeasure.measure.recorder.snapshot(); return JSON.stringify({ ms: performance.now() - t0, keys: Object.keys(s) }); } catch (e) { return 'threw ' + e.message; } })()`, 15000)}`);
  const block = async (on) => {
    await c.evaluate(`(() => { const m = ${M}; if (m.recording !== ${on}) m.toggleRecord(); window.__vrRefine.resetStats(); })()`);
    await sleep(blockMs);
    return c.evaluate(`(() => { const s = window.__vrRefine.stats(); return { n: s.n, cpu: s.cpu && s.cpu.mean, cpuP95: s.cpu && s.cpu.p95,
      mean: s.interval && s.interval.mean, p95: s.interval && s.interval.p95 }; })()`);
  };
  const offs = [], ons = [];
  for (let i = 0; i < blocks; i++) { offs.push(await block(false)); ons.push(await block(true)); }
  const buildMs = await c.evaluate(`[...${M}.recorder.buildMs]`);
  await c.evaluate(`(async () => { const m = ${M}; if (m.recording) await m.stopRecord('overhead measured'); })()`);
  const mean = (a, k) => a.reduce((s, x) => s + (x[k] || 0), 0) / a.length;
  const sorted = [...buildMs].sort((a, b) => a - b);
  const o = {
    off: { cpuMs: mean(offs, 'cpu'), cpuP95: mean(offs, 'cpuP95'), frameMs: mean(offs, 'mean'), frameP95: mean(offs, 'p95'), frames: offs.reduce((s, x) => s + x.n, 0) },
    on: { cpuMs: mean(ons, 'cpu'), cpuP95: mean(ons, 'cpuP95'), frameMs: mean(ons, 'mean'), frameP95: mean(ons, 'p95'), frames: ons.reduce((s, x) => s + x.n, 0) },
    sampleBuildMs: { n: sorted.length, p50: sorted[Math.floor(sorted.length / 2)], max: sorted.at(-1) },
  };
  results.measure.overhead = o;
  const f2 = (v) => (Number.isFinite(v) ? v.toFixed(3) : 'n/a');
  check('measure: recording adds no frame-time cost (CPU per frame and frame interval, off vs on)',
    o.on.frameMs - o.off.frameMs < 0.5 && o.on.cpuMs - o.off.cpuMs < 0.3 && o.sampleBuildMs.max < 4,
    `frame ${f2(o.off.frameMs)} → ${f2(o.on.frameMs)} ms (p95 ${f2(o.off.frameP95)} → ${f2(o.on.frameP95)}), CPU ${f2(o.off.cpuMs)} → ${f2(o.on.cpuMs)} ms, `
    + `sample built in ${f2(o.sampleBuildMs.p50)} ms (max ${f2(o.sampleBuildMs.max)}, ${o.sampleBuildMs.n} samples)`);

  // ── 2. A record from the page's switch, the guided run through it ─────────
  say('the guided run');
  await c.evaluate(`document.getElementById('record').click()`);
  const id = await c.evaluate(`${M}.recorder.id`);
  check('measure: the page\'s switch starts a record', await c.evaluate(`${M}.recording`) === true, id);
  // A notice left by earlier checks (an error outranks the run's prompts) goes first.
  await c.evaluate(`(() => { const sh = window.__vrShell; for (const id of [...sh.notices.items.keys()]) sh.notices.clear(id); sh.render(); })()`);
  const t0 = Date.now();
  const started = await c.evaluate(`(() => { const r = window.__vrMeasure.start({ scale: ${scale}, auto: true }); return ${M}.running; })()`);
  check('measure: the run starts on the loaded cube', started === true);
  const firstPrompt = await waitFor(c, `(() => { const n = window.__vrShell.notices.current(); return n && /^measure-/.test(n.id) ? n.title : null; })()`, 5000, 50);
  const onShow = await c.evaluate(`JSON.stringify([...window.__vrShell.notices.items.values()].map((it) => [it.notice.id, it.notice.level, it.at]))`);
  check('measure: its prompt is on the message card', !!firstPrompt.value, firstPrompt.value || onShow);
  const ended = await waitFor(c, `!${M}.running`, 180000, 250);
  const runMs = Date.now() - t0;
  check('measure: the run ends by itself', !!ended.value, `${(runMs / 1000).toFixed(1)} s`);
  const restored = await c.evaluate(`({ governor: window.__vrState.governorOn, refine: window.__vrRefine.refiner.enabled,
    transport: window.__vrRefine.refiner.o.transport, page: document.getElementById('refine').checked })`);
  check('measure: the run gives back the governor and the refinement as they were', restored.governor === true && restored.refine === restored.page,
    JSON.stringify(restored));
  await c.evaluate(`document.getElementById('record').click()`);
  await waitFor(c, `!${M}.recording && ${M}.sender.queue.length === 0 && !${M}.sender.inflight`, 20000, 100);
  const sender = await c.evaluate(`({ sent: ${M}.sender.sentLines, dropped: ${M}.sender.dropped, file: ${M}.sender.file, status: ${M}.sender.status })`);
  const lines = readRecord(dir, id);
  if (!lines) { check('measure: the record is in the backend\'s directory', false, `${dir} has ${existsSync(dir) ? readdirSync(dir).join(', ') : 'nothing'}`); return; }
  results.measure.record = { id, lines: lines.length, file: sender.file };
  const header = lines[0];
  check('measure: the record is in the backend\'s directory, every line stored', lines.length === sender.sent && sender.dropped === 0
    && sender.file === `vr-measure-${id}.jsonl`, `${lines.length} lines, ${sender.sent} sent, ${sender.dropped} dropped, ${sender.file}`);
  check('measure: header with format, units, definitions, device and link',
    header.k === 'h' && header.format === 'visivo-vr-measure/1' && header.units && header.definitions && header.device && header.device.user_agent
    && header.link && header.link.kind === (paired ? 'paired' : 'direct') && header.page && header.page.build,
    JSON.stringify({ link: header.link, backend: header.backend, cube: header.cube && header.cube.dims }));
  const samples = lines.filter((l) => l.k === 's');
  const events = lines.filter((l) => l.k === 'e');
  const byStep = {};
  for (const s of samples) if (s.proto) (byStep[`${s.proto.condition}/${s.proto.phase}/${s.proto.part}`] ||= []).push(s);
  const skipped = events.filter((e) => e.name === 'condition-skipped').map((e) => `${e.data.condition}: ${e.data.why}`);
  const measured = ['local', 'jpeg', 'webrtc'].filter((cond) => ['still', 'turn', 'stops'].every((p) => (byStep[`${cond}/${p}/measure`] || []).length));
  results.measure.measured = measured;
  results.measure.skipped = skipped;
  check('measure: every condition is measured in every phase, or skipped with its reason',
    measured.includes('local') && ['jpeg', 'webrtc'].every((cond) => measured.includes(cond) || skipped.some((s) => s.startsWith(cond))),
    `measured ${measured.join(', ')}${skipped.length ? `; skipped ${skipped.join('; ')}` : ''}`);
  const end = events.find((e) => e.name === 'protocol-end');
  const phases = events.filter((e) => e.name === 'phase').length;
  check('measure: the record tags every step (phase events, condition events, the run\'s start and end)',
    events.some((e) => e.name === 'protocol-start') && end && end.data.cancelled === false && phases >= 7
    && events.filter((e) => e.name === 'condition').length >= 1, `${phases} phase events, end: ${end && end.data.why}`);
  // Within a step nothing but the condition changes: one look, one binning, governor held off.
  const stepsEqual = Object.entries(byStep).filter(([k]) => k.endsWith('/measure')).every(([, ss]) =>
    new Set(ss.map((s) => s.look)).size === 1 && new Set(ss.map((s) => JSON.stringify(s.cube && s.cube.factors))).size === 1
    && ss.every((s) => s.render && s.render.governor === false));
  const looksAcross = new Set(samples.filter((s) => s.proto && s.proto.part === 'measure').map((s) => s.look));
  check('measure: within and across conditions the look, the binning and the governor are held', stepsEqual && looksAcross.size === 1,
    `${looksAcross.size} look(s)`);
  // The transport actually used matches the condition for the refined ones measured.
  const viaOk = measured.filter((cond) => cond !== 'local').every((cond) => samples.filter((s) => s.proto && s.proto.condition === cond
    && s.proto.part === 'measure').every((s) => s.refine && s.refine.on && (s.refine.via === cond || s.refine.status !== 'refined')));
  check('measure: the refined frames came over the transport the condition names', viaOk);
  const motion = samples.filter((s) => s.proto && s.proto.condition === 'local' && s.proto.part === 'measure');
  // The local condition's samples never see refinement on (each step's last
  // sample closes before the next condition is applied).
  const leaked = samples.filter((s) => s.proto && s.proto.condition === 'local' && s.refine && s.refine.on).length;
  check('measure: no sample of a step spans a change of condition', leaked === 0, `${leaked} local samples with refinement on`);
  check('measure: samples carry the frame figures and their raw intervals', motion.length > 0 && motion.every((s) => s.frames.n === s.iv.length && s.frames.n > 0),
    `${motion.length} local samples`);
  const text = JSON.stringify(lines);
  const leaks = secrets.filter((s) => s && text.includes(s));
  check('measure: no token, no path in the record', !leaks.length && !text.includes(out) && !/\/Users\/|\/home\/|\.fits/.test(text),
    leaks.length ? 'a token is in the record' : '');
  const refineEvents = events.filter((e) => e.name === 'refined-first').length;
  results.measure.refinedFirst = refineEvents;

  // The report.
  const rep = spawnSync(python, [join(HERE, 'vr_measure_report.py'), join(dir, `vr-measure-${id}.jsonl`), '--out', join(out, 'measure_report')], { encoding: 'utf8' });
  const csvPath = join(out, 'measure_report', 'per_phase.csv');
  const csv = existsSync(csvPath) ? readFileSync(csvPath, 'utf8') : '';
  check('measure: the report script writes the tables and the plot', rep.status === 0 && /^record,condition,phase/.test(csv)
    && /,local,still,/.test(csv) && existsSync(join(out, 'measure_report', 'frame_time.png')), (rep.stderr || rep.stdout || '').trim().slice(0, 200));
  results.measure.report = join(out, 'measure_report', 'report.md');

  // ── 3. A run cancelled from the card ──────────────────────────────────────
  await c.evaluate(`window.__vrMeasure.start({ scale: 1, auto: true })`);
  const id2 = await c.evaluate(`${M}.recorder.id`);
  await waitFor(c, `!!window.__vrShell.card.model`, 5000, 50);
  await sleep(300);
  const pressed = await c.evaluate(`window.__vrCard.press('measure-cancel')`);
  const stopped = await waitFor(c, `!${M}.running && !${M}.recording`, 10000, 100);
  await waitFor(c, `${M}.sender.queue.length === 0 && !${M}.sender.inflight`, 10000, 100);
  const l2 = readRecord(dir, id2) || [];
  const end2 = l2.find((l) => l.name === 'protocol-end');
  check('measure: Cancel on the card ends the run and the record says so', pressed && !!stopped.value && end2 && end2.data.cancelled === true,
    `pressed ${pressed}, ${end2 ? end2.data.why : 'no end event'}; ${await c.evaluate(`JSON.stringify([...window.__vrShell.notices.items.values()].map((it) => [it.notice.id, it.notice.level]))`)}`);
  const errs = c.pageErrors.filter((e) => !/abort/i.test(e));
  check('measure: no uncaught page exception', !errs.length, errs.join(' / '));
}

// A cube's passes over all its voxels off the main thread (issue #246): the
// values the worker returns are those computed here, the voxels go there and
// back (transferred, never copied, never lost), the synchronous fallback, the
// newest cube winning over a stale result, and the renderer's write into the
// capacity in slabs over several steps.
// node --test backend/tests/vr/cube-prep.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';
import * as V from '../../app/static/vr/js/volume.js';
import { prepareCube, CubePrep, Newest, ownsBuffer, CMAX_QUANTILE, CMAX_SAMPLE } from '../../app/static/vr/js/cube-prep.js';
import { Renderer, WRITE_SLAB_BYTES } from '../../app/static/vr/js/renderer.js';

function rampCube(dims, seed = 1) {
  const n = dims[0] * dims[1] * dims[2];
  const a = new Uint8Array(n);
  for (let i = 0; i < n; i++) a[i] = (i * 37 + seed * 101) % 255;
  return a;
}

function halfCube(dims) {
  const n = dims[0] * dims[1] * dims[2];
  const a = new Uint16Array(n);
  for (let i = 0; i < n; i++) a[i] = i % 11 === 0 ? 0xbc00 : 0x3000 + (i % 977);   // -1 (blank) and [0.125, 0.4]
  return a;
}

/**
 * The page's worker, in this process: the real worker module (cube-prep-worker.js)
 * loaded once with a stand-in `self`; messages cross with structuredClone and
 * their transfer lists, so buffers are detached as in a browser. `hold` keeps
 * replies until release() (to answer out of order).
 */
let workerSelf = null;
async function workerModule() {
  if (workerSelf) return workerSelf;
  workerSelf = { postMessage: () => {}, onmessage: null };   // its "ready" at load goes nowhere
  globalThis.self = workerSelf;      // the module reads `self` when a message comes: kept (one process per test file)
  await import('../../app/static/vr/js/cube-prep-worker.js');
  return workerSelf;
}

async function inProcessWorker({ hold = false, ready = true } = {}) {
  const self = await workerModule();
  const held = [];
  const w = {
    onmessage: null, onerror: null, terminated: false, sent: [],
    postMessage(msg, transfer = []) {
      w.sent.push(msg.id);
      const copy = structuredClone(msg, { transfer });
      self.postMessage = (reply, tr = []) => {
        const back = structuredClone(reply, { transfer: tr });
        if (hold) held.push(back); else queueMicrotask(() => w.onmessage && w.onmessage({ data: back }));
      };
      self.onmessage({ data: copy });
    },
    terminate() { w.terminated = true; },
    release(order) { for (const i of order) { const m = held[i]; queueMicrotask(() => w.onmessage({ data: m })); } },
  };
  if (ready) setTimeout(() => w.onmessage && w.onmessage({ data: { type: 'ready' } }), 0);
  return w;
}

const settle = () => new Promise((r) => setTimeout(r, 0));

test('prepareCube: the skip grid, the colour range and the apron, as the page computed them', () => {
  for (const [enc, data] of [['uint8', rampCube([20, 17, 9])], ['float16', halfCube([20, 17, 9])]]) {
    const dims = [20, 17, 9];
    const p = prepareCube(data, dims, enc, { cell: 8, aboves: [0, 0.3, 0.3], store: [24, 17, 16] });
    const m = V.buildMacrocells(data, dims, enc, 8);
    assert.deepEqual(p.macro.dims, m.dims);
    assert.deepEqual([...p.macro.data], [...m.data]);
    assert.deepEqual(p.quantiles.map(([a]) => a), [0, 0.3], 'one per distinct threshold');
    for (const [a, q] of p.quantiles) assert.equal(q, V.normQuantile(data, enc, CMAX_QUANTILE, CMAX_SAMPLE, a));
    const ap = V.apronBlocks(data, dims, [24, 17, 16]);
    assert.equal(p.apron.length, ap.length);
    p.apron.forEach((b, i) => { assert.deepEqual(b.offset, ap[i].offset); assert.deepEqual([...b.data], [...ap[i].data]); });
    assert.equal(prepareCube(data, dims, enc).apron, null, 'no storage, no apron');
  }
});

test('CubePrep: without a worker (node, an old browser) everything is computed here', async () => {
  for (const make of [null, () => { throw new Error('no module workers'); }, () => null]) {
    const P = new CubePrep(make);
    assert.equal(P.usesWorker, false);
    const cube = { data: rampCube([16, 16, 16]), dims: [16, 16, 16], encoding: 'uint8' };
    const before = cube.data;
    const p = await P.prepare(cube, { aboves: [0.1] });
    assert.equal(cube.data, before, 'the voxels never left');
    assert.ok(!p.offMain);
    assert.deepEqual([...p.macro.data], [...V.buildMacrocells(before, [16, 16, 16], 'uint8', 8).data]);
    assert.deepEqual(P.stats, { worker: 0, sync: 1 });
  }
});

test('CubePrep: through the worker the voxels are transferred there and back, the values the same', async () => {
  const w = await inProcessWorker();
  const P = new CubePrep(() => w);
  await settle();
  assert.equal(P.usesWorker, true);
  const dims = [33, 20, 18];
  const data = halfCube(dims);
  const expect = prepareCube(data.slice(), dims, 'float16', { aboves: [0.2], store: [40, 24, 24] });
  const cube = { data, dims, encoding: 'float16' };
  const pending = P.prepare(cube, { aboves: [0.2], store: [40, 24, 24] });
  assert.equal(data.buffer.byteLength, 0, 'transferred, not copied: detached on the page meanwhile');
  const p = await pending;
  assert.ok(p.offMain);
  assert.ok(cube.data instanceof Uint16Array && cube.data.length === 33 * 20 * 18, 'the voxels are back on the cube');
  assert.deepEqual([...cube.data], [...halfCube(dims)]);
  assert.deepEqual([...p.macro.data], [...expect.macro.data]);
  assert.deepEqual(p.quantiles, expect.quantiles);
  assert.equal(p.apron.length, expect.apron.length);
  assert.deepEqual(P.stats, { worker: 1, sync: 0 });
});

test('CubePrep: a view that does not own its buffer, or a worker not yet ready, is computed here', async () => {
  const w = await inProcessWorker({ ready: false });
  const P = new CubePrep(() => w, { readyTimeoutMs: 50 });
  const cube = { data: rampCube([8, 8, 8]), dims: [8, 8, 8], encoding: 'uint8' };
  await P.prepare(cube);
  assert.equal(w.sent.length, 0, 'nothing sent to a worker that has not said it is ready');
  assert.ok(cube.data.length === 512);
  // A worker that never says it is ready is given up: the page stays on the synchronous path.
  await new Promise((r) => setTimeout(r, 80));
  assert.equal(P.broken, true);
  assert.equal(w.terminated, true);
  const w2 = await inProcessWorker();
  const P2 = new CubePrep(() => w2);
  await settle();
  const big = new Uint8Array(1024);
  const view = { data: big.subarray(0, 512), dims: [8, 8, 8], encoding: 'uint8' };
  assert.equal(ownsBuffer(view.data), false);
  await P2.prepare(view);
  assert.equal(big.buffer.byteLength, 1024, 'a buffer shared with more than the cube is never transferred');
  assert.equal(w2.sent.length, 0);
});

test('CubePrep: replies answered out of order go to their own cubes; a failed worker fails its cubes, then the page computes', async () => {
  const w = await inProcessWorker({ hold: true });
  const P = new CubePrep(() => w);
  w.onmessage({ data: { type: 'ready' } });
  const a = { data: rampCube([16, 8, 8], 1), dims: [16, 8, 8], encoding: 'uint8' };
  const b = { data: rampCube([8, 8, 24], 2), dims: [8, 8, 24], encoding: 'uint8' };
  const pa = P.prepare(a), pb = P.prepare(b);
  w.release([1, 0]);
  const [ra, rb] = await Promise.all([pa, pb]);
  assert.deepEqual(ra.macro.dims, [2, 1, 1]);
  assert.deepEqual(rb.macro.dims, [1, 1, 3]);
  assert.deepEqual([...a.data], [...rampCube([16, 8, 8], 1)]);
  assert.deepEqual([...b.data], [...rampCube([8, 8, 24], 2)]);
  // A worker that fails with a cube in flight: that cube's load fails (its voxels went with it), the next is computed here.
  const c = { data: rampCube([8, 8, 8]), dims: [8, 8, 8], encoding: 'uint8' };
  const pc = P.prepare(c);
  w.onerror(new Event('error'));
  await assert.rejects(pc, /load the cube again/);
  assert.equal(P.usesWorker, false);
  const d = { data: rampCube([8, 8, 8]), dims: [8, 8, 8], encoding: 'uint8' };
  const pd = await P.prepare(d);
  assert.ok(!pd.offMain && d.data.length === 512);
});

test('Newest: a newer cube wins over a stale result from the worker', async () => {
  const w = await inProcessWorker({ hold: true });
  const P = new CubePrep(() => w);
  w.onmessage({ data: { type: 'ready' } });
  const order = new Newest();
  const shown = [];
  const install = async (name, cube) => {
    const t = order.begin();
    await P.prepare(cube);
    if (!order.isCurrent(t)) return false;
    shown.push(name);
    return true;
  };
  const old = { data: rampCube([8, 8, 8], 1), dims: [8, 8, 8], encoding: 'uint8' };
  const neu = { data: rampCube([8, 8, 8], 2), dims: [8, 8, 8], encoding: 'uint8' };
  const i1 = install('old', old);
  const i2 = install('new', neu);
  w.release([1, 0]);          // the newer cube's result first, the stale one after
  assert.deepEqual(await Promise.all([i1, i2]), [false, true]);
  assert.deepEqual(shown, ['new']);
  assert.equal(old.data.length, 512, 'the stale cube got its voxels back all the same');
});

// ── The renderer's write in slabs ───────────────────────────────────────────

function fakeGL() {
  const calls = {};
  const log = [];
  let lost = false;
  const gl = new Proxy({}, {
    get(t, k) {
      if (typeof k !== 'string') return undefined;
      if (/^[A-Z0-9_]+$/.test(k)) return k;
      return (...a) => {
        calls[k] = (calls[k] || 0) + 1;
        if (/^tex(Image|SubImage|Storage)3D$/.test(k)) log.push([k, ...a]);
        if (/^create/.test(k)) return { k };
        if (k === 'getShaderParameter' || k === 'getProgramParameter') return a[1] === 'ACTIVE_UNIFORMS' ? 0 : true;
        if (k === 'getParameter') return a[0] === 'MAX_3D_TEXTURE_SIZE' ? 2048 : 0;
        if (k === 'getAttribLocation') return 0;
        if (k === 'getExtension') return null;
        if (k === 'isContextLost') return lost;
        return undefined;
      };
    },
  });
  return { gl, calls, log, lose: () => { lost = true; }, reset: () => { for (const k of Object.keys(calls)) delete calls[k]; log.length = 0; } };
}

test('renderer: in VR a cube is written into the capacity in slabs over several steps, the cube on show untouched until the end', () => {
  const { gl, calls, log, reset } = fakeGL();
  const R = new Renderer(gl);
  R.allocCapacity([128, 128, 128], 'uint8');
  R.showVolume({ data: rampCube([64, 64, 64]), dims: [64, 64, 64], encoding: 'uint8' });
  R.xrActive = true;
  const shown = R.slot;
  const dims = [128, 128, 100];
  const data = rampCube(dims, 3);
  const prep = prepareCube(data, dims, 'uint8', { store: [128, 128, 128], aboves: [0] });
  reset();
  const job = R.beginWrite({ data, dims, encoding: 'uint8' }, 8, prep);
  const planes = Math.floor(WRITE_SLAB_BYTES / (128 * 128));
  assert.equal(job.slabs, Math.ceil(100 / planes));
  let steps = 0;
  while (!job.step(0)) {
    steps++;
    assert.equal(R.slot, shown, 'the cube on show stays while the other bank is written');
    assert.ok(!shown.released);
  }
  assert.ok(steps >= job.slabs, `${steps} steps for ${job.slabs} slabs`);
  const subs = log.filter((c) => c[0] === 'texSubImage3D');
  // The slabs cover z = 0..100 in order, each a view of the cube's voxels (no copy).
  const slabs = subs.slice(0, job.slabs);
  let z = 0;
  for (const s of slabs) {
    assert.equal(s[5], z);
    assert.deepEqual([s[6], s[7]], [128, 128]);
    assert.equal(s[11].buffer, data.buffer);
    assert.equal(s[11].byteOffset, z * 128 * 128);
    z += s[8];
  }
  assert.equal(z, 100);
  // Then the apron (z only: x and y fill the storage) and the prepared skip grid, not built again.
  assert.deepEqual(subs[job.slabs].slice(3, 9), [0, 0, 100, 128, 128, 1]);
  assert.equal(subs[job.slabs + 1][11], prep.macro.data);
  assert.equal(subs.length, job.slabs + 2);
  assert.equal(calls.texStorage3D || 0, 0);
  assert.equal(calls.texImage3D || 0, 0);
  assert.equal(job.slot.macroMs, prep.ms, 'the grid\'s time is the worker\'s');
  R.showWritten(job.slot, { data, dims, encoding: 'uint8' });
  assert.equal(R.slot, job.slot);
  assert.deepEqual(R.dims, dims);
  assert.equal(R._volume.data, data);
});

test('renderer: a write in steps is cancelled by another write into its bank, the capacity given back or a lost context', () => {
  const { gl, lose } = fakeGL();
  const R = new Renderer(gl);
  R.allocCapacity([64, 64, 64], 'uint8');
  R.showVolume({ data: rampCube([8, 8, 8]), dims: [8, 8, 8], encoding: 'uint8' });
  R.xrActive = true;
  const big = { data: rampCube([64, 64, 64]), dims: [64, 64, 64], encoding: 'uint8' };
  const first = R.beginWrite(big, 8);
  assert.equal(first.step(0), false);
  // A cube shown at once meanwhile writes the same bank: the slow write gives way.
  R.showVolume({ data: rampCube([16, 16, 16]), dims: [16, 16, 16], encoding: 'uint8' });
  assert.throws(() => first.step(0), (e) => e.code === 'write-cancelled');
  const second = R.beginWrite(big, 8);
  second.step(0);
  R.xrActive = false;
  R.releaseCapacity();
  assert.throws(() => second.step(0), (e) => e.code === 'write-cancelled');
  R.allocCapacity([64, 64, 64], 'uint8');
  R.xrActive = true;
  const third = R.beginWrite(big, 8);
  lose();
  assert.throws(() => third.step(0), (e) => e.code === 'write-cancelled');
});

test('renderer: the same textures with the worker\'s skip grid and apron as without', () => {
  const run = (usePrep) => {
    const { gl, log, reset } = fakeGL();
    const R = new Renderer(gl);
    R.allocCapacity([48, 48, 48], 'float16');
    const dims = [40, 33, 21];
    const data = halfCube(dims);
    const prep = usePrep ? prepareCube(data, dims, 'float16', { store: [48, 48, 48] }) : null;
    reset();
    R.showVolume({ data, dims, encoding: 'float16' }, 8, prep);
    return log.filter((c) => c[0] === 'texSubImage3D').map((c) => [c.slice(1, 11).join(), [...c[11]].join()]);
  };
  assert.deepEqual(run(true), run(false));
  // A prepared grid of another cell size or shape is not used.
  const { gl } = fakeGL();
  const R = new Renderer(gl);
  R.allocCapacity([48, 48, 48], 'uint8');
  const data = rampCube([16, 16, 16]);
  const wrong = prepareCube(rampCube([24, 16, 16]), [24, 16, 16], 'uint8', { cell: 8 });
  R.showVolume({ data, dims: [16, 16, 16], encoding: 'uint8' }, 8, wrong);
  assert.deepEqual(R.macroDims, [2, 2, 2]);
});

// ── main.js's install, run with stand-ins (main.js cannot be imported in node) ──

/** installCube, installCurrent and installPrepared from main.js, in a context of stand-ins. */
async function installHarness({ xrActive = false } = {}) {
  const { readFileSync } = await import('node:fs');
  const vm = await import('node:vm');
  const main = readFileSync(new URL('../../app/static/vr/js/main.js', import.meta.url), 'utf8');
  const fn = (name) => {
    const i = main.indexOf(`function ${name}(`);
    assert.ok(i >= 0, name);
    const a = main.lastIndexOf('\n', i) + 1;
    return main.slice(a, main.indexOf('\n}\n', i) + 2);
  };
  let finish = null;
  const jobs = [];
  const shown = [];
  const ctx = {
    state: { cube: { name: 'B' }, load: {} },
    installs: new Newest(),
    loads: { latest: 1 },
    cubePrep: { stats: { worker: 0, sync: 0 }, prepare() { this.stats.worker++; return new Promise((r) => { finish = r; }); } },
    R: {
      xrActive, cap: { dims: [64, 64, 64] }, _volume: null,
      showVolume: () => ({ via: 'showVolume' }),
      beginWrite: () => {
        const job = { steps: 0, maxStepMs: 0, cancelled: false, slot: {}, step: () => { job.steps++; return job.steps >= 3; } };
        jobs.push(job);
        return job;
      },
      showWritten: () => ({ via: 'showWritten' }),
    },
    gl: { isContextLost: () => false },
    mustWait: () => false, dropBenchPrep: () => {}, thresholdNorm: () => 0, ensureCapacity: () => {}, stageCube: () => {},
    prepNow: () => ({ cell: 8, quantiles: [] }),
    blockClock: () => ({ pause() {}, resume() {} }),
    nextTask: () => new Promise((r) => setTimeout(r, 0)),
    WRITE_STEP_MS: 4,
    showInstalled: (info, cube) => { ctx.state.cube = cube; shown.push(cube.name); },
  };
  vm.createContext(ctx);
  vm.runInContext(`${fn('installCube')}\n${fn('installCurrent')}\n${fn('installPrepared')}\nthis.installCube = installCube;`, ctx);
  return { ctx, shown, jobs, finish: (p) => finish(p) };
}

const cubeNamed = (name, prep = undefined) => ({ name, data: new Uint8Array(8), dims: [2, 2, 2], encoding: 'uint8', prep });

test('install (main.js): a newer load begun while the worker prepares the cube wins (the desktop asking again for the cube on show)', async () => {
  const h = await installHarness();
  const pending = h.ctx.installCube({}, cubeNamed('A'), {}, 'A');
  h.ctx.loads.latest++;         // a new load, or showFromDesktop's "already on show" answer
  h.finish({ cell: 8, quantiles: [] });
  assert.equal(await pending, 'superseded');
  assert.equal(h.ctx.state.cube.name, 'B', 'the cube on show stays');
  assert.deepEqual(h.shown, []);
});

test('install (main.js): in VR a newer cube wins between two steps of the write; the newest is shown', async () => {
  const h = await installHarness({ xrActive: true });
  const a = h.ctx.installCube({}, cubeNamed('A', { cell: 8, quantiles: [] }), {}, 'A');
  // Prepared already (a cube that waited for a session's end): its first step at once, the next after a task.
  assert.equal(h.jobs.length, 1);
  assert.equal(h.jobs[0].steps, 1, 'A is between two steps of its write');
  const c = h.ctx.installCube({}, cubeNamed('C', { cell: 8, quantiles: [] }), {}, 'C');
  assert.deepEqual(await Promise.all([a, c]), ['superseded', 'shown']);
  assert.deepEqual(h.shown, ['C']);
  assert.equal(h.jobs[0].cancelled, true, 'the stale write is cancelled');
});

test('install (main.js): the longest blocking slice counts a preparation done on this thread with the write after it', async () => {
  for (const worker of [false, true]) {
    const h = await installHarness();
    const { readFileSync } = await import('node:fs');
    const vm = await import('node:vm');
    const main = readFileSync(new URL('../../app/static/vr/js/main.js', import.meta.url), 'utf8');
    const i = main.indexOf('function blockClock(');
    let now = 0;
    h.ctx.performance = { now: () => now };
    vm.runInContext(`${main.slice(i, main.indexOf('\n}\n', i) + 2)}\nthis.blockClock = blockClock;`, h.ctx);
    const stats = { worker: 0, sync: 0 };
    h.ctx.cubePrep = {
      stats,
      prepare: () => {
        if (worker) { stats.worker++; return new Promise((r) => setTimeout(() => { now += 100; r({ cell: 8, quantiles: [] }); }, 0)); }
        stats.sync++; now += 100; return Promise.resolve({ cell: 8, quantiles: [] });
      },
    };
    h.ctx.R.showVolume = () => { now += 50; return {}; };
    let measured = null;
    h.ctx.showInstalled = (...a) => { measured = a[a.length - 1].end(); };
    assert.equal(await h.ctx.installCube({}, cubeNamed('A'), {}, 'A'), 'shown');
    assert.equal(measured.blockMs, worker ? 50 : 150, worker ? 'the worker\'s time is not the page\'s' : 'no frame between the preparation and the write');
  }
});

test('install (main.js): without a newer cube or load, the prepared cube is shown', async () => {
  const h = await installHarness();
  const p = h.ctx.installCube({}, cubeNamed('A'), {}, 'A');
  h.finish({ cell: 8, quantiles: [] });
  assert.equal(await p, 'shown');
  assert.deepEqual(h.shown, ['A']);
});

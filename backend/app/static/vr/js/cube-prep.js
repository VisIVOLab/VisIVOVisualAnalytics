// A cube's passes over all its voxels before it is shown (issue #246): the
// empty-space-skipping grid, the colour range's percentile above each
// candidate threshold, the apron of the capacity's corner. On a Quest 3 they
// take hundreds of milliseconds, so the page runs them in a module Web Worker
// (cube-prep-worker.js), the voxels transferred there and back, never copied;
// where no worker can be made (node, an old browser, a worker that failed to
// start) the same function runs here. No DOM, no GL: node runs the tests.

import { buildMacrocells, normQuantile, apronBlocks } from './volume.js';

/** The colour range's top: this quantile of the values above the threshold,
 *  from a stride sample of about this many voxels (main.js thresholdNorms). */
export const CMAX_QUANTILE = 0.99;
export const CMAX_SAMPLE = 4_000_000;

/**
 * Everything a cube needs computed from its voxels: `macro` ({data, dims}),
 * `quantiles` ([[above, q], ...] for each of `aboves`), `apron` (apronBlocks
 * for the storage `store`, or null without one) and `ms`, the time it took.
 */
export function prepareCube(data, dims, encoding, { cell = 8, aboves = [], store = null } = {}) {
  const t0 = Date.now();
  const macro = buildMacrocells(data, dims, encoding, cell);
  const quantiles = [];
  for (const a of new Set(aboves)) quantiles.push([a, normQuantile(data, encoding, CMAX_QUANTILE, CMAX_SAMPLE, a)]);
  const apron = store ? apronBlocks(data, dims, store) : null;
  return { macro, quantiles, apron, store: store ? store.slice() : null, cell, ms: Date.now() - t0 };
}

/** The buffers of a prepared cube, to be transferred with it. */
export function preparedBuffers(p) {
  return [p.macro.data.buffer, ...(p.apron || []).map((a) => a.data.buffer)];
}

/** Can `data`'s buffer be handed to a worker and back (the view owns it whole)? */
export function ownsBuffer(data) {
  return !!data && ArrayBuffer.isView(data) && data.byteOffset === 0 && data.byteLength === data.buffer.byteLength
    && !(typeof SharedArrayBuffer !== 'undefined' && data.buffer instanceof SharedArrayBuffer);
}

/**
 * The page's side of the worker. `makeWorker()` returns a Worker-like object
 * ({postMessage, terminate, onmessage, onerror}) or throws; null or a factory
 * that throws means the synchronous path. The worker is used only once it has
 * said it is ready (a worker whose script failed to load never receives
 * voxels, which would be lost with it). prepare() hands the cube's buffer
 * over and puts the one that comes back into `cube.data` (whether or not the
 * caller still wants the result); a worker that fails takes the page back to
 * the synchronous path for every later cube.
 */
export class CubePrep {
  constructor(makeWorker = null, { readyTimeoutMs = 5000 } = {}) {
    this.worker = null;
    this.ready = false;
    this.broken = false;
    this.pending = new Map();
    this.nextId = 1;
    this.stats = { worker: 0, sync: 0 };
    if (!makeWorker) return;
    try {
      this.worker = makeWorker();
    } catch {
      this.worker = null;
    }
    if (!this.worker) return;
    this.worker.onmessage = (e) => this._message(e.data);
    this.worker.onerror = () => this._fail('the cube preparation worker failed');
    // A worker that never says it is ready (its script did not load) is not used.
    this._readyTimer = setTimeout(() => { if (!this.ready) this._fail('the cube preparation worker did not start'); }, readyTimeoutMs);
    if (this._readyTimer && this._readyTimer.unref) this._readyTimer.unref();
  }

  /** Whether the next prepare() goes to the worker. */
  get usesWorker() { return !!this.worker && this.ready && !this.broken; }

  _message(m) {
    if (!m) return;
    if (m.type === 'ready') { this.ready = true; clearTimeout(this._readyTimer); return; }
    const job = this.pending.get(m.id);
    if (!job) return;
    this.pending.delete(m.id);
    // The voxels come back whatever the outcome.
    if (m.data) job.cube.data = m.data;
    if (m.error) job.reject(new Error(m.error));
    else job.resolve({ ...m.prep, offMain: true });
  }

  _fail(why) {
    this.broken = true;
    this.ready = false;
    clearTimeout(this._readyTimer);
    try { if (this.worker) this.worker.terminate(); } catch { /* gone */ }
    this.worker = null;
    // Voxels in flight are lost with the worker: their loads fail (and are loaded again).
    for (const job of this.pending.values()) job.reject(new Error(`${why}: load the cube again`));
    this.pending.clear();
  }

  /**
   * The cube's prepared values (prepareCube), from the worker when it can
   * take the cube's buffer, else computed here. `opts`: {cell, aboves, store}.
   * `cube.data` is detached while the worker holds it.
   */
  prepare(cube, opts = {}) {
    const { data, dims, encoding } = cube;
    if (!this.usesWorker || !ownsBuffer(data)) {
      this.stats.sync++;
      try { return Promise.resolve(prepareCube(data, dims, encoding, opts)); } catch (e) { return Promise.reject(e); }
    }
    this.stats.worker++;
    const id = this.nextId++;
    return new Promise((resolve, reject) => {
      this.pending.set(id, { cube, resolve, reject });
      try {
        this.worker.postMessage({ id, data, dims, encoding, opts }, [data.buffer]);
      } catch (e) {
        this.pending.delete(id);
        this.stats.worker--; this.stats.sync++;
        // Not sent: computed here, while the buffer is still the page's.
        if (data.buffer.byteLength === 0 && data.length) { reject(new Error('the cube\'s voxels were lost on their way to the worker: load the cube again')); return; }
        try { resolve(prepareCube(data, dims, encoding, opts)); } catch (e2) { reject(e2); }
      }
    });
  }
}

/** The newest request wins: begin() a ticket, isCurrent(ticket) after each wait. */
export class Newest {
  constructor() { this.latest = 0; }
  begin() { return ++this.latest; }
  isCurrent(t) { return t === this.latest; }
}

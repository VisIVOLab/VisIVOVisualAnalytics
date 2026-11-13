// The real controller models in the headset (WebXR Input Profiles assets,
// vendored under backend/app/static/vr/models/ and served with the page;
// nothing is fetched from a CDN). For each controller (an XRInputSource
// without a hand) the first of its `profiles` the page has an asset for is
// loaded once (profile JSON, .glb, material textures), uploaded once to
// the GPU (renderer.uploadModel) and shared by every input source with the
// same asset; each source gets its own rig (controller-profiles.js) so its
// buttons move its own model. Until the model is ready, or when no profile
// matches or loading fails, the caller draws its own model (models.js).
//
// Memory, at session start above all (the Quest's GPU, issue #229): one
// upload per asset; textures decoded straight to at most MODEL_TEXTURE_MAX a
// side (the size read from the image's header, never the full 2048² first),
// one image at a time and one asset at a time, the bitmaps closed once
// uploaded (a lost context decodes them again from the file's bytes). The
// profiles a headset used last are remembered (localStorage) and loaded on
// the 2-D page, before Enter VR; a first session loads them when the
// controllers appear. A failed load is tried once more after 10 s.

import { parseGLB, readGLTF, imageSize, fitSize, imageUses } from './gltf.js';
import { selectProfileId, layoutFor, makeRig, applyResponses, updateWorld } from './controller-profiles.js';
import { MODEL_TEXTURE_MAX } from './renderer.js';

const REMEMBER_KEY = 'visivo.vr.controllerProfiles';
/** A failed source is tried again once, this long after its failure. */
export const RETRY_MS = 10000;
const MAX_TRIES = 2;

const DECODE_OPTS = { imageOrientation: 'none', premultiplyAlpha: 'none', colorSpaceConversion: 'none' };

/** An embedded image as an ImageBitmap at most `max` a side, decoded at that size; or null. */
async function decodeImage(im, max) {
  if (typeof createImageBitmap !== 'function') return null;
  const blob = new Blob([im.bytes], { type: im.mime });
  const size = imageSize(im.bytes);
  if (!size) return createImageBitmap(blob, DECODE_OPTS);
  const fit = fitSize(size, max);
  if (fit.width === size.width && fit.height === size.height) return createImageBitmap(blob, DECODE_OPTS);
  return createImageBitmap(blob, { ...DECODE_OPTS, resizeWidth: fit.width, resizeHeight: fit.height, resizeQuality: 'high' });
}

/**
 * The images a model's materials use (base colour, metallic-roughness,
 * emissive: gltf.js imageUses), one at a time; all closed if one fails.
 */
export async function decodeModelImages(model) {
  const used = imageUses(model);
  const out = model.images.map(() => null);
  try {
    for (let i = 0; i < model.images.length; i++) {
      if (used.has(i)) out[i] = await decodeImage(model.images[i], MODEL_TEXTURE_MAX);
    }
    return out;
  } catch (err) {
    for (const b of out) if (b && b.close) b.close();
    throw err;
  }
}

function storage() {
  try { return typeof localStorage !== 'undefined' ? localStorage : null; } catch { return null; }
}

export class ControllerModels {
  /**
   * `renderer` a Renderer (uploadModel); `base` the models directory's URL
   * (by default ../models/ beside this module, so a page served under a
   * proxy prefix finds it too); `log(level, event, detail)` optional;
   * `now()` a clock in ms (tests).
   */
  constructor(renderer, { base = new URL('../models/', import.meta.url), fetchImpl, log, now } = {}) {
    this.R = renderer;
    this.base = base;
    this.fetch = fetchImpl || ((u) => fetch(u));
    this.log = log || (() => {});
    this.now = now || (() => performance.now());
    this.list = null;                // profilesList.json, once
    this.profiles = new Map();       // profile URL -> Promise<profile JSON>
    this.assets = new Map();         // asset URL -> Promise<{model, handle}>
    this.queue = Promise.resolve();  // one asset loads at a time
    this.sources = new WeakMap();    // XRInputSource -> entry
    this.status = {};                // handedness -> entry: for the page's checks
    this.preloaded = [];             // "id/hand" loaded on the 2-D page
  }

  _cached(map, key, make) {
    if (!map.has(key)) {
      const p = make();
      map.set(key, p);
      p.catch(() => { if (map.get(key) === p) map.delete(key); });   // tried again next time
    }
    return map.get(key);
  }

  async _profiles() {
    if (!this.list) {
      const p = (async () => {
        const r = await this.fetch(new URL('profilesList.json', this.base));
        if (!r.ok) throw new Error(`profilesList.json: HTTP ${r.status}`);
        return r.json();
      })();
      this.list = p;
      p.catch(() => { if (this.list === p) this.list = null; });
    }
    return this.list;
  }

  async _asset(profileId, handedness) {
    const list = await this._profiles();
    if (!Object.prototype.hasOwnProperty.call(list, profileId)) throw new Error(`${profileId} is not vendored`);
    const profileUrl = new URL(list[profileId].path, this.base).href;
    const profile = await this._cached(this.profiles, profileUrl, async () => {
      const r = await this.fetch(profileUrl);
      if (!r.ok) throw new Error(`${profileId} profile: HTTP ${r.status}`);
      return r.json();
    });
    const layout = layoutFor(profile, handedness);
    if (!layout || !layout.assetPath) throw new Error(`${profileId} has no ${handedness} layout`);
    const url = new URL(layout.assetPath, profileUrl).href;
    const { model, handle } = await this._cached(this.assets, url, () => {
      // Queued: one asset fetched, decoded and uploaded at a time.
      const run = this.queue.then(async () => {
        const r = await this.fetch(url);
        if (!r.ok) throw new Error(`${layout.assetPath}: HTTP ${r.status}`);
        const model = readGLTF(parseGLB(await r.arrayBuffer()));
        const bitmaps = await decodeModelImages(model);
        return { model, handle: this.R.uploadModel(model, bitmaps, { redecode: () => decodeModelImages(model) }) };
      });
      this.queue = run.catch(() => {});
      return run;
    });
    return { model, handle, layout };
  }

  /**
   * On the 2-D page: load the profiles this browser's controllers used last
   * time (nothing when none is remembered), so entering VR finds them ready.
   */
  preload() {
    let remembered = null;
    try { remembered = JSON.parse((storage() && storage().getItem(REMEMBER_KEY)) || 'null'); } catch { remembered = null; }
    if (!remembered || typeof remembered !== 'object') return Promise.resolve([]);
    const jobs = [];
    for (const hand of ['left', 'right']) {
      const id = remembered[hand];
      if (typeof id !== 'string') continue;
      jobs.push(this._asset(id, hand).then(() => { this.preloaded.push(`${id}/${hand}`); return `${id}/${hand}`; },
        (err) => { this.log('warn', 'controller-model-preload-failed', `${hand}: ${String((err && err.message) || err)}`); return null; }));
    }
    return Promise.all(jobs);
  }

  _remember(hand, id) {
    const s = storage();
    if (!s || (hand !== 'left' && hand !== 'right')) return;
    try {
      const cur = JSON.parse(s.getItem(REMEMBER_KEY) || '{}') || {};
      if (cur[hand] === id) return;
      cur[hand] = id;
      s.setItem(REMEMBER_KEY, JSON.stringify(cur));
    } catch { /* not kept: loaded at the next session's start instead */ }
  }

  _load(src, e) {
    const hand = src.handedness || 'none';
    e.status = 'loading';
    e.tries++;
    (async () => {
      const list = await this._profiles();
      const id = selectProfileId(Array.from(src.profiles || []), list);
      if (!id) throw Object.assign(new Error(`no vendored profile among [${Array.from(src.profiles || []).join(', ')}]`), { final: true });
      e.profileId = id;
      const { model, handle, layout } = await this._asset(id, hand);
      e.rig = makeRig(model, layout);
      e.handle = handle;
      e.status = 'ready';
      this._remember(hand, id);
      this.log('info', 'controller-model', `${hand}: ${id}`);
    })().catch((err) => {
      e.status = 'failed';
      e.error = String((err && err.message) || err);
      e.failedAt = this.now();
      if (err && err.final) e.tries = MAX_TRIES;      // no profile matches: nothing to retry
      this.log('warn', 'controller-model-failed', `${hand}: ${e.error}`);
    });
  }

  /** The entry of a source, loading its model the first time it is seen (and once more 10 s after a failure). */
  entry(src) {
    let e = this.sources.get(src);
    if (!e) {
      e = { status: 'loading', profileId: null, rig: null, handle: null, error: '', tries: 0, failedAt: 0 };
      this.sources.set(src, e);
      this.status[src.handedness || 'none'] = e;
      this._load(src, e);
    } else if (e.status === 'failed' && e.tries < MAX_TRIES && this.now() - e.failedAt >= RETRY_MS) {
      this._load(src, e);
    }
    return e;
  }

  /**
   * This frame's pose of a source's model: the rig moved by its gamepad and
   * placed at the grip pose `grip` (4×4). Returns the entry when it can be
   * drawn, else null (the caller draws its own model). No allocation once loaded.
   */
  pose(src, grip, gamepad) {
    const e = this.entry(src);
    if (e.status !== 'ready') return null;
    applyResponses(e.rig, gamepad);
    updateWorld(e.rig, grip);
    return e;
  }
}

// The real controller models in the headset (WebXR Input Profiles assets,
// vendored under backend/app/static/vr/models/ and served with the page;
// nothing is fetched from a CDN). For each controller (an XRInputSource
// without a hand) the first of its `profiles` the page has an asset for is
// loaded once (profile JSON, .glb, base-colour textures), uploaded once to
// the GPU (renderer.uploadModel) and shared by every input source with the
// same asset; each source gets its own rig (controller-profiles.js) so its
// buttons move its own model. Until the model is ready, or when no profile
// matches or loading fails, the caller draws its own model (models.js).
//
// GPU memory is bounded: at most one upload per vendored asset (eight .glb
// files), textures at most MODEL_TEXTURE_MAX a side.

import { parseGLB, readGLTF } from './gltf.js';
import { selectProfileId, layoutFor, makeRig, applyResponses, updateWorld } from './controller-profiles.js';
import { MODEL_TEXTURE_MAX } from './renderer.js';

/** An embedded image as an ImageBitmap at most `max` a side, or null. */
async function decodeImage(im, max) {
  if (typeof createImageBitmap !== 'function') return null;
  const blob = new Blob([im.bytes], { type: im.mime });
  const opts = { imageOrientation: 'none', premultiplyAlpha: 'none', colorSpaceConversion: 'none' };
  const full = await createImageBitmap(blob, opts);
  const s = Math.min(1, max / Math.max(full.width, full.height));
  if (s >= 1) return full;
  const w = Math.max(1, Math.round(full.width * s)), h = Math.max(1, Math.round(full.height * s));
  const small = await createImageBitmap(full, { ...opts, resizeWidth: w, resizeHeight: h, resizeQuality: 'high' });
  full.close();
  return small;
}

export class ControllerModels {
  /**
   * `renderer` a Renderer (uploadModel); `base` the models directory's URL
   * (by default ../models/ beside this module, so a page served under a
   * proxy prefix finds it too); `log(level, event, detail)` optional.
   */
  constructor(renderer, { base = new URL('../models/', import.meta.url), fetchImpl, log } = {}) {
    this.R = renderer;
    this.base = base;
    this.fetch = fetchImpl || ((u) => fetch(u));
    this.log = log || (() => {});
    this.list = null;                // profilesList.json, once
    this.assets = new Map();         // asset URL -> Promise<{model, handle, profile}>
    this.sources = new WeakMap();    // XRInputSource -> {status, profileId, rig, handle, error}
    this.status = {};                // handedness -> {status, profileId, error}: for the page's checks
  }

  async _profiles() {
    if (!this.list) {
      this.list = (async () => {
        const r = await this.fetch(new URL('profilesList.json', this.base));
        if (!r.ok) throw new Error(`profilesList.json: HTTP ${r.status}`);
        return r.json();
      })();
      this.list.catch(() => { this.list = null; });   // tried again by the next new source
    }
    return this.list;
  }

  async _asset(profileId, handedness) {
    const list = await this._profiles();
    const profileUrl = new URL(list[profileId].path, this.base);
    const pr = await this.fetch(profileUrl);
    if (!pr.ok) throw new Error(`${profileId} profile: HTTP ${pr.status}`);
    const profile = await pr.json();
    const layout = layoutFor(profile, handedness);
    if (!layout || !layout.assetPath) throw new Error(`${profileId} has no ${handedness} layout`);
    const url = new URL(layout.assetPath, profileUrl).href;
    if (!this.assets.has(url)) {
      const p = (async () => {
        const r = await this.fetch(url);
        if (!r.ok) throw new Error(`${layout.assetPath}: HTTP ${r.status}`);
        const model = readGLTF(parseGLB(await r.arrayBuffer()));
        // Only the images a material uses as its base colour are decoded.
        const used = new Set(model.materials.map((m) => m.texture).filter((t) => t >= 0));
        const bitmaps = await Promise.all(model.images.map((im, i) => (used.has(i) ? decodeImage(im, MODEL_TEXTURE_MAX) : null)));
        return { model, handle: this.R.uploadModel(model, bitmaps) };
      })();
      this.assets.set(url, p);
      p.catch(() => this.assets.delete(url));   // a failed asset may be tried again by a new source
    }
    const { model, handle } = await this.assets.get(url);
    return { model, handle, layout };
  }

  /** The entry of a source, loading its model the first time it is seen. */
  entry(src) {
    let e = this.sources.get(src);
    if (e) return e;
    e = { status: 'loading', profileId: null, rig: null, handle: null, error: '' };
    this.sources.set(src, e);
    const hand = src.handedness || 'none';
    this.status[hand] = e;
    (async () => {
      const list = await this._profiles();
      const id = selectProfileId(Array.from(src.profiles || []), list);
      if (!id) throw new Error(`no vendored profile among [${Array.from(src.profiles || []).join(', ')}]`);
      e.profileId = id;
      const { model, handle, layout } = await this._asset(id, hand);
      e.rig = makeRig(model, layout);
      e.handle = handle;
      e.status = 'ready';
      this.log('info', 'controller-model', `${hand}: ${id}`);
    })().catch((err) => {
      e.status = 'failed';
      e.error = String((err && err.message) || err);
      this.log('warn', 'controller-model-failed', `${hand}: ${e.error}`);
    });
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

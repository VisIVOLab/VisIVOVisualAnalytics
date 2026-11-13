// Sharp text in the headset through WebXR composition layers (issue #226,
// point 2). The hand menu, the controller labels, the cursor readout, the
// result windows, the numbers and the message card are each a quad layer
// (XRWebGLBinding.createQuadLayer): the compositor samples their canvas at
// its own resolution, so they stay sharp whatever the eye-buffer scale the
// frame-rate governor chose, and they cost no ray-marching pixels.
//
// Design:
// - The page's projection layer stays the XRWebGLLayer it always drew into;
//   with layers it is the first entry of the session's `layers` array (a
//   session with layers has no baseLayer), the quads follow, back to front.
// - Each overlay is the same canvas, pose (a 4×4 model) and size in metres
//   as the in-scene panel, so the ray code (vr-panel.js rayPanel) hits the
//   same plane: menus and windows are used exactly as before.
// - A quad's pixels are copied only when its canvas changed (the renderer's
//   panel version) or the compositor asks (needsRedraw); its pose and size
//   every frame. The array changes only when the set of overlays on show
//   changes. A quad stays in its pool when it leaves the screen (bounded:
//   one per overlay id).
// - Fallback: no XRWebGLBinding, the 'layers' feature not granted, or any
//   failure (creating a quad, updating the render state) turns the layers off
//   for the session; the page then draws the same overlays in the scene as
//   before (drawPanel), and says which in its diagnostics.
//
// Pure where it can be (the pose split, the array diff): unit-tested with a
// fake binding in backend/tests/vr/xr-layers.test.mjs.

/** A quaternion [x, y, z, w] from an orthonormal basis (columns x, y, z). */
export function quatFromBasis(x, y, z) {
  const [m00, m10, m20] = x, [m01, m11, m21] = y, [m02, m12, m22] = z;
  const tr = m00 + m11 + m22;
  let q;
  if (tr > 0) {
    const s = Math.sqrt(tr + 1) * 2;
    q = [(m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25 * s];
  } else if (m00 > m11 && m00 > m22) {
    const s = Math.sqrt(1 + m00 - m11 - m22) * 2;
    q = [0.25 * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s];
  } else if (m11 > m22) {
    const s = Math.sqrt(1 + m11 - m00 - m22) * 2;
    q = [(m01 + m10) / s, 0.25 * s, (m12 + m21) / s, (m02 - m20) / s];
  } else {
    const s = Math.sqrt(1 + m22 - m00 - m11) * 2;
    q = [(m02 + m20) / s, (m12 + m21) / s, 0.25 * s, (m10 - m01) / s];
  }
  const n = Math.hypot(...q) || 1;
  return q.map((v) => v / n);
}

/**
 * A panel's model (column-major 4×4: rotation × uniform or per-axis scale,
 * then translation) split into what a quad layer takes: a rigid transform
 * (position, orientation) and the scale of its x and y axes, by which the
 * panel's size in metres is multiplied.
 */
export function rigidFromModel(m) {
  const col = (i) => [m[4 * i], m[4 * i + 1], m[4 * i + 2]];
  const len = (v) => Math.hypot(v[0], v[1], v[2]) || 1;
  const x = col(0), y = col(1), z = col(2);
  const sx = len(x), sy = len(y), sz = len(z);
  const q = quatFromBasis(x.map((v) => v / sx), y.map((v) => v / sy), z.map((v) => v / sz));
  return { position: [m[12], m[13], m[14]], orientation: q, scale: [sx, sy] };
}

/** Did the ordered list of overlay ids change? */
export function orderChanged(prev, next) {
  if (!prev || prev.length !== next.length) return true;
  for (let i = 0; i < next.length; i++) if (prev[i] !== next[i]) return true;
  return false;
}

/** Can this session have quad layers at all (the binding exists, and the
 *  'layers' feature was granted where the browser says what it granted)? */
export function layersAvailable(session, scope = globalThis) {
  if (!session || typeof scope.XRWebGLBinding !== 'function') return false;
  const f = session.enabledFeatures;
  if (f && typeof f.includes === 'function') return f.includes('layers');
  return true;
}

/**
 * The quad layers of one session. `opts`:
 * - binding: an XRWebGLBinding (or a fake in tests);
 * - session, gl, space (the reference space the models are in);
 * - makeTransform(position, orientation): an XRRigidTransform;
 * - flipY: canvas rows top-down into the GL texture (true: the compositor
 *   reads a GL texture bottom-up, as everywhere in WebGL);
 * - onFail(message): told once when the layers are turned off.
 */
export class QuadLayers {
  constructor({ binding, session, gl, space, makeTransform, flipY = true, onFail = () => {} }) {
    Object.assign(this, { binding, session, gl, space, makeTransform, flipY, onFail });
    this.pool = new Map();        // id -> {layer, w, h, version, fresh}
    this.shown = null;            // the ids in the array now
    this.submitted = new Set();   // the quads of the last render state set (active from the next frame)
    this.base = null;
    this.active = true;
    this.why = '';
    this.copies = 0;              // canvases copied into a quad
    this.copyMs = [];
  }

  /** The projection layer (an XRWebGLLayer): first in the array. A new one
   *  (a smaller eye buffer) replaces the old at once, the quads kept, so no
   *  frame is drawn into a layer the session does not show. */
  setBase(glLayer) {
    const first = !this.base;
    this.base = glLayer;
    if (first) return;            // the session's array is set by the caller, then by frame()
    try {
      const quads = this.active && this.shown ? this.shown.map((id) => this.pool.get(id).layer) : [];
      this._submit(quads);
    } catch (err) {
      this._fail(err);
    }
  }

  _fail(err) {
    if (!this.active) return;
    this.active = false;
    this.why = String(err && err.message ? err.message : err);
    // Back to the projection layer alone; the page draws the overlays itself.
    try { this.session.updateRenderState({ layers: [this.base] }); } catch { /* the session decides */ }
    this.onFail(this.why);
  }

  _layerFor(item) {
    const c = item.handle && item.handle.canvas;
    if (!c) return null;
    const w = c.width, h = c.height;
    let rec = this.pool.get(item.id);
    if (rec && (rec.w !== w || rec.h !== h)) {
      try { rec.layer.destroy && rec.layer.destroy(); } catch { /* gone */ }
      this.pool.delete(item.id);
      this.shown = null;
      rec = null;
    }
    if (!rec) {
      const layer = this.binding.createQuadLayer({
        space: this.space, viewPixelWidth: w, viewPixelHeight: h, layout: 'mono',
        width: item.size[0], height: item.size[1],
      });
      rec = { layer, w, h, version: -1, fresh: true };
      this.pool.set(item.id, rec);
    }
    return rec;
  }

  /**
   * One XR frame: `items` [{id, model, size, handle}] back to front (handle:
   * a renderer panel with .canvas and .version). Returns true when they are
   * drawn as layers (the page must not draw them), false when it must.
   */
  frame(xrFrame, items) {
    if (!this.active || !this.base) return false;
    try {
      // getSubImage takes only a layer of the session's active array, and a
      // render state set now applies from the next frame: a quad is copied
      // from the frame after the one that submitted it (for that one frame
      // a new quad may show nothing).
      const live = this.submitted;
      const recs = [];
      for (const it of items) {
        const rec = this._layerFor(it);
        if (!rec) continue;
        const { position, orientation, scale } = rigidFromModel(it.model);
        rec.layer.transform = this.makeTransform(position, orientation);
        rec.layer.width = it.size[0] * scale[0];
        rec.layer.height = it.size[1] * scale[1];
        const v = it.handle.version || 0;
        if (live.has(rec.layer) && (rec.fresh || rec.version !== v || rec.layer.needsRedraw)) this._copy(xrFrame, rec, it.handle.canvas, v);
        recs.push([it.id, rec]);
      }
      const ids = recs.map(([id]) => id);
      if (orderChanged(this.shown, ids)) {
        this._submit(recs.map(([, r]) => r.layer));
        // A quad that comes back on show after a while is copied again (its
        // swapchain image may have been recycled while it was off).
        for (const [id, r] of this.pool) if (!ids.includes(id)) r.fresh = true;
        this.shown = ids;
      }
      return true;
    } catch (err) {
      this._fail(err);
      return false;
    }
  }

  /** The session's array: the projection layer, then the quads. */
  _submit(quads) {
    this.session.updateRenderState({ layers: [this.base, ...quads] });
    this.submitted = new Set(quads);
  }

  _copy(xrFrame, rec, canvas, version) {
    const gl = this.gl;
    const t0 = typeof performance !== 'undefined' ? performance.now() : 0;
    const sub = this.binding.getSubImage(rec.layer, xrFrame);
    const vp = sub.viewport || { x: 0, y: 0 };
    gl.bindTexture(gl.TEXTURE_2D, sub.colorTexture);
    gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, this.flipY);
    gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, true);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 4);
    try {
      gl.texSubImage2D(gl.TEXTURE_2D, 0, vp.x, vp.y, gl.RGBA, gl.UNSIGNED_BYTE, canvas);
    } finally {
      gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, false);
      gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, false);
      gl.bindTexture(gl.TEXTURE_2D, null);
    }
    rec.version = version;
    rec.fresh = false;
    this.copies++;
    if (typeof performance !== 'undefined') {
      this.copyMs.push(performance.now() - t0);
      if (this.copyMs.length > 60) this.copyMs.shift();
    }
  }

  /** The session ended: the quads go. */
  destroy() {
    for (const r of this.pool.values()) { try { r.layer.destroy && r.layer.destroy(); } catch { /* gone */ } }
    this.pool.clear();
    this.shown = null;
    this.submitted = new Set();
  }

  /** For the HUD and the diagnostics. */
  describe() {
    if (!this.active) return `in the scene (layers off: ${this.why || 'not available'})`;
    return `quad layers: ${this.shown ? this.shown.length : 0} on show, ${this.pool.size} made, ${this.copies} copies`;
  }
}

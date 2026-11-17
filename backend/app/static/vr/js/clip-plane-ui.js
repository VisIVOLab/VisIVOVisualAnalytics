// The page's side of the free clipping plane (issue #369, clip-plane.js): the
// plane of the simulation volume on show, the Range page's presses, the
// trigger's grab (and the mouse's drag on the 2-D page), the primary stick,
// the gizmo, the shader's numbers, the render session's command and the
// undo step. main.js owns the cube and the modes; this module owns only the
// plane, and is told when the cube changes (onCube).
//
// For other draws (isosurfaces, issue #370): `current()` is the plane in
// effect (null when none), `shader()` its uniforms for CLIP_GLSL
// (clip-plane.js setClipUniforms), both cached until the plane changes.

import * as CP from './clip-plane.js';
import * as M from './mat4.js';
import { faceEye } from './readout.js';

/** The handle's dot, metres in the world (as the crop's, crop-gizmo.js HANDLE_M). */
const HANDLE_M = 0.03;
/** The primary stick ↕ moves the plane this fraction of the cube's longest side a second. */
const MOVE_PER_S = 0.25;

/**
 * `deps`: {R (renderer), state (main.js state: cube, info, extent, model,
 * clip), isSimulation(), toolOn() (the Range tool in use is the plane),
 * snapshot() (the look, for an undo step), pushUndo(snap), changed() (labels
 * and decorations to redraw), buzz(kind) (the primary hand), dot(hot) (the
 * handle's texture)}.
 */
export function createClipPlane(deps) {
  const { R, state } = deps;
  let plane = null;            // clip-plane.js state, for the cube of plane.dims
  let grab = null;             // a trigger or mouse drag under way
  let hover = false;           // the ray on the plane's section
  let linesKey = '';
  let cache = { plane: null, avail: false, shader: null, cmd: null };

  const dims = () => (state.info && Array.isArray(state.info.full_shape) ? state.info.full_shape.map(Number) : null);
  /** Is there a plane to cut with (a simulation volume on show)? */
  const available = () => !!(state.cube && state.extent && deps.isSimulation() && plane);
  /** The plane in effect: on, over a simulation volume; else null. */
  const current = () => (available() && plane.on ? plane : null);

  function refresh() {
    // Kept until the plane changes or stops (or starts) applying: a radio cube
    // of the same shape after a simulation volume is never cut by its plane.
    const avail = available();
    if (cache.plane === plane && cache.avail === avail) return;
    const cur = current();
    cache = { plane, avail, shader: cur ? CP.shaderParams(cur) : null, cmd: CP.command(cur) };
  }

  function set(p, { undoSnap = null } = {}) {
    if (undoSnap) deps.pushUndo(undoSnap);
    plane = p;
    cache.plane = undefined;
    linesKey = '';
    deps.changed();
  }

  /** A cube was shown: a plane of its own unless it has the same shape (another variable or snapshot of the same run). */
  function onCube() {
    const d = dims();
    finishGrab();
    if (!d || d.length !== 3) { plane = null; cache.plane = undefined; return; }
    if (!CP.fitsCube(plane, d)) { plane = CP.defaultPlane(d); cache.plane = undefined; linesKey = ''; }
    // Not a simulation volume: no plane (a radio cube of the same shape starts without one).
    if (!deps.isSimulation() && plane.on) { plane = { ...CP.copyPlane(plane), on: false }; cache.plane = undefined; linesKey = ''; }
  }

  // ── The menu ─────────────────────────────────────────────────────────────
  /** A Range page press; true when it was the plane's. */
  function press(id) {
    if (!id.startsWith('plane:') || !available()) return false;
    const snap = deps.snapshot();
    const what = id.slice(6);
    if (what === 'on') set({ ...CP.copyPlane(plane), on: !plane.on }, { undoSnap: snap });
    else if (what === 'flip') set(CP.flip(plane), { undoSnap: snap });
    else if (what === 'snap') set(CP.snap(plane), { undoSnap: snap });
    else if (what === 'slab') set({ ...CP.copyPlane(plane), slab: !plane.slab }, { undoSnap: snap });
    else if (what === 'reset') set(CP.reset(plane), { undoSnap: snap });
    else return false;
    // Flip, snap, slab and reset turn the plane on: they are what it is for.
    if (what !== 'on' && !plane.on) set({ ...CP.copyPlane(plane), on: true });
    return true;
  }

  /** The hand menu's context: {available, on, slab}. */
  function menuContext() {
    return { available: available(), on: !!(plane && plane.on), slab: !!(plane && plane.slab) };
  }

  /** The Range page's line about the plane. */
  const status = () => (available() ? CP.describe(plane) : '');

  // ── The trigger, the stick, the mouse ────────────────────────────────────
  const toModel = (m) => M.multiply(M.invert(state.model), m);
  const modelRay = (r) => {
    const inv = M.invert(state.model);
    const o = M.transformPoint(inv, r.o);
    return { o, d: M.sub(M.transformPoint(inv, M.add(r.o, r.d)), o) };
  };
  /** Does the (model) ray meet the plane's section inside the crop? */
  function onSection(ray) {
    if (!current()) return false;
    const hit = CP.rayPlane(plane, state.extent, ray);
    if (!hit) return false;
    const t = CP.modelToBox(hit.point, state.extent);
    return t.every((v, i) => v >= state.clip.min[i] - 1e-6 && v <= state.clip.max[i] + 1e-6);
  }

  /**
   * The plane under the controller this frame: `worldRay` (null when the menu
   * or a window takes it), the trigger held (`pressed`) and just pressed
   * (`edge`), the hand's matrix (world). The trigger anywhere off the menus
   * takes the plane (turning it on). Returns whether a grab holds the trigger.
   */
  function pointer(worldRay, pressed, edge, handWorld) {
    if (!available() || !deps.toolOn()) { finishGrab(); hover = false; return false; }
    const ray = worldRay ? modelRay(worldRay) : null;
    if (grab) {
      if (pressed && ray && handWorld) {
        const p = CP.dragGrab(grab.g, state.extent, ray, toModel(handWorld));
        plane = { ...p, on: true };
        cache.plane = undefined;
        deps.changed();
        return true;
      }
      if (pressed) return true;
      finishGrab();
    }
    hover = !!ray && onSection(ray);
    if (edge && ray && handWorld) {
      // The undo step's plane before the grab, before it is turned on: a click that only turns it on is a step too.
      const snap = deps.snapshot(), before = JSON.stringify(plane);
      if (!plane.on) { plane = { ...CP.copyPlane(plane), on: true }; cache.plane = undefined; }
      grab = { g: CP.startGrab(plane, state.extent, ray, toModel(handWorld)), snap, before };
      deps.buzz('press');
      deps.changed();
      return true;
    }
    return false;
  }

  /** A grab ends: one undo step, when it changed the plane. */
  function finishGrab() {
    if (!grab) return;
    const g = grab;
    grab = null;
    if (g.snap && JSON.stringify(plane) !== g.before) deps.pushUndo(g.snap);
    linesKey = '';
    deps.changed();
  }

  /** The primary stick with the plane tool: ↕ along the normal, ↔ the slab's thickness.
   *  Its undo step is main.js's: one per push of a stick, from rest to rest
   *  (the look, the plane in it, taken before the push's first change). */
  function stick(action, v, dtS) {
    if (!available() || !plane.on) return false;
    if (action === 'plane-move') {
      const L = Math.max(...plane.dims);
      plane = CP.nudge(plane, v * dtS * MOVE_PER_S * L);
    } else if (action === 'plane-width') {
      if (!plane.slab) return true;
      plane = CP.scaleThickness(plane, Math.exp(v * dtS));
    } else {
      return false;
    }
    cache.plane = undefined;
    deps.changed();
    return true;
  }

  /** The 2-D page: a press on the plane's section (or anywhere with Shift)
   *  starts a drag; true when the plane took it. */
  function mouseDown(worldRay, shift) {
    if (!available() || !deps.toolOn()) return false;
    const ray = modelRay(worldRay);
    if (!(current() && onSection(ray)) && !shift) return false;
    const snap = deps.snapshot(), before = JSON.stringify(plane);
    if (!plane.on) { plane = { ...CP.copyPlane(plane), on: true }; cache.plane = undefined; }
    grab = { mouse: true, shift, d0: ray.d, u0: CP.alongNormal(plane, state.extent, ray), p0: CP.copyPlane(plane),
      snap, before };
    deps.changed();
    return true;
  }
  /** The drag goes on: along the normal, or (Shift) tilted as the pointer turns. */
  function mouseMove(worldRay) {
    if (!grab || !grab.mouse) return false;
    const ray = modelRay(worldRay);
    if (grab.shift) {
      plane = CP.tiltByRays(grab.p0, state.extent, grab.d0, ray.d, 3);
    } else {
      const u = CP.alongNormal(grab.p0, state.extent, ray);
      if (u !== null && grab.u0 !== null) plane = CP.moveModel(grab.p0, state.extent, u - grab.u0);
    }
    cache.plane = undefined;
    deps.changed();
    return true;
  }
  function mouseUp() { if (grab && grab.mouse) finishGrab(); }
  /** The pointer over the 2-D page with no button down: the section lit under it. */
  function mouseHover(worldRay) {
    hover = !!(available() && deps.toolOn() && worldRay && onSection(modelRay(worldRay)));
  }

  // ── Drawing ──────────────────────────────────────────────────────────────
  /** The gizmo for one eye (`eye` its position, for the dot facing it): while the plane tool is in use. */
  function draw(view, eye) {
    if (!current() || !deps.toolOn()) return;
    const hot = !!grab || hover;
    // Made again only when the plane, the crop, the box or the lighting changed
    // (each is a new object when it changes): nothing allocated on a still frame.
    const k = drawn;
    if (k.plane !== plane || k.clip !== state.clip || k.extent !== state.extent || k.hot !== hot || linesKey !== 'drawn') {
      const g = CP.gizmo(plane, state.extent, state.clip, { hot });
      R.setLines(g.edges, R.planeLines);
      Object.assign(k, { plane, clip: state.clip, extent: state.extent, hot, handle: g.handle, size: hot ? HANDLE_M * 1.5 : HANDLE_M });
      linesKey = 'drawn';
    }
    R.drawLines(view, state.model, R.planeLines);
    R.drawPanel(view, faceEye(M.transformPoint(state.model, k.handle), eye), deps.dot(hot), [k.size, k.size]);
  }
  const drawn = { plane: null, clip: null, extent: null, hot: false, handle: [0, 0, 0], size: HANDLE_M };

  // ── The shader, the session, the undo ────────────────────────────────────
  function shader() { refresh(); return cache.shader; }
  function command() { refresh(); return cache.cmd; }
  /** What makes a refined frame stale: the plane in effect. */
  const lookKey = () => { const c = current(); return c ? [c.origin, c.normal, c.slab, c.slab ? c.thickness : 0] : null; };
  const lookError = () => CP.planeError(current());
  function snapshot() { return plane ? CP.copyPlane(plane) : null; }
  function restore(s) {
    finishGrab();
    if (s && plane && CP.fitsCube(s, plane.dims)) { plane = CP.copyPlane(s); cache.plane = undefined; linesKey = ''; }
  }

  return {
    get plane() { return plane; }, current, available, onCube, press, menuContext, status, pointer, finishGrab, stick,
    mouseDown, mouseMove, mouseUp, mouseHover, draw, shader, command, lookKey, lookError, snapshot, restore,
    get dragging() { return !!grab; },
  };
}

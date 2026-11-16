// The page's side of the isosurfaces (issue #370; isosurface.js is the pure
// part, isosurface-gl.js the drawing): the surfaces wanted (a level in the
// field's units, a colour, an opacity, shown or hidden), their extraction on
// the backend (/v1/isosurface/extract as a task, then each mesh), their place
// in the GPU buffers, the Surfaces page's presses, sliders and lines, and the
// exports. main.js owns the cube on show and tells this module when it
// changes (onCube); the volume is hidden while "Surfaces only" is chosen.
// No coarse preview is made on the headset while the backend works: the
// menu shows the extraction's progress, and the surface appears whole.

import * as ISO from './isosurface.js';
import { IsoSurfaceGL } from './isosurface-gl.js';
import { hookedFetch } from './api.js';

/** The wait after the last change of a level before it is asked for (ms). */
const SETTLE_MS = 350;
/** The poll of an extraction task (ms). */
const POLL_MS = 300;
/** A surface whose extraction or fetch failed for a reason that may pass (no
 *  answer, a 5xx, a mesh gone from the backend's cache) is asked again this
 *  many times, RETRY_MS × the attempt apart. */
const RETRIES = 3;
const RETRY_MS = 1000;
const NO_ROOM = 'no room left in the headset\'s surface buffers';

/** An error worth asking again for: no answer, a server error, a mesh not
 *  found (evicted from the cache: extracted again). A refusal (4xx) or a
 *  failed task (the backend said why) is not. */
export function transient(err) {
  if (!err) return false;
  if (err.network || err.timedOut) return true;
  const st = Number(err.status) || 0;
  return st >= 500 || st === 404;
}

/**
 * `deps`: {gl, state (main.js: cube, info, open), backend() (the Backend or
 * null), datasetId(), thrNorm(), cmaxNorm(), changed() (menu and labels to
 * redraw), log(level, event, detail), toast(text, key), budget (triangles a
 * surface; ?isotris=)}.
 */
export function createIsosurfaces(deps) {
  const { state } = deps;
  const glr = new IsoSurfaceGL(deps.gl);
  const iso = {
    surfaces: [],        // [{value, colour, opacity, shown, made, mesh, at, pending, progress, error}]
    edit: 0,
    volumeHidden: false,
    dataset: '',         // the dataset the surfaces are of
    gen: 0,              // bumped by a cube change: answers for an older one are dropped
    running: false,
    task: '',            // the extraction task under way (cancelled when superseded)
    scale: '',           // the cube's scale the surfaces were asked on
    timer: null,
    status: '',          // the last extraction's or export's line
    error: '',
    last: null,          // {ms, triangles, hits}
  };
  const budget = Number.isFinite(deps.budget) && deps.budget >= 1000 ? Math.min(deps.budget, ISO.SURFACE_TRIANGLES) : ISO.SURFACE_TRIANGLES;

  const cube = () => state.cube;
  const usable = () => !!(cube() && state.open && cube().mode !== 'synthetic');
  const scaleOf = () => (cube() && cube().scale === 'log' ? 'log' : 'linear');
  const valueOfNorm = (t) => { const c = cube(); const q = c.lo + t * (c.hi - c.lo); return c.scale === 'log' ? 10 ** q : q; };
  const normOfValue = (v) => {
    const c = cube();
    if (!(c.hi > c.lo)) return 0;
    const t = c.scale === 'log' ? (v > 0 ? (Math.log10(v) - c.lo) / (c.hi - c.lo) : 0) : (v - c.lo) / (c.hi - c.lo);
    return Math.min(1, Math.max(0, t));
  };
  const unit = () => (cube() && cube().bunit) || '';

  /** The program and the buffers (13.2 MB), made once: main.js calls this
   *  when a cube is shown outside VR, so a session neither compiles nor
   *  allocates (else the first surface does). They are buffers, not 3-D
   *  textures (#229); nothing is allocated again afterwards. */
  function prepare() { if (usable()) glr.ensure(); }

  /** The extraction under way stopped on the backend (it is superseded). */
  function cancelTask() {
    const id = iso.task;
    iso.task = '';
    const b = id ? deps.backend() : null;
    if (b) b._json('DELETE', `/v1/tasks/${encodeURIComponent(id)}`).catch(() => {});
  }

  /** Another cube on show: its surfaces are another field's; all go (the
   *  old cube's extraction is cancelled). The same cube on another scale:
   *  its surfaces are asked again on that scale. */
  function onCube() {
    const id = state.open ? state.open.dataset_id : '';
    if (id === iso.dataset) {
      if (iso.scale && cube() && scaleOf() !== iso.scale && iso.surfaces.length) {
        iso.scale = scaleOf();
        for (const s of iso.surfaces) s.failures = 0;
        if (iso.running) cancelTask();
        schedule(0);
      }
      return;
    }
    cancelTask();
    iso.dataset = id;
    iso.scale = cube() ? scaleOf() : '';
    iso.gen++;
    iso.running = false;           // the old cube's answer is dropped
    iso.surfaces = [];
    iso.edit = 0;
    iso.error = iso.status = '';
    clearTimeout(iso.timer);
    pack();
  }

  function add() {
    if (!usable() || iso.surfaces.length >= ISO.MAX_SURFACES) return false;
    prepare();
    const t = ISO.newLevelNorm(iso.surfaces.map((s) => normOfValue(s.value)), deps.thrNorm(), deps.cmaxNorm());
    iso.surfaces.push({ value: valueOfNorm(t), colour: ISO.nextColour(iso.surfaces.map((s) => s.colour)), opacity: iso.surfaces.length ? 0.55 : 1,
      shown: true, made: null, mesh: null, at: null, pending: false, progress: 0, error: '', failures: 0 });
    iso.edit = iso.surfaces.length - 1;
    schedule(0);
    return true;
  }

  function remove(i) {
    if (!iso.surfaces[i]) return;
    iso.surfaces.splice(i, 1);
    iso.edit = Math.max(0, Math.min(iso.edit, iso.surfaces.length - 1));
    pack();
  }

  /** Every surface's place in the buffers; the moved or new ones written again. */
  function pack() {
    const list = iso.surfaces.map((s) => (s.mesh ? s.mesh : null));
    const at = ISO.planPacking(list);
    for (let i = 0; i < iso.surfaces.length; i++) {
      const s = iso.surfaces[i];
      if (!s.mesh) { s.at = null; continue; }
      if (!at[i]) { s.at = null; s.error = NO_ROOM; continue; }
      if (s.error === NO_ROOM) s.error = '';      // room again (a surface removed or smaller)
      if (!s.at || s.at.vOff !== at[i].vOff || s.at.tOff !== at[i].tOff || s.uploadedFor !== s.mesh) {
        glr.upload(s.mesh, at[i]);
        s.uploadedFor = s.mesh;
      }
      s.at = at[i];
    }
    syncSlots();
  }

  function syncSlots() {
    glr.setSlots(iso.surfaces.filter((s) => s.at && s.mesh).map((s) => ({ at: s.at, mesh: s.mesh, rgb: ISO.colourOf(s.colour), opacity: s.opacity, shown: s.shown })));
  }

  /** Ask for the surfaces whose level changed, `ms` after the last change. */
  function schedule(ms = SETTLE_MS) {
    clearTimeout(iso.timer);
    iso.timer = setTimeout(() => { iso.timer = null; run(); }, ms);
  }

  const wanted = (s) => !s.made || s.made.level !== s.value || s.made.scale !== scaleOf();

  async function run() {
    if (!usable()) return;
    if (iso.running) {
      // A level moved on while it was made: that extraction is stopped, the
      // newest asked for when its task ends (cancelled).
      if (iso.inFlight && iso.inFlight.some((s) => iso.surfaces.includes(s) && s.value !== s.askedFor)) cancelTask();
      return;
    }
    const todo = iso.surfaces.filter(wanted);
    if (!todo.length) return;
    const b = deps.backend();
    if (!b) return;
    const gen = iso.gen;
    const { body, refused } = ISO.extractRequest(state.open.dataset_id, todo.map((s) => s.value), scaleOf(), { budget });
    for (const s of todo) {
      if (refused.includes(s.value)) { s.error = 'a log scale takes levels above zero'; s.made = { level: s.value, scale: scaleOf(), triangles: 0 }; }
      else { s.pending = true; s.progress = 0; s.error = ''; s.askedFor = s.value; }
    }
    if (!body.levels.length) { deps.changed(); return; }
    iso.running = true;
    iso.inFlight = todo;
    iso.error = '';
    const t0 = performance.now();
    deps.changed();
    try {
      const created = await b._json('POST', '/v1/isosurface/extract', body);
      if (gen !== iso.gen) return;
      iso.task = created.task_id;
      let task = null;
      for (;;) {
        task = await b._json('GET', `/v1/tasks/${encodeURIComponent(created.task_id)}`);
        if (gen !== iso.gen) return;
        if (task.status !== 'pending' && task.status !== 'running') break;
        for (const s of todo) if (s.pending) s.progress = task.progress;
        deps.changed();
        await new Promise((r) => setTimeout(r, POLL_MS));
      }
      if (iso.task === created.task_id) iso.task = '';
      if (task.status === 'cancelled') {
        // Superseded (a level moved on): not an error; the newest is asked below.
        for (const s of todo) s.pending = false;
        schedule(0);
        return;
      }
      if (task.status !== 'completed') {
        const e = new Error(task.error || `the extraction ${task.status}`);
        e.status = 422;                 // the backend said why: not asked again as it is
        throw e;
      }
      const meshes = task.result.meshes;
      let tris = 0;
      for (let k = 0; k < meshes.length; k++) {
        const made = meshes[k];
        const resp = await hookedFetch(`/v1/isosurface/mesh/${made.mesh_id}`, { headers: b.headers(false) });
        if (!resp.ok) {
          const e = new Error(`the surface at ${ISO.levelText(made.level)} could not be read (${resp.status})`);
          e.status = resp.status;
          throw e;
        }
        const mesh = ISO.decodeMesh(await resp.arrayBuffer());
        if (!ISO.indicesValid(mesh)) throw new Error('a surface came with indices past its vertices');
        if (gen !== iso.gen) return;
        tris += mesh.triangles;
        // Every surface still at this level takes it (a level moved on since is asked again).
        for (const s of iso.surfaces) {
          if (s.value === body.levels[k] && wanted(s)) {
            s.made = { ...made, level: s.value, scale: body.scale };
            s.mesh = mesh;
            s.pending = false;
            s.error = '';
            s.failures = 0;
          }
        }
      }
      iso.last = { ms: performance.now() - t0, triangles: tris, hits: task.result.cache_hits };
      deps.log('info', 'isosurface', `${meshes.length} surface(s), ${tris} triangles, ${Math.round(iso.last.ms)} ms (${task.result.cache_hits} from the cache)`);
      pack();
    } catch (err) {
      if (gen !== iso.gen) return;
      iso.error = String(err && err.message ? err.message : err);
      const again = transient(err);
      let retryIn = 0;
      for (const s of todo) {
        if (!s.pending) continue;
        s.pending = false;
        s.failures = (s.failures || 0) + 1;
        // The old mesh is not left drawn as if it were this level's.
        s.mesh = null;
        s.made = null;
        if (again && s.failures < RETRIES) {
          s.error = `${iso.error} (asked again)`;
          retryIn = Math.max(retryIn, RETRY_MS * s.failures);
        } else {
          s.error = iso.error;
          s.made = { level: s.value, scale: scaleOf(), triangles: 0, failed: true };
        }
      }
      pack();
      deps.log('warning', 'isosurface-failed', iso.error);
      if (retryIn) schedule(retryIn);
    } finally {
      if (gen === iso.gen) { iso.running = false; iso.inFlight = null; }
      deps.changed();
    }
    // A level moved while this one was made (or the extraction was
    // cancelled for it): the newest is asked for.
    if (gen === iso.gen && !iso.timer && iso.surfaces.some((s) => wanted(s) && !s.pending)) schedule(0);
  }

  async function exportMesh(format) {
    const s = iso.surfaces[iso.edit];
    const b = deps.backend();
    if (!s || !s.made || !s.made.mesh_id || !b) return;
    try {
      const q = new URLSearchParams({ format, save: 'true', color: ISO.cssColour(s.colour) });
      const out = await b._json('GET', `/v1/isosurface/mesh/${s.made.mesh_id}/export?${q}`);
      iso.status = `Saved ${out.path.split('/').pop()} in the workspace's exports (isosurfaces/).`;
      deps.toast(`Surface ${iso.edit + 1} saved as ${format === 'glb' ? 'glTF' : 'PLY'}`, 'iso-export');
    } catch (err) {
      iso.status = `The export failed: ${err && err.message ? err.message : err}`;
    }
    deps.changed();
  }

  // ── The menu (hand-menu.js isoPage) ───────────────────────────────────────

  function menuContext() {
    if (!cube()) return null;
    return {
      none: usable() ? '' : 'Open a cube from the backend first.',
      surfaces: iso.surfaces.map((s) => ({ colour: s.colour, shown: s.shown, made: !!(s.made && s.made.mesh_id && s.made.triangles) })),
      edit: iso.edit, volumeHidden: iso.volumeHidden, max: ISO.MAX_SURFACES, busy: iso.running,
    };
  }

  function view(values, notes, status) {
    const s = iso.surfaces[iso.edit];
    const u = unit();
    if (s && cube()) {
      values['iso-level'] = normOfValue(s.value);
      values['iso-opacity'] = s.opacity;
      notes.isolevel = `${ISO.levelText(s.value, u)}${scaleOf() === 'log' ? ' (log scale)' : ''}`;
      notes.isoopacity = s.opacity.toFixed(2);
      notes.isocolour = s.colour;
    }
    notes.isolist = iso.surfaces.length ? '' : 'Add a surface: it starts at the threshold.';
    iso.surfaces.forEach((x, i) => status.push([ISO.surfaceLine(i, x, u), x.error ? '#ff8a8a' : x.pending ? '#ffd27a' : '#b9c2d0']));
    if (iso.last && !iso.running) status.push([`Last: ${(iso.last.triangles / 1000).toFixed(0)} k triangles in ${(iso.last.ms / 1000).toFixed(1)} s${iso.last.hits ? `, ${iso.last.hits} from the cache` : ''}.`, '#b9c2d0']);
    if (iso.status) status.push([iso.status, '#b9c2d0']);
  }

  function press(id, w) {
    if (!id.startsWith('iso-')) return false;
    if (id === 'iso-page') return false;              // main.js turns the page
    if (id === 'iso-add') { add(); deps.changed(); return true; }
    if (w.group === 'iso-edit') { iso.edit = w.value; deps.changed(); return true; }
    if (w.group === 'iso-show') { iso.volumeHidden = w.value === 'surfaces'; deps.changed(); return true; }
    const s = iso.surfaces[iso.edit];
    if (!s) return true;
    if (id === 'iso-remove') remove(iso.edit);
    else if (id === 'iso-shown') { s.shown = !s.shown; syncSlots(); }
    else if (w.group === 'iso-colour') { s.colour = w.value; syncSlots(); }
    else if (id === 'iso-thr') { s.value = valueOfNorm(deps.thrNorm()); s.failures = 0; schedule(0); }
    else if (id === 'iso-export:ply') exportMesh('ply');
    else if (id === 'iso-export:glb') exportMesh('glb');
    deps.changed();
    return true;
  }

  function slide(id, value) {
    if (!id.startsWith('iso-')) return false;
    const s = iso.surfaces[iso.edit];
    if (!s) return true;
    if (id === 'iso-level') { s.value = valueOfNorm(Math.min(1, Math.max(0, value))); s.failures = 0; schedule(); }
    else if (id === 'iso-opacity') { s.opacity = Math.min(1, Math.max(0.05, value)); syncSlots(); }
    deps.changed();
    return true;
  }

  // ── Drawing ───────────────────────────────────────────────────────────────

  function draw(view, model, p) { if (iso.surfaces.length) glr.draw(view, model, p); }
  /** The volume left out: "Surfaces only" and a surface shown. */
  function hidesVolume() { return iso.volumeHidden && iso.surfaces.some((s) => s.shown && s.mesh && s.mesh.triangles); }

  function afterRestore() {
    glr.restore();
    for (const s of iso.surfaces) { s.at = null; s.uploadedFor = null; }
    if (usable() && iso.surfaces.length) prepare();
    pack();
  }

  /** One line for the HUD, or ''. */
  function hudLine() {
    if (!iso.surfaces.length) return '';
    return `surfaces ${iso.surfaces.length}: ${(glr.triangles / 1000).toFixed(0)} k triangles drawn, ${(glr.bytes / 1e6).toFixed(1)} MB buffers`;
  }

  return { iso, gl: glr, prepare, onCube, add, remove, run, menuContext, view, press, slide, draw, hidesVolume, afterRestore, hudLine,
    setLevel: (i, v) => { if (iso.surfaces[i]) { iso.surfaces[i].value = v; iso.surfaces[i].failures = 0; schedule(0); } } };
}

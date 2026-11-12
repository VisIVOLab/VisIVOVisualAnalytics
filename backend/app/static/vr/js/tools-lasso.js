// Kinematic Lasso refinement in the headset (issue #191): add and carve seeds,
// Isolate and Remove, through the desktop's routes and with its parameters
// (src/gui/vtkWindowCube_Lasso.cpp, src/app/LassoReplay.cpp).
//
// - A seed in mode "add" joins the selection (the desktop's Shift+double-click),
//   "carve" cuts a region out of it (Alt+double-click), "new" starts again
//   (a plain double-click). Every grow is one /v1/cube/kinematic_select_bin
//   with the primary seed and the seed lists, as the desktop's recipe holds it.
// - Isolate and Remove post the mask shown (the grow's own bytes, never a
//   re-grow) to /v1/cube/mask_apply_bin, with blank_outside or blank_inside,
//   and make a new cube in the Workspace Exports, as the desktop's buttons do.
//
// The pure parts (seed lists, the apply call, its Provenance and HISTORY) are
// tested by node (backend/tests/vr/tools-lasso.test.mjs).

import * as Rc from './recipe.js';

export const SEED_MODES = ['new', 'add', 'carve'];
export const SEED_MODE_NAMES = { new: 'new selection', add: 'add a source', carve: 'carve out' };

/** The desktop's two apply modes, by the names of its buttons. */
export const APPLY_MODES = { isolate: 'blank_outside', remove: 'blank_inside' };

/**
 * The seeds of the next grow, as the desktop's click handler makes them: a
 * plain seed starts a new selection; "add" and "carve" append to the current
 * one's lists, and need one (without a selection they start a new one, as the
 * desktop's ⇧/⌥-click does without a seed). Seeds are [z, y, x].
 */
export function nextSeeds(current, mode, seedZYX) {
  const s = [seedZYX[0], seedZYX[1], seedZYX[2]];
  if (current && (mode === 'add' || mode === 'carve')) {
    const add = current.add.map((v) => [...v]);
    const sub = current.sub.map((v) => [...v]);
    (mode === 'add' ? add : sub).push(s);
    return { seed: [...current.seed], add, sub };
  }
  return { seed: s, add: [], sub: [] };
}

/** The request of POST /v1/cube/kinematic_select_bin for these seeds. */
export function growRequest(seeds, params) {
  return Rc.lassoGrowRequest(seeds.seed, params, seeds.add, seeds.sub);
}

const ROI_FROM_GROW = 'with origin_x, origin_y, origin_z from its X-Visivo-Origin header and '
  + 'width, height, depth from X-Visivo-Width, X-Visivo-Height, X-Visivo-Depth';

/** The apply step of the recipe, as LassoReplay's lassoApplyStep(mode,
 *  handEdited) writes it: the mode only, the mask from the grow's response, or
 *  (a mask edited by hand) the edited mask, which only the cube records. */
export function applyStep(mode, handEdited = false) {
  return {
    route: '/v1/cube/mask_apply_bin',
    request: { mode },
    body: handEdited
      ? 'the hand-edited selection as a little-endian float32 mask, which only the cube itself records '
        + `(see replay_caveat); a replay of the grow alone posts the previous call's response body ${ROI_FROM_GROW}`
      : `the previous call's response body, the selection as a little-endian float32 mask, posted back ${ROI_FROM_GROW}`,
  };
}

/** The desktop's replay caveat of a cube made from a hand-edited mask
 *  (vtkWindowCube::applyLassoMask). */
export const HAND_EDITED_CAVEAT = 'The mask was edited by hand after the grow: the parameters regrow only its '
  + 'geodesic part. The cube itself, with its HISTORY, is the exact record.';

/** Where a received selection was made, as the page says it: the desktop's
 *  madeIn seen from the headset (deliberately not the desktop's wording),
 *  'own' being this page's own selection echoed back by the hub. */
export function madeIn(L) {
  const src = L && L.source;
  if (src === 'own') return 'this VR page';
  if (src === 'headset') return 'another VR headset';
  return 'the desktop';
}

/** A received box's [x0, x1, y0, y1, z0, z1] (inclusive): its own, or its mask's box. */
export function boxOf(L) {
  if (Array.isArray(L.box) && L.box.length === 6) return L.box.map(Number);
  const [ox, oy, oz] = L.origin, [w, h, d] = L.dims;
  return [ox, ox + w - 1, oy, oy + h - 1, oz, oz + d - 1];
}

/** SharedSelection's sharedBoxApplyStep: the one step that records its ROI,
 *  since for a box the ROI is the selection. */
export function boxApplyStep(box, mode) {
  return {
    route: '/v1/cube/mask_apply_bin',
    request: { origin_x: box[0], origin_y: box[2], origin_z: box[4], width: box[1] - box[0] + 1,
      height: box[3] - box[2] + 1, depth: box[5] - box[4] + 1, mode },
    body: 'a little-endian float32 mask of ones over the box (width x height x depth values, x fastest): every voxel of the box',
  };
}

/** The desktop's caveats of a received selection (SharedSelection.cpp). */
export const BOX_EDITED_CAVEAT = 'The mask was edited by hand after the box was made: the box gives only its '
  + 'unedited part. The cube itself, with its HISTORY, is the exact record.';
export const MASK_ONLY_CAVEAT = 'The selection was received without the request it was made with: no call '
  + 'regenerates it. The cube itself, with its HISTORY, is the exact record.';
const MASK_ONLY_BODY = 'the selection as a little-endian float32 mask, received through the session\'s selection hub '
  + '(GET /v1/share/selection/mask) without the request it was made with, which only the cube itself records '
  + '(see replay_caveat); posted with origin_x, origin_y, origin_z from that response\'s X-Visivo-Origin header and '
  + 'width, height, depth from X-Visivo-Width, X-Visivo-Height, X-Visivo-Depth';

/**
 * How an applied selection is recorded, as the desktop's lassoApplyRecord
 * (src/app/SharedSelection.cpp) does: {params, history (the HISTORY items),
 * replay, caveat}. A received box: its box step over the box. A received
 * selection without its grow: the mask only, with the desktop's caveat. Else
 * (grown here, or received with its creator's grow): the grow, then the apply
 * (hand-edited: its body and caveat). Never a grow step without a request.
 */
export function applyRecord(L, mode, trimBox = 0) {
  const hand = isHandEdited(L);
  const params0 = L.parameters && typeof L.parameters === 'object' ? L.parameters : {};
  if (L.kind === 'box') {
    const b = boxOf(L);
    const params = { Region: 'box', Pixels: `x ${b[0]}..${b[1]}, y ${b[2]}..${b[3]}`, Channels: `${b[4]}..${b[5]}` };
    if (params0['Channels from'] !== undefined) params['Channels from'] = params0['Channels from'];
    params['Made in'] = madeIn(L);
    return { params, history: ['selection=box', `box_x=${b[0]}..${b[1]}`, `box_y=${b[2]}..${b[3]}`, `box_z=${b[4]}..${b[5]}`,
      `made_in=${madeIn(L)}`], replay: [boxApplyStep(b, mode)], caveat: hand ? BOX_EDITED_CAVEAT : '' };
  }
  // Whatever the flags say: without a request there is no grow to record.
  if (!L.request) {
    return {
      params: { ...params0, 'Made in': madeIn(L), 'Recorded as': 'the mask only (received without its grow request)',
        'Hand-edited': hand ? 'yes' : 'no' },
      history: ['selection=mask', `made_in=${madeIn(L)}`],
      replay: [{ route: '/v1/cube/mask_apply_bin', request: { mode }, body: MASK_ONLY_BODY }],
      caveat: MASK_ONLY_CAVEAT,
    };
  }
  // A received grow's noise sigma was set where it was made: the creator's own
  // label when it came with one, else attributed to that place.
  const fromElsewhere = L.received && L.source !== 'own';
  const params = Rc.lassoDisplayParams(L.request, fromElsewhere ? `set in ${madeIn(L)}` : Rc.VR_NOISE_WHERE);
  if (L.received && params0['Noise σ'] !== undefined) params['Noise σ'] = params0['Noise σ'];
  params['Hand-edited'] = hand ? 'yes' : 'no';
  if (trimBox > 0) {
    params['Domain trimmed'] = `yes: one or more grows exceeded the graph budget and were cut to a seed-centred box, largest half-width ${trimBox} voxels`;
  }
  const history = historyRecipe(L.request, trimBox, hand).split('; ');
  if (L.received) { params['Made in'] = madeIn(L); history.push(`made_in=${madeIn(L)}`); }
  return { params, history, replay: [{ route: '/v1/cube/kinematic_select_bin', request: L.request }, applyStep(mode, hand)],
    caveat: hand ? HAND_EDITED_CAVEAT : '' };
}

/** Was this selection's mask edited by hand (a shared selection says so)? */
export function isHandEdited(L) {
  return !!(L && (L.handEdited || L.hand_edited));
}

/** The FITS HISTORY items of the new cube (LassoReplay's lassoHistoryItems),
 *  joined as the desktop sends them in the `recipe` query. */
export function historyRecipe(g, trimBox = 0, handEdited = false) {
  const compact = (s) => `(${s[0]},${s[1]},${s[2]})`;
  const items = [
    `seed_zyx=${g.seed_z},${g.seed_y},${g.seed_x}`,
    `reach=${Rc.formatG(g.reach, 4)}`,
    `lambda_v=${g.lambda_v.toFixed(2)}`,
    `nsigma=${g.nsigma.toFixed(1)}`,
    g.sigma > 0 ? `sigma=${Rc.formatG(g.sigma, 17)} (noise estimate)` : 'sigma=robust (whole cube)',
    `smooth_zyx=${g.smooth_v || 0},${g.smooth_xy || 0},${g.smooth_xy || 0}`,
    `reach_max=${Rc.formatG(g.reach_max, 4)}`,
    `add_seeds=${g.add_seeds ? g.add_seeds.map(compact).join('|') : 'none'}`,
    `sub_seeds=${g.sub_seeds ? g.sub_seeds.map(compact).join('|') : 'none'}`,
    `hand_edited=${handEdited ? 'yes' : 'no'}`,
  ];
  if (trimBox > 0) items.push(`domain_trimmed=largest box half-width ${trimBox} voxels`);
  return items.join('; ');
}

/** The query of POST /v1/cube/mask_apply_bin for the grow's mask; with
 *  `basename`, the new cube's file name (else the backend's
 *  <stem>_lasso-outside/inside.fits, as the desktop's). */
export function applyQuery(datasetId, origin, dims, mode, recipe, basename = '') {
  const q = {
    dataset_id: datasetId, origin_x: origin[0], origin_y: origin[1], origin_z: origin[2],
    width: dims[0], height: dims[1], depth: dims[2], mode, recipe,
  };
  if (basename) q.output_basename = basename;
  return q;
}

/** The marker of a cube isolated only for a moment: its name, so it is never
 *  taken for (or deleted as) a cube someone asked for. */
export const TEMP_MARK = '_vr-moment-tmp-';

/** The file name of such a cube: <stem>_vr-moment-tmp-<tag>.fits. */
export function tempBasename(path, tag) {
  const stem = (path || 'cube').split('/').pop().replace(/\.fits(\.gz|\.fz)?$/i, '') || 'cube';
  return `${stem}${TEMP_MARK}${tag}.fits`;
}

/** The Provenance of the new cube, as the desktop's applyLassoMask labels it. */
export function applyParams(g, mode, count, trimBox, file, handEdited = false) {
  const p = Rc.lassoDisplayParams(g);
  p['Hand-edited'] = handEdited ? 'yes' : 'no';
  if (trimBox > 0) {
    p['Domain trimmed'] = `yes: one or more grows exceeded the graph budget and were cut to a seed-centred box, largest half-width ${trimBox} voxels`;
  }
  p['Selected voxels'] = String(count);
  p.Mode = mode === APPLY_MODES.remove ? 'remove (inside blanked)' : 'isolate (outside blanked)';
  if (file) p.File = file;
  return p;
}

/** The seeds of a grow request ({seed, add, sub}, each [z, y, x]), or null:
 *  the selection carries its own seeds, wherever it came from. */
export function seedsOf(g) {
  if (!g || g.seed_z === undefined) return null;
  return { seed: [g.seed_z, g.seed_y, g.seed_x], add: (g.add_seeds || []).map((s) => [...s]),
    sub: (g.sub_seeds || []).map((s) => [...s]) };
}

/**
 * The tools, on the selection of select-ui.js. ctx: {selection (createSelection's
 * object), backend(), datasetId(), onChange(), showDataset(id, path)}.
 *
 * One slot, for the selection shown: its mask, and the cube each mode made
 * from it (one request per mask and mode, however often it is pressed, shared
 * by concurrent presses). A moment uses the cube Isolate made for this mask,
 * or else a temporary one of its own, named <stem>_vr-moment-tmp-<tag>.fits
 * and deleted when the selection changes; a cube asked for with Isolate or
 * Remove is kept. ctx.path() gives the cube's path (the temporary name's stem),
 * ctx.cube() the cube shown (its identity: a new load is a new object).
 *
 * Every action belongs to the cube it started on: the slot captures the
 * dataset id and the cube, every call posts that dataset id, and after every
 * await the action stops if another cube was loaded meanwhile (a mask of cube
 * A is never applied to cube B).
 */
export function createLassoTools(ctx) {
  const t = {
    busy: '', error: '', notice: '',
    applied: null,       // the last cube asked for: {mode, id, file, path, dims, ms}
    slot: null,          // {lasso, mask: Promise|null, modes: {mode: {promise, out, implicit, recorded}}}
    gen: 0,
  };
  const sel = () => ctx.selection.sel;
  const changed = () => ctx.onChange && ctx.onChange();
  const msg = (e) => String(e && e.message ? e.message : e);

  /** Delete a cube made only for a moment (its session entry, then its file);
   *  never a file without the temporary marker. `quiet`: say only a failure. */
  async function discard(out, quiet = false) {
    const b = ctx.backend();
    if (!b || !out.file.includes(TEMP_MARK)) return;
    try {
      await b._json('DELETE', `/v1/datasets/${encodeURIComponent(out.id)}`);
      const r = await b._json('DELETE', `/v1/exports/${encodeURIComponent(out.file)}`);
      if (!r.valid) throw new Error(r.error || 'not deleted');
      if (!quiet) t.notice = `deleted ${out.file} (made for the moment of the previous selection)`;
    } catch (e) {
      t.notice = `${out.file} (made for the moment of the previous selection) stays in the Workspace Exports: ${msg(e)}`;
    }
    changed();
  }

  /** The slot changes with the selection: the old one's implicit cubes go. */
  function retire(slot) {
    if (!slot) return;
    for (const e of Object.values(slot.modes)) {
      e.promise.then((out) => (e.temp ? discard(out) : null), () => {});
    }
  }

  const cubeNow = () => (ctx.cube ? ctx.cube() : null);
  /** Is the cube the slot was made on still the one shown? */
  const stillOn = (slot) => ctx.datasetId() === slot.dataset && cubeNow() === slot.cube;
  const CHANGED = 'Another cube was loaded meanwhile: nothing was applied to it.';
  /** A cube change, said on the panel (t.notice survives the new cube's
   *  reset, which drops the action's own error), and thrown. */
  const cubeChanged = (text) => {
    t.notice = text;
    changed();
    const e = new Error(text);
    e.cubeChanged = true;
    return e;
  };

  function slotFor(L) {
    if (!t.slot || t.slot.lasso !== L || !stillOn(t.slot)) {
      retire(t.slot);
      t.slot = L ? { lasso: L, mask: null, modes: {}, dataset: ctx.datasetId(), cube: cubeNow() } : null;
    }
    return t.slot;
  }

  /** Follow the selection shown (a new grow, Clear, a shared selection). */
  function sync() {
    const L = sel().lasso;
    if (t.slot && (t.slot.lasso !== L || !stillOn(t.slot))) slotFor(L);
  }

  /** The mask of the selection: its own bytes, origin and dims whenever it
   *  has them (authoritative, even with a grow request beside them: it may
   *  have been edited by hand); else (a selection that came with its grow only, such as a
   *  shared one) the grow of its request, which is the same voxels (the
   *  recipe's replay), checked against its origin, dims and count. A
   *  hand-edited selection without its mask is refused: its grow is not it. */
  function maskOf(slot) {
    const L = slot.lasso;
    if (!slot.mask) {
      slot.mask = (async () => {
        if (L.bytes && L.origin && L.dims) {
          const n = L.dims[0] * L.dims[1] * L.dims[2] * 4;
          const got = L.bytes.byteLength ?? L.bytes.length;
          if (got !== n) throw new Error(`this selection's mask has ${got} bytes, its box ${L.dims.join('×')} needs ${n}`);
          return { bytes: L.bytes, origin: L.origin, dims: L.dims, trimBox: L.trimBox || 0 };
        }
        if (isHandEdited(L)) {
          throw new Error('This selection was edited by hand and came without its mask: apply it on the desktop, where the mask is.');
        }
        if (!L.request) throw new Error('This selection came without its mask or its grow request: it cannot be applied here.');
        const r = await ctx.backend().kinematicSelect(slot.dataset, L.request);
        if (!stillOn(slot)) throw cubeChanged(CHANGED);
        const same = (a, b) => !a || (a.length === b.length && a.every((v, i) => v === b[i]));
        if ((L.count != null && r.count !== L.count) || !same(L.origin, r.origin) || !same(L.dims, r.dims)) {
          throw new Error(`The grow of this selection's request gives ${r.count} voxels in ${r.dims.join('×')} at `
            + `(${r.origin.join(', ')}), not its ${L.count ?? '?'} voxels`
            + `${L.dims ? ` in ${L.dims.join('×')} at (${L.origin.join(', ')})` : ''}: not applied.`);
        }
        return { bytes: r.bytes, origin: r.origin, dims: r.dims, trimBox: r.trimBox || 0 };
      })();
      slot.mask.catch(() => { if (t.slot === slot) slot.mask = null; });
    }
    return slot.mask;
  }

  /**
   * Post the selection's mask and register the new cube; resolves to {id,
   * file, path, dims, mode, ms, temp}. `temp`: for a moment, the cube Isolate
   * made (or is making) for this mask, else a temporary one, not recorded and
   * deleted when the selection changes.
   */
  async function apply(mode, { record = true, temp = false } = {}) {
    const L = sel().lasso;
    const b = ctx.backend();
    if (!L || !(L.bytes || L.request)) throw new Error('Grow a selection first (Kinematic Lasso).');
    if (!L.request && !(L.bytes && L.origin && L.dims)) throw new Error('This selection came without its mask or its grow request: it cannot be applied here.');
    if (!b || !ctx.datasetId()) throw new Error('This needs the backend.');
    if (sel().busy) throw new Error('Wait for the selection to finish before applying.');
    const slot = slotFor(L);
    const key = temp && !slot.modes[mode] ? `${mode}:tmp` : mode;
    let e = slot.modes[key];
    if (!e) {
      e = { temp: key !== mode, recorded: false, out: null };
      slot.modes[key] = e;
      const basename = e.temp ? tempBasename(ctx.path ? ctx.path() : '', `${Date.now().toString(36)}${Math.random().toString(36).slice(2, 6)}`) : '';
      e.promise = (async () => {
        const t0 = performance.now();
        let m;
        try { m = await maskOf(slot); } catch (err) { if (!stillOn(slot) && !err.cubeChanged) throw cubeChanged(CHANGED); throw err; }
        if (!stillOn(slot)) throw cubeChanged(CHANGED);
        // The dataset the selection was made on, never the one shown now.
        const q = new URLSearchParams(applyQuery(slot.dataset, m.origin, m.dims, mode,
          applyRecord(L, mode, m.trimBox).history.join('; '), basename));
        const { buf } = await b._binary(`/v1/cube/mask_apply_bin?${q}`, m.bytes, 'application/octet-stream');
        const r = JSON.parse(new TextDecoder().decode(buf));
        if (!r.valid) throw new Error(r.error || 'Masking failed.');
        e.out = { id: r.new_dataset_id, file: r.workspace_filename, path: r.output_path,
          dims: [r.width, r.height, r.depth], mode, ms: performance.now() - t0, trimBox: m.trimBox, temp: e.temp };
        if (!stillOn(slot)) {
          // Made correctly, from the previous cube's selection, but that cube is
          // gone: a temporary cube is deleted, one asked for is kept, not recorded
          // (the record would name the cube shown now).
          if (e.temp) discard(e.out, true);
          throw cubeChanged(`Another cube was loaded meanwhile: ${e.out.file} was made from the previous cube's selection${e.temp ? ' and deleted' : ' and is not recorded'}.`);
        }
        return e.out;
      })();
      e.promise.catch(() => { if (slot.modes[key] === e) delete slot.modes[key]; });
    }
    const out = await e.promise;
    if (record && !e.temp && !e.recorded) {
      // Not when another cube is shown now: the record would name it.
      if (!stillOn(slot)) {
        throw cubeChanged(`Another cube was loaded meanwhile: ${out.file} was made from the previous cube's selection and is not recorded.`);
      }
      e.recorded = true;
      const removed = mode === APPLY_MODES.remove;
      const rec = applyRecord(L, mode, out.trimBox);
      const product = { kind: 'lasso', label: removed ? 'Lasso: removed cube' : 'Lasso: isolated cube', replay: rec.replay };
      if (rec.caveat) product.replay_caveat = rec.caveat;
      // As the desktop's applyLassoMask adds them to every record.
      const params = { ...rec.params, 'Selected voxels': String(L.count ?? ''),
        Mode: mode === APPLY_MODES.remove ? 'remove (inside blanked)' : 'isolate (outside blanked)', File: out.file };
      if (L.count == null) delete params['Selected voxels'];
      ctx.selection.record(product, params, null);
    }
    return out;
  }

  /** Isolate or Remove from a button: busy text, the result on the panel. */
  async function run(which) {
    const mode = APPLY_MODES[which];
    const gen = ++t.gen;
    const L = sel().lasso;
    t.error = '';
    t.busy = which === 'remove' ? 'removing the selection from the cube…' : 'isolating the selection…';
    changed();
    try {
      const out = await apply(mode);
      if (gen !== t.gen) return;
      if (sel().lasso === L) t.applied = out;   // a newer selection: not offered as "that cube"
      t.busy = '';
    } catch (e) {
      if (gen !== t.gen) return;
      t.busy = '';
      t.error = msg(e);
    }
    changed();
  }

  /** Show the last cube asked for in the headset (it becomes the cube drawn). */
  async function showApplied() {
    const a = t.applied;
    if (!a || !ctx.showDataset) return;
    t.error = '';
    try { await ctx.showDataset(a.id, a.path); } catch (e) { t.error = msg(e); }
    changed();
  }

  function setSeedMode(m) {
    if (!SEED_MODES.includes(m)) return;
    sel().seedMode = m;
    changed();
  }

  /** A new cube: forget the old selection's cubes (deleting the implicit ones). */
  function reset() {
    t.gen++;
    t.busy = t.error = t.notice = '';
    t.applied = null;
    retire(t.slot);
    t.slot = null;
  }

  function lines() {
    const s = sel();
    const out = [];
    if (t.error) out.push(t.error);
    const seeds = seedsOf(s.lasso && s.lasso.request);
    out.push(`seeds: ${SEED_MODE_NAMES[s.seedMode || 'new']}${seeds ? ` (1 + ${seeds.add.length} added, ${seeds.sub.length} carved)` : ''}`);
    if (t.busy) out.push(t.busy);
    if (t.applied) {
      out.push(`${t.applied.mode === APPLY_MODES.remove ? 'removed' : 'isolated'}: ${t.applied.file} (${t.applied.dims.join('×')}), ${t.applied.ms.toFixed(0)} ms`);
    }
    if (t.notice) out.push(t.notice);
    return out;
  }

  return { state: t, apply, run, showApplied, setSeedMode, reset, sync, lines };
}


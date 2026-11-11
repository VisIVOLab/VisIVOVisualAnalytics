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

/** The apply step of the recipe, as LassoReplay's lassoApplyStep(mode, false)
 *  writes it: the mode only, the mask from the grow's response. */
export function applyStep(mode) {
  return {
    route: '/v1/cube/mask_apply_bin',
    request: { mode },
    body: `the previous call's response body, the selection as a little-endian float32 mask, posted back ${ROI_FROM_GROW}`,
  };
}

/** The FITS HISTORY items of the new cube (LassoReplay's lassoHistoryItems),
 *  joined as the desktop sends them in the `recipe` query. */
export function historyRecipe(g, trimBox = 0) {
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
    'hand_edited=no',
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
export function applyParams(g, mode, count, trimBox, file) {
  const p = Rc.lassoDisplayParams(g);
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
 * Remove is kept. ctx.path() gives the cube's path (the temporary name's stem).
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
   *  never a file without the temporary marker. */
  async function discard(out) {
    const b = ctx.backend();
    if (!b || !out.file.includes(TEMP_MARK)) return;
    try {
      await b._json('DELETE', `/v1/datasets/${encodeURIComponent(out.id)}`);
      const r = await b._json('DELETE', `/v1/exports/${encodeURIComponent(out.file)}`);
      if (!r.valid) throw new Error(r.error || 'not deleted');
      t.notice = `deleted ${out.file} (made for the moment of the previous selection)`;
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

  function slotFor(L) {
    if (!t.slot || t.slot.lasso !== L) {
      retire(t.slot);
      t.slot = L ? { lasso: L, mask: null, modes: {} } : null;
    }
    return t.slot;
  }

  /** Follow the selection shown (a new grow, Clear, a shared selection). */
  function sync() {
    const L = sel().lasso;
    if (t.slot && t.slot.lasso !== L) slotFor(L);
  }

  /** The mask of the selection: its own bytes, origin and dims whenever it
   *  has them; else (a selection that came with its grow only, such as a
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
        if (L.handEdited || L.hand_edited) {
          throw new Error('This selection was edited by hand and came without its mask: apply it on the desktop, where the mask is.');
        }
        if (!L.request) throw new Error('This selection came without its mask or its grow request: it cannot be applied here.');
        const r = await ctx.backend().kinematicSelect(ctx.datasetId(), L.request);
        const same = (a, b) => !a || (a.length === b.length && a.every((v, i) => v === b[i]));
        if ((L.count !== undefined && r.count !== L.count) || !same(L.origin, r.origin) || !same(L.dims, r.dims)) {
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
        const m = await maskOf(slot);
        const q = new URLSearchParams(applyQuery(ctx.datasetId(), m.origin, m.dims, mode, historyRecipe(L.request, m.trimBox), basename));
        const { buf } = await b._binary(`/v1/cube/mask_apply_bin?${q}`, m.bytes, 'application/octet-stream');
        const r = JSON.parse(new TextDecoder().decode(buf));
        if (!r.valid) throw new Error(r.error || 'Masking failed.');
        e.out = { id: r.new_dataset_id, file: r.workspace_filename, path: r.output_path,
          dims: [r.width, r.height, r.depth], mode, ms: performance.now() - t0, trimBox: m.trimBox, temp: e.temp };
        return e.out;
      })();
      e.promise.catch(() => { if (slot.modes[key] === e) delete slot.modes[key]; });
    }
    const out = await e.promise;
    if (record && !e.temp && !e.recorded) {
      e.recorded = true;
      const removed = mode === APPLY_MODES.remove;
      ctx.selection.record({ kind: 'lasso', label: removed ? 'Lasso: removed cube' : 'Lasso: isolated cube',
        replay: [{ route: '/v1/cube/kinematic_select_bin', request: L.request }, applyStep(mode)] },
      applyParams(L.request, mode, L.count, out.trimBox, out.file), null);
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


// Mask layers in the page (issue #282): the mask over the cube (loaded from
// the desktop's `mask` command or from the page), how it is drawn, the source
// picked and refined with the Kinematic Lasso, undo, and the save back to the
// desktop as a FITS product with its recipe. The logic is mask-layer.js's
// (pure, tested); this module keeps state, draws the Mask window and talks to
// the backend.

import * as ML from './mask-layer.js';
import * as MW from './mask-window.js';
import * as Rc from './recipe.js';

const APPLICATION = 'VisIVO Visual Analytics VR page';

/**
 * ctx: {renderer (createPanel, updatePanel, setMaskLayer), state (the page's:
 * info, open, clip, extent, openPath), backend() -> backend or null,
 * seedMode() / setSeedMode(m) (the lasso's), armPick() (Pick armed: the
 * trigger on the cube is Select's), onVisit(row, src), onChange() (labels
 * and the volume changed), report(recipe, detail) -> the hub's promise of
 * its send ({file, posted}) or null when no hub runs, buzz(kind)}
 */
export function createMasks(ctx) {
  const canvas = typeof document !== 'undefined'
    ? Object.assign(document.createElement('canvas'), { width: MW.MASK_PX[0], height: MW.MASK_PX[1] }) : null;
  let tex = null, texKey = '';
  const S = {
    layer: null, mask: null,       // {path, name, datasetId} of the loaded mask
    cube: null,                    // the cube (dataset id) it was loaded over
    mode: 'outline', labels: true, selected: 0, picking: false, newSource: false,
    shown: false, busy: '', status: [], steps: [], loadMs: 0, encodeMs: 0, saved: null,
  };
  const pointer = { hover: null };
  const info = () => ctx.state.info;
  const datasetId = () => (ctx.state.open ? ctx.state.open.dataset_id : '');
  let stamp = 0;                  // bumped by every change the view shows that the layer's version does not
  const touch = () => { stamp++; texKey = ''; };
  const say = (text, colour = '#b9c2d0') => { S.status = [[text, colour]]; touch(); };

  /** The layer drawn again: its encoding on the reduced grid, into the mask storage. */
  function redraw() {
    const i = info();
    if (!S.layer || !i) { try { ctx.renderer.setMaskLayer(null); } catch { /* no volume */ } return; }
    if (S.mode === 'off') { ctx.renderer.setMaskLayer(null); ctx.onChange(); return; }
    const t0 = performance.now();
    // An empty mask (every source removed) is still a layer: Mask only then draws nothing.
    const enc = ML.encodeLayer(S.layer, i.factors, i.reduced_shape, S.selected) || ML.emptyEncoding();
    ctx.renderer.setMaskLayer(enc);
    S.encodeMs = performance.now() - t0;
    ctx.onChange();
  }

  /** Loads a mask over the cube on show: {maskPath} or {maskDatasetId}. */
  async function load(mask) {
    const b = ctx.backend();
    const id = datasetId();
    if (!b || !id) throw new Error('A mask needs the cube from the backend: open it first.');
    S.busy = 'loading the mask…';
    touch();
    const t0 = performance.now();
    try {
      const buf = await b.maskLayer(id, mask);
      if (datasetId() !== id) return null;          // another cube meanwhile
      const parsed = ML.parseLayer(buf);
      S.layer = new ML.MaskLayer(parsed);
      S.mask = { path: parsed.meta.mask_path || mask.maskPath || '', name: parsed.meta.mask_name || '', datasetId: mask.maskDatasetId || '' };
      S.cube = id;
      S.selected = 0; S.picking = false; S.newSource = false; S.steps = []; S.saved = null;
      if (S.mode === 'off') S.mode = 'outline';
      S.loadMs = performance.now() - t0;
      redraw();
      say(`${S.layer.ids().length} sources, ${parsed.nVoxels.toLocaleString('en')} voxels, loaded in ${S.loadMs.toFixed(0)} ms`);
      return S.layer;
    } finally {
      S.busy = '';
      touch();
    }
  }

  function forget() {
    S.layer = null; S.mask = null; S.cube = null; S.selected = 0; S.picking = false; S.newSource = false; S.steps = [];
    S.saved = null; S.status = [];
    try { ctx.renderer.setMaskLayer(null); } catch { /* no volume */ }
    touch();
  }

  function select(id) {
    S.selected = id || 0;
    S.newSource = false;
    touch();
    redraw();
    const s = S.layer && S.selected ? S.layer.get(S.selected) : null;
    if (s) say(`Source ${s.id}: ${s.count.toLocaleString('en')} voxels, channels ${s.bbox[4]}–${s.bbox[5]}`);
  }

  /** A trigger on the cube while Pick is armed: the source under the ray (a box-space ray). */
  function pickRay(ray) {
    if (!S.picking || !S.layer) return false;
    S.picking = false;
    touch();
    const r = ML.pickSource(S.layer, ray.o, ray.d, ctx.state.clip);
    if (!r) { say('No source of the mask along that ray.', '#ffd27a'); ctx.buzz('error'); return true; }
    select(r.id);
    ctx.buzz('press');
    return true;
  }

  /** Is a source being refined (the lasso's presses edit it)? */
  const refining = () => !!S.layer && S.mode !== 'off' && (!!S.selected || S.newSource);

  /** A lasso press while refining: a grow from this seed alone, applied to the source. */
  async function lasso(pick, seedMode, params, signal, stale) {
    const b = ctx.backend(), id = datasetId();
    // What the grow is for, fixed now: a result that comes back after the
    // layer, the source picked or the layer's edits changed is dropped.
    const layer = S.layer, target = S.selected, wasNew = S.newSource, version = layer ? layer.version : -1;
    const [x, y, z] = pick.seed;
    const req = Rc.lassoGrowRequest([z, y, x], params || {}, [], []);
    S.busy = 'growing on the full cube…';
    touch();
    try {
      const r = await b.kinematicSelect(id, req, signal);
      if (stale()) return;
      if (S.layer !== layer || S.selected !== target || S.newSource !== wasNew || !layer || layer.version !== version) {
        say('The mask or the source changed during the grow: not applied.', '#ffd27a');
        return;
      }
      const grow = ML.growIndices(r.mask, r.dims, r.origin, S.layer.shape);
      const before = S.selected;
      const out = S.layer.apply(S.selected || null, grow, seedMode);
      if (!out.id) { say('Subtract needs a source: Pick one first.', '#ffd27a'); return; }
      S.selected = out.id;
      S.newSource = false;
      S.steps.push({ id: out.id, op: seedMode, request: req });
      redraw();
      const s = S.layer.get(out.id);
      const verb = seedMode === 'carve' ? 'subtracted from' : seedMode === 'add' ? 'added to' : before ? 'replaced' : 'made';
      say(`Source ${out.id} ${verb}: ${s.count.toLocaleString('en')} voxels (grow of ${grow.length.toLocaleString('en')})`);
    } finally {
      S.busy = '';
      touch();
    }
  }

  function undo() {
    if (!S.layer || !S.layer.canUndo()) return false;
    const id = S.layer.undo();
    S.steps.pop();
    if (S.selected && !S.layer.get(S.selected)) S.selected = 0;
    redraw();
    say(`Undone: source ${id}`);
    return true;
  }

  /** The refined mask saved by the backend and reported to the desktop with its recipe. */
  async function save() {
    const b = ctx.backend(), id = datasetId();
    if (!b || !id || !S.layer) throw new Error('Nothing to save.');
    // hgc44-run5_mask.fits -> hgc44-run5_vr_mask.fits: still a mask by its name (the desktop's rule).
    const stem = (S.mask.name || 'mask').replace(/\.fits?(\.gz)?$/i, '').replace(/[_-]mask$/i, '') || 'cube';
    const history = S.steps.slice(-20).map((s) => `lasso ${s.op} on source ${s.id}`);
    // The path always, with the dataset id (#305): a mask opened by its id in
    // a session that has gone since (a backend restart, the desktop's session
    // ended) is still found by its file.
    const { body, edits, removed } = S.layer.saveBody({ dataset_id: id, mask_path: S.mask.path || '',
      mask_dataset_id: S.mask.datasetId || '', output_basename: `${stem}_vr_mask`, history });
    if (!edits && !removed) throw new Error('Nothing edited yet.');
    S.busy = 'saving the mask…';
    touch();
    try {
      const r = await b.maskLayerSave(body);
      if (!r.valid) throw new Error(r.error || 'not saved');
      S.saved = r;
      touch();
      const replay = [{ route: '/v1/cube/mask_layer_bin', request: { dataset_id: id, mask_path: S.mask.path } },
        ...S.steps.map((s) => ({ route: '/v1/cube/kinematic_select_bin', request: s.request, body: `then ${s.op} on source ${s.id}` })),
        { route: '/v1/cube/mask_layer_save', request: { dataset_id: id, mask_path: S.mask.path, output_basename: `${stem}_vr_mask` },
          body: 'the edited sources\' voxel lists' }];
      const recipe = Rc.makeRecipe({ application: APPLICATION, exported: new Date().toISOString().replace(/\.\d+Z$/, 'Z'),
        product: { id: `vr-mask-${Date.now()}`, kind: 'mask', label: `Refined mask ${r.workspace_filename}`, replay,
          replay_caveat: 'Each lasso grow is replayed from its seed; the edit it made (replace, add, subtract) is named in its step.' },
        dataset: { label: (ctx.state.openPath || '').split('/').pop(), path: ctx.state.openPath || '', backend_dataset_id: id,
          backend_session_id: b.session || '' },
        parameters: { 'Mask': S.mask.name, 'Sources edited': r.edited.join(', ') || 'none', 'Sources removed': r.removed.join(', ') || 'none',
          'Sources': String(r.n_sources), 'Voxels': String(r.n_voxels), 'Made in': 'the VR headset' } });
      const done = ctx.report(recipe, { product: 'mask', file: r.workspace_filename, dataset_id: id,
        label: `Refined mask ${r.workspace_filename}: ${r.edited.length} sources edited, ${r.removed.length} removed` });
      tellDesktop(done, r);
      return r;
    } finally {
      S.busy = '';
      touch();
    }
  }

  /** The desktop told of a saved mask (#305): said sent only once posted, as
   *  the source list's Send does. `done`: ctx.report's promise of the hub's
   *  send ({file, posted}), or null/false when no hub runs for this page. A
   *  newer save or another mask meanwhile keeps its own status. */
  function tellDesktop(done, r) {
    const file = r.workspace_filename;
    if (!done) { say(`Saved ${file} (no desktop session: not reported)`, '#ffd27a'); return; }
    say(`Saved ${file}, telling the desktop…`);
    const mine = () => S.saved === r;
    Promise.resolve(done).then((v) => {
      if (!mine()) return;
      if (v && v.posted === false) say(`Saved ${file}, desktop not told${v.why ? `: ${v.why}` : ''}`, '#ffd27a');
      else say(`Saved ${file}, sent to the desktop`);
    }, (e) => {
      if (!mine()) return;
      say(`Saved ${file}, desktop not told: ${e && e.stale ? 'the page stood down or moved to another session' : (e && e.message) || e}`, '#ff8a8a');
      ctx.buzz('error');
    });
  }

  /** What the view is made of, as a short key: the view is made again only when it changes. */
  const viewKeyNow = () => {
    const L = S.layer;
    return `${L ? L.version : -1}|${stamp}|${S.mode}|${S.labels}|${S.selected}|${S.picking}|${S.newSource}|${ctx.seedMode()}|${S.busy}`;
  };
  let viewKey = '', viewCache = null;
  function view() {
    const k = viewKeyNow();
    if (k === viewKey && viewCache) return viewCache;
    viewKey = k;
    viewCache = makeView();
    return viewCache;
  }
  function makeView() {
    const L = S.layer;
    const s = L && S.selected ? L.get(S.selected) : null;
    const notes = {
      show: L ? (S.mode === 'off' ? 'The mask is not drawn.' : `${ML.MODE_NAMES[S.mode]}${S.labels ? ', labels at the centroids' : ''}`) : '',
      source: S.picking ? 'Trigger on the cube: the source under the ray.' : s ? `Source ${s.id}${s.isNew ? ' (new)' : ''}: ${s.count.toLocaleString('en')} voxels`
        : S.newSource ? 'New source: the next lasso makes it.' : '',
      refine: refining() ? 'Trigger with the Kinematic Lasso (Select): the grow edits this source.' : '',
      save: S.saved ? `Last saved: ${S.saved.workspace_filename}` : '',
    };
    const status = S.busy ? [[S.busy, '#ffd27a']] : S.status;
    const ch = L ? L.changes() : null;
    const n = L ? L.ids().length : 0;
    return {
      loaded: !!L, mode: S.mode, labels: S.labels, selected: s ? s.id : 0, picking: S.picking, newSource: S.newSource,
      seedMode: ctx.seedMode(), canUndo: !!L && L.canUndo(), canSave: !!ch && ch.edits.length + ch.removed.length > 0,
      nSources: n,
      title: L ? `Mask: ${S.mask.name || 'mask'}` : 'Mask', notes, status,
      info: L ? `${n} sources, highest ID ${L.maxId}${L.maxId > L.origMaxId ? ` (new from ${L.origMaxId + 1})` : ''}` : 'No mask over the cube.',
    };
  }
  function layout() { return MW.maskLayout(view()); }
  function texture() {
    const v = view();
    const key = `${viewKey}|${pointer.hover}`;
    if (!tex) tex = ctx.renderer.createPanel();
    if (key !== texKey && canvas) {
      const lay = MW.maskLayout(v);
      MW.drawMask(canvas.getContext('2d'), lay, v, { hover: pointer.hover });
      ctx.renderer.updatePanel(tex, canvas);
      texKey = key;
    }
    return tex;
  }

  function step(dir) {
    if (!S.layer) return false;
    const ids = S.layer.ids();
    if (!ids.length) return false;
    const i = ids.indexOf(S.selected);
    select(ids[i < 0 ? (dir > 0 ? 0 : ids.length - 1) : (i + dir + ids.length) % ids.length]);
    return true;
  }
  function visit() {
    if (!S.layer || !S.selected) return false;
    ctx.onVisit(ML.sourceRow(S.layer, S.selected), ML.sourceOutline(S.layer, S.selected));
    return true;
  }

  /** A press on the window's content, or on the Select tab's Mask row. */
  function press(id) {
    if (id.startsWith('mode:')) { S.mode = id.slice(5); redraw(); touch(); return true; }
    if (id === 'labels') { S.labels = !S.labels; ctx.onChange(); touch(); return true; }
    if (id === 'pick') {
      S.picking = !S.picking && !!S.layer;
      // The trigger on the cube picks in Select (#305): in Explore it would pin
      // the readout, in Clip move the slab.
      if (S.picking && ctx.armPick) ctx.armPick();
      touch();
      return true;
    }
    if (id === 'src-prev' || id === 'src-next') return step(id === 'src-next' ? 1 : -1);
    if (id === 'visit') return visit();
    if (id === 'new-source') { S.newSource = !S.newSource; if (S.newSource) S.selected = 0; redraw(); touch(); return true; }
    if (id.startsWith('seed:')) { ctx.setSeedMode(id.slice(5)); touch(); return true; }
    if (id === 'undo-edit') return undo();
    if (id === 'deselect') { select(0); return true; }
    if (id === 'save') { save().catch((e) => { say(`Not saved: ${e.message || e}`, '#ff8a8a'); ctx.buzz('error'); }); return true; }
    return false;
  }

  return {
    get state() { return S; },
    pointer, load, forget, pickRay, refining, lasso, undo, save, press, view, layout, texture, redraw, select,
    /** The shader's mode for the volume (renderer.js uMaskMode). */
    modeCode: () => (S.layer ? ML.MODE_CODE[S.mode] : 0),
    /** Refined frames from the backend show the whole cube: not under Mask only and Residuals. */
    hidesRefined: () => !!S.layer && (S.mode === 'only' || S.mode === 'residual'),
    labels: () => (S.layer && S.labels && S.mode !== 'off' && info()
      ? ML.layerLabels(S.layer, ctx.state.extent, ctx.state.clip, S.selected) : []),
    setShown(on) { S.shown = !!on; },
    say,
    /** The cube changed: a layer of another cube goes. */
    onCube() { if (S.layer && S.cube !== datasetId()) { forget(); say(''); S.status = []; } },
    /** A restored context: R.restore() made the panel's texture again in the
     *  same handle (#305: a new handle each time leaked the old one); drawn
     *  again on the next texture(). */
    restore() { texKey = ''; },
    /** For the checks: how often the view was made (it is cached between changes). */
    get viewKey() { return viewKey; },
  };
}

// Selections of the VR page (issue #171): the tools behind the triggers and
// the mouse, the backend calls on the full cube, the results panel (drawn in
// the page and, as a texture, in the headset) and the recipe records. The
// pure parts are in selection.js and recipe.js.

import * as S from './selection.js';
import * as Rc from './recipe.js';
import * as M from './mat4.js';
import { nextSeeds, seedsOf } from './tools-lasso.js';

export const TOOLS = ['probe', 'lasso', 'box'];
export const TOOL_NAMES = { probe: 'Spectrum probe', lasso: 'Kinematic Lasso', box: 'Box region' };
const APPLICATION = 'VisIVO Visual Analytics VR page';

function fmt(v, digits = 4) {
  if (v === null || v === undefined || !Number.isFinite(v)) return 'n/a';
  const a = Math.abs(v);
  if (a !== 0 && (a < 1e-3 || a >= 1e5)) return v.toExponential(digits - 1);
  return v.toPrecision(digits);
}

/**
 * ctx: {state, renderer, backend() -> Backend|null, onChange(), status(text)}.
 * state is the page's: info, cube, open, clip, extent, texScale, thrNorm, mode.
 */
export function createSelection(ctx) {
  const sel = {
    tool: 'probe',
    params: { ...Rc.LASSO_DEFAULTS },
    seedAt: 'peak',    // 'peak': brightest sample of the object the ray enters; 'hit': first sample drawn
    snap: true,        // move the seed to the brightest full voxel of its reduced voxel's block
    hover: null,       // {p, px} where the trigger would land now
    pick: null,        // the last pick: {p, px, seed, reduced, world, sky, snapped}
    corner: null,      // first corner of a box being made (box coordinates)
    lasso: null,       // {request, count, origin, dims, ms, stats, statsMs, reduced, trimBox, bytes}
    seedMode: 'new',   // the next lasso seed (issue #191): 'new', 'add' (a source joins), 'carve'
    spectrum: null,    // {x, y, values, axis, axisLabel, unit, ms, channel}
    box: null,         // {range, stats, ms}
    recipes: [],
    busy: '',
    error: '',
    generation: 0,     // a newer action makes an older answer stale
  };
  const panel = (typeof OffscreenCanvas !== 'undefined')
    ? new OffscreenCanvas(768, 560)
    : (typeof document !== 'undefined' ? Object.assign(document.createElement('canvas'), { width: 768, height: 560 }) : null);   // null: node tests
  const domCanvas = typeof document !== 'undefined' ? document.createElement('canvas') : null;
  if (domCanvas) { domCanvas.width = 768; domCanvas.height = 560; domCanvas.className = 'sel-canvas'; }
  let panelTex = null;
  let panelDirty = true;

  const st = () => ctx.state;
  const vol = () => {
    const c = st().cube;
    return c ? { data: c.data, dims: c.dims, encoding: c.encoding, texScale: st().texScale } : null;
  };
  const datasetId = () => (st().open ? st().open.dataset_id : '');
  const changed = () => { panelDirty = true; ctx.onChange && ctx.onChange(); };

  /** A model-space ray to box coordinates (0..1 over the cube's pixels). */
  function modelRayToBox(o, d) {
    const e = st().extent;
    return { o: [0, 1, 2].map((i) => o[i] / e[i] + 0.5), d: [0, 1, 2].map((i) => d[i] / e[i]) };
  }

  /** A world-space ray through the cube's model matrix to box coordinates. */
  function worldRayToBox(model, o, d) {
    const inv = M.invert(model);
    const mo = M.transformPoint(inv, o);
    const m1 = M.transformPoint(inv, [o[0] + d[0], o[1] + d[1], o[2] + d[2]]);
    return modelRayToBox(mo, M.sub(m1, mo));
  }

  function pickBox(ray, stepVox = 0.25) {
    const v = vol();
    if (!v) return null;
    const s = st();
    return S.pickRay(v, ray.o, ray.d, {
      thr: s.thrNorm, boxMin: s.clip.min, boxMax: s.clip.max, mode: s.mode === 1 ? 1 : 0, stepVox,
    });
  }

  /** Where the trigger would land now (drawn as a marker), cheap enough per frame. */
  function hover(ray) {
    const r = ray ? pickBox(ray, 0.6) : null;
    sel.hover = r ? { p: r.hit.p } : null;
  }

  async function snapSeed(px) {
    // The reduced voxel the pick landed in stands for a block of full voxels;
    // the seed is that block's brightest finite voxel (for max binning, the
    // voxel the drawn value came from), so a grow never starts on a noise
    // voxel the block mean hid. A large block snaps to its brightest 3 x 3
    // spatial mean instead, where a lone noise spike cannot win. Without a
    // backend, the pick's own pixel.
    const s = st();
    const r = S.reducedVoxelOf(px, s.info.factors);
    const b = ctx.backend();
    if (!sel.snap || !b || !datasetId()) return { seed: px, reduced: r, snapped: '' };
    const blk = S.blockOfReduced(r, s.info.factors, s.info.full_shape);
    const n = s.info.full_shape;
    const voxels = (blk[0][1] - blk[0][0] + 1) * (blk[1][1] - blk[1][0] + 1) * (blk[2][1] - blk[2][0] + 1);
    if (voxels > S.SMOOTH_SNAP_ABOVE) {
      const x0 = Math.max(0, blk[0][0] - 1), x1 = Math.min(n[0] - 1, blk[0][1] + 1);
      const y0 = Math.max(0, blk[1][0] - 1), y1 = Math.min(n[1] - 1, blk[1][1] + 1);
      const sub = await b.subvolume(datasetId(), x0, x1, y0, y1, blk[2][0], blk[2][1]);
      const best = S.brightestSmoothed(sub.values, sub.dims, sub.origin, blk);
      return { seed: best || px, reduced: r, snapped: best ? 'smoothed' : '', block: blk };
    }
    const sub = await b.subvolume(datasetId(), blk[0][0], blk[0][1], blk[1][0], blk[1][1], blk[2][0], blk[2][1]);
    const best = S.brightestInBlock(sub.values, sub.dims, sub.origin);
    return { seed: best || px, reduced: r, snapped: best ? 'voxel' : '', block: blk };
  }

  function worldText(px) {
    const s = st();
    const w = S.worldOfPixel(s.info, px);
    return w;
  }

  function stale(gen) { return gen !== sel.generation; }

  async function describePick(gen, hit) {
    const s = st();
    const px = S.boxToFullPixel(hit.p, s.info.full_shape);
    const t0 = performance.now();
    const snap = await snapSeed(px);
    if (stale(gen)) return null;
    const pick = { p: hit.p, px, ...snap, world: worldText(snap.seed), sky: null, snapMs: performance.now() - t0 };
    sel.pick = pick;
    changed();
    const b = ctx.backend();
    if (b && datasetId()) {
      b.pixelToSky(datasetId(), pick.seed[0], pick.seed[1], pick.seed[2])
        .then((r) => { if (!stale(gen) && r.valid) { pick.sky = r; changed(); } })
        .catch(() => {});
    }
    return pick;
  }

  function datasetRecord() {
    const s = st();
    const path = s.open && ctx.path ? ctx.path() : '';
    return {
      label: path ? path.split('/').pop() : 'synthetic cube',
      path: path || '',
      backend_dataset_id: datasetId(),
      backend_session_id: ctx.backend() ? ctx.backend().session : '',
    };
  }

  /** A product's recipe; with `replaceId` the record of that product is
   *  replaced (a product the desktop re-registers under the same id, such as a
   *  PV diagram whose path was extended). */
  function record(product, parameters, origin, replaceId = '') {
    const at = replaceId ? sel.recipes.findIndex((x) => x.product.id === replaceId) : -1;
    const r = Rc.makeRecipe({
      application: APPLICATION, exported: new Date().toISOString().replace(/\.\d+Z$/, 'Z'),
      product: { id: at >= 0 ? replaceId : `vr-${product.kind}-${sel.recipes.length + 1}`, ...product },
      dataset: datasetRecord(), parameters, origin,
    });
    if (at >= 0) sel.recipes[at] = r; else sel.recipes.push(r);
    return r;
  }

  function needBackend() {
    if (!ctx.backend() || !datasetId()) {
      throw new Error('This needs the backend: load a cube with "Open and load" (the synthetic cube has no full cube behind it).');
    }
  }

  async function runProbe(gen, pick) {
    needBackend();
    const [x, y, z] = pick.seed;
    const req = Rc.probeRequest(x, y);
    const t0 = performance.now();
    const r = await ctx.backend().spectralProbe(datasetId(), req);
    if (stale(gen)) return;
    const values = r.values;
    const fv = st().info.axes.z.full_velocity_kms;
    const useVel = Array.isArray(fv) && fv.length === values.length;
    sel.spectrum = {
      x, y, channel: z, values, ms: performance.now() - t0,
      axis: useVel ? fv : r.spectral_axis,
      axisLabel: useVel ? 'velocity (km/s)' : `${r.spectral_type || 'spectral'} (${r.spectral_unit || ''})`,
      unit: st().info.bunit || '',
    };
    const w = pick.world;
    record({ kind: 'spectrum', label: `Spectrum at (${x}, ${y})`, replay: [{ route: '/v1/spectral/probe', request: req }] }, {
      Position: `pixel (${x}, ${y})${w.velocity !== null ? `, channel ${z} at ${w.velocity.toFixed(2)} km/s` : `, channel ${z}`}`,
      Channels: String(values.length),
      'Measured on': `probe at voxel (${x}, ${y}, ${z})`,
      'Picked in': `VR, reduced voxel (${pick.reduced.join(', ')}) of ${st().info.factors.join('×')} binning`,
    }, Rc.probeOrigin(pick.seed));
  }

  async function runLasso(gen, pick) {
    needBackend();
    const [x, y, z] = pick.seed;
    // Add and carve seeds join the selection shown (issue #191), as the
    // desktop's Shift and Alt double-clicks do; the seeds are kept only with
    // the mask they grew.
    const seeds = nextSeeds(sel.lasso ? seedsOf(sel.lasso.request) : null, sel.seedMode, [z, y, x]);
    const req = Rc.lassoGrowRequest(seeds.seed, sel.params, seeds.add, seeds.sub);
    const b = ctx.backend();
    const r = await b.kinematicSelect(datasetId(), req);
    if (stale(gen)) return;
    const t0 = performance.now();
    const stats = await b.maskStats(datasetId(), r.bytes, r.origin, r.dims);
    // Drawn and recorded only together, and only while still the latest action:
    // a superseded grow neither replaces the overlay nor leaves a record.
    if (stale(gen)) return;
    const s = st();
    const reduced = S.reduceMask(r.mask, r.dims, r.origin, s.info.factors, s.info.reduced_shape);
    ctx.renderer.setMask(reduced);
    // The full-resolution mask is kept (bytes, mask, origin, dims): a tool
    // that acts on the selection posts this mask, never a regrow.
    sel.lasso = { request: req, count: r.count, origin: r.origin, dims: r.dims, ms: r.ms, trimBox: r.trimBox,
      bytes: r.bytes, mask: r.mask, fullShape: s.info.full_shape,
      reduced: { count: reduced.count, dims: reduced.dims, origin: reduced.origin },
      stats, statsMs: performance.now() - t0 };
    const params = Rc.lassoDisplayParams(req);
    params['Selected voxels'] = String(r.count);
    params['Picked in'] = `VR, reduced voxel (${pick.reduced.join(', ')}) of ${s.info.factors.join('×')} binning`;
    if (r.trimBox > 0) {
      params['Domain trimmed'] = `yes: one or more grows exceeded the graph budget and were cut to a seed-centred box, largest half-width ${r.trimBox} voxels`;
    }
    record({ kind: 'lasso', label: 'Lasso: selection', replay: Rc.lassoReplay(req) }, params,
      { kind: 'probe', voxel: [seeds.seed[2], seeds.seed[1], seeds.seed[0]] });
    // Shared with the desktop (issue #188), with the record it was made with.
    if (ctx.onSelection) ctx.onSelection({ kind: 'lasso', bytes: r.bytes, origin: r.origin, dims: r.dims, request: req, parameters: params, trimBox: r.trimBox });
  }

  /** A selection the desktop made (issue #188), drawn as the page's own lasso
   *  is, with the statistics the backend measured on the full cube when it
   *  was shared. Nothing is recorded: the side that made it records it. */
  function showShared(info, m, own = false) {
    const s = st();
    if (!s.info || !s.cube) return false;   // not drawn yet: offered again (share.js)
    sel.generation++;          // the latest selection wins over one in flight here
    const reduced = S.reduceMask(m.mask, m.dims, m.origin, s.info.factors, s.info.reduced_shape);
    ctx.renderer.setMask(reduced);
    const stats = info.stats && info.stats.valid ? info.stats : null;
    // Kept as a local grow keeps it (bytes, mask, origin, dims): the tools
    // that act on the selection apply this received mask itself.
    // The contract of a received selection (shared with the tools that apply
    // a selection, #195): the float32 mask over full-cube voxels, and what a
    // tool records when it applies it (a box, the creator's grow, or the mask
    // only), as the creator described it.
    const meta = info.meta || {};
    sel.lasso = { request: meta.grow || null, count: info.count, origin: m.origin, dims: m.dims,
      bytes: m.bytes, mask: m.mask, fullShape: info.full_shape || s.info.full_shape,
      received: true,
      kind: info.kind === 'box' ? 'box' : 'lasso',
      source: own ? 'own' : (info.source === 'headset' ? 'headset' : 'desktop'),
      handEdited: !!meta.hand_edited,
      // Inclusive [x0, x1, y0, y1, z0, z1]: the box is its origin and dims (the
      // backend refuses a meta.box that is not).
      box: info.kind === 'box'
        ? [0, 1, 2].flatMap((i) => [m.origin[i], m.origin[i] + m.dims[i] - 1]) : null,
      parameters: meta.parameters || null,
      ms: 0, trimBox: Number((info.meta && info.meta.trim_box) || 0),
      reduced: { count: reduced.count, dims: reduced.dims, origin: reduced.origin },
      stats, statsMs: 0, shared: { source: own ? 'hub (this page\'s)' : info.source, kind: info.kind, seq: info.seq, own } };
    sel.busy = '';
    sel.error = stats ? '' : `shared selection: ${(info.stats && info.stats.error) || 'no statistics'}`;
    changed();
    return true;
  }

  async function runBox(gen, a, b2) {
    needBackend();
    const s = st();
    // Two corners placed in the cube (the controller's tip) make a 3-D box; a
    // corner placed by a ray (the mouse, or pointing from outside) fixes a
    // place on the sky but no depth, and the channels are the velocity slab's.
    const range = a.tip && b2.tip ? S.boxFromCorners(a.p, b2.p, s.info.full_shape)
      : S.boxFromRayCorners(a.p, b2.p, s.info.full_shape, [s.clip.min[2], s.clip.max[2]]);
    const [xr, yr, zr] = range;
    const depth = a.tip && b2.tip ? 'corners' : 'velocity slab';
    const t0 = performance.now();
    // One call: every voxel of the box, read on the backend one channel at a time.
    const steps = Rc.boxReplay(range);
    const stats = await ctx.backend().boxStats(datasetId(), steps[0].request);
    if (stale(gen)) return;
    sel.box = { range, stats, ms: performance.now() - t0, depth };
    const vlo = S.channelVelocity(s.info, zr[0]), vhi = S.channelVelocity(s.info, zr[1]);
    record({ kind: 'region', label: '3-D box region stats', replay: steps }, {
      Region: 'box',
      Pixels: `x ${xr[0]}..${xr[1]}, y ${yr[0]}..${yr[1]}`,
      Channels: `${zr[0]}..${zr[1]}${vlo !== null ? ` (${vlo.toFixed(2)} to ${vhi.toFixed(2)} km/s)` : ''}`,
      'Valid voxels': `${stats.valid_voxels} / ${stats.selected_voxels}`,
      'Blanked / NaN': String(stats.blank_voxels),
      Min: fmt(stats.min), Max: fmt(stats.max), Mean: fmt(stats.mean), 'Std Dev': fmt(stats.rms), Sum: fmt(stats.sum),
      'Flux (Jy km/s)': fmt(stats.flux_jy_kms),
      'Channels from': depth === 'corners' ? 'the two corners' : 'the velocity slab (clipping range)',
      'Measured on': 'every voxel of the box on the full cube',
    }, Rc.boxOrigin(xr, yr, zr));
    if (ctx.onSelection) ctx.onSelection({ kind: 'box', range, parameters: sel.recipes[sel.recipes.length - 1].parameters });
  }

  function fail(err) {
    sel.busy = '';
    sel.error = String(err && err.message ? err.message : err);
    changed();
  }

  /** The trigger (or a mouse click) with a box-space ray; `tip` is the
   *  controller's tip in box coordinates (a box corner may be placed there). */
  async function act(ray, tip = null) {
    if (!st().cube) return;
    const gen = ++sel.generation;
    sel.error = '';
    const tool = sel.tool;
    if (tool === 'box') {
      const inside = tip && tip.every((v, i) => v >= st().clip.min[i] && v <= st().clip.max[i]);
      let point = inside ? { p: tip, tip: true } : null;
      if (!point) {
        const r = pickBox(ray);
        if (r) point = { p: r.hit.p, tip: false };
      }
      if (!point) {
        const span = S.rayBox(ray.o, ray.d, st().clip.min, st().clip.max);
        if (span) point = { p: [0, 1, 2].map((i) => ray.o[i] + ray.d[i] * 0.5 * (span.tn + span.tf)), tip: false };
      }
      if (!point) { sel.error = 'The ray misses the cube.'; changed(); return; }
      if (!sel.corner) {
        sel.corner = point;
        sel.box = null;
        changed();
        return;
      }
      const a = sel.corner;
      sel.corner = null;
      sel.busy = 'box: measuring on the full cube…';
      changed();
      try { await runBox(gen, a, point); } catch (e) { if (!stale(gen)) fail(e); return; }
      if (!stale(gen)) { sel.busy = ''; changed(); }
      return;
    }
    const r = pickBox(ray);
    if (!r) { sel.error = 'Nothing drawn along that ray: point at the emission.'; changed(); return; }
    const hit = sel.seedAt === 'hit' ? r.hit : r.peak;
    sel.busy = 'picking…';
    changed();
    try {
      const pick = await describePick(gen, hit);
      if (!pick) return;
      if (tool === 'probe') {
        sel.busy = 'spectrum from the full cube…';
        changed();
        await runProbe(gen, pick);
      } else {
        sel.busy = 'Kinematic Lasso on the full cube…';
        changed();
        await runLasso(gen, pick);
      }
      if (!stale(gen)) { sel.busy = ''; changed(); }
    } catch (e) {
      if (!stale(gen)) fail(e);
    }
  }

  function cycleTool() {
    setTool(TOOLS[(TOOLS.indexOf(sel.tool) + 1) % TOOLS.length]);
  }

  function setTool(t) {
    // A request in flight for the previous tool is dropped: it must not draw
    // or record under the new one.
    sel.generation++;
    sel.tool = t;
    sel.corner = null;
    sel.busy = '';
    changed();
  }

  function clear() {
    if (ctx.onClear) ctx.onClear();   // issue #188: what the hub holds is no longer shown
    sel.generation++;
    sel.pick = sel.lasso = sel.spectrum = sel.box = sel.corner = null;
    sel.busy = sel.error = '';
    ctx.renderer.setMask(null);
    changed();
  }

  // ── Drawing ───────────────────────────────────────────────────────────────

  /** Model-space segments: the pick marker, the box being made or measured,
   *  the selection's bounding box. */
  function segments() {
    const s = st();
    if (!s.info) return [];
    const e = s.extent;
    const toModel = (p) => [0, 1, 2].map((i) => (p[i] - 0.5) * e[i]);
    const out = [];
    const cross = (p, size, color) => {
      const m = toModel(p);
      for (let i = 0; i < 3; i++) {
        const a = [...m], b = [...m];
        a[i] -= size; b[i] += size;
        out.push({ a, b, color });
      }
    };
    const boxLines = (lo, hi, color) => {
      const c = (x, y, z) => [x ? hi[0] : lo[0], y ? hi[1] : lo[1], z ? hi[2] : lo[2]];
      const edges = [[0, 0, 0, 1, 0, 0], [0, 1, 0, 1, 1, 0], [0, 0, 1, 1, 0, 1], [0, 1, 1, 1, 1, 1],
        [0, 0, 0, 0, 1, 0], [1, 0, 0, 1, 1, 0], [0, 0, 1, 0, 1, 1], [1, 0, 1, 1, 1, 1],
        [0, 0, 0, 0, 0, 1], [1, 0, 0, 1, 0, 1], [0, 1, 0, 0, 1, 1], [1, 1, 0, 1, 1, 1]];
      for (const [a, b, cc, d, f, g] of edges) out.push({ a: toModel(c(a, b, cc)), b: toModel(c(d, f, g)), color });
    };
    const fullBox = (range) => {
      const n = s.info.full_shape;
      return [[0, 1, 2].map((i) => range[i][0] / n[i]), [0, 1, 2].map((i) => (range[i][1] + 1) / n[i])];
    };
    if (sel.hover) cross(sel.hover.p, 0.012, [1, 1, 1, 0.8]);
    if (sel.pick) cross(S.fullPixelToBox(sel.pick.seed, s.info.full_shape), 0.02, [1, 0.9, 0.1, 1]);
    if (sel.corner) cross(sel.corner.p, 0.025, [1, 0.5, 0.9, 1]);
    if (sel.box) { const [lo, hi] = fullBox(sel.box.range); boxLines(lo, hi, [1, 0.5, 0.9, 0.95]); }
    const extra = sel.lasso ? seedsOf(sel.lasso.request) : null;
    if (extra) {
      // The extra seeds (from the selection's own request): green added, red carved.
      for (const z of extra.add) cross(S.fullPixelToBox([z[2], z[1], z[0]], s.info.full_shape), 0.018, [0.3, 1, 0.5, 1]);
      for (const z of extra.sub) cross(S.fullPixelToBox([z[2], z[1], z[0]], s.info.full_shape), 0.018, [1, 0.35, 0.35, 1]);
    }
    if (sel.lasso && sel.lasso.stats && sel.lasso.stats.bbox) {
      const bb = sel.lasso.stats.bbox;
      const [lo, hi] = fullBox([[bb[0], bb[1]], [bb[2], bb[3]], [bb[4], bb[5]]]);
      boxLines(lo, hi, [0.25, 1, 0.45, 0.6]);
    }
    return out;
  }

  function pickLines() {
    const p = sel.pick;
    if (!p) return [];
    const w = p.world;
    const lines = [];
    if (p.sky) lines.push(`RA ${S.formatRA(p.sky.ra_deg)}  Dec ${S.formatDec(p.sky.dec_deg)} (ICRS)`);
    else if (w.xName === 'RA') lines.push(`RA ${S.formatRA(w.x)}  Dec ${S.formatDec(w.y)} (axis labels)`);
    else lines.push(`${w.xName} ${fmt(w.x, 7)} ${w.xUnit}  ${w.yName} ${fmt(w.y, 7)} ${w.yUnit}`);
    lines.push(w.velocity !== null && w.velocity !== undefined
      ? `velocity ${w.velocity.toFixed(2)} km/s (${st().info.axes.z.velocity_convention || st().info.axes.z.name})`
      : `spectral ${fmt(w.spectral, 7)} ${w.spectralUnit || ''}`);
    lines.push(`pixel (${p.seed[0]}, ${p.seed[1]}), channel ${p.seed[2]}`);
    lines.push(`drawn voxel (${p.reduced.join(', ')}) of ${st().info.factors.join('×')}`
      + (p.snapped === 'voxel' ? ', its brightest' : p.snapped === 'smoothed' ? ', its brightest 3×3 mean' : ''));
    return lines;
  }

  function drawSpectrum(ctx2, x0, y0, w, h, axis, values, marker, color, yLabel, xLabel) {
    const pts = [];
    for (let i = 0; i < values.length; i++) {
      const a = axis[i], v = values[i];
      if (a !== null && v !== null && Number.isFinite(a) && Number.isFinite(v)) pts.push([a, v]);
    }
    ctx2.strokeStyle = '#3a4357';
    ctx2.strokeRect(x0, y0, w, h);
    if (pts.length < 2) return;
    let ax0 = Infinity, ax1 = -Infinity, v0 = Infinity, v1 = -Infinity;
    for (const [a, v] of pts) { ax0 = Math.min(ax0, a); ax1 = Math.max(ax1, a); v0 = Math.min(v0, v); v1 = Math.max(v1, v); }
    if (v1 <= v0) v1 = v0 + 1;
    const X = (a) => x0 + ((a - ax0) / (ax1 - ax0 || 1)) * w;
    const Y = (v) => y0 + h - ((v - v0) / (v1 - v0)) * h;
    if (v0 < 0 && v1 > 0) {
      ctx2.strokeStyle = '#59627a';
      ctx2.beginPath(); ctx2.moveTo(x0, Y(0)); ctx2.lineTo(x0 + w, Y(0)); ctx2.stroke();
    }
    ctx2.strokeStyle = color;
    ctx2.lineWidth = 2;
    ctx2.beginPath();
    pts.forEach(([a, v], i) => (i ? ctx2.lineTo(X(a), Y(v)) : ctx2.moveTo(X(a), Y(v))));
    ctx2.stroke();
    ctx2.lineWidth = 1;
    if (marker !== null && marker !== undefined && Number.isFinite(marker)) {
      ctx2.strokeStyle = '#ffd33d';
      ctx2.beginPath(); ctx2.moveTo(X(marker), y0); ctx2.lineTo(X(marker), y0 + h); ctx2.stroke();
    }
    ctx2.fillStyle = '#9aa3b2';
    ctx2.font = '18px ui-monospace, Menlo, monospace';
    ctx2.fillText(`${fmt(v1, 3)}`, x0 + 4, y0 + 18);
    ctx2.fillText(`${fmt(v0, 3)} ${yLabel}`, x0 + 4, y0 + h - 6);
    ctx2.fillText(`${fmt(ax0, 5)}`, x0, y0 + h + 20);
    const right = `${fmt(ax1, 5)} ${xLabel}`;
    ctx2.fillText(right, x0 + w - ctx2.measureText(right).width, y0 + h + 20);
  }

  function renderPanel() {
    if (!panel) return;
    const c = panel.getContext('2d');
    const W = panel.width, H = panel.height;
    c.clearRect(0, 0, W, H);
    c.fillStyle = 'rgba(10, 14, 22, 0.88)';
    c.fillRect(0, 0, W, H);
    c.textBaseline = 'alphabetic';
    c.fillStyle = '#9fd0ff';
    c.font = 'bold 24px -apple-system, system-ui, sans-serif';
    c.fillText(`${TOOL_NAMES[sel.tool]}${sel.tool === 'box' && sel.corner ? ' (second corner)' : ''}`, 16, 32);
    c.font = '20px ui-monospace, Menlo, monospace';
    let y = 62;
    const line = (t, color = '#e6e9ef') => { c.fillStyle = color; c.fillText(t, 16, y); y += 26; };
    if (sel.busy) line(sel.busy, '#ffd27a');
    if (sel.error) line(sel.error.slice(0, 64), '#ff8a8a');
    if (sel.tool !== 'box') for (const t of pickLines()) line(t);
    if (sel.tool !== 'lasso' && sel.lasso) line(`green: the lasso selection${sel.lasso.shared ? ` from the ${sel.lasso.shared.source}` : ''}, ${sel.lasso.count} voxels (Clear removes it)`, '#7dffa0');
    const L = sel.lasso;
    if (sel.tool === 'lasso' && L) {
      const s = L.stats;
      line(L.shared
        ? `${L.count} voxels from the ${L.shared.source} (${L.reduced.count} drawn)`
        : `${L.count} voxels selected (${L.reduced.count} drawn), ${L.ms.toFixed(0)} ms`, '#7dffa0');
      const sd = seedsOf(L.request);
      if (sd && (sd.add.length || sd.sub.length)) {
        line(`seeds: 1 + ${sd.add.length} added, ${sd.sub.length} carved`, '#9aa3b2');
      }
      if (s) {
        line(`flux ${fmt(s.flux_jy_kms)} Jy km/s   sum ${fmt(s.sum)} ${s.bunit}`);
        line(s.velocity_min_kms !== null
          ? `velocity ${s.velocity_min_kms.toFixed(1)} to ${s.velocity_max_kms.toFixed(1)} km/s (ch ${s.channel_min}-${s.channel_max})`
          : `channels ${s.channel_min}-${s.channel_max}`);
        line(`peak ${fmt(s.max)} at (${(s.peak_voxel || []).join(', ')})`);
        if (s.flux_note) line(s.flux_note.slice(0, 64), '#9aa3b2');
        if (s.spectrum && s.spectrum.length > 1) {
          const ax = s.spectrum_velocity_kms && s.spectrum_velocity_kms.length === s.spectrum.length
            ? s.spectrum_velocity_kms : s.spectrum_channels;
          drawSpectrum(c, 60, y + 4, W - 90, H - y - 40, ax, s.spectrum, null, '#7dffa0', s.spectrum_unit,
            ax === s.spectrum_channels ? 'channel' : 'km/s');
        }
      }
    } else if (sel.tool === 'box' && sel.box) {
      const b = sel.box, s = b.stats;
      const [xr, yr, zr] = b.range;
      line(`x ${xr[0]}-${xr[1]}  y ${yr[0]}-${yr[1]}  ch ${zr[0]}-${zr[1]} (${b.depth})`);
      const vlo = S.channelVelocity(st().info, zr[0]), vhi = S.channelVelocity(st().info, zr[1]);
      if (vlo !== null) line(`velocity ${vlo.toFixed(1)} to ${vhi.toFixed(1)} km/s`);
      line(`voxels ${s.valid_voxels} valid / ${s.selected_voxels}  (${s.blank_voxels} blank)`);
      line(`mean ${fmt(s.mean)}  rms ${fmt(s.rms)} ${st().info.bunit || ''}`);
      line(`min ${fmt(s.min)}  max ${fmt(s.max)}  sum ${fmt(s.sum)}`);
      line(`flux ${fmt(s.flux_jy_kms)} Jy km/s   (${b.ms.toFixed(0)} ms)`, '#f0a0ff');
    } else if (sel.tool === 'probe' && sel.spectrum) {
      const sp = sel.spectrum;
      line(`spectrum at (${sp.x}, ${sp.y}), ${sp.values.length} channels, ${sp.ms.toFixed(0)} ms`, '#9fd0ff');
      const marker = sp.axis[sp.channel];
      drawSpectrum(c, 60, y + 4, W - 90, H - y - 40, sp.axis, sp.values, marker, '#4fa3ff', sp.unit, sp.axisLabel);
    } else if (!sel.pick) {
      line('Trigger (or click): ' + (sel.tool === 'box' ? 'two corners of a box' : 'point at the emission'), '#9aa3b2');
      line('Left trigger (or the page): next tool', '#9aa3b2');
    }
    if (domCanvas) {
      const d = domCanvas.getContext('2d');
      d.clearRect(0, 0, domCanvas.width, domCanvas.height);
      d.drawImage(panel, 0, 0);
    }
  }

  /** The panel as a texture for the headset, redrawn only when it changed. */
  function panelTexture() {
    if (!panelTex) panelTex = ctx.renderer.createPanel();
    if (panelDirty) {
      renderPanel();
      ctx.renderer.updatePanel(panelTex, panel);
      panelDirty = false;
    }
    return panelTex;
  }

  /** Redraw the page's copy (desktop mode, no XR frame loop to pull it). */
  function refreshDom() {
    if (panelDirty) { renderPanel(); }
  }

  function hudLines() {
    const out = [`tool ${TOOL_NAMES[sel.tool]}${sel.busy ? ` (${sel.busy})` : ''}`];
    const t = [];
    if (sel.pick) t.push(`pick+snap ${sel.pick.snapMs.toFixed(0)} ms`);
    if (sel.lasso && !sel.lasso.shared) t.push(`lasso ${sel.lasso.ms.toFixed(0)} ms${sel.lasso.statsMs ? ` + stats ${sel.lasso.statsMs.toFixed(0)} ms` : ''}`);
    if (sel.spectrum) t.push(`spectrum ${sel.spectrum.ms.toFixed(0)} ms`);
    if (sel.box && sel.box.stats) t.push(`box ${sel.box.ms.toFixed(0)} ms`);
    if (t.length) out.push(t.join('  '));
    return out;
  }

  function recipesJson(which = 'all') {
    const list = which === 'last' ? sel.recipes.slice(-1) : sel.recipes;
    return JSON.stringify(which === 'last' ? list[0] || null : list, null, 2);
  }

  return {
    sel, act, hover, cycleTool, setTool, clear, segments, showShared, panelTexture, refreshDom, hudLines,
    recipesJson, worldRayToBox, modelRayToBox, domCanvas, record, pickBox,
    isDirty: () => panelDirty,
  };
}

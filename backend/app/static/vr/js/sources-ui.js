// The source list in the page (issue #279): its state (the rows of the
// catalogue over the cube and of the sources found by eye, the sort, the
// page, the current source, the flags and notes), the Sources window's
// texture, its presses, and what it hands to main.js: the visit of a source,
// the overlay's colours and culling, the marks drawn over the cube, and the
// classification sent to the desktop. The logic is source-list.js's (pure,
// tested); this module only keeps state and draws.

import * as SL from './source-list.js';
import * as SW from './source-window.js';

const STORE_PREFIX = 'visivo-vr-sources:';

/**
 * ctx: {
 *   renderer: {createPanel, updatePanel}, state (the page's: info, open, clip,
 *   extent, openPath), overlay() -> the catalogue over the cube (catalogue_pixels'
 *   answer) or null, backend() -> the backend or null, selection (its `sel`),
 *   settings() -> {flagSet, noteSet}, storage (localStorage or null),
 *   onVisit(row, source) (main.js moves the cube), onChange() (the marks
 *   over the cube changed), report(detail) (a product for the desktop),
 *   status(text) }
 */
export function createSources(ctx) {
  const canvas = typeof document !== 'undefined'
    ? Object.assign(document.createElement('canvas'), { width: SW.SRC_PX[0], height: SW.SRC_PX[1] }) : null;
  let tex = null, texKey = '';
  const S = {
    shown: false,               // the window is open
    sort: { key: 'name', dir: 1 },
    page: 0,
    current: null,              // the current source's key
    user: [],                   // the sources found by eye
    nextUser: 1,
    edits: 0,                   // found sources added or removed (with cls.version: what was sent)
    cls: null,
    overlay: null,              // the overlay the rows were made of
    cubeId: '',                 // the cube they were made over (path and dataset)
    rows: [], sorted: [], rowsKey: '',
    status: ['', ''],
    sent: { version: -1, file: '' },
    busy: false,
    version: 0,                 // bumped whenever what is drawn over the cube changes
  };

  function sets() {
    const s = ctx.settings ? ctx.settings() : {};
    return { flags: SL.parseSet(s.flagSet, SL.DEFAULT_FLAGS, SL.MAX_FLAGS), notes: SL.parseSet(s.noteSet, SL.DEFAULT_NOTES, SL.MAX_NOTES) };
  }
  S.cls = new SL.Classification(sets().flags, sets().notes);

  /** The storage key of the work on this catalogue over this cube: the
   *  cube's full path and the catalogue's fingerprint (its sources). */
  const storeKey = () => SL.storageKey(ctx.state.openPath || '', S.overlay, STORE_PREFIX);
  function save() {
    try {
      if (!ctx.storage) return;
      ctx.storage.setItem(storeKey(), JSON.stringify({ cls: S.cls.toJSON(), user: S.user, nextUser: S.nextUser }));
    } catch { /* storage refused: the work stays in this page */ }
  }
  function restore() {
    try {
      const text = ctx.storage && ctx.storage.getItem(storeKey());
      const saved = text ? JSON.parse(text) : null;
      if (!saved) return 0;
      S.user = Array.isArray(saved.user) ? saved.user.filter((u) => u && typeof u.key === 'string' && /^u:\d+$/.test(u.key) && Number.isFinite(u.x)) : [];
      S.nextUser = Number.isInteger(saved.nextUser) ? saved.nextUser : S.user.length + 1;
      // Only the marks of sources listed now: the overlay's and the found ones read back.
      const keys = new Set(SL.sourceRows(S.overlay, ctx.state.info, S.user).map((r) => r.key));
      return S.cls.restore(saved.cls, keys);
    } catch { return 0; }
  }

  /** The rows, made again when the overlay, the cube or the found sources change. */
  function refresh() {
    const ov = ctx.overlay();
    const info = ctx.state.info;
    // The work belongs to a cube and a catalogue: another of either (or none) has its own.
    const cube = `${ctx.state.openPath || ''}|${ctx.state.open ? ctx.state.open.dataset_id : ''}`;
    if (ov !== S.overlay || cube !== S.cubeId) {
      S.overlay = ov;
      S.cubeId = cube;
      const s = sets();
      S.cls = new SL.Classification(s.flags, s.notes);
      S.user = []; S.nextUser = 1; S.edits = 0; S.current = null; S.page = 0; S.sent = { version: -1, file: '' };
      S.status = ['', ''];
      // Read back with or without a catalogue (found sources alone are work too).
      const n = ctx.state.openPath ? restore() : 0;
      if (n) S.status = [`${n} classifications of this catalogue read back from this browser`, '#b9c2d0'];
      S.version++;
    }
    const key = `${S.version}|${info ? info.full_shape.join('x') : ''}|${S.sort.key}|${S.sort.dir}|${S.cls.version}`;
    if (key === S.rowsKey) return;
    S.rowsKey = key;
    S.rows = SL.sourceRows(ov, info, S.user);
    S.sorted = SL.sortRows(S.rows, S.sort.key, S.sort.dir, (k) => S.cls.flagOf(k), S.cls.flagSet);
    S.page = SW.clampPage(S.page, S.sorted.length);
  }

  const rowOf = (key) => S.rows.find((r) => r.key === key) || null;
  const sourceOf = (r) => (r && r.index >= 0 && S.overlay && S.overlay.sources ? S.overlay.sources[r.index] : null);

  function view() {
    refresh();
    const per = SW.ROWS_PER_PAGE;
    const cur = rowOf(S.current);
    const ov = S.overlay;
    const c = S.cls.counts((k) => !k.startsWith('u:'));   // the found sources are counted apart
    const frame = ov ? (ov.source_frame || ov.frame) : 'icrs';
    const title = ov ? `${ov.name || ov.source}: ${(ov.sources || []).length} sources` : 'Sources you found';
    const flagged = c.flagged ? `${c.flagged} flagged (${S.cls.flagSet.map((f) => `${c.byFlag[f] || 0} ${f}`).join(', ')})` : 'none flagged yet';
    const found = S.user.length ? `, ${S.user.length} found by eye` : '';
    const unchanged = S.sent.version === workVersion();
    const sentNote = S.sent.file === 'pending' ? ', sending' : S.sent.file
      ? (!unchanged ? ', changed since sent' : S.sent.posted === false ? ', saved, desktop not told' : ', sent') : '';
    return {
      title, info: `${flagged}${found}${sentNote}`,
      page: S.page, pages: SL.pageCount(S.sorted.length, per), total: S.sorted.length,
      rows: S.sorted.slice(S.page * per, S.page * per + per), current: S.current, currentRow: cur,
      sort: S.sort, flagSet: S.cls.flagSet, noteSet: S.cls.noteSet, mark: cur ? S.cls.get(cur.key) : null,
      frame, canSend: !S.busy && S.sent.file !== 'pending' && (c.flagged > 0 || S.user.length > 0 || [...S.cls.marks.values()].some((m) => m.note)),
      marks: (k) => S.cls.get(k), status: S.status,
      empty: 'No catalogue over the cube: place one on the page, or add sources you find (Select tab).',
    };
  }

  const pointer = { hover: null };
  /** The window's widgets now (for the pointer). */
  function layout() { return SW.sourceLayout(view()); }

  /** The window's texture, drawn again only when what it shows changed. */
  function texture() {
    const v = view();
    const lay = SW.sourceLayout(v);
    const key = JSON.stringify([v.title, v.info, v.page, v.pages, v.current, v.sort, v.rows.map((r) => [r.key, S.cls.flagOf(r.key)]),
      v.mark, v.canSend, v.status, pointer.hover, v.total]);
    if (!tex) tex = ctx.renderer.createPanel();
    if (key !== texKey && canvas) {
      SW.drawSources(canvas.getContext('2d'), lay, v, { hover: pointer.hover });
      ctx.renderer.updatePanel(tex, canvas);
      texKey = key;
    }
    return tex;
  }

  function changed() { S.version++; S.rowsKey = ''; if (ctx.onChange) ctx.onChange(); }

  /** Chooses a source (it becomes the current one, highlighted); `visit` brings it to the centre. */
  function choose(key, visit = true) {
    refresh();
    const r = rowOf(key);
    if (!r) return false;
    S.current = key;
    S.page = SL.pageOfKey(S.sorted, key, SW.ROWS_PER_PAGE);
    changed();
    if (visit && ctx.onVisit) ctx.onVisit(r, sourceOf(r));
    return true;
  }
  function step(dir) {
    refresh();
    const k = SL.stepKey(S.sorted, S.current, dir);
    return k ? choose(k, true) : false;
  }

  /** A source found by eye, from the selection: the lasso's brightest voxel,
   *  else the probe's voxel. Its sky position from the cube's full WCS. */
  async function addFound() {
    const sel = ctx.selection.sel;
    const info = ctx.state.info;
    let px = null, how = '', extra = {};
    if (sel.lasso && !sel.lasso.received) {
      px = SL.lassoVoxel(sel.lasso); how = 'lasso';
      extra = { voxels: sel.lasso.count };
    } else if (sel.spectrum) {
      px = [sel.spectrum.x, sel.spectrum.y, sel.spectrum.channel]; how = 'probe';
    } else if (sel.pick && sel.pick.seed) {
      px = sel.pick.seed.slice(); how = 'probe';
    }
    if (!px || !info) throw new Error('Select it first: a Kinematic Lasso or a Spectrum probe on the source.');
    // The work it goes into, fixed now (#305): another cube or catalogue
    // during the sky lookup has its own list, and this voxel is not of it.
    refresh();
    const cubeAt = S.cubeId, overlay = S.overlay;
    let sky = null;
    const b = ctx.backend();
    const id = ctx.state.open && ctx.state.open.dataset_id;
    if (b && id) { try { sky = await b.pixelToSky(id, px[0], px[1], px[2]); } catch { sky = null; } }
    refresh();
    if (S.cubeId !== cubeAt || S.overlay !== overlay) {
      throw new Error('The cube or the catalogue changed during the sky lookup: the source was not added.');
    }
    const n = S.nextUser++;
    const u = SL.foundSource(n, px, info, sky, how, extra);
    S.user.push(u);
    S.edits += 1;
    S.status = [`${u.label} added: voxel (${px.join(', ')}) by the ${how === 'lasso' ? 'Kinematic Lasso' : 'Spectrum probe'}`, '#b9c2d0'];
    save();
    changed();
    choose(u.key, false);
    return u;
  }
  /** What the sent VOTable was made from: the marks and every add or remove
   *  of a found source (a remove then an add keeps the count, not this). */
  function workVersion() { return `${S.cls.version}:${S.edits}`; }
  function removeFound(key) {
    const i = S.user.findIndex((u) => u.key === key);
    if (i < 0) return false;
    S.user.splice(i, 1);
    S.edits += 1;
    S.cls.forget(key);
    if (S.current === key) S.current = null;
    save();
    changed();
    return true;
  }

  /** The classification to the desktop: the VOTable written by the backend,
   *  then the hub's product event (main.js report → headset.js). */
  function send() {
    refresh();
    const id = ctx.state.open && ctx.state.open.dataset_id;
    if (!id || !ctx.backend()) throw new Error('Sending needs the backend: open the cube from it.');
    const ov = S.overlay;
    if (S.sent.file === 'pending') throw new Error('A send is under way.');
    const request = SL.votableRequest({ datasetId: id, catalogueId: ov ? ov.catalogueId || '' : '',
      sourceFrame: ov ? (ov.source_frame || 'icrs') : 'icrs', rows: S.rows, cls: S.cls });
    const classification = SL.hubClassification({ rows: S.rows, cls: S.cls,
      catalogue: { name: ov ? (ov.cataloguePath || '').split('/').pop() || ov.name || '' : '', path: ov ? ov.cataloguePath || '' : '',
        dataset_id: ov ? ov.catalogueId || '' : '' } });
    const label = SL.classificationSummary(classification);
    const version = workVersion(), overlay = S.overlay, cubeAt = S.cubeId;
    // Saved before but the desktop not told, nothing changed since: that file
    // is told again, not written a second time (#279 review).
    const file = S.sent.posted === false && S.sent.version === version && S.sent.file ? S.sent.file : '';
    const done = ctx.report({ product: 'classification', dataset_id: id, request, classification, label, ...(file ? { file } : {}) });
    if (!done) throw new Error('No desktop session hub for this page: nothing was sent.');
    const same = () => S.overlay === overlay && S.cubeId === cubeAt;     // the answer is for the work on show
    const before = S.sent;
    S.sent = { version: -1, file: 'pending' };
    S.status = [file ? `Sending: telling the desktop about ${file}…` : 'Sending: writing the VOTable…', '#ffd27a'];
    changed();
    // Marked sent only once saved and posted; a failure says why.
    Promise.resolve(done).then((r) => {
      if (!same()) return;
      if (r && r.posted === false) {
        // Written to Exports, but the desktop does not know (#279 review).
        S.sent = { version, file: r.file, posted: false };
        S.status = [`Saved as ${r.file}, desktop not told${r.why ? `: ${r.why}` : ''}`, '#ffd27a'];
      } else {
        S.sent = { version, file: (r && r.file) || 'sent', posted: true };
        S.status = [`Sent to the desktop: ${(r && r.file) || 'the VOTable'}`, '#b9c2d0'];
      }
      texKey = '';
      changed();
    }, (e) => {
      if (!same()) return;
      // Nothing saved this time: a file saved before (not told) is still there.
      S.sent = before.posted === false && before.version === version ? before : { version: -1, file: '' };
      S.status = [`Not sent: ${e && e.stale ? 'the page stood down or sent again' : (e && e.message) || e}`, '#ff8a8a'];
      texKey = '';
      changed();
    });
    return done;
  }

  /** A press on the window's content (its widget id). Returns true when handled. */
  function press(id) {
    refresh();
    if (id.startsWith('sort:')) {
      const k = id.slice(5);
      S.sort = S.sort.key === k ? { key: k, dir: -S.sort.dir } : { key: k, dir: 1 };
      S.rowsKey = '';
      refresh();
      if (S.current) S.page = SL.pageOfKey(S.sorted, S.current, SW.ROWS_PER_PAGE);
      return true;
    }
    if (id.startsWith('row:')) return choose(id.slice(4), true);
    if (id === 'prev') return step(-1);
    if (id === 'next') return step(1);
    if (id === 'visit') { const r = rowOf(S.current); if (r && ctx.onVisit) ctx.onVisit(r, sourceOf(r)); return !!r; }
    if (id === 'page-prev' || id === 'page-next') { S.page = SW.clampPage(S.page + (id === 'page-next' ? 1 : -1), S.sorted.length); return true; }
    if (id === 'found-remove') return removeFound(S.current);
    if (id.startsWith('flag:') && S.current) { S.cls.toggleFlag(S.current, id.slice(5)); save(); changed(); return true; }
    if (id.startsWith('note:') && S.current) { S.cls.toggleNote(S.current, id.slice(5)); save(); changed(); return true; }
    if (id === 'send') { send(); return true; }
    return false;
  }

  /** What the catalogue overlay and the marks over the cube need (layers.js placeOverlay). */
  function decor() {
    refresh();
    const info = ctx.state.info;
    if (!info) return null;
    const cur = rowOf(S.current);
    const focus = cur ? [cur.x, cur.y, cur.z === null ? info.full_shape[2] / 2 : cur.z] : null;
    const listed = S.rows.filter((r) => r.origin !== 'user');
    const drawn = SL.nearestKeys(listed, focus, SL.MAX_DRAWN);
    const labelled = SL.nearestKeys(listed, focus, SL.MAX_LABELS);
    const byIndex = new Map(listed.map((r) => [r.index, r]));
    const opts = {
      colourOf: (i) => { const r = byIndex.get(i); return r ? SL.flagColour(S.cls.flagOf(r.key), S.cls.flagSet) : null; },
    };
    if (drawn) opts.drawn = (i) => { const r = byIndex.get(i); return !!r && drawn.has(r.key); };
    if (labelled && focus) opts.labelled = (i) => { const r = byIndex.get(i); return !!r && labelled.has(r.key); };
    return { opts, segs: SL.listSegments(S.rows, S.current, info.full_shape, ctx.state.extent, ctx.state.clip) };
  }

  return {
    get state() { return S; },
    get canvas() { return canvas; },
    pointer, layout, texture, view, press, choose, step, addFound, removeFound, send, decor, refresh,
    /** The window opened (true) or closed. */
    setShown(on) { S.shown = !!on; },
    setStatus(text, colour = '#b9c2d0') { S.status = [text, colour]; texKey = ''; },
    /** A restored context: R.restore() made the panel's texture again in the
     *  same handle (#305: a new handle each time leaked the old one); drawn
     *  again on the next texture(). */
    restore() { texKey = ''; },
    rowOf,
  };
}

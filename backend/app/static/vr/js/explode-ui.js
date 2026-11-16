// The page's side of the exploded view (issue #371, explode.js): which
// pieces there are (the overlaid fields, or value ranges of one field), their
// layout and the explode amount's animation, drawing them (each the same
// resident 3-D texture with its own matrix and uniforms: renderer.js
// drawPiece), grabbing one with a hand, and the Explode page's presses,
// slider, notes and status. main.js draws the pieces in place of the volume
// while active() and hands the grips over; this module owns nothing else.

import * as EX from './explode.js';
import * as M from './mat4.js';
import { normToData } from './volume.js';
import * as LV from './levels.js';

/**
 * `deps`: {R (renderer), state (main.js state), overlay (overlay-ui.js),
 * volParams(override) (the single cube's draw parameters), dataValue(norm)
 * (the single cube's code to a value), isSimulation(), log(level, event,
 * detail), changed() (the menu and labels to redraw), dropDetail(why), now()
 * (ms)}.
 */
export function createExplode(deps) {
  const { R, state, overlay } = deps;
  const ex = {
    source: 'bands',          // 'fields' | 'bands'
    bands: EX.DEFAULT_BANDS,
    layout: 'row',
    spread: EX.DEFAULT_SPREAD,
    want: false,              // exploded (the animation's target) or assembled
    anim: EX.newAnim(0),
    pieces: [],               // [{kind: 'field' | 'band', src: 'overlay' | 'cube', channel, band, map}]
    users: [],                // what the hands did to each piece (explode.js newUser)
    hidden: [],
    held: null,               // {hand, i, rel} while a hand holds a piece
    sig: null,                // what the pieces were built from (refresh)
    why: '',                  // why there are no pieces ('' when there are)
  };
  // Reused every frame: nothing is allocated per piece.
  const local = [], world = [], uni = [];
  for (let i = 0; i < EX.MAX_PIECES; i++) { local.push(new Float32Array(16)); world.push(new Float32Array(16)); uni.push(EX.pieceUniforms()); }
  const scratch = { t: [0, 0, 0], q: [0, 0, 0, 1], s: [1, 1, 1] };
  const centres = new Float32Array(3 * EX.MAX_PIECES);
  const order = new Int32Array(EX.MAX_PIECES);
  const eye = [0, 0, 0];
  const pp = { extent: null, boxMin: null, boxMax: null, texScale: null, spv: 1, maxSteps: 4096, skip: true, mode: 0, src: 'cube', ov: null };
  let lay = { n: -1, kind: '', spread: NaN, e0: NaN, e1: NaN, e2: NaN, offsets: [], scale: 1 };
  let bandCache = { lo: NaN, hi: NaN, n: 0, list: [] };
  const stats = { draws: 0, pieces: 0 };

  const cubeOk = () => !!(state.cube && state.info && deps.isSimulation());

  /** The overlay's loaded fields when it is drawn (its storage on show), else null. */
  function ovLoaded() {
    const L = overlay.ov.loaded;
    return overlay.drawn() && L && L.fields.length >= 2 ? L : null;
  }

  /** The overlaid field the value ranges split while the overlay is drawn (the one edited on its page). */
  function bandField(L) {
    const name = overlay.ov.fields[overlay.ov.edit];
    const k = L.fields.findIndex((f) => f.name === name);
    return k >= 0 ? k : 0;
  }

  /**
   * The pieces for what is on show now, rebuilt when that changed (the
   * source, the count, the overlay loaded or the field edited, the cube):
   * what the hands did is kept while the pieces stay the same in kind and
   * number. Cheap when nothing changed (a few comparisons, no allocation).
   */
  function refresh() {
    const L = ovLoaded();
    const ok = cubeOk();
    const edit = L ? bandField(L) : -1;
    const s = ex.sig;
    if (s && s.ok === ok && s.source === ex.source && s.bands === ex.bands && s.L === L && s.edit === edit && s.cube === state.cube) {
      if (ex.source === 'fields' && L) {
        // A colour map changed on the Overlay page: the atlas again.
        let same = true;
        for (let i = 0; i < L.looks.length; i++) if (L.looks[i].colormap !== ex.pieces[i].map) { same = false; break; }
        if (same) return ex.pieces.length;
      } else return ex.pieces.length;
    }
    const pieces = [];
    let why = '';
    if (!ok) why = 'An exploded view is for a simulation volume.';
    else if (ex.source === 'fields') {
      if (!L) why = 'Fields: switch the overlay on first (Display, Overlay ▸).';
      else L.fields.forEach((f, i) => pieces.push({ kind: 'field', src: 'overlay', channel: i, band: -1, map: L.looks[i].colormap }));
    } else {
      const maps = EX.bandMaps(ex.bands);
      for (let b = 0; b < ex.bands; b++) pieces.push({ kind: 'band', src: L ? 'overlay' : 'cube', channel: L ? edit : 0, band: b, map: maps[b] });
    }
    const same = pieces.length === ex.pieces.length && pieces.every((p, i) => p.kind === ex.pieces[i].kind && p.channel === ex.pieces[i].channel);
    if (!same) {
      ex.users = pieces.map(() => EX.newUser());
      ex.hidden = pieces.map(() => false);
      ex.held = null;
    }
    ex.pieces = pieces;
    ex.why = why;
    ex.sig = { ok, source: ex.source, bands: ex.bands, L, edit, cube: state.cube };
    R.setPieceColormaps(pieces.map((p) => p.map));
    deps.changed();
    return pieces.length;
  }

  /** Is the exploded view drawn in place of the volume (exploded, or on its way back)? */
  function active() {
    if (!ex.want && EX.amountAt(ex.anim, deps.now()) <= 0) return false;
    return refresh() > 0;
  }

  /** The layout for `n` pieces now (kept while it holds). */
  function layoutNow(n) {
    const e = state.extent;
    if (lay.n !== n || lay.kind !== ex.layout || lay.spread !== ex.spread || lay.e0 !== e[0] || lay.e1 !== e[1] || lay.e2 !== e[2]) {
      const L = EX.layout(n, ex.layout, e, ex.spread);
      lay = { n, kind: ex.layout, spread: ex.spread, e0: e[0], e1: e[1], e2: e[2], offsets: L.offsets, scale: L.scale };
    }
    return lay;
  }

  /** The value ranges between `lo` and `hi` (kept while they hold). */
  function bandsNow(lo, hi, n) {
    if (bandCache.lo !== lo || bandCache.hi !== hi || bandCache.n !== n) bandCache = { lo, hi, n, list: EX.bands(lo, hi, n) };
    return bandCache.list;
  }

  /** Every piece's matrix (model frame and world) at amount `e`. */
  function place(model, e) {
    const n = ex.pieces.length;
    const L = layoutNow(n);
    for (let i = 0; i < n; i++) {
      EX.pieceMatrix(local[i], L.offsets[i], ex.users[i], L.scale, e, scratch);
      EX.pieceWorld(world[i], model, local[i]);
      centres[3 * i] = world[i][12]; centres[3 * i + 1] = world[i][13]; centres[3 * i + 2] = world[i][14];
    }
    return n;
  }

  /** The uniforms of every piece, from the looks on show now. Returns the draw mode. */
  function uniforms(base, override) {
    const L = ovLoaded();
    const n = ex.pieces.length;
    if (ex.source === 'fields' || (L && ex.pieces[0] && ex.pieces[0].src === 'overlay')) {
      if (!L) return -1;
      const q = overlay.params(base, override);
      const u = q.ov;
      if (ex.source === 'fields') {
        for (let i = 0; i < n; i++) {
          const look = L.looks[i];
          EX.setPieceUniforms(uni[i], i, { lo: look.thrNorm, hi: 2, cmin: u.cmin[i], cmax: u.cmax[i], stretch: u.stretch[i], stretchA: u.stretchA[i],
            opacity: look.opacity, row: EX.pieceRow(i) });
        }
      } else {
        const k = ex.pieces[0].channel, look = L.looks[k];
        const list = bandsNow(look.thrNorm, look.cmaxNorm, n);
        for (let i = 0; i < n; i++) {
          const b = list[i];
          EX.setPieceUniforms(uni[i], k, { lo: b.lo, hi: b.hi, cmin: b.cmin, cmax: b.cmax, stretch: u.stretch[k], stretchA: u.stretchA[k],
            opacity: look.opacity, row: EX.pieceRow(i) });
        }
      }
      return q.mode;
    }
    // Value ranges of the single cube: its threshold to its colour max.
    const list = bandsNow(base.thr, base.cmax, n);
    for (let i = 0; i < n; i++) {
      const b = list[i];
      EX.setPieceUniforms(uni[i], 0, { lo: b.lo, hi: b.hi, cmin: b.cmin, cmax: b.cmax, stretch: base.stretch, stretchA: base.stretchA,
        opacity: base.opacity, row: EX.pieceRow(i) });
    }
    return base.mode;
  }

  /** The eye's position from a view matrix (−Rᵀt), into `eye`. */
  function eyeOf(v) {
    eye[0] = -(v[0] * v[12] + v[1] * v[13] + v[2] * v[14]);
    eye[1] = -(v[4] * v[12] + v[5] * v[13] + v[6] * v[14]);
    eye[2] = -(v[8] * v[12] + v[9] * v[13] + v[10] * v[14]);
    return eye;
  }

  /**
   * The pieces for one eye, in place of the volume: the farthest first, each
   * with its box outline once they are apart. `override` as main.js
   * volParams's (mode 2: steps per ray). Returns the pieces drawn.
   */
  function draw(view, model, override = undefined) {
    const n = refresh();
    if (!n) return 0;
    const e = EX.amountAt(ex.anim, deps.now());
    const base = deps.volParams({ ...(override || {}), showMask: false, region: null });
    const mode = uniforms(base, override || {});
    if (mode < 0) return 0;
    place(model, e);
    EX.farthestFirst(order, centres, n, eyeOf(view.view));
    pp.extent = base.extent; pp.boxMin = base.boxMin; pp.boxMax = base.boxMax; pp.texScale = base.texScale;
    pp.spv = base.spv; pp.maxSteps = base.maxSteps; pp.skip = base.skip; pp.mode = mode;
    let drawn = 0;
    for (let k = 0; k < n; k++) {
      const i = order[k];
      if (ex.hidden[i]) continue;
      pp.src = ex.pieces[i].src;
      pp.ov = uni[i];
      if (R.drawPiece(view, world[i], pp)) drawn++;
    }
    if (e > 0.02) for (let i = 0; i < n; i++) if (!ex.hidden[i]) R.drawLines(view, world[i]);
    stats.draws++;
    stats.pieces = drawn;
    return drawn;
  }

  // ── Grabbing a piece ──────────────────────────────────────────────────────

  /** Are the pieces fully apart and still (only then is one grabbed)? */
  function apart() {
    const t = deps.now();
    return ex.want && !EX.animating(ex.anim, t) && EX.amountAt(ex.anim, t) >= 1 && ex.pieces.length > 0;
  }

  /**
   * A hand's grip (main.js handleInput): `starting` on the grip's first
   * frame. A grip that starts inside a shown piece holds that piece, which
   * then follows the hand (moved and turned about where it was held); the
   * pieces' places are kept as the user's part (explode.js userFromPiece).
   * Returns true while this hand holds a piece (main.js then leaves the
   * model where it is).
   */
  function grab(hand, handM, model, starting) {
    if (!starting) {
      if (!ex.held || ex.held.hand !== hand || !apart()) { ex.held = null; return false; }
      const i = ex.held.i;
      const P = M.multiply(M.invert(model), M.multiply(handM, ex.held.rel));
      EX.userFromPiece(ex.users[i], P, layoutNow(ex.pieces.length).offsets[i]);
      return true;
    }
    ex.held = null;
    if (!apart()) return false;
    place(model, 1);
    const invs = ex.pieces.map((_, i) => M.invert(world[i]));
    const i = EX.pieceAt([handM[12], handM[13], handM[14]], invs, state.extent, state.clip, ex.hidden);
    if (i < 0) return false;
    ex.held = { hand, i, rel: M.multiply(M.invert(handM), world[i]) };
    deps.log('info', 'explode-grab', `piece ${i + 1} of ${ex.pieces.length}`);
    return true;
  }

  /** No grip held any more. */
  function release() { ex.held = null; }

  // ── The Explode page ──────────────────────────────────────────────────────

  const fmtV = (v) => {
    if (!Number.isFinite(v)) return 'n/a';
    const a = Math.abs(v);
    return a !== 0 && (a < 1e-2 || a >= 1e4) ? v.toExponential(1) : String(Number(v.toPrecision(2)));
  };

  /** A piece's chip label: the field's name, or its value range in data units. */
  function labelOf(p, i) {
    const L = ovLoaded();
    if (p.kind === 'field') return L ? L.fields[p.channel].name : `field ${i + 1}`;
    let lo, hi, value;
    if (p.src === 'overlay' && L) {
      const f = L.fields[p.channel], look = L.looks[p.channel];
      const list = EX.bands(look.thrNorm, look.cmaxNorm, ex.pieces.length);
      ({ lo, hi } = list[i]);
      value = (n) => normToData(n, f.lo, f.hi, f.scale);
    } else {
      const list = EX.bands(state.thrNorm, Math.max(state.cmaxNorm, state.thrNorm + 1e-3), ex.pieces.length);
      ({ lo, hi } = list[i]);
      value = deps.dataValue;
    }
    return hi > 1 ? `≥ ${fmtV(value(lo))}` : `${fmtV(value(lo))}–${fmtV(value(hi))}`;
  }

  /** The hand menu's context (hand-menu.js ctx.ex), or null when it has none. */
  function menuContext() {
    if (!cubeOk()) return null;
    refresh();
    const t = deps.now();
    return { source: ex.source, bands: ex.bands, layout: ex.layout, exploded: ex.want, animating: EX.animating(ex.anim, t),
      pieces: ex.pieces.map((p, i) => ({ label: labelOf(p, i), hidden: !!ex.hidden[i] })),
      fieldsWhy: ovLoaded() ? '' : 'Switch the overlay on first (Display, Overlay ▸).', why: ex.why };
  }

  /** The Explode page's slider position, notes and status lines. */
  function view(values, notes, status) {
    values['ex-spread'] = ex.spread / EX.SPREAD_MAX;
    notes.spread = `${ex.spread.toFixed(2)} of a piece apart`;
    const n = ex.pieces.length;
    const L = ovLoaded();
    if (ex.source === 'bands') {
      const what = L ? `${L.fields[ex.pieces[0] ? ex.pieces[0].channel : 0].name} (the field edited on the Overlay page)` : 'the variable on show';
      notes.bands = `${what}: threshold to colour max`;
    }
    if (ex.why) status.push([ex.why, '#ffd27a']);
    if (n) {
      const s = EX.pieceScale(n);
      status.push([`${n} pieces, each at ${Math.round(100 * s)} % of the cube: about ${(n * s * s).toFixed(2)}× its pixels to draw.`, '#b9c2d0']);
      status.push(['Grip inside a piece to move or turn it; grip elsewhere moves the whole model.', '#b9c2d0']);
    }
  }

  /** A press on the Explode page (true when it was one of its widgets). */
  function press(id, w) {
    if (!id.startsWith('ex-')) return false;
    const t = deps.now();
    if (w.group === 'ex-src') ex.source = w.value;
    else if (w.group === 'ex-bands') ex.bands = w.value;
    else if (w.group === 'ex-layout') ex.layout = w.value;
    else if (w.group === 'ex-piece') { if (w.value < ex.hidden.length) ex.hidden[w.value] = !ex.hidden[w.value]; }
    else if (id === 'ex-go') {
      ex.want = !ex.want;
      EX.retarget(ex.anim, ex.want ? 1 : 0, t);
      ex.held = null;
      if (ex.want) deps.dropDetail('the exploded view is on');
      deps.log('info', 'explode', `${ex.want ? 'exploded' : 'assembled'}: ${ex.pieces.length} ${ex.source}, ${ex.layout}`);
    } else if (id === 'ex-reset') {
      ex.users = ex.pieces.map(() => EX.newUser());
      ex.hidden = ex.pieces.map(() => false);
      ex.held = null;
    }
    refresh();
    deps.changed();
    return true;
  }

  /** A slider of the Explode page held (true when it was one). */
  function slide(id, value) {
    if (id !== 'ex-spread') return false;
    ex.spread = Math.max(0, Math.min(EX.SPREAD_MAX, value * EX.SPREAD_MAX));
    deps.changed();
    return true;
  }

  /** Explode (true) or assemble (false) from code (tests, the measurement run). */
  function set(want, { source, bands, layout, spread, now = false } = {}) {
    if (source) ex.source = source;
    if (bands) ex.bands = bands;
    if (layout) ex.layout = layout;
    if (Number.isFinite(spread)) ex.spread = spread;
    ex.want = !!want;
    const t = deps.now();
    EX.retarget(ex.anim, ex.want ? 1 : 0, t, now ? 0 : EX.ANIM_MS);
    if (now) ex.anim = EX.newAnim(ex.want ? 1 : 0);
    if (ex.want) deps.dropDetail('the exploded view is on');
    refresh();
    deps.changed();
  }

  /**
   * The colour bars while the view is exploded: one a shown piece, in its
   * colour map over its own colour window (a field's look, or a value
   * range's lo to hi), the first piece at the bottom (levels.js volumeBar
   * objects with `colormap` and `key`, as overlay-ui.js bars). Remade only
   * when a look changes.
   */
  let barsCache = { key: '', bars: [] };
  function bars() {
    const n = ex.pieces.length;
    if (!n) return [];
    const L = ovLoaded();
    const c = state.cube;
    const base = L ? null : deps.volParams({});
    const key = JSON.stringify([ex.source, ex.hidden, ex.pieces.map((p) => p.map),
      L ? [L.key, L.looks.map((l) => [l.thrNorm, l.cmaxNorm, l.stretch])] : [state.thrNorm, state.cmaxNorm, c && c.lo, c && c.hi, c && c.scale, base.stretch, base.stretchA]]);
    if (key === barsCache.key) return barsCache.bars;
    const out = [];
    ex.pieces.forEach((p, i) => {
      if (ex.hidden[i]) return;
      let f, look, thr, cmax, stretch, stretchA, title;
      if (p.src === 'overlay' && L) {
        f = L.fields[p.channel]; look = L.looks[p.channel];
        const q = overlay.params({}, {}).ov;
        stretch = look.stretch; stretchA = q.stretchA[p.channel];
        if (p.kind === 'field') { thr = look.thrNorm; cmax = look.cmaxNorm; title = `${i + 1} ${f.name}`; }
        else {
          const b = EX.bands(look.thrNorm, look.cmaxNorm, n)[i];
          thr = b.cmin; cmax = b.cmax; title = `${i + 1} ${f.name} range`;
        }
        f = { lo: f.lo, hi: f.hi, scale: f.scale, unit: f.bunit || '' };
      } else {
        if (!c) return;
        const b = EX.bands(base.thr, base.cmax, n)[i];
        f = { lo: c.lo, hi: c.hi, scale: c.scale, unit: c.bunit || '' };
        thr = b.cmin; cmax = b.cmax; stretch = state.stretch; stretchA = base.stretchA; title = `${i + 1} range`;
      }
      const bar = LV.volumeBar({ thrNorm: thr, cmaxNorm: cmax, lo: f.lo, hi: f.hi, scale: f.scale, stretch, stretchA, title, unit: f.unit });
      out.push({ ...bar, colormap: p.map, key: JSON.stringify([i, key]) });
    });
    barsCache = { key, bars: out };
    return out;
  }

  /** The wrist readout's line while it is on. */
  function hudLine() {
    if (!ex.want && EX.amountAt(ex.anim, deps.now()) <= 0) return '';
    const n = ex.pieces.length;
    return n ? `exploded ${n} ${ex.source === 'fields' ? 'fields' : 'ranges'} (${ex.layout}), ${stats.pieces} drawn, scale ${EX.pieceScale(n).toFixed(2)}` : '';
  }

  return { ex, active, draw, grab, release, menuContext, view, press, slide, set, bars, hudLine, refresh, stats,
    amount: () => EX.amountAt(ex.anim, deps.now()), world: (i) => world[i] };
}

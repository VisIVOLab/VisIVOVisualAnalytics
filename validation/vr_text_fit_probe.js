// Text fit of every canvas the VR page draws (issue #276), run in a browser
// page by validation/vr_text_fit.mjs: injected as a script, it defines
// window.__vrTextFit(opts) which imports the page's own modules from
// opts.base (…/vr/js/), draws every canvas for every page, tab, data kind and
// state into a canvas whose fillText is recorded, and returns what does not
// fit. The fonts are the browser's own, the ones the page draws with.
//
// A text is measured where it is drawn (measureText's ink box, through the
// canvas transform; a fillText squeezed by its maxWidth counts squeezed).
// Each case gives the boxes of its widgets (buttons, toggles, tabs, bars);
// then:
//   - a text whose centre lies in a widget must be inside it;
//   - a text outside every widget must not overlap one;
//   - no two texts overlap; no two widgets overlap;
//   - every text is inside the canvas, a few pixels from its edge;
//   - a widget's label is never cut (no ellipsis): a short label fits;
//   - a text squeezed below 80 % of its width does not count as fitting.
// With opts.live (the lifecycle run's page, a cube loaded), the menu is also
// drawn with the page's own values (window.__vrMenu).
(() => {
  const EDGE = 4;      // px from the canvas edge
  const TOL = 1;       // px of overlap forgiven (antialiasing)
  const MIN_SQUEEZE = 0.8;
  const ELLIPSIS = '…';

  // opts.font: the page's sans-serif stack replaced by this family (to try
  // the check with another machine's font, e.g. Roboto as on the Quest).
  let FORCE_FONT = '';
  const fontDesc = Object.getOwnPropertyDescriptor(CanvasRenderingContext2D.prototype, 'font');

  function recorder(w, h) {
    const cv = document.createElement('canvas');
    cv.width = w; cv.height = h;
    const c = cv.getContext('2d');
    if (FORCE_FONT) {
      Object.defineProperty(c, 'font', {
        get() { return fontDesc.get.call(this); },
        set(v) { fontDesc.set.call(this, String(v).replace(/-apple-system,\s*system-ui,\s*sans-serif/, `"${FORCE_FONT}"`)); },
      });
    }
    const recs = [];
    const fill = c.fillText;
    c.fillText = function (t, x, y, maxW) {
      const s = String(t);
      if (s.trim()) {
        const m = this.measureText(s);
        let l = x - m.actualBoundingBoxLeft, r = x + m.actualBoundingBoxRight;
        let squeeze = 1;
        if (maxW !== undefined && m.width > maxW) {
          squeeze = maxW / m.width;
          l = x + (l - x) * squeeze; r = x + (r - x) * squeeze;
        }
        const top = y - m.actualBoundingBoxAscent, bot = y + m.actualBoundingBoxDescent;
        const T = this.getTransform();
        const pts = [[l, top], [r, top], [l, bot], [r, bot]].map(([px, py]) => [T.a * px + T.c * py + T.e, T.b * px + T.d * py + T.f]);
        const xs = pts.map((p) => p[0]), ys = pts.map((p) => p[1]);
        recs.push({ text: s, box: [Math.min(...xs), Math.min(...ys), Math.max(...xs), Math.max(...ys)], font: this.font, squeeze });
      }
      return fill.call(this, t, x, y, maxW);
    };
    return { cv, c, recs };
  }

  const inter = (a, b) => Math.min(a[2], b[2]) - Math.max(a[0], b[0]) > TOL && Math.min(a[3], b[3]) - Math.max(a[1], b[1]) > TOL;
  const inside = (a, b) => a[0] >= b[0] - TOL && a[1] >= b[1] - TOL && a[2] <= b[2] + TOL && a[3] <= b[3] + TOL;
  const rectBox = (r) => [r[0], r[1], r[0] + r[2], r[1] + r[3]];
  const r1 = (b) => b.map((v) => Math.round(v));

  /**
   * The problems of one drawn case. widgets: [{id, kind, rect}] in canvas
   * pixels ('drag' areas hold texts but are not buttons).
   */
  function analyse(name, size, recs, widgets = []) {
    const out = [];
    const [W, H] = size;
    const boxes = widgets.map((w) => ({ ...w, b: rectBox(w.rect) }));
    for (let i = 0; i < boxes.length; i++) {
      for (let j = i + 1; j < boxes.length; j++) {
        if (inter(boxes[i].b, boxes[j].b)) out.push({ name, kind: 'widgets overlap', text: `${boxes[i].id} / ${boxes[j].id}`, detail: `${r1(boxes[i].b)} ${r1(boxes[j].b)}` });
      }
    }
    const homes = recs.map((t) => {
      const cx = (t.box[0] + t.box[2]) / 2, cy = (t.box[1] + t.box[3]) / 2;
      const hold = boxes.filter((w) => cx >= w.b[0] && cx <= w.b[2] && cy >= w.b[1] && cy <= w.b[3]);
      hold.sort((a, b) => (a.b[2] - a.b[0]) * (a.b[3] - a.b[1]) - (b.b[2] - b.b[0]) * (b.b[3] - b.b[1]));
      return hold[0] || null;
    });
    recs.forEach((t, i) => {
      const home = homes[i];
      if (t.box[0] < EDGE || t.box[1] < EDGE || t.box[2] > W - EDGE || t.box[3] > H - EDGE) {
        out.push({ name, kind: 'past the canvas edge', text: t.text, detail: `${r1(t.box)} in ${W}×${H}` });
      }
      if (t.squeeze < MIN_SQUEEZE) out.push({ name, kind: 'squeezed', text: t.text, detail: `${(100 * t.squeeze).toFixed(0)} %` });
      if (home) {
        if (!inside(t.box, home.b)) out.push({ name, kind: 'wider than its box', text: t.text, detail: `${r1(t.box)} in ${home.id} ${r1(home.b)}` });
        // A label from the data (a column's name: `free`) may be cut; its tooltip has it whole.
        if (home.kind !== 'drag' && !home.free && t.text.includes(ELLIPSIS)) out.push({ name, kind: 'label cut', text: t.text, detail: home.id });
      } else {
        for (const w of boxes) if (inter(t.box, w.b)) out.push({ name, kind: 'text over a widget', text: t.text, detail: `${w.id} ${r1(t.box)}` });
      }
    });
    for (let i = 0; i < recs.length; i++) {
      for (let j = i + 1; j < recs.length; j++) {
        if (inter(recs[i].box, recs[j].box)) out.push({ name, kind: 'texts overlap', text: `${recs[i].text} / ${recs[j].text}`, detail: `${r1(recs[i].box)} ${r1(recs[j].box)}` });
      }
    }
    return out;
  }

  window.__vrTextFit = async function (opts = {}) {
    const base = opts.base;
    FORCE_FONT = opts.font || '';
    // document.fonts.check() says true for a font that is not installed: the
    // font must change the width of a text drawn with monospace as the fallback.
    if (FORCE_FONT) {
      const c = document.createElement('canvas').getContext('2d');
      const w = (font) => { c.font = font; return c.measureText('Wide glyphs: mMwW iIl1 0Oo .,;'.repeat(2)).width; };
      if (w(`40px "${FORCE_FONT}", monospace`) === w('40px monospace')) throw new Error(`no font ${FORCE_FONT} here`);
    }
    const imp = (m) => import(`${base}${m}`);
    const [HM, TP, WR, NT, ON, LC, WN, PC, RO, CO, SU] = await Promise.all(['hand-menu.js', 'tool-palette.js', 'wrist.js',
      'notice.js', 'onboarding.js', 'lifecycle.js', 'windows.js', 'product-canvas.js', 'readout.js', 'controls.js', 'select-ui.js'].map(imp));
    const report = { cases: 0, texts: 0, problems: [], ellipses: [], perCanvas: {}, snaps: [] };
    const snap = opts.snap ? new RegExp(opts.snap) : null;
    const run = (canvas, name, size, draw, widgets = []) => {
      const { cv, c, recs } = recorder(size[0], size[1]);
      draw(c, cv);
      if (snap && snap.test(`${canvas}: ${name}`) && report.snaps.length < 24) report.snaps.push({ name: `${canvas}: ${name}`, url: cv.toDataURL('image/png') });
      report.cases++;
      report.texts += recs.length;
      report.perCanvas[canvas] = (report.perCanvas[canvas] || 0) + 1;
      for (const p of analyse(`${canvas}: ${name}`, size, recs, widgets)) report.problems.push(p);
      for (const t of recs) if (t.text.includes(ELLIPSIS)) report.ellipses.push(`${canvas}: ${name}: ${t.text}`);
      return recs;
    };
    const LONG_FILE = 'WALLABY_Hydra_TR2_chan_v1.0_cube_with_a_long_survey_name.fits';

    // ── The menu: every page, data kind and state, every widget hovered ──
    const fields = ['x', 'y', 'z', 'velocity_x', 'velocity_y', 'velocity_z', 'temperature_kelvin', 'stellar_mass_msun', 'metallicity', 'density'];
    const ctxFor = (kind, busy, debug) => {
      const ctx = HM.defaultContext();
      Object.assign(ctx, { kind, debug, spectralName: 'Velocity range', canUndo: busy });
      if (busy) {
        Object.assign(ctx, {
          cat: { loaded: true, show: true, labels: true, pins: true }, clipTool: 'box',
          sel: { tool: 'lasso', seedMode: 'carve', lasso: true, box: true, applied: true },
          meas: { lasso: true, moment: 2, pvArmed: true, spectrum: true, pinned: true },
          pts: { fields, field: 'velocity_z', log: true, style: 'splats', budget: '12.0 M', box: true, fieldPage: 1 },
          flow: { stream: true, arrows: true, box: true },
          more: { leftHanded: true, labels: true, haptics: false, hud: true },
          dbg: { governor: true, refine: true, refineDepth: true, refineScale: '0.8×', record: true, run: 'Cancel run', thrSource: 'reduced' },
        });
      }
      return ctx;
    };
    const viewFor = (kind, page) => {
      const notes = {
        cmap: 'inferno', thr: '2.345e-03 Jy/beam (starts from the full cube\'s noise, as on the desktop)', cmax: '1.234e-01 Jy/beam',
        opacity: '1.00', render: 'Emission: the voxels add up along each ray', slab: 'channels 12–108 of 109, 1180.4 to 1561.2 km/s',
        trigger: 'Trigger held near a face of the crop: drag it.', crop: 'Reset crop keeps the range.',
        crop0: 'x 12–187 of 196 px', crop1: 'y 0–279 of 280 px', crop2: 'z 3–106 of 109 px',
        tool: 'Trigger: Kinematic Lasso', selection: 'Lasso 132,160 voxels, flux 6.061 Jy km/s',
        moment: 'Whole field over the velocity range, the desktop\'s automatic threshold mask.',
        pv: 'Trigger: the next point of the slice.', pvw: '3 px = 18.0″',
        cat: '⚠ 12 sources outside the cube (+2 more on the page)',
      };
      if (kind === 'points') Object.assign(notes, { field: 'velocity_z (log scale)', scale: '-1.234e+03 to 2.345e+03', style: 'splat size 0.0123',
        intensity: '100 %', shown: '1.23 M drawn of 4.00 M sampled, 11.52 M rows', crop0: '-12.34 to 45.67', crop1: '-12.34 to 45.67', crop2: '-12.34 to 45.67' });
      if (kind === 'flow') Object.assign(notes, { speed: '12.34 to 1234 km/s', arrows: '× 1.41', seeds: '5000 seeds asked, 4821 streamlines',
        box: 'box 1234 Mpc (read-only)', lines: '4821 streamlines, 32768 arrows, grid 64×64×64' });
      const status = [];
      if (page === 'select') status.push(['Lasso: 1 + 3 added, 2 subtracted, grow 0.62 s', '#b9c2d0'], ['Isolated cube: lasso_isolate_NGC3137_2026-11-14T19-30-05.fits saved on the computer', '#b9c2d0']);
      if (page === 'measure') status.push(['moment 2: 0 to 198.8 km/s', '#b9c2d0'], ['PV along 4 points, 3 px wide: 128 samples × 109 channels', '#b9c2d0']);
      if (page === 'debug') status.push('Refinement: on, 1× q95, depth reprojection, last frame 72 ms after the stop');
      status.push(['The backend answered: 503 Service Unavailable while computing the moment of the selection, try again', '#ff8a8a']);
      return { title: `VisIVO VR · ${LONG_FILE}`, notes, values: { thr: 0.4, cmax: 0.8, opacity: 0.5, slab: [0.1, 0.9] }, status };
    };
    const drawMenuCase = (label, page, ctx, view, hover = null) => {
      const spec = HM.pageSpec(page, ctx);
      const layout = HM.menuLayout(page, ctx);
      run('menu', `${label} hover=${hover || '-'}`, HM.MENU_PX, (c) => HM.drawMenu(c, page, spec, view, { hover }, ctx), layout);
    };
    for (const kind of HM.KINDS) {
      for (const busy of [false, true]) {
        for (const page of HM.PAGES) {
          const debug = page === 'debug' || busy;
          const ctx = ctxFor(kind, busy, debug);
          const view = viewFor(kind, page);
          const label = `${kind} ${page}${busy ? ' (busy)' : ''}`;
          drawMenuCase(label, page, ctx, view, null);
          // Hovering shows a widget's tooltip in its row: each widget once.
          for (const w of HM.menuLayout(page, ctx)) drawMenuCase(label, page, ctx, view, w.id);
        }
      }
    }
    if (opts.live && window.__vrMenu) {
      const m = window.__vrMenu;
      const page0 = m.ui.page;
      for (const page of HM.PAGES) {
        m.ui.page = page;
        m.layout();
        const ctx = m.menu.ctx;
        if (page === 'debug' && !ctx.debug) continue;
        const view = m.view(ctx);
        for (const hover of [null, ...HM.menuLayout(page, ctx).map((w) => w.id)]) drawMenuCase(`live ${ctx.kind} ${page}`, page, ctx, view, hover);
      }
      m.ui.page = page0;
      m.layout();
    }

    // ── The tool palette and its tooltips ──
    for (const kind of HM.KINDS) {
      for (const active of ['explore', 'lasso', 'crop', 'pv']) {
        const ctx = { kind, active };
        const layout = TP.paletteLayout(ctx);
        for (const hover of [null, ...layout.map((w) => w.id)]) {
          run('palette', `${kind} active=${active} hover=${hover || '-'}`, TP.PALETTE_PX, (c) => TP.drawPalette(c, layout, ctx, { hover }), layout);
        }
      }
    }

    // ── Wrist cards: the legend for every mode and tool, both hands, controllers and tracked hands; the cube card ──
    const modes = [['explore', null, false], ['select', 'lasso', false], ['select', 'probe', false], ['select', 'box', false],
      ['clip', 'slab', false], ['clip', 'box', false], ['explore', null, true]];
    for (const leftHanded of [false, true]) {
      for (const input of ['controller', 'hand']) {
        for (const [mode, sub, pvArmed] of modes) {
          const map = CO.controlMap({ mode, sub, leftHanded, pvArmed, input: { primary: input, secondary: input } });
          const title = CO.modeLine(mode, sub, { pvArmed });
          run('wrist legend', `${title} ${leftHanded ? 'left' : 'right'}-handed ${input}`, WR.LEGEND_PX,
            (c) => WR.drawWristCard(c, title, WR.legendRows(map), WR.LEGEND_PX));
        }
      }
    }
    const bigSel = [
      { tool: 'lasso', lasso: { count: 1234567, shared: { source: 'desktop' }, stats: { flux_jy_kms: 1234.5678, velocity_min_kms: -1234.5, velocity_max_kms: 4321.9 } } },
      { tool: 'box', box: { stats: { valid_voxels: 9876543, mean: 1.23e-4, rms: 4.56e-5, flux_jy_kms: 0.000123 } } },
      { tool: 'probe', spectrum: { x: 1234, y: 2345, channel: 3, unit: 'Jy/beam', values: [1, 2, 3, 4e-5, 5] } },
      { tool: 'probe', error: 'The backend answered: 503 Service Unavailable while computing the spectrum' },
      { tool: 'box', corner: [0.1, 0.2, 0.3] },
    ];
    for (const sel of bigSel) {
      const lines = WR.infoLines({ cube: { name: LONG_FILE, shape: '1966×2800×1090 (binned 4×4×2)' }, modeLine: CO.modeLine('select', sel.tool), selection: WR.selectionLines(sel, { bunit: 'Jy/beam' }) });
      run('wrist cube card', sel.tool, WR.WRIST_PX, (c) => WR.drawWristCard(c, 'This cube', lines, WR.WRIST_PX));
    }
    run('wrist cube card', 'no cube', WR.WRIST_PX, (c) => WR.drawWristCard(c, 'This cube', WR.infoLines({}), WR.WRIST_PX));

    // ── Toasts ──
    const toasts = [
      ...bigSel.map((s) => WR.resultToast('done', WR.selectionLines(s, { bunit: 'Jy/beam' }))),
      WR.resultToast('done', null, '', 'moment of the selection…'), WR.resultToast('error', null, 'The backend answered: 503 Service Unavailable while computing the spectrum'),
      ...['restarted', 'reopened'].map((k) => LC.noticeFor(k).toast),
      LC.noticeFor('cube-from-desktop', { detail: LONG_FILE }).toast, LC.noticeFor('cube-from-desktop', { detail: LONG_FILE, done: true }).toast,
      ...(opts.extraToasts || []),
    ].filter(Boolean);
    for (const t of toasts) run('toast', t.slice(0, 40), WR.TOAST_PX, (c) => WR.drawToast(c, t));

    // ── Notices: every lifecycle notice (room cards included) and the guide, in and out of VR ──
    const ctxs = { attempts: 3, status: 503, detail: `${LONG_FILE}: the file is no longer there, or it moved`, why: 'it is larger than the room kept for cubes in VR (512×512×256)',
      name: LONG_FILE, origin: 'http://localhost:8000' };
    const kinds = ['unreachable', 'pair-lost', 'http-error', 'restarted', 'reopened', 'reopen-failed', 'auth', 'reload', 'update-app', 'context-lost',
      'context-failed', 'context-restored', 'xr-ended', 'cube-waiting', 'app-loading', 'app-empty', 'app-failed', 'cube-from-desktop', 'budget'];
    const notices = [];
    for (const k of kinds) {
      for (const v of [{}, { inVR: true }, { inVR: true, app: true }, { paired: true }, { loopback: true, headset: true, inVR: true, app: true },
        { soft: true }, { wasInVR: true }, { connected: true }, { why: 'expired' }, { done: true }]) {
        notices.push([`${k} ${JSON.stringify(v)}`, LC.noticeFor(k, { ...ctxs, ...v, ...(v.why ? { why: v.why } : {}) })]);
      }
    }
    for (const leftHanded of [false, true]) {
      for (let i = 0; i < ON.guidePages().length; i++) notices.push([`guide ${i + 1} ${leftHanded ? 'left' : 'right'}-handed`, ON.guideNotice(i, { leftHanded })]);
    }
    for (const [label, n] of notices) {
      const layout = NT.cardLayout(n);
      for (const hover of [null, ...(layout[0] ? [layout[0].id] : [])]) {
        run('notice', `${label}${hover ? ' hover' : ''}`, NT.CARD_PX, (c) => NT.drawCard(c, n, layout, { hover }), layout);
      }
    }

    // ── Result windows: their bars, the products, the selection's panel ──
    const titles = [WN.windowTitle('result', { tool: 'lasso' }), WN.windowTitle('result', { tool: 'probe' }), WN.windowTitle('result', { tool: 'box' }),
      WN.windowTitle('moment', { momentOrder: 2 }), WN.windowTitle('pv'), `Moment 1 of ${LONG_FILE}`];
    for (const t of titles) for (const hover of [null, 'drag', 'close']) run('window bar', `${t}${hover ? ` hover=${hover}` : ''}`, WN.BAR_PX, (c) => WN.drawBar(c, t, { hover }), WN.barLayout());
    const img = (w, h) => { const a = new Uint8ClampedArray(w * h * 4); for (let i = 0; i < a.length; i += 4) { a[i] = (i / 4) % 255; a[i + 3] = 255; } return a; };
    const products = [
      { title: 'Moment 2 of the whole field', lines: ['Spectral dispersion σ, channels 1–109', '58,800 pixels, 812 ms · automatic threshold mask'],
        w: 196, h: 280, xLabel: 'x (pixel of 196)', yLabel: 'y (pixel)', bar: { lo: 0, hi: 198.8, unit: 'km/s' }, x: [-0.5, 195.5], y: [-0.5, 279.5] },
      { title: 'Moment 0 of the selection', lines: ['Integrated intensity, channels 12–108', `13,216 pixels, 122 ms · lasso_isolate_${LONG_FILE}`],
        w: 64, h: 40, xLabel: 'x (pixel of 1966)', yLabel: 'y (pixel)', bar: { lo: -1.234e-3, hi: 12.345, unit: 'Jy/beam km/s' }, x: [1200.5, 1263.5], y: [-0.5, 39.5] },
      { title: 'PV along 12 points, 15 px wide', lines: ['1234 px of path, 1024 samples × 1090 channels, 2345 ms', 'rows: velocity (radio) increasing upwards, from -1234.5 to 4321.9 km/s'],
        w: 128, h: 109, xLabel: 'offset along the path (arcsec)', yLabel: 'velocity (km/s)', bar: { lo: -0.00123, hi: 0.0456, unit: 'Jy/beam' }, x: [-12.3, 4567.8], y: [-1234.5, 4321.9] },
    ];
    for (const p of products) {
      run('product', p.title, PC.PLANE_PX, (c, cv) => PC.drawProduct(cv, { ...p, rgba: img(p.w, p.h), aspect: 1, table: PC.lut('viridis'),
        xTicks: PC.ticks(p.x[0], p.x[1]), yTicks: PC.ticks(p.y[0], p.y[1]) }));
    }
    // The selection's panel (Selection window), through select-ui's own drawing.
    const info = { full_shape: [196, 280, 109], factors: [2, 2, 1], bunit: 'Jy/beam', axes: { z: { name: 'velocity', velocity_convention: 'radio' } } };
    const spec = (n) => Array.from({ length: n }, (_, i) => Math.sin(i / 7) * 1e-3);
    const vel = (n) => Array.from({ length: n }, (_, i) => -1234.5 + i * 3.9);
    const panels = [
      ['probe', { spectrum: { x: 1234, y: 2345, values: spec(109), axis: vel(109), axisLabel: 'km/s', unit: 'Jy/beam', ms: 85, channel: 40 },
        pinned: null }],
      ['lasso', { lasso: { count: 1234567, reduced: { count: 34567 }, ms: 712, request: { seed: [1, 2, 3] },
        stats: { flux_jy_kms: 6.061, sum: 1234.5, bunit: 'Jy/beam', velocity_min_kms: -1234.5, velocity_max_kms: 4321.9, channel_min: 12, channel_max: 108,
          max: 0.0123, peak_voxel: [1234, 2345, 67], flux_note: 'Flux in Jy km/s: the sum times the channel width over the beam area (BMAJ, BMIN from the header).',
          spectrum: spec(97), spectrum_velocity_kms: vel(97), spectrum_unit: 'Jy' } } }],
      ['box', { box: { range: [[12, 187], [0, 279], [3, 106]], depth: 'full cube', ms: 158,
        stats: { valid_voxels: 9876543, selected_voxels: 9999999, blank_voxels: 123456, mean: 1.23e-4, rms: 4.56e-5, min: -0.0123, max: 0.456, sum: 1234.5, flux_jy_kms: 0.000123 } } }],
      ['probe', { busy: 'Spectrum of the full cube at (1234, 2345)…', error: 'The backend answered: 503 Service Unavailable while computing the spectrum of the full cube' }],
    ];
    for (const [tool, fields2] of panels) {
      run('selection panel', `${tool}${fields2.busy ? ' busy' : ''}`, [768, 560], (c, cv) => {
        const s = SU.createSelection({ state: { info, open: { dataset_id: 'x' }, cube: null }, renderer: { createPanel: () => ({}), updatePanel() {} },
          panelCanvas: cv, backend: () => null, onChange() {}, status() {} });
        s.sel.tool = tool;
        Object.assign(s.sel, fields2);
        s.panelTexture();
      });
    }

    // ── The readout card ──
    for (const lines of [
      ['EXPLORE', 'RA 10h18m12.34s  Dec -28°45′12.3″', 'v 4321.12 km/s (radio)  ch 108', '1.234e-03 Jy/beam (binned 2×2×1)'],
      ['SELECT · Kinematic Lasso', 'GLON 123.456789 deg  GLAT -12.345678 deg', 'FREQ 1.420405e+09 Hz  ch 1089', 'blank  (binned 4×4×2)'],
    ]) run('readout', lines[0], RO.READOUT_PX, (c) => RO.drawReadout(c, lines, false));

    // The check itself: a label wider than its button, a hint over it, a
    // text past the edge, two overlapping widgets must all be found (else
    // the recording broke and every case above passed for nothing).
    {
      const { c, recs } = recorder(200, 100);
      c.font = '26px sans-serif'; c.textBaseline = 'middle'; c.textAlign = 'center';
      c.fillText('A label far too long', 40, 30);
      c.textAlign = 'left';
      c.fillText('a hint', 20, 32);
      c.fillText('past the edge', 150, 80);
      const found = analyse('self-test', [200, 100], recs, [{ id: 'b1', kind: 'button', rect: [10, 15, 60, 30] }, { id: 'b2', kind: 'button', rect: [60, 15, 40, 30] }]);
      const kinds = new Set(found.map((p) => p.kind));
      report.selfTest = { ok: ['wider than its box', 'texts overlap', 'past the canvas edge', 'widgets overlap'].every((k) => kinds.has(k)), kinds: [...kinds] };
    }
    report.problems = report.problems.slice(0, opts.maxProblems || 5000);
    return report;
  };
})();

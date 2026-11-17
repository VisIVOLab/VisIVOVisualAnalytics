// Simulation volumes on the VR page (issue #317), run by vr_lifecycle.mjs
// --simulation (and --regressions): a synthetic VBT volume (a density over
// seven decades, 1e-12 to 1e-5, on a grid whose z step is twice its x and y
// steps, and a signed velocity field beside it) opened by its field
// reference. The page must read it as a simulation volume on a log scale,
// draw its box to the grid spacing (not to the voxel counts), show values
// (not their logarithms) as threshold and readout, say nothing of channels,
// velocities, noise σ, beams or rest lines, grey the spectral tools, and fit
// every text of its menu with these values; the velocity field (signed) stays
// on a linear scale.

import { mkdtempSync } from 'node:fs';
import { spawnSync } from 'node:child_process';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { runTextFitChecks } from './vr_text_fit.mjs';
import { runClipPlaneChecks } from './vr_clip_plane.mjs';

/** A VBT volume of two fields (rho, vx) on a 64 x 48 x 24 grid, spacing 1 x 1 x 2 (kpc). */
export function makeSimulationVolume(python) {
  const dir = mkdtempSync(join(tmpdir(), 'vr-simulation-'));
  const code = `
import sys, numpy as np
nz, ny, nx = 24, 48, 64
z, y, x = np.mgrid[0:nz, 0:ny, 0:nx].astype(np.float32)
r = np.sqrt((x - nx / 2) ** 2 + (y - ny / 2) ** 2 + (2 * (z - nz / 2)) ** 2)
rho = (1e-12 * 10 ** (7 * np.exp(-(r - 14) ** 2 / 10))).astype('<f4')
vx = (100 * np.sin(x / 6) * np.cos(y / 9)).astype('<f4')
data = sys.argv[1] + '/sim.bin'
with open(data, 'wb') as fh:
    rho.tofile(fh); vx.tofile(fh)
open(data + '.head', 'w').write('\\n'.join(['float', '2', f'{nx * ny * nz} {nx} {ny} {nz} 1.0 1.0 2.0', 'little', 'rho', 'vx']) + '\\n')
print(data)
`;
  const r = spawnSync(python, ['-c', code, dir], { encoding: 'utf8' });
  if (r.status !== 0) throw new Error(`could not write the simulation volume: ${r.stderr}`);
  return r.stdout.trim();
}

const SPECTRAL_WORDS = /channel|velocity|km\/s|beam|rest |σ|kinematic|spectrum/i;

export async function runSimulationChecks({ c, check, waitFor, base, token, python, cube = null, clipPlane = false, results = {} }) {
  const L = 'simulation';
  const data = makeSimulationVolume(python);
  const ref = `${data}::vbt-field=0`;
  await c.send('Page.navigate', { url: `${base}/vr/?debug&refine=0&n=simulation#token=${encodeURIComponent(token)}` });
  const up = await waitFor(c, `!!window.__vrShell && window.__vrShell.life.state === 'ok' && location.search.includes('n=simulation')`, 30000);
  if (!up.value) { check(`${L}: page up`, false); return results; }
  const kind = await c.evaluate(`window.__vrLayers.kindOf(${JSON.stringify(ref)})`);
  check(`${L}: a field reference opens as a volume, not a point table`, kind === 'cube', kind);
  const load = async (path) => {
    await c.evaluate(`(() => { const $ = (id) => document.getElementById(id); $('encoding').value = 'uint8'; $('fxy').value = '1'; $('fz').value = '1';
      $('scale').value = 'auto'; $('path').value = ${JSON.stringify(path)}; $('load').click(); })()`);
    return waitFor(c, `!!(window.__vrState.open && window.__vrState.cube && window.__vrState.loading === 0
      && window.__vrState.openPath === ${JSON.stringify(path)} && /upload/.test(document.getElementById('load-status').textContent))`, 60000);
  };
  // A radio cube first, with a velocity stretch, the Kinematic Lasso and the
  // velocity-range trigger: all given back after the simulation volume.
  if (cube) {
    const r = await load(cube);
    check(`${L}: the radio cube loaded first`, !!r.value, `${r.ms} ms`);
    await c.evaluate(`(() => { const z = document.getElementById('zstretch'); z.value = '200'; z.dispatchEvent(new Event('input'));
      window.__vrSelection.setTool('lasso'); })()`);
  }
  const loaded = await load(ref);
  check(`${L}: the density loaded`, !!loaded.value, `${loaded.ms} ms`);
  if (!loaded.value) return results;
  const s = await c.evaluate(`(() => { const st = window.__vrState, cu = st.cube, i = st.info;
    return { simulation: i.simulation, spacing: i.spacing, scale: cu.scale, lo: cu.lo, hi: cu.hi, vmin: cu.valueMin, vmax: cu.valueMax,
      extent: st.extent, shape: i.full_shape, zStretch: st.zStretch, thr: i.default_threshold, axes: ['x', 'y', 'z'].map((k) => i.axes[k].name),
      tool: window.__vrSelection.sel.tool }; })()`);
  results.state = s;
  check(`${L}: reduced_info says simulation, spacing 1 x 1 x 2`, s.simulation === true && JSON.stringify(s.spacing) === '[1,1,2]',
    JSON.stringify({ simulation: s.simulation, spacing: s.spacing, axes: s.axes }));
  check(`${L}: the density is on a log scale (auto), its codes over log10 of 1e-12..1e-5`,
    s.scale === 'log' && Math.abs(s.lo + 12) < 0.05 && Math.abs(s.hi + 5) < 0.05 && s.vmin > 0.9e-12 && s.vmax < 1.1e-5,
    `scale ${s.scale}, log ${s.lo.toFixed(3)}..${s.hi.toFixed(3)}, values ${s.vmin.toExponential(2)}..${s.vmax.toExponential(2)}`);
  // Box: 64 x 48 x (24 x 2) = 1 : 0.75 : 0.75 (by voxel counts it would be 1 : 0.75 : 0.375).
  const want = [1, 0.75, 0.75];
  check(`${L}: the box follows the grid spacing (1 : 0.75 : 0.75), not the voxel counts`,
    s.extent.every((v, i) => Math.abs(v - want[i]) < 1e-9) && s.zStretch === 1, JSON.stringify(s.extent));
  check(`${L}: the selection tool is the Box region (no Kinematic Lasso or Spectrum probe)`, s.tool === 'box', s.tool);
  // Values shown are values: the threshold label is the 99th percentile in g/cm3-like units, not -5.x.
  const thrText = await c.evaluate(`document.getElementById('thr-value').textContent`);
  const shown = Number((thrText.match(/-?\d[\d.]*e[-+]\d+|-?\d+\.\d+/) || [''])[0]);
  check(`${L}: the threshold is shown as a value (the 99th percentile), not its logarithm`,
    shown > 0 && Math.abs(shown / s.thr - 1) < 0.05, `"${thrText}" vs ${s.thr.toExponential(3)}`);
  // The page's own panel: no σ presets, no velocity stretch, no spectral wording.
  const dom = await c.evaluate(`(() => { const $ = (id) => document.getElementById(id);
    return { sigHidden: document.querySelector('button.sig').parentElement.hidden, zsHidden: $('zstretch').closest('label').hidden,
      info: $('cube-info').textContent, scale: $('scale-text').textContent,
      lassoOpt: $('sel-tool').querySelector('option[value="lasso"]').disabled }; })()`);
  check(`${L}: the σ presets, the velocity stretch and the spectral tools are hidden on the page`,
    dom.sigHidden && dom.zsHidden && dom.lassoOpt, JSON.stringify(dom));
  check(`${L}: the info line and the scale line say no channel, velocity, beam or rest line`,
    !SPECTRAL_WORDS.test(dom.info) && !SPECTRAL_WORDS.test(dom.scale) && /Simulation volume/.test(dom.info) && /log scale/.test(dom.info),
    `${dom.info} | ${dom.scale}`);
  // The menu, every page: no spectral wording in its notes and status lines, the spectral tools greyed.
  const menu = await c.evaluate(`(() => { const m = window.__vrMenu, out = {}; const page0 = m.ui.page;
    for (const page of ['display', 'levels', 'range', 'select', 'measure']) {
      m.ui.page = page; const lay = m.layout(); const v = m.view(m.menu.ctx);
      out[page] = { text: [...Object.values(v.notes), ...v.status.map((l) => (Array.isArray(l) ? l[0] : l))].filter(Boolean).join(' | '),
        enabled: lay.filter((w) => !w.disabled).map((w) => w.id) };
    }
    m.ui.page = page0; m.layout(); return out; })()`);
  // The menu's notes are those of every page at once (notes of other pages included): check them all.
  const bad = Object.entries(menu).filter(([, v]) => SPECTRAL_WORDS.test(v.text.replace(/Kinematic Lasso/g, '')));
  const around = (t) => { const i = t.search(SPECTRAL_WORDS); return t.slice(Math.max(0, i - 60), i + 60); };
  check(`${L}: the menu's notes and status lines speak of no channel, velocity or σ`, !bad.length,
    bad.map(([p, v]) => `${p}: "…${around(v.text)}…"`).join(' / '));
  const offered = ['tool:lasso', 'tool:probe', 'moment:0', 'pv-draw', 'slab', 'sig:3'].filter((id) => Object.values(menu).some((v) => v.enabled.includes(id)));
  check(`${L}: the menu offers no spectral tool (lasso, probe, moment, PV, velocity range, σ)`, !offered.length, offered.join(', '));
  check(`${L}: the menu crops z as x and y`, menu.range.enabled.includes('crop:2'), menu.range.enabled.join(','));
  // The readout at the box centre: X, Y, Z and a value.
  const ro = await c.evaluate(`(async () => { const s = document.querySelector('script[type=module][src*="main.js"]');
    const RO = await import(new URL('readout.js', s ? s.src : location.href + 'js/').href);
    const st = window.__vrState; return RO.readoutLines('EXPLORE', [0.5, 0.5, 0.5], { info: st.info, cube: st.cube, texScale: st.texScale }); })()`);
  check(`${L}: the readout gives X, Y, Z and a value, no channel`, Array.isArray(ro) && ro.length === 4 && /^X /.test(ro[1]) && /^Z .*voxel/.test(ro[2])
    && !/ch \d|km\/s/.test(ro.join(' ')) && /e-\d+/.test(ro[3]), JSON.stringify(ro));
  // At its default threshold (the 99th percentile) the shell is a thin one; at the 90th it fills the box.
  const at99 = await c.evaluate('window.__vrDrawnFraction()');
  await c.evaluate(`window.__vrMenu.act({ type: 'press', id: 'pct:90' })`);
  await new Promise((r) => setTimeout(r, 300));
  const at90 = await c.evaluate('window.__vrDrawnFraction()');
  check(`${L}: the density is drawn, more of it at the 90th percentile than at the 99th`, at99.fraction > 0 && at90.fraction > 0.003
    && at90.fraction > at99.fraction, `${(100 * at99.fraction).toFixed(2)} % at the 99th, ${(100 * at90.fraction).toFixed(2)} % at the 90th`);
  // The clipping plane (issue #369) on this density, drawn at the 90th percentile.
  if (clipPlane) results.clipPlane = await runClipPlaneChecks({ c, check });
  // Every text of the menu fits with these values (the live menu of a simulation volume).
  results.textFit = await runTextFitChecks({ c, check, live: true });
  // The signed field beside it: a simulation too, on a linear scale.
  const vx = await load(`${data}::vbt-field=1`);
  const v = vx.value ? await c.evaluate(`({ scale: window.__vrState.cube.scale, simulation: window.__vrState.info.simulation, lo: window.__vrState.cube.lo })`) : null;
  check(`${L}: the signed velocity field stays on a linear scale`, !!v && v.scale === 'linear' && v.simulation === true && v.lo < 0, JSON.stringify(v));
  if (cube) {
    const r = await load(cube);
    const back = r.value ? await c.evaluate(`({ z: window.__vrState.zStretch, tool: window.__vrSelection.sel.tool,
      clip: window.__vrMenu.ui.clipTool, scale: window.__vrState.cube.scale, sim: !!window.__vrState.info.simulation,
      sig: document.querySelector('button.sig').parentElement.hidden })`) : null;
    check(`${L}: the radio cube after it gets its velocity stretch, lasso and velocity range back, linear, σ shown`,
      !!back && back.z === 2 && back.tool === 'lasso' && back.clip === 'slab' && back.scale === 'linear' && !back.sim && !back.sig,
      JSON.stringify(back));
  }
  // The colour bar, the readout, the menu drawn all along: no page error.
  const errs = await c.evaluate(`window.__vrShell.diag.items.filter((e) => e.level === 'error'
    && !(e.event === 'request-failed' && e.message.includes('/v1/render/'))).map((e) => e.event + ': ' + e.message)`);
  check(`${L}: no page error`, !errs.length && !(c.pageErrors || []).length, [...errs, ...(c.pageErrors || [])].join(' / '));
  return results;
}

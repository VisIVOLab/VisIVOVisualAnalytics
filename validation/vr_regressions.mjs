// Integration regressions for epic #166. Run by vr_lifecycle.mjs --regressions
// against its private backend and synthetic files, also in CI.
import { spawnSync } from 'node:child_process';
import { dirname, join } from 'node:path';

export async function runRegressions({ c, check, waitFor, api, python, makeCube, token, cube }) {
  const backend = 'window.__vrRefine.refiner.backend()';
  const state = 'window.__vrState';
  const layers = 'window.__vrLayers';
  async function load(path) {
    const before = await c.evaluate(`${state}.loadGen`);
    await c.evaluate(`(() => { document.getElementById('path').value = ${JSON.stringify(path)};
      document.getElementById('load').click(); })()`);
    const done = await waitFor(c, `${state}.loadGen > ${before} && !${state}.loading`, 30000);
    if (!done.value) throw new Error(`load did not finish: ${path}`);
    const error = await c.evaluate(`document.getElementById('load-status').textContent`);
    if (/^(Error|Failed):/.test(error)) throw new Error(error);
  }

  // A real 404: recover the cube and the sharing controller together.
  const old = await c.evaluate(`({ id: ${state}.open.dataset_id, session: ${backend}.session })`);
  await api('DELETE', `/v1/datasets/${old.id}`, token, { session: old.session });
  const recovered = await c.evaluate(`window.__vrShell.recover('user')`);
  const shared = await c.evaluate(`({ id: ${state}.open.dataset_id, followed: window.__vrShare.st.dataset })`);
  check('recovery restarts selection sharing on the new dataset', recovered?.ok
    && shared.id !== old.id && shared.followed === shared.id);

  // A catalogue answer held while another cube is installed.
  const cubeB = makeCube('regression_B.fits', { shape: [24, 40, 48], ra: 151 });
  await c.evaluate(`(() => {
    const b = ${backend}; const original = b._json.bind(b);
    window.__restoreJson = () => { b._json = original; };
    b._json = (method, path, body) => path === '/v1/cube/catalogue_pixels'
      ? new Promise(resolve => { window.__releaseOverlay = () => resolve({ valid: true,
        name: 'catalogue-A', sources: [{ x: 32, y: 32, z: 20, label: 'from-A' }], polylines: [], n_sources: 1 }); })
      : original(method, path, body);
    document.getElementById('ct-source').value = 'crossmatch:simbad';
    window.__overlayPromise = ${layers}.loadOverlay();
  })()`);
  await waitFor(c, '!!window.__releaseOverlay', 5000);
  await load(cubeB);
  await c.evaluate('window.__releaseOverlay(); window.__overlayPromise');
  check('catalogue of the previous cube is discarded', await c.evaluate(`${layers}.state.overlay === null`));
  await c.evaluate('window.__restoreJson()');

  // Trace again is a UI request, not an initial load: its late reply must not
  // overwrite the new cube's extent, axes or labels.
  const flow = join(dirname(cubeB), 'regression_flow.fits');
  const made = spawnSync(python, ['-c', `import sys, numpy as np
from astropy.io import fits
g=np.linspace(-1,1,16); z,y,x=np.meshgrid(g,g,g,indexing='ij')
fits.writeto(sys.argv[1],np.stack([-x+.5*y,-y-.5*x,-.3*z]).astype(np.float32)*100,overwrite=True)`, flow]);
  if (made.status !== 0) throw new Error(`could not make flow fixture: ${made.stderr}`);
  await load(flow);
  check('velocity field fixture loads', await c.evaluate(`${layers}.mode === 'flow' && !!${layers}.state.flow?.info`));
  // The menu on a velocity field (#257): its Display controls, Range greyed but
  // for the crop; a crop and a colour window dragged, drawn, and undone.
  const fm = await c.evaluate(`(async () => {
    const m = window.__vrMenu, s = ${state};
    m.ui.page = 'display';
    const disp = m.layout().map((w) => w.id);
    m.ui.page = 'range';
    const range = m.layout();
    m.act({ type: 'range', id: 'crop:0', value: [0.3, 1] });
    m.act({ type: 'range', id: 'fl-range', value: [0.2, 0.8] });
    await new Promise((r) => setTimeout(r, 400));          // frames drawn with the crop and the window
    const cropped = s.clip.min[0], win = ${layers}.state.s.flC0, steps = m.ui.undo.size;
    m.act({ type: 'press', id: 'undo' });
    m.act({ type: 'press', id: 'undo' });
    m.ui.page = 'display';
    return { disp: ['fl-range', 'fl-retrace', 'fl-box'].every((id) => disp.includes(id)),
      slabGreyed: !!range.find((w) => w.id === 'slab').disabled, cropOn: !range.find((w) => w.id === 'crop:2').disabled,
      cropped, win, steps, back: [s.clip.min[0], ${layers}.state.s.flC0] };
  })()`);
  check('menu on a velocity field: Display controls, crop and colour window, undone (#257)', fm.disp && fm.slabGreyed && fm.cropOn
    && fm.cropped === 0.3 && Math.abs(fm.win - 0.2) < 1e-9 && fm.steps === 2 && fm.back[0] === 0 && fm.back[1] === 0, JSON.stringify(fm));
  await c.evaluate(`(() => {
    const b = ${backend}; const original = b._binary.bind(b);
    window.__restoreBinary = () => { b._binary = original; };
    b._binary = async (...args) => {
      const result = await original(...args);
      if (args[0] === '/v1/velocity/lines_bin') return new Promise(resolve => { window.__releaseLines = () => resolve(result); });
      return result;
    };
    document.getElementById('fl-retrace').click();
  })()`);
  const pending = await waitFor(c, '!!window.__releaseLines', 20000);
  if (!pending.value) throw new Error('flow retrace did not answer');
  await load(cube);
  const extent = await c.evaluate(`${state}.extent`);
  await c.evaluate('window.__releaseLines()');
  // Flush the completion through CDP before inspecting the installed scene.
  await c.evaluate('new Promise(resolve => setTimeout(resolve, 0))');
  const after = await c.evaluate(`${state}.extent`);
  check('late flow retrace cannot change cube geometry', String(extent) === String(after));
  await c.evaluate('window.__restoreBinary()');

  // Automatic binning may resolve beyond the explicit-factor API limit.
  const wide = makeCube('regression_wide.fits', { shape: [3, 4, 17000] });
  await c.evaluate(`document.getElementById('maxdim').value = '256'`);
  await c.evaluate(`(() => {
    const prototype = ${backend}.constructor.prototype; const original = prototype.reducedInfo;
    window.__restoreInfo = () => { prototype.reducedInfo = original; };
    prototype.reducedInfo = async function (...args) {
      const info = await original.apply(this, args);
      // Editing the controls while metadata loads must not change this load
      // or the factors used when it is recovered later.
      document.getElementById('maxdim').value = '128';
      return info;
    };
  })()`);
  await load(wide);
  await c.evaluate('window.__restoreInfo()');
  check('automatic binning above 64 loads in the page', await c.evaluate(`${state}.info.factors[0] === 67
    && ${state}.cube.factors[0] === 67 && ${state}.openPath === ${JSON.stringify(wide)}`));
  const unchanged = await c.evaluate(`window.__vrShell.recover('user')`);
  check('large automatic factors survive recovery', unchanged?.ok);

  // The same shape is not proof of the same data. A changed WCS must not be
  // attached to the old texture and sky labels; require an explicit reload.
  const replaced = spawnSync(python, ['-c', `import sys
from astropy.io import fits
d,h=fits.getdata(sys.argv[1],header=True); h['CRVAL1']=151.0
fits.writeto(sys.argv[1],d,h,overwrite=True)`, wide]);
  if (replaced.status !== 0) throw new Error('could not replace the temporary FITS');
  const oldId = await c.evaluate(`${state}.open.dataset_id`);
  const result = await c.evaluate(`window.__vrShell.recover('user')`);
  check('same-shape changed FITS requires a reload', result && !result.ok && /open it again/.test(result.detail)
    && await c.evaluate(`${state}.open.dataset_id === ${JSON.stringify(oldId)}`));
  await load(wide);
  const verified = await c.evaluate(`(async () => {
    const current = await ${backend}.reducedInfo({ ...${state}.cube.reduction, dataset_id: ${state}.open.dataset_id });
    return { sameRevision: current.file_revision === ${state}.info.file_revision,
      sameAxes: JSON.stringify(current.axes) === JSON.stringify(${state}.info.axes),
      factors: current.factors, shownFactors: ${state}.info.factors };
  })()`);
  check('explicit reload installs current file revision and coordinates', verified.sameRevision && verified.sameAxes, JSON.stringify(verified));
  check('no uncaught regression-test page exception', !c.pageErrors.length, c.pageErrors.join(' / '));
}

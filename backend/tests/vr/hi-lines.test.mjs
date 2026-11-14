// The HI measurements of a selection as text (issue #280): the selection
// window's lines, the wrist card's, the record's labels, and the replay
// carrying the session's noise (node --test).
import test from 'node:test';
import assert from 'node:assert/strict';
import * as HI from '../../app/static/vr/js/hi-lines.js';
import * as WR from '../../app/static/vr/js/wrist.js';
import * as Rc from '../../app/static/vr/js/recipe.js';

// HCG 44's SoFiA-2 source 1 (J101723.73+214802.8), as the route answers it.
const STATS = {
  flux_jy_kms: 0.46542, velocity_min_kms: 1890.1, velocity_max_kms: 1960.3,
  hi: {
    flux_jy_kms: 0.46542, flux_err_jy_kms: 0.012651, noise_sigma: 2.3478e-4, noise_unit: 'JY/BEAM',
    noise_source: 'local: 1.4826 x median |value| of the unselected voxels', noise_voxels: 123456,
    peak: 0.0047522, peak_unit: 'JY/BEAM', peak_snr: 20.24,
    w20_kms: 62.170, w50_kms: 42.318, vsys_kms: 1926.602,
    vsys_definition: 'midpoint of the two W20 crossings of the integrated spectrum',
    velocity_convention: 'optical (AIPS VELO-HEL, no VELREF)', velocity_frame: 'BARYCENT',
    centroid_ra_deg: 154.348875, centroid_dec_deg: 21.800778, centroid_frame: 'ICRS',
    centroid_velocity_kms: 1926.805,
    major_axis_arcsec: 51.63, minor_axis_arcsec: 43.08, pa_deg: 1.77,
    size_definition: 'semi-axes', notes: [],
  },
};

test('sexagesimal positions read as SoFiA-2 names the source', () => {
  assert.equal(HI.sexagesimal(154.348875, true), '10:17:23.73');
  assert.equal(HI.sexagesimal(21.800778), '+21:48:02.8');
  assert.equal(HI.sexagesimal(-29.048065), '-29:02:53.0');
  assert.equal(HI.sexagesimal(359.99999999, true), '00:00:00.00', 'rounds into the next hour, then wraps');
  assert.equal(HI.sexagesimal(Number.NaN), 'n/a');
});

test('the selection window and the wrist show the measurements', () => {
  assert.equal(HI.fluxText(STATS), 'flux 0.465 ± 0.013 Jy km/s');
  const lines = HI.panelLines(STATS);
  assert.deepEqual(lines.slice(0, 5), [
    'peak 0.00475 JY/BEAM, S/N 20.2',
    'W20 62.2  W50 42.3  Vsys 1926.6 km/s',
    'velocity: optical (AIPS VELO-HEL, no VELREF), BARYCENT',
    'centroid 10:17:23.73 +21:48:02.8, 1926.8 km/s',
    'size 51.6" × 43.1" (semi-axes), PA 2°',
  ]);
  assert.match(lines[5], /^σ 2\.35e-4 JY\/BEAM \(local\)$/);
  const lasso = { tool: 'lasso', lasso: { count: 1335, stats: STATS } };
  assert.deepEqual(WR.selectionLines(lasso), ['Lasso 1,335 voxels', 'flux 0.465 ± 0.013 Jy km/s', 'Vsys 1926.6 W50 42 km/s']);
  // An older backend (no `hi`): the lines as before.
  assert.deepEqual(HI.panelLines({ flux_jy_kms: 1 }), []);
  assert.equal(HI.wristLines({ flux_jy_kms: 1 }), null);
  assert.equal(HI.fluxText({ flux_jy_kms: 1.5 }), 'flux 1.50 Jy km/s');
});

test('the record carries the measurements with their conventions', () => {
  const p = HI.recordParams(STATS);
  assert.equal(p['Flux (Jy km/s)'], '0.4654 ± 0.013');
  assert.equal(p['W20 / W50 (km/s)'], '62.17 / 42.32');
  assert.match(p['Systemic velocity (km/s)'], /^1926\.60 \(midpoint of the two W20 crossings.*optical.*BARYCENT\)$/);
  assert.match(p['Centroid (RA, Dec)'], /^10:17:23\.73, \+21:48:02\.8 \(ICRS; 154\.348875°, 21\.800778°\)$/);
  assert.equal(p['Peak S/N'], '20.2');
  assert.deepEqual(HI.recordParams({}), {});
});

test('the replay gives the statistics the noise the grow used', () => {
  const g = Rc.lassoGrowRequest([5, 6, 7], { sigma: 0.002 });
  assert.deepEqual(Rc.lassoReplay(g)[1].request, { sigma: 0.002 });
  assert.equal(Rc.lassoReplay(Rc.lassoGrowRequest([5, 6, 7]))[1].request, undefined, 'measured: no request');
  assert.equal(Rc.boxStatsRequest([[0, 1], [0, 1], [0, 1]], 0.003).sigma, 0.003);
  assert.equal('sigma' in Rc.boxStatsRequest([[0, 1], [0, 1], [0, 1]]), false);
});

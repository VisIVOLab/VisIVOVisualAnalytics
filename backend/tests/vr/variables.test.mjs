// The variable picker of a simulation volume (issue #317, sim-controls.js and
// the hand menu's Display page): which variables are offered, their chips,
// each variable's look, the hub's word on a switch (node --test).
import test from 'node:test';
import assert from 'node:assert/strict';
import * as SC from '../../app/static/vr/js/sim-controls.js';
import * as HM from '../../app/static/vr/js/hand-menu.js';
import { dataToNorm, normToData } from '../../app/static/vr/js/volume.js';

const close = (a, b, eps = 1e-9) => assert.ok(Math.abs(a - b) <= eps, `${a} != ${b}`);

const PLUTO = (names, current) => ({
  fields: names.map((n) => ({ name: n, path: `/d/rho.0000.flt::pluto-var=${n}` })), field_name: current });

test('variablesOf: two or more fields make a picker, one or none hide it', () => {
  const v = SC.variablesOf(PLUTO(['rho', 'vx1', 'prs'], 'vx1'));
  assert.deepEqual(v.names, ['rho', 'vx1', 'prs']);
  assert.equal(v.current, 'vx1');
  assert.equal(v.paths.prs, '/d/rho.0000.flt::pluto-var=prs');
  assert.equal(SC.variablesOf(PLUTO(['rho'], 'rho')), null, 'a 2048³ run copied one variable at a time');
  assert.equal(SC.variablesOf({ fields: [], field_name: '' }), null, 'a radio cube');
  assert.equal(SC.variablesOf({}), null, 'an older backend: no fields in reduced_info');
  assert.equal(SC.variablesOf(null), null);
  // Malformed rows are dropped; a name twice counts once; an unlisted current is null.
  const w = SC.variablesOf({ fields: [{ name: 'a', path: 'p' }, { name: 'a', path: 'q' }, { name: '', path: 'r' }, null, { name: 'b', path: 's' }], field_name: 'z' });
  assert.deepEqual(w.names, ['a', 'b']);
  assert.equal(w.paths.a, 'p');
  assert.equal(w.current, null);
});

test('variableChips: all when they fit, else the page holding the variable on show and a next cell', () => {
  assert.deepEqual(SC.variableChips(['a', 'b', 'c'], 'b'), { chips: ['a', 'b', 'c'], more: false, pages: 1, page: 0 });
  assert.deepEqual(SC.variableChips(['a', 'b', 'c', 'd', 'e'], 'e').chips, ['a', 'b', 'c', 'd', 'e']);
  const names = ['rho', 'vx1', 'vx2', 'vx3', 'Bx1', 'Bx2', 'Bx3', 'prs', 'tr1'];
  assert.deepEqual(SC.variableChips(names, 'rho'), { chips: ['rho', 'vx1', 'vx2', 'vx3'], more: true, pages: 3, page: 0 });
  assert.deepEqual(SC.variableChips(names, 'Bx2').chips, ['Bx1', 'Bx2', 'Bx3', 'prs']);
  assert.deepEqual(SC.variableChips(names, 'tr1').chips, ['tr1']);
  assert.equal(SC.variableChips(names, 'nope').page, 0);
  // Every variable is reached by stepping (cycleField), cyclic both ways.
  let at = 'rho';
  const seen = new Set([at]);
  for (let i = 0; i < names.length; i++) { at = SC.nextVariable(names, at, 1); seen.add(at); }
  assert.equal(seen.size, names.length);
  assert.equal(SC.nextVariable(names, 'tr1', 1), 'rho');
  assert.equal(SC.nextVariable(names, 'rho', -1), 'tr1');
});

test('VariableLooks: each variable\'s look by its path, a copy each time, the oldest dropped past the cap', () => {
  const L = new SC.VariableLooks(2);
  const a = { colormap: 'viridis', threshold: 1e-3, colourMax: 1, scale: 'log' };
  L.save('/p::pluto-var=rho', a);
  a.colormap = 'changed';
  assert.equal(L.get('/p::pluto-var=rho').colormap, 'viridis');
  L.save('/p::pluto-var=vx1', { colormap: 'gray' });
  L.save('/p::pluto-var=prs', { colormap: 'hot' });
  assert.equal(L.get('/p::pluto-var=rho'), null);
  assert.equal(L.size, 2);
  L.save('', a);
  assert.equal(L.size, 2);
  // Saved again: the newest, so not the next to go.
  L.save('/p::pluto-var=vx1', { colormap: 'gray' });
  L.save('/p::pluto-var=rho', { colormap: 'viridis' });
  assert.equal(L.get('/p::pluto-var=prs'), null);
  assert.equal(L.get('/p::pluto-var=vx1').colormap, 'gray');
});

test('lookNorms: a saved threshold and colour max put back on the cube\'s codes, linear or log', () => {
  const log = { lo: -3, hi: 3, scale: 'log' };
  const n = SC.lookNorms({ threshold: 0.1, colourMax: 100 }, log, dataToNorm);
  close(n.thrNorm, 2 / 6);
  close(n.cmaxNorm, 5 / 6);
  close(normToData(n.thrNorm, log.lo, log.hi, 'log'), 0.1, 1e-12);
  const lin = { lo: -2, hi: 2, scale: 'linear' };
  assert.deepEqual(SC.lookNorms({ threshold: -5, colourMax: 9 }, lin, dataToNorm), { thrNorm: 0, cmaxNorm: 1 }, 'clamped to the cube\'s range');
  // A value at or below zero on a log cube is below every code; the colour max stays above the threshold.
  const z = SC.lookNorms({ threshold: 50, colourMax: -1 }, log, dataToNorm);
  close(z.cmaxNorm, Math.min(1, z.thrNorm + 0.01));
  assert.equal(SC.lookNorms({ threshold: NaN, colourMax: 1 }, log, dataToNorm), null);
  assert.equal(SC.lookNorms(null, log, dataToNorm), null);
});

test('variableSwitch: the hub says a variable of the same source, not an ordinary switch', () => {
  const name = (p) => p.split('/').pop();
  const info = PLUTO(['rho', 'vx1'], 'vx1');
  assert.deepEqual(SC.variableSwitch('/d/rho.0000.flt::pluto-var=vx1', '/d/rho.0000.flt::pluto-var=rho', info, name),
    { summary: 'Variable vx1 of rho.0000.flt', field: 'vx1', from: '/d/rho.0000.flt::pluto-var=rho' });
  assert.equal(SC.variableSwitch('/d/rho.0000.flt::pluto-var=vx1', '/d/other.fits', info, name), null);
  // A switch made with the picker: told whatever the spelling of the previous path (/var vs /private/var).
  assert.equal(SC.variableSwitch('/private/var/d/rho.0000.flt::pluto-var=vx1', '/var/d/rho.0000.flt::pluto-var=rho',
    PLUTO(['rho', 'vx1'], 'vx1'), name, true).summary, 'Variable vx1 of rho.0000.flt');
  // The cube on show opened by its data file: the same source.
  assert.equal(SC.variableSwitch('/d/rho.0000.flt::pluto-var=vx1', '/d/rho.0000.flt', info, name).from, '/d/rho.0000.flt');
  assert.equal(SC.fieldSource('/d/v.bin::vbt-field=1'), '/d/v.bin');
  assert.equal(SC.fieldSource('/d/c.fits'), '/d/c.fits');
  assert.equal(SC.variableSwitch('/d/rho.0000.flt::pluto-var=vx1', '', info, name), null);
  assert.equal(SC.variableSwitch('/d/rho.0000.flt::pluto-var=rho', '/d/rho.0000.flt::pluto-var=rho', PLUTO(['rho', 'vx1'], 'rho'), name), null);
  const vbt = { fields: [{ name: 'rho', path: '/d/v.bin::vbt-field=0' }, { name: 'T', path: '/d/v.bin::vbt-field=1' }], field_name: 'T' };
  assert.equal(SC.variableSwitch('/d/v.bin::vbt-field=1', '/d/v.bin::vbt-field=0', vbt, name).summary, 'Variable T of v.bin');
});

test('Display: a simulation volume with several variables picks one on its last row; with one, the row is the catalogue\'s', () => {
  const base = { ...HM.defaultContext(), kind: 'cube', simulation: true };
  const many = HM.pageSpec('display', { ...base, vars: { names: ['rho', 'vx1', 'prs'], current: 'vx1', pending: null } });
  assert.deepEqual(many.rows[5], ['var', 'Variable']);
  const row = many.widgets.filter((w) => w.row === 5);
  assert.deepEqual(row.map((w) => w.id), ['var:rho', 'var:vx1', 'var:prs']);
  assert.deepEqual(row.filter((w) => w.on).map((w) => w.value), ['vx1']);
  assert.ok(row.every((w) => w.group === 'var' && !w.disabled && w.free));
  assert.ok(!many.widgets.some((w) => w.id.startsWith('ct-')));
  // Nine variables: four chips (the page of the one on show) and a next cell.
  const nine = ['rho', 'vx1', 'vx2', 'vx3', 'Bx1', 'Bx2', 'Bx3', 'prs', 'tr1'];
  const paged = HM.pageSpec('display', { ...base, vars: { names: nine, current: 'prs', pending: null } }).widgets.filter((w) => w.row === 5);
  assert.deepEqual(paged.map((w) => w.id), ['var:Bx1', 'var:Bx2', 'var:Bx3', 'var:prs', 'var-next']);
  // While one loads, the row is greyed with its reason.
  const busy = HM.pageSpec('display', { ...base, vars: { names: ['rho', 'vx1'], current: 'rho', pending: 'vx1' } }).widgets.filter((w) => w.row === 5);
  assert.ok(busy.every((w) => w.disabled === HM.WHY.varLoading));
  // One variable, or not a simulation: the catalogue row as before.
  for (const ctx of [{ ...base, vars: null }, { ...base, simulation: false, vars: { names: ['a', 'b'], current: 'a' } }]) {
    const s = HM.pageSpec('display', ctx);
    assert.deepEqual(s.rows[5], ['cat', 'Catalogue']);
    assert.deepEqual(s.widgets.filter((w) => w.row === 5).map((w) => w.id), ['ct-show', 'ct-labels', 'ct-pins']);
  }
});

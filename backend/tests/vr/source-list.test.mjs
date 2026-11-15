// The source list of a catalogue over a cube (issue #279): rows, sort, pages,
// steps, flags and notes, the VOTable request and the hub event (round trip),
// found sources, the visit, the culling, and the Sources window's layout.
import test from 'node:test';
import assert from 'node:assert/strict';
import * as SL from '../../app/static/vr/js/source-list.js';
import * as SW from '../../app/static/vr/js/source-window.js';
import * as W from '../../app/static/vr/js/windows.js';
import * as HUB from '../../app/static/vr/js/headset.js';
import * as HM from '../../app/static/vr/js/hand-menu.js';
import * as CT from '../../app/static/vr/js/catalogue.js';
import * as M from '../../app/static/vr/js/mat4.js';
import { PanelPointer } from '../../app/static/vr/js/vr-panel.js';
import { readFileSync } from 'node:fs';
import { classificationFixture } from './classification_fixture.mjs';
import { parseProduct, openIn, parseClassification } from './desktop_hub_parser.mjs';

const close = (a, b, eps = 1e-6) => assert.ok(Math.abs(a - b) <= eps, `${a} != ${b}`);
const info = { full_shape: [64, 64, 40], axes: { z: { full_velocity_kms: Array.from({ length: 40 }, (_, i) => 1000 + 5 * i) } } };
const ov = {
  valid: true, name: 'cat.csv', source: 'catalogue', frame: 'icrs', source_frame: 'icrs',
  sources: [
    { x: 10, y: 12, z: 5.5, label: 'SoFiA J2', lon: 150.01, lat: 2.01, row: 1 },
    { x: 40, y: 30, z: null, label: 'SoFiA J10', lon: 149.95, lat: 2.05, row: 0 },
    { x: 32, y: 30, z: 20, label: 'SoFiA J1', lon: 150.0, lat: 1.98, row: 2 },
  ],
};

test('rows carry the backend pixel, the catalogue position and the cube velocity at z', () => {
  const rows = SL.sourceRows(ov, info);
  assert.deepEqual(rows.map((r) => r.key), ['c:1', 'c:0', 'c:2']);
  close(rows[0].velocity, 1027.5);          // between channels 5 and 6
  assert.equal(rows[1].velocity, null);     // placed on the sky only
  assert.equal(rows[2].velocity, 1100);
  assert.equal(rows[0].origin, 'catalogue');
  const vlkb = SL.sourceRows({ valid: true, frame: 'galactic', sources: [{ x: 1, y: 2, z: null, label: 'G1', lon: 30, lat: 0.5 }] }, info);
  assert.equal(vlkb[0].key, 'o:0');
  assert.equal(vlkb[0].origin, 'overlay');
  assert.deepEqual(SL.positionText(vlkb[0]), ['l 30.0000', 'b +0.5000']);
  assert.deepEqual(SL.positionNames('galactic'), ['GLON', 'GLAT']);
  assert.equal(SL.velocityAt(info, -3), 1000);
  assert.equal(SL.velocityAt({ axes: { z: {} } }, 3), null);
});

test('sorting: names numerically, empty values last either way, ties keep the order', () => {
  const rows = SL.sourceRows(ov, info);
  assert.deepEqual(SL.sortRows(rows, 'name', 1).map((r) => r.label), ['SoFiA J1', 'SoFiA J2', 'SoFiA J10']);
  assert.deepEqual(SL.sortRows(rows, 'name', -1).map((r) => r.label), ['SoFiA J10', 'SoFiA J2', 'SoFiA J1']);
  assert.deepEqual(SL.sortRows(rows, 'velocity', -1).map((r) => r.key), ['c:2', 'c:1', 'c:0']);
  assert.deepEqual(SL.sortRows(rows, 'velocity', 1).map((r) => r.key), ['c:1', 'c:2', 'c:0']);
  const flags = { 'c:0': 'spurious', 'c:2': 'real' };
  assert.deepEqual(SL.sortRows(rows, 'flag', 1, (k) => flags[k] || '').map((r) => r.key), ['c:2', 'c:0', 'c:1']);
  assert.deepEqual(SL.sortRows(rows, 'lat', 1).map((r) => r.key), ['c:2', 'c:1', 'c:0']);
});

test('pages and steps wrap, and the page follows the current source', () => {
  const rows = Array.from({ length: 23 }, (_, i) => ({ key: `o:${i}` }));
  assert.equal(SL.pageCount(23, 10), 3);
  assert.equal(SL.pageCount(0, 10), 1);
  assert.equal(SL.pageOfKey(rows, 'o:21', 10), 2);
  assert.equal(SL.stepKey(rows, 'o:22', 1), 'o:0');
  assert.equal(SL.stepKey(rows, 'o:0', -1), 'o:22');
  assert.equal(SL.stepKey(rows, null, 1), 'o:0');
  assert.equal(SL.stepKey(rows, null, -1), 'o:22');
  assert.equal(SL.stepKey([], 'x', 1), null);
  assert.equal(SW.clampPage(-1, 23), 2);
  assert.equal(SW.clampPage(3, 23), 0);
});

test('flag and note sets: configurable, cleaned, defaults when empty', () => {
  assert.deepEqual(SL.parseSet('', SL.DEFAULT_FLAGS, 4), ['real', 'doubtful', 'spurious']);
  assert.deepEqual(SL.parseSet(' yes, no ,yes, maybe, a, b', SL.DEFAULT_FLAGS, 4), ['yes', 'no', 'maybe', 'a']);
  assert.equal(SL.parseSet('averyveryverylongflag', [], 4)[0].length, SL.MAX_SET_LABEL);
});

test('a flag or note pressed again is taken back; counts; saved and read back', () => {
  const c = new SL.Classification();
  assert.equal(c.toggleFlag('c:1', 'real'), 'real');
  c.toggleFlag('c:2', 'spurious');
  c.toggleNote('c:2', 'RFI');
  assert.equal(c.toggleFlag('c:1', 'real'), '');
  assert.equal(c.marks.has('c:1'), false);
  assert.deepEqual(c.counts(), { byFlag: { real: 0, doubtful: 0, spurious: 1 }, flagged: 1 });
  const back = new SL.Classification();
  assert.equal(back.restore(JSON.parse(JSON.stringify(c.toJSON()))), 1);
  assert.deepEqual(back.get('c:2'), { flag: 'spurious', note: 'RFI' });
  // A flag no longer offered is not read back.
  const other = new SL.Classification(['yes', 'no']);
  assert.equal(other.restore(c.toJSON()), 1);
  assert.deepEqual(other.get('c:2'), { flag: '', note: 'RFI' });
  assert.deepEqual(SL.flagColour('real', SL.DEFAULT_FLAGS), SL.FLAG_COLOURS[0]);
  assert.equal(SL.flagColour('x', SL.DEFAULT_FLAGS), null);
});

test('the VOTable request keeps the classified rows and the found sources; the hub event round-trips', () => {
  const user = [SL.foundSource(1, [33, 31, 21], info, { valid: true, ra_deg: 150.002, dec_deg: 1.981 }, 'lasso', { voxels: 120 })];
  const rows = SL.sourceRows(ov, info, user);
  const c = new SL.Classification();
  c.toggleFlag('c:2', 'real');
  c.toggleFlag('c:0', 'doubtful');
  c.toggleNote('c:0', 'edge');
  c.toggleFlag('u:1', 'real');
  const req = SL.votableRequest({ datasetId: 'cube1', catalogueId: 'cat1', rows, cls: c });
  assert.equal(req.catalogue_id, 'cat1');
  assert.deepEqual(req.flag_set, ['real', 'doubtful', 'spurious']);
  assert.deepEqual(req.sources.map((s) => [s.origin, s.row, s.flag, s.note]),
    [['catalogue', 0, 'doubtful', 'edge'], ['catalogue', 2, 'real', ''], ['user', undefined, 'real', '']]);
  assert.equal(req.sources[2].frame, 'icrs');
  close(req.sources[2].lon, 150.002);
  assert.equal(req.sources[2].velocity_kms, 1105);
  // An overlay with no catalogue behind it: every row goes.
  const all = SL.votableRequest({ datasetId: 'cube1', catalogueId: '', rows: SL.sourceRows({ ...ov, sources: ov.sources.map(({ row, ...s }) => s) }, info), cls: c });
  assert.equal(all.sources.length, 3);
  // The hub event: through the page's payload code and back as JSON, as the desktop reads it.
  const h = SL.hubClassification({ rows, cls: c, catalogue: { name: 'cat.csv', path: '/d/cat.csv', dataset_id: 'cat1' } });
  assert.equal(h.flagged, 2, 'the catalogue\'s sources; the found one apart');
  assert.equal(h.found, 1);
  assert.equal(h.catalogue.rows, 3);
  const detail = { product: 'classification', dataset_id: 'cube1', request: req, classification: h, label: SL.classificationSummary(h) };
  const plan = HUB.savePlan(detail, 'cube');
  assert.equal(plan.route, '/v1/catalogue/classified_votable');
  assert.equal(plan.body.dataset_id, 'cube1');
  const payload = HUB.productPayload(detail, 'cat_vr_classified.xml', null);
  assert.equal(payload.request, undefined, 'the VOTable body stays out of the event');
  const back = JSON.parse(JSON.stringify(HUB.fitPayload(payload).payload));
  assert.equal(back.product, 'classification');
  assert.equal(back.file, 'cat_vr_classified.xml');
  assert.deepEqual(back.classification.sources.map((s) => [s.k, s.flag, s.note, s.o]),
    [['c:0', 'doubtful', 'edge', 'c'], ['c:2', 'real', '', 'c'], ['u:1', 'real', '', 'u']]);
  assert.equal(back.classification.sources[0].row, 0);
  assert.match(HUB.productSummary(detail, 'cube.fits', 'cat_vr_classified.xml'), /Classified 2 of 3 sources of cat\.csv \(1 real, 1 doubtful, 0 spurious\), 1 found by eye/);
  assert.ok(HUB.PRODUCT_KINDS.includes('classification'));
});

test('the desktop\'s fixture (classification_event.json) is what the page sends now', () => {
  const file = JSON.parse(readFileSync(new URL('./classification_event.json', import.meta.url), 'utf8'));
  assert.deepEqual(file, JSON.parse(JSON.stringify(classificationFixture())), 'regenerate classification_event.json (classification_fixture.mjs)');
  // What the desktop makes of it (the mirror of VrHeadsetHub.h).
  assert.equal(openIn(parseProduct(file)), 'CatalogueTable');
  const c = parseClassification(file.payload);
  assert.deepEqual(c.counts, [1, 0, 1]);
  assert.deepEqual(c.sources.map((x) => [x.key, x.flag, x.origin, x.row]),
    [['c:0', 'spurious', 'catalogue', 0], ['c:2', 'real', 'catalogue', 2], ['u:1', 'doubtful', 'user', -1]]);
  assert.ok(Number.isNaN(c.sources[0].z) && c.sources[1].velocityKms === 1100);
});

test('a large classification is cut to the hub budget and says so', () => {
  const many = { valid: true, source_frame: 'icrs', sources: Array.from({ length: 2000 }, (_, i) => ({ x: i % 64, y: 1, z: 2, label: `Source number ${i}`, lon: 150, lat: 2, row: i })) };
  const rows = SL.sourceRows(many, info);
  const c = new SL.Classification();
  for (const r of rows) c.toggleFlag(r.key, 'real');
  const h = SL.hubClassification({ rows, cls: c });
  assert.ok(h.truncated);
  assert.ok(h.sources.length > 20 && h.sources.length < 2000);
  assert.ok(HUB.jsonBytes(h) < 16 * 1024 - 2000, `${HUB.jsonBytes(h)} bytes`);
  assert.equal(h.flagged, 2000);
});

test('a found source sits at the lasso brightest voxel, else the centre of its box', () => {
  assert.deepEqual(SL.lassoVoxel({ stats: { peak_voxel: [3.2, 4.8, 5] }, origin: [0, 0, 0], dims: [9, 9, 9] }), [3, 5, 5]);
  assert.deepEqual(SL.lassoVoxel({ origin: [10, 20, 30], dims: [5, 3, 1] }), [12, 21, 30]);
  assert.equal(SL.lassoVoxel(null), null);
  const u = SL.foundSource(4, [1, 2, 3], info, null, 'probe');
  assert.equal(u.label, 'VR-4');
  assert.equal(u.lon, null);
  assert.equal(u.velocity, 1015);
});

test('the visit brings the source to the target at the asked size, orientation kept', () => {
  const model = M.multiply(M.translation(0.3, 1.2, -0.8), M.multiply(M.rotation([0, 1, 0], 0.7), M.scaling(0.6)));
  const p = [0.1, -0.05, 0.2];
  const target = [0, 1.5, -0.5];
  const out = SL.visitModel(model, p, 0.05, target, 0.25);
  const got = M.transformPoint(out, p);
  for (let i = 0; i < 3; i++) close(got[i], target[i], 1e-5);
  close(SL.modelScale(out), 5, 1e-4);
  // The same rotation: directions unchanged.
  const d0 = M.normalize(M.sub(M.transformPoint(model, [1, 0, 0]), M.transformPoint(model, [0, 0, 0])));
  const d1 = M.normalize(M.sub(M.transformPoint(out, [1, 0, 0]), M.transformPoint(out, [0, 0, 0])));
  for (let i = 0; i < 3; i++) close(d0[i], d1[i], 1e-5);
  // Clamped scale.
  close(SL.modelScale(SL.visitModel(model, p, 1e-6, target, 0.25)), 40, 1e-3);
  // The target ahead of the eyes along the gaze, below them.
  const head = M.multiply(M.translation(1, 1.6, 2), M.rotation([0, 1, 0], Math.PI / 2));
  const t = SL.visitTarget(head, 0.5, 0.1);
  close(t[0], 0.5, 1e-5); close(t[1], 1.5, 1e-6); close(t[2], 2, 1e-5);
});

test('the visited position matches the overlay marker, and the slab moves only when needed', () => {
  const shape = [64, 64, 40], extent = [1, 1, 0.625];
  const r = SL.sourceRows(ov, info)[0];
  const p = SL.rowModel(r, shape, extent);
  close(p[0], CT.pixelToModel(10, 64, 1));
  close(p[2], CT.pixelToModel(5.5, 40, 0.625));
  const sky = SL.rowModel(SL.sourceRows(ov, info)[1], shape, extent, { min: [0, 0, 0.5], max: [1, 1, 1] });
  close(sky[2], CT.pixelToModel(0.75 * 40 - 0.5, 40, 0.625));
  assert.equal(SL.slabFor(5.5, 40, { min: [0, 0, 0], max: [1, 1, 1] }), null);
  const s = SL.slabFor(5.5, 40, { min: [0, 0, 0.5], max: [1, 1, 0.8] });
  close(s[1] - s[0], 0.3);
  assert.ok((5.5 + 0.5) / 40 >= s[0] && (5.5 + 0.5) / 40 <= s[1]);
  assert.equal(s[0], 0);
  assert.equal(SL.slabFor(null, 40, { min: [0, 0, 0.5], max: [1, 1, 0.8] }), null);
  close(SL.rowSizeModel(null, [200, 200, 40], extent), 0.08);
  close(SL.rowSizeModel(null, shape, extent), 6 / 64);     // never under 6 pixels
  close(SL.rowSizeModel({ ellipse: [[0, 0], [8, 0], [8, 4], [0, 4]] }, shape, extent), 20 / 64);
});

test('a large catalogue is drawn nearest the focus first, and the overlay honours it', () => {
  const rows = [{ key: 'a', x: 0, y: 0, z: 0 }, { key: 'b', x: 50, y: 50, z: 30 }, { key: 'c', x: 2, y: 1, z: null }];
  assert.equal(SL.nearestKeys(rows, [0, 0, 0], 5), null);
  assert.deepEqual([...SL.nearestKeys(rows, [0, 0, 0], 2)].sort(), ['a', 'c']);
  const opts = { drawn: (i) => i !== 1, colourOf: (i) => (i === 0 ? [0, 1, 0] : null), labels: true, labelled: (i) => i === 2 };
  const segs = CT.overlaySegments(ov, [64, 64, 40], [1, 1, 0.625], { min: [0, 0, 0], max: [1, 1, 1] }, opts);
  assert.equal(segs.drawn, 2);
  assert.deepEqual(segs.segs[0].color.slice(0, 3), [0, 1, 0]);
  assert.deepEqual(segs.labels.map((l) => l.text), ['SoFiA J1']);
  const marks = SL.listSegments(SL.sourceRows(ov, info, [SL.foundSource(1, [5, 5, 5], info, null, 'probe')]), 'c:1', [64, 64, 40], [1, 1, 0.625], { min: [0, 0, 0], max: [1, 1, 1] });
  assert.equal(marks.length, 8 + 5);      // the bracket's 8 strokes, the found diamond's 5
});

test('the Sources window: no overlap, rows paged, greyed reasons, pressed through the pointer', () => {
  const rows = SL.sourceRows(ov, info);
  const v = { page: 0, pages: 1, total: 3, rows, current: 'c:2', currentRow: rows[2], sort: { key: 'name', dir: 1 },
    flagSet: SL.DEFAULT_FLAGS, noteSet: SL.DEFAULT_NOTES, mark: { flag: 'real', note: '' }, frame: 'icrs', canSend: true };
  const lay = SW.sourceLayout(v);
  const box = (w) => [w.rect[0], w.rect[1], w.rect[0] + w.rect[2], w.rect[1] + w.rect[3]];
  for (let i = 0; i < lay.length; i++) {
    const a = box(lay[i]);
    assert.ok(a[0] >= 0 && a[1] >= 0 && a[2] <= SW.SRC_PX[0] && a[3] <= SW.SRC_PX[1], lay[i].id);
    for (let j = i + 1; j < lay.length; j++) {
      const b = box(lay[j]);
      const over = Math.min(a[2], b[2]) > Math.max(a[0], b[0]) && Math.min(a[3], b[3]) > Math.max(a[1], b[1]);
      assert.ok(!over, `${lay[i].id} / ${lay[j].id}`);
    }
  }
  assert.ok(lay.find((w) => w.id === 'flag:real').on);
  assert.ok(lay.find((w) => w.id === 'row:c:2').on);
  assert.match(lay.find((w) => w.id === 'found-remove').disabled, /found/);
  assert.equal(lay.find((w) => w.id === 'page-next').disabled, 'One page only.');
  // Nothing chosen: flags greyed with the reason.
  const none = SW.sourceLayout({ ...v, current: null, currentRow: null, mark: null });
  assert.equal(none.find((w) => w.id === 'flag:real').disabled, 'Choose a source first.');
  // A press reaches the widget through the window's content pointer.
  const wp = new W.WindowPointer(lay);
  const row = lay.find((w) => w.id === 'row:c:1');
  const hit = { x: row.rect[0] + 20, y: row.rect[1] + 10, t: 1 };
  wp.update({ bar: null, content: hit }, false);
  const acts = wp.update({ bar: null, content: hit }, true);
  assert.deepEqual(acts.map((a) => [a.type, a.id, a.content]), [['press', 'row:c:1', true]]);
  assert.ok(wp.takesRay());
  // The window's size keeps the panel's pixel density.
  close(SW.SRC_SIZE[0] / SW.SRC_PX[0], 1 / 2133);
  assert.deepEqual(W.WINDOWS.sources.contentPx, SW.SRC_PX);
  assert.ok(new PanelPointer(lay));
});

test('the Select tab has the Sources row, two presses from opening, greyed until it applies', () => {
  const ctx = { ...HM.defaultContext(), kind: 'cube' };
  const empty = HM.menuLayout('select', ctx);
  assert.match(HM.widgetById(empty, 'src-next').disabled, /No sources/);
  assert.match(HM.widgetById(empty, 'src-add').disabled, /Select the source first/);
  assert.equal(HM.widgetById(empty, 'src-list').disabled, '');
  const busy = { ...ctx, src: { total: 18, shown: true, canAdd: true } };
  assert.equal(HM.pressesTo('src-next', 'display', busy), 2);
  assert.equal(HM.pressesTo('src-list', 'select', busy), 1);
  assert.ok(HM.widgetById(HM.menuLayout('select', busy), 'src-list').on);
  assert.equal(HM.pageSpec('select', busy).status, 6, 'the Mask row under it (#282)');
});

// A classification event as the hub relays it (issue #279), made by the
// page's own code: source-list.test.mjs checks that classification_event.json
// is this, and the desktop's test (tests/test_vr_headset_hub.cpp) reads that
// file, so the page and VrHeadsetHub.h parseClassification are held to one
// payload. Regenerate the JSON with JSON.stringify(classificationFixture(), null, 1).
import * as SL from '../../app/static/vr/js/source-list.js';
import * as HUB from '../../app/static/vr/js/headset.js';

export function classificationFixture() {
  const info = { full_shape: [64, 64, 40], axes: { z: { full_velocity_kms: Array.from({ length: 40 }, (_, i) => 1000 + 5 * i) } } };
  const ov = { valid: true, name: 'cat.csv', source: 'catalogue', catalogueId: 'cat1', source_frame: 'galactic', sources: [
    { x: 10, y: 12, z: 5.5, label: 'G030.1+0.5', lon: 30.1, lat: 0.5, row: 1 },
    { x: 40, y: 30, z: null, label: 'G030.2-0.1', lon: 30.2, lat: -0.1, row: 0 },
    { x: 32, y: 30, z: 20, label: 'G030.0+0.0', lon: 30.0, lat: 0.0, row: 2 },
  ] };
  const user = [SL.foundSource(1, [33, 31, 21], info, { valid: true, ra_deg: 281.5, dec_deg: -2.5 }, 'lasso')];
  const rows = SL.sourceRows(ov, info, user);
  const cls = new SL.Classification();
  cls.toggleFlag('c:2', 'real');
  cls.toggleFlag('c:0', 'spurious');
  cls.toggleNote('c:0', 'RFI');
  cls.toggleFlag('u:1', 'doubtful');
  const classification = SL.hubClassification({ rows, cls, catalogue: { name: 'cat.csv', path: '/data/cat.csv', dataset_id: 'cat1' } });
  const detail = { product: 'classification', dataset_id: 'cube1', classification, label: SL.classificationSummary(classification) };
  const payload = HUB.productPayload(detail, 'cat_vr_classified.xml', null);
  return { seq: 7, client: 'q3', time: 1794700002.1, kind: 'product', cube_path: '/data/cube.fits',
    summary: HUB.productSummary(detail, 'cube.fits', 'cat_vr_classified.xml'),
    payload: { ...payload, output_path: '/srv/exports/cat_vr_classified.xml' } };
}

// What the VR page records of a selection (issue #171): the requests it sent,
// under the API's field names, as the desktop records them (visivo-recipe/1,
// src/app/ProductRecipe.cpp and src/app/LassoReplay.cpp), so a selection made
// in the headset is replayed by the same calls a desktop recipe is replayed
// with. Pure functions: node runs the tests.

export const SCHEMA = 'visivo-recipe/1';

/** The Kinematic Lasso's parameters as the desktop's panel starts: reach 8,
 *  velocity coupling 1, threshold 2.5 sigma, the noise measured by the service
 *  (sigma 0), an object-bounded grow (roi_radius 0), the level-field budget of
 *  the Reach slider (64), no smoothing. */
export const LASSO_DEFAULTS = Object.freeze({
  reach: 8, lambda_v: 1, sigma: 0, nsigma: 2.5, roi_radius: 0, reach_max: 64,
  smooth_v: 0, smooth_xy: 0,
});

/**
 * The body of POST /v1/cube/kinematic_select_bin for a seed [z, y, x], as
 * LassoReplay's lassoGrowRequestMap builds it: the seed lists and the
 * smoothing only when set, sigma 0 when the service measures its own. Without
 * dataset_id (a replay opens the path and uses the id the open returns).
 */
export function lassoGrowRequest(seedZYX, params = {}, addSeeds = [], subSeeds = []) {
  const p = { ...LASSO_DEFAULTS, ...params };
  const req = { seed_z: seedZYX[0], seed_y: seedZYX[1], seed_x: seedZYX[2] };
  if (addSeeds.length) req.add_seeds = addSeeds.map((s) => [s[0], s[1], s[2]]);
  if (subSeeds.length) req.sub_seeds = subSeeds.map((s) => [s[0], s[1], s[2]]);
  req.reach = Number(p.reach);
  req.lambda_v = Number(p.lambda_v);
  req.sigma = p.sigma > 0 ? Number(p.sigma) : 0;
  req.nsigma = Number(p.nsigma);
  req.roi_radius = Math.round(Number(p.roi_radius));
  req.reach_max = Number(p.reach_max);
  if (p.smooth_v > 0 || p.smooth_xy > 0) {
    req.smooth_v = Number(p.smooth_v);
    req.smooth_xy = Number(p.smooth_xy);
  }
  return req;
}

/** Where a mask posted back to the service takes its box from: the grow's
 *  response headers (never a recorded number: the grow answers with the
 *  selection's own bounding box). The desktop's wording. */
const ROI_FROM_GROW = 'with origin_x, origin_y, origin_z from its X-Visivo-Origin header and '
  + 'width, height, depth from X-Visivo-Width, X-Visivo-Height, X-Visivo-Depth';

/** The replay of a lasso selection made in the headset: the grow, then the
 *  statistics of the mask it returns (the headset makes no new cube). */
export function lassoReplay(growRequest) {
  return [
    { route: '/v1/cube/kinematic_select_bin', request: growRequest },
    {
      route: '/v1/cube/mask_stats_bin',
      body: `the previous call's response body, the selection as a little-endian float32 mask, posted back ${ROI_FROM_GROW}`,
    },
  ];
}

/** A number as Qt's QString::arg(v, 0, 'g', precision) writes it: %g with
 *  `precision` significant digits, trailing zeros dropped, a two-digit exponent. */
export function formatG(v, precision = 17) {
  if (v === 0) return '0';
  if (!Number.isFinite(v)) return String(v);
  const exp = Math.floor(Math.log10(Math.abs(Number(v.toPrecision(precision)))));
  const strip = (t) => (t.includes('.') ? t.replace(/0+$/, '').replace(/\.$/, '') : t);
  if (exp < -4 || exp >= precision) {
    const [m, e] = v.toExponential(precision - 1).split('e');
    const n = Number(e);
    return `${strip(m)}e${n < 0 ? '-' : '+'}${String(Math.abs(n)).padStart(2, '0')}`;
  }
  return strip(v.toFixed(Math.max(0, precision - 1 - exp)));
}

/** Where a noise sigma typed on the VR page came from, for the Provenance label. */
export const VR_NOISE_WHERE = 'typed on the VR page';

/** The Provenance labels of the desktop (lassoDisplayParams); `noiseWhere`
 *  says where a supplied sigma came from, as the desktop's label does. */
export function lassoDisplayParams(growRequest, noiseWhere = VR_NOISE_WHERE) {
  const g = growRequest;
  const params = {
    'Seed (z, y, x)': `(${g.seed_z}, ${g.seed_y}, ${g.seed_x})`,
    Reach: formatG(g.reach, 4),
    'Level-field budget (reach_max)': formatG(g.reach_max, 4),
    'Velocity coupling': g.lambda_v.toFixed(2),
    Threshold: `${g.nsigma.toFixed(1)}σ`,
    'Noise σ': g.sigma > 0 ? `${formatG(g.sigma, 17)} (noise estimate, ${noiseWhere})` : 'robust, whole cube',
    'Smoothing (v, y, x)': g.smooth_v > 0 || g.smooth_xy > 0
      ? `(${g.smooth_v}, ${g.smooth_xy}, ${g.smooth_xy})` : 'off',
    'Hand-edited': 'no',
  };
  const fmt = (s) => `(${s[0]}, ${s[1]}, ${s[2]})`;
  if (g.add_seeds) params['Extra seeds added'] = g.add_seeds.map(fmt).join('; ');
  if (g.sub_seeds) params['Extra seeds carved'] = g.sub_seeds.map(fmt).join('; ');
  return params;
}

/** The body of POST /v1/spectral/probe for full pixel (x, y), as the
 *  desktop's BackendClient::spectralProbe sends it (less dataset_id). */
export function probeRequest(x, y) {
  return { x, y };
}

/** The desktop's origin of a probed spectrum (ProductOrigin encodeProbeOrigin). */
export function probeOrigin(px) {
  return { kind: 'probe', voxel: [px[0], px[1], px[2]] };
}

/** The query of POST /v1/cube/mask_stats_bin for every voxel of the box
 *  [[x0, x1], [y0, y1], [z0, z1]] (inclusive), with no body (box=true). */
export function boxStatsRequest(range) {
  const [[x0, x1], [y0, y1], [z0, z1]] = range;
  return { origin_x: x0, origin_y: y0, origin_z: z0,
    width: x1 - x0 + 1, height: y1 - y0 + 1, depth: z1 - z0 + 1, box: true };
}

/** The replay of a box: one call, no body. */
export function boxReplay(range) {
  return [{ route: '/v1/cube/mask_stats_bin', request: boxStatsRequest(range),
    body: 'none: box=true measures every voxel of the box' }];
}

/** The desktop's origin of a box region (encodeRegionOrigin), with the channel
 *  range the 3-D box spans. */
export function boxOrigin(xr, yr, zr) {
  return { kind: 'region', shape: 'box', anchor: [xr[0], yr[0]], current: [xr[1], yr[1]], channels: [zr[0], zr[1]] };
}

/**
 * A visivo-recipe/1 record (ProductRecipe::productRecipe): schema,
 * application, export time, the product with its replay steps, the dataset
 * (its path, the volatile ids of the session that made it), the Provenance
 * parameters and the origin geometry.
 */
export function makeRecipe({ application, exported, product, dataset, parameters = {}, origin = null }) {
  const p = { id: product.id, kind: product.kind, label: product.label };
  if (product.replay && product.replay.length) p.replay = product.replay;
  if (product.replay_caveat) p.replay_caveat = product.replay_caveat;
  const ds = { label: dataset.label || '', path: dataset.path || '' };
  if (dataset.backend_dataset_id) ds.backend_dataset_id = dataset.backend_dataset_id;
  if (dataset.backend_session_id) ds.backend_session_id = dataset.backend_session_id;
  const r = { schema: SCHEMA, application, exported, product: p, dataset: ds, parameters };
  if (origin) r.origin = origin;
  return r;
}

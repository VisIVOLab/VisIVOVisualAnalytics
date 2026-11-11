// A PV slice along a path drawn with the controller (issue #191), through the
// desktop's route (/v1/cube/pv: the request of BackendClient::requestPv and of
// its task, 0-based fractional pixel vertices, a width in pixels) and with
// the desktop's orientation of the velocity axis (PvPathGeometry's
// rowsOnUniformSpectralGrid: rows in the order of the channels' spectral
// coordinate, lowest at the bottom, whatever the sign of CDELT3). Each trigger
// press adds a vertex where the ray picks the cube; from two vertices on the
// diagram follows the path, as the desktop's editable path does.
//
// Pure parts tested by node: backend/tests/vr/tools-pv.test.mjs.

import * as PC from './product-canvas.js';
import * as S from './selection.js';

/** The desktop's default slit width (vtkWindowCube::pvWidthPixels). */
export const DEFAULT_WIDTH = 1;
export const MAX_WIDTH = 25;

/** A box point (0..1 over the cube's pixels, pixel i's centre at (i + 0.5)/n)
 *  as a 0-based fractional pixel (x, y) of the full cube, within it. */
export function boxToVertex(p, fullShape) {
  return [0, 1].map((i) => Math.max(0, Math.min(fullShape[i] - 1, p[i] * fullShape[i] - 0.5)));
}

/** The vertex's box point (inverse of boxToVertex). */
export function vertexToBox(v, fullShape) {
  return [(v[0] + 0.5) / fullShape[0], (v[1] + 0.5) / fullShape[1]];
}

/** The body of POST /v1/cube/pv (less dataset_id). */
export function pvRequest(vertices, width = DEFAULT_WIDTH) {
  return { vertices: vertices.map((v) => [v[0], v[1]]), width_pixels: Math.max(1, Math.round(width)) };
}

/** The desktop's product origin of a PV path (ProductOrigin encodePvPathOrigin). */
export function pvOrigin(vertices, width) {
  if (vertices.length < 2) return null;
  return { kind: 'pvpath', points: vertices.map((v) => [v[0], v[1]]), width };
}

/** The desktop's Provenance of a PV diagram (vtkWindowCube, pvParams). */
export function pvParams(vertices, width) {
  const a = vertices[0], b = vertices[vertices.length - 1];
  return {
    Path: `${vertices.length} points`,
    Width: `${width} px`,
    From: `(${a[0].toFixed(1)}, ${a[1].toFixed(1)})`,
    To: `(${b[0].toFixed(1)}, ${b[1].toFixed(1)})`,
    'Measured on': `a ${vertices.length}-point PV path, ${width} px wide`,
  };
}

/** The rows from the bottom up: the channels in increasing spectral
 *  coordinate (the desktop's one-row-per-channel order; a stable sort keeps
 *  equal coordinates in channel order). */
export function rowOrder(spectral) {
  const idx = Array.from({ length: spectral.length }, (_, i) => i);
  return idx.sort((a, b) => (spectral[a] - spectral[b]) || (a - b));
}

/** The PV answer decoded: values [depth × samples] (channel-major, as the
 *  backend's pv.reshape(-1)), the offsets along the path in arcsec (or
 *  pixels), the channels' spectral coordinates. */
export async function decodePv(r) {
  const n = r.num_samples, d = r.depth;
  const raw = async (b64) => {
    const bytes = PC.bytesFromBase64(b64);
    const u = r.compression === 'qt-zlib' ? await PC.qtUncompress(bytes) : bytes;
    return new Float32Array(u.buffer, u.byteOffset, u.byteLength / 4);
  };
  const values = await raw(r.data_base64);
  if (values.length !== n * d) throw new Error(`PV payload has ${values.length} values, expected ${n * d}`);
  const arcsec = r.positions_arcsec_base64 ? await raw(r.positions_arcsec_base64) : null;
  const positions = arcsec && arcsec.length === n ? arcsec : await raw(r.positions_base64);
  const spectral = PC.f64FromBase64(r.spectral_coordinates_base64);
  return { values, samples: n, depth: d, positions, positionUnit: arcsec && arcsec.length === n ? (r.spatial_unit || 'arcsec') : 'pixel',
    spectral: spectral.length === d ? spectral : null, unit: r.spectral_axis_unit, type: r.spectral_axis_type, bunit: r.bunit };
}

/** The velocities (km/s) of the rows from the bottom up, from the page's
 *  per-channel velocities (reduced_info's full_velocity_kms), or null. */
export function rowVelocities(order, fullVelocityKms) {
  if (!Array.isArray(fullVelocityKms) || fullVelocityKms.length !== order.length) return null;
  const v = order.map((ch) => fullVelocityKms[ch]);
  // A channel without a velocity (null): the rows are labelled with the
  // spectral coordinate instead.
  return v.every((x) => typeof x === 'number' && Number.isFinite(x)) ? v : null;
}

/** The caption of the rows' axis, from the direction its values actually
 *  go (on a frequency cube the velocity falls as the frequency rises). */
export function rowCaption(rows, name) {
  if (!rows || rows.length < 2 || !Number.isFinite(rows[0]) || !Number.isFinite(rows[rows.length - 1])
      || rows[0] === rows[rows.length - 1]) return name;
  return `${name}: ${rows[rows.length - 1] > rows[0] ? 'increasing' : 'decreasing'} upward`;
}

/** Path length in pixels (sum of the segments). */
export function pathLength(vertices) {
  let L = 0;
  for (let i = 1; i < vertices.length; i++) L += Math.hypot(vertices[i][0] - vertices[i - 1][0], vertices[i][1] - vertices[i - 1][1]);
  return L;
}

/**
 * ctx: {selection, backend(), datasetId(), info(), state() (the page's state:
 * clip, extent), onChange()}.
 */
export function createPvTool(ctx) {
  const t = { armed: false, armedTool: null, vertices: [], width: DEFAULT_WIDTH, busy: '', error: '', result: null,
    gen: 0, recipeId: '', canvas: PC.makeCanvas(...PC.PLANE_PX), pending: false, inFlight: false };
  const changed = () => ctx.onChange && ctx.onChange();

  function arm(on = !t.armed) {
    t.armed = !!on;
    // A selection tool chosen afterwards disarms the path (see poll).
    t.armedTool = ctx.selection.sel.tool;
    changed();
  }

  /** Disarm when another selection tool was chosen (panel, left trigger, page). */
  function poll() {
    if (t.armed && ctx.selection.sel.tool !== t.armedTool) { t.armed = false; changed(); }
  }

  /** The trigger or a click with a box-space ray: a vertex where the ray picks
   *  the cube (the first drawn sample, else the middle of the ray in the box). */
  function act(ray) {
    const s = ctx.state();
    const r = ctx.selection.pickBox(ray);
    let p = r ? r.hit.p : null;
    if (!p) {
      const span = S.rayBox(ray.o, ray.d, s.clip.min, s.clip.max);
      if (span) p = [0, 1, 2].map((i) => ray.o[i] + ray.d[i] * 0.5 * (span.tn + span.tf));
    }
    if (!p) { t.error = 'The ray misses the cube.'; changed(); return; }
    addVertex(boxToVertex(p, ctx.info().full_shape));
  }

  function addVertex(v) {
    t.error = '';
    if (!t.vertices.length) t.recipeId = '';   // a new path is a new product
    t.vertices.push(v);
    changed();
    if (t.vertices.length >= 2) request();
  }

  function undo() {
    t.vertices.pop();
    if (t.vertices.length < 2) { t.gen++; t.result = null; t.busy = ''; }
    else request();
    changed();
  }

  function newPath() {
    t.gen++;
    t.vertices = [];
    t.result = null;
    t.busy = t.error = '';
    t.recipeId = '';
    changed();
  }

  function setWidth(w) {
    t.width = Math.max(1, Math.min(MAX_WIDTH, Math.round(w)));
    if (t.vertices.length >= 2) request();
    changed();
  }

  /** Latest wins: one request in flight, the newest path asked for after it. */
  function request() {
    if (t.inFlight) { t.pending = true; return; }
    run();
  }

  async function run() {
    const b = ctx.backend();
    if (!b || !ctx.datasetId()) { t.error = 'This needs the backend.'; changed(); return; }
    const gen = ++t.gen;
    const verts = t.vertices.map((v) => [...v]);
    const req = pvRequest(verts, t.width);
    t.inFlight = true; t.pending = false;
    t.busy = `PV along ${verts.length} points…`;
    changed();
    try {
      const t0 = performance.now();
      const r = await b._json('POST', '/v1/cube/pv', { dataset_id: ctx.datasetId(), ...req });
      if (!r.valid) throw new Error(r.error || 'The PV failed.');
      const d = await decodePv(r);
      if (gen === t.gen) {
        const order = d.spectral ? rowOrder(d.spectral) : Array.from({ length: d.depth }, (_, i) => i);
        const fv = ctx.info().axes.z.full_velocity_kms;
        t.result = { ...d, order, velocities: rowVelocities(order, fv), req, vertices: verts, ms: performance.now() - t0,
          range: PC.finiteRange(d.values) || [0, 1] };
        const rec = ctx.selection.record({ kind: 'pv', label: 'PV diagram', replay: [{ route: '/v1/cube/pv', request: req }] },
          pvParams(verts, req.width_pixels), pvOrigin(verts, req.width_pixels), t.recipeId);
        t.recipeId = rec.product.id;
        t.busy = '';
        draw();
      }
    } catch (e) {
      if (gen === t.gen) { t.busy = ''; t.error = String(e && e.message ? e.message : e); }
    }
    t.inFlight = false;
    if (t.pending && t.vertices.length >= 2) run();
    changed();
  }

  function draw() {
    const R = t.result;
    if (!R || !t.canvas) return;
    const table = PC.lut('viridis');
    const v = R.velocities;
    // The rows' own values (their order is the spectral coordinate's): ticks
    // interpolated between them, since an optical velocity is not linear in
    // frequency; the caption says which way the velocity goes.
    const rows = v || (R.spectral ? R.order.map((ch) => R.spectral[ch]) : R.order.map((_, i) => i));
    const xLo = R.positions[0], xHi = R.positions[R.samples - 1];
    const xHalf = R.samples > 1 ? (xHi - xLo) / (R.samples - 1) / 2 : 0.5;
    const zc = ctx.info().axes.z;
    PC.drawProduct(t.canvas, {
      title: `PV along ${R.vertices.length} points, ${R.req.width_pixels} px wide`,
      lines: [
        `${PC.fmt(pathLength(R.vertices), 4)} px of path, ${R.samples} samples × ${R.depth} channels, ${R.ms.toFixed(0)} ms`,
        rowCaption(rows, v ? `velocity${zc.velocity_convention ? ` (${zc.velocity_convention})` : ''}` : `${R.type} (${R.unit})`),
      ],
      rgba: PC.colourise(R.values, R.samples, R.depth, R.range, table, R.order), w: R.samples, h: R.depth,
      aspect: Math.max(0.2, Math.min(5, (R.depth / R.samples) * 1.2)), table,
      xTicks: PC.ticks(xLo - xHalf, xHi + xHalf), yTicks: PC.rowTicks(rows),
      xLabel: `offset along the path (${R.positionUnit})`, yLabel: v ? 'velocity (km/s)' : `${R.type} (${R.unit})`,
      bar: { lo: R.range[0], hi: R.range[1], unit: R.bunit || '' },
    });
  }

  /** Model-space segments: the path on the front and back of the velocity
   *  slab, a post at each vertex, the next segment to the ray while armed. */
  function segments(hoverP = null) {
    const s = ctx.state(), info = ctx.info();
    if (!info || (!t.vertices.length && !t.armed)) return [];
    const e = s.extent;
    const n = info.full_shape;
    const toModel = (vx, z) => { const b = vertexToBox(vx, n); return [(b[0] - 0.5) * e[0], (b[1] - 0.5) * e[1], (z - 0.5) * e[2]]; };
    const out = [];
    const col = [1, 0.6, 0.15, 0.95];
    const z0 = s.clip.min[2], z1 = s.clip.max[2];
    for (let i = 0; i < t.vertices.length; i++) {
      const v = t.vertices[i];
      out.push({ a: toModel(v, z0), b: toModel(v, z1), color: col });
      if (i) {
        const u = t.vertices[i - 1];
        out.push({ a: toModel(u, z0), b: toModel(v, z0), color: col });
        out.push({ a: toModel(u, z1), b: toModel(v, z1), color: col });
      }
    }
    if (t.armed && hoverP && t.vertices.length) {
      const last = t.vertices[t.vertices.length - 1];
      const next = boxToVertex(hoverP, n);
      const zc = (z0 + z1) / 2;
      out.push({ a: toModel(last, zc), b: toModel(next, zc), color: [1, 0.6, 0.15, 0.45] });
    }
    return out;
  }

  function lines() {
    const out = [];
    out.push(t.armed ? `PV path: trigger adds a point (${t.vertices.length} so far)` : t.vertices.length ? `PV path of ${t.vertices.length} points` : '');
    if (t.busy) out.push(t.busy);
    if (t.error) out.push(t.error);
    return out.filter(Boolean);
  }

  return { state: t, arm, poll, act, addVertex, undo, newPath, setWidth, segments, lines, draw };
}

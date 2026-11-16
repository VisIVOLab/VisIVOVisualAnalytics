// Isosurfaces of the cube's field (issue #370): the pure part, run headless
// by node (backend/tests/vr/isosurface.test.mjs). The backend extracts each
// surface from the original data (app/isosurface.py) and sends it as a
// compact mesh; this module reads that mesh, places the surfaces in the
// headset's pre-sized buffers, and keeps the surfaces' list (levels in the
// field's units, colours, opacity). The drawing is isosurface-gl.js, the
// page's side (menu, requests) isosurface-ui.js.

/** The mesh's header: "VISO", version, vertices, triangles, bbox lo, bbox hi. */
export const MAGIC = 0x4f534956;   // "VISO" read as a little-endian uint32
export const VERSION = 1;
export const HEADER_BYTES = 40;

/** Surfaces shown at once, and the triangles each is decimated to: the
 *  headset's buffers hold MAX_SURFACES × SURFACE_TRIANGLES (600 k triangles,
 *  7.2 MB of indices and at most as many vertices, 6 MB), allocated once. */
export const MAX_SURFACES = 4;
export const SURFACE_TRIANGLES = 150000;
export const MAX_TRIANGLES = MAX_SURFACES * SURFACE_TRIANGLES;
export const MAX_VERTICES = MAX_TRIANGLES;
/** Bytes per vertex in the buffers, as the mesh comes: 3 uint16 positions,
 *  4 int8 normals (uploaded as they are, no copy). */
export const POS_BYTES = 6;
export const NRM_BYTES = 4;

/** The surfaces' colours, in order (a new surface takes the first unused). */
export const PALETTE = [
  { name: 'Orange', rgb: [1.0, 0.6, 0.24] },
  { name: 'Cyan', rgb: [0.24, 0.78, 1.0] },
  { name: 'Magenta', rgb: [1.0, 0.31, 0.85] },
  { name: 'Lime', rgb: [0.61, 0.9, 0.39] },
  { name: 'Yellow', rgb: [1.0, 0.88, 0.3] },
  { name: 'White', rgb: [0.91, 0.91, 0.91] },
];

/**
 * The mesh of /v1/isosurface/mesh/{id} (app/isosurface.py encode_mesh), as
 * views on `buf` (no copy): {vertices, triangles, lo, hi, positions
 * (Uint16Array, 3 a vertex, over lo..hi), normals (Int8Array, 4 a vertex, box
 * coordinates), indices (Uint32Array, 3 a triangle)}. Throws on anything else.
 */
export function decodeMesh(buf) {
  if (!(buf instanceof ArrayBuffer) || buf.byteLength < HEADER_BYTES) throw new Error('the isosurface came empty');
  const dv = new DataView(buf);
  if (dv.getUint32(0, true) !== MAGIC) throw new Error('not an isosurface mesh');
  const version = dv.getUint32(4, true);
  if (version !== VERSION) throw new Error(`isosurface mesh version ${version}, this page reads ${VERSION}`);
  const n = dv.getUint32(8, true), m = dv.getUint32(12, true);
  const lo = [dv.getFloat32(16, true), dv.getFloat32(20, true), dv.getFloat32(24, true)];
  const hi = [dv.getFloat32(28, true), dv.getFloat32(32, true), dv.getFloat32(36, true)];
  const posAt = HEADER_BYTES;
  const nrmAt = posAt + 6 * n + ((4 - ((6 * n) % 4)) % 4);
  const idxAt = nrmAt + 4 * n;
  const end = idxAt + 12 * m;
  if (buf.byteLength < end) throw new Error(`the isosurface came with ${buf.byteLength} bytes for ${n} vertices and ${m} triangles`);
  if (m && !n) throw new Error('an isosurface with triangles and no vertices');
  const indices = new Uint32Array(buf, idxAt, 3 * m);
  return {
    vertices: n, triangles: m, lo, hi,
    positions: new Uint16Array(buf, posAt, 3 * n),
    normals: new Int8Array(buf, nrmAt, 4 * n),
    indices, bytes: end,
  };
}

/** The largest index of a decoded mesh is below its vertex count (checked
 *  once, on arrival: a bad index would read past the surface's vertices). */
export function indicesValid(mesh) {
  const idx = mesh.indices;
  for (let i = 0; i < idx.length; i++) if (idx[i] >= mesh.vertices) return false;
  return true;
}

/** A decoded mesh's vertex position k in box coordinates (tests, picking). */
export function vertexAt(mesh, k, out = [0, 0, 0]) {
  for (let a = 0; a < 3; a++) out[a] = mesh.lo[a] + (mesh.positions[3 * k + a] / 65535) * (mesh.hi[a] - mesh.lo[a]);
  return out;
}

/**
 * Where each mesh goes in the shared buffers, in order: {vOff, tOff} (in
 * vertices and triangles), or null for a mesh that no longer fits (the
 * surfaces before it keep their place). `meshes`: [{vertices, triangles}].
 */
export function planPacking(meshes, capV = MAX_VERTICES, capT = MAX_TRIANGLES) {
  let v = 0, t = 0;
  return meshes.map((m) => {
    if (!m || v + m.vertices > capV || t + m.triangles > capT) return null;
    const at = { vOff: v, tOff: t };
    v += m.vertices;
    t += m.triangles;
    return at;
  });
}

/** The first palette colour no surface in `used` (names) has. */
export function nextColour(used) {
  const free = PALETTE.find((p) => !used.includes(p.name));
  return (free || PALETTE[used.length % PALETTE.length]).name;
}

export const colourOf = (name) => (PALETTE.find((p) => p.name === name) || PALETTE[0]).rgb;
export const cssColour = (name) => `#${colourOf(name).map((c) => Math.round(c * 255).toString(16).padStart(2, '0')).join('')}`;

/**
 * The level of a new surface, as a normalised value of the cube's range (in
 * its scale: log10 of the values on a log cube): the first at the
 * threshold, the next ones between it and the colour max, away from the
 * levels already there.
 */
export function newLevelNorm(existing, thrNorm, cmaxNorm) {
  if (!existing.length) return clamp01(thrNorm);
  const span = Math.max(1e-3, cmaxNorm - thrNorm);
  const candidates = [0.5, 0.25, 0.75, 0.125, 0.875].map((f) => clamp01(thrNorm + f * span));
  // The first candidate a tenth of the span away from every level there, else the farthest.
  let best = candidates[0], far = -1;
  for (const c of candidates) {
    const d = Math.min(...existing.map((e) => Math.abs(e - c)));
    if (d >= 0.1 * span) return c;
    if (d > far + 1e-9) { far = d; best = c; }
  }
  return best;
}

const clamp01 = (x) => Math.min(1, Math.max(0, Number.isFinite(x) ? x : 0));

/**
 * The extraction request for the surfaces wanted (/v1/isosurface/extract):
 * the levels in the field's units, the cube's scale (a log cube is contoured
 * in log space), a budget each. Levels a log scale cannot take (at or below
 * zero) are left out (returned in `refused`).
 */
export function extractRequest(datasetId, levels, scale, { budget = SURFACE_TRIANGLES, lod = -1 } = {}) {
  const ok = [], refused = [];
  for (const v of levels) {
    if (!Number.isFinite(v) || (scale === 'log' && !(v > 0))) refused.push(v);
    else if (!ok.includes(v)) ok.push(v);
  }
  return { body: { dataset_id: datasetId, levels: ok, scale: scale === 'log' ? 'log' : 'linear', lod, max_triangles: budget }, refused };
}

/** A level as text (three significant digits, as volume.js valueText). */
export function levelText(v, unit = '') {
  if (!Number.isFinite(v)) return 'n/a';
  const a = Math.abs(v);
  const t = a !== 0 && (a < 1e-3 || a >= 1e5) ? v.toExponential(2) : String(Number(v.toPrecision(3)));
  return unit ? `${t} ${unit}` : t;
}

/** One line about a made surface (the menu's status, the HUD). */
export function surfaceLine(i, s, unit = '') {
  const head = `${i + 1} ${s.colour}: ${levelText(s.value, unit)}`;
  if (s.error) return `${head}: ${s.error}`;
  if (s.pending) return `${head}: extracting${Number.isFinite(s.progress) ? ` ${Math.round(100 * s.progress)}%` : '…'}`;
  const m = s.made;
  if (!m) return head;
  if (!m.triangles) return `${head}: no surface (the level is outside the values)`;
  const raw = m.raw_triangles > m.triangles ? ` of ${(m.raw_triangles / 1e6).toFixed(m.raw_triangles >= 1e7 ? 0 : 1)} M` : '';
  const grid = Array.isArray(m.grid) ? m.grid.join('×') : '';
  return `${head}: ${(m.triangles / 1000).toFixed(0)} k triangles${raw}, ${grid} ${m.source === 'pyramid' ? `pyramid level ${m.lod}` : m.lod ? `every ${m.factor}th voxel` : 'full resolution'}`;
}

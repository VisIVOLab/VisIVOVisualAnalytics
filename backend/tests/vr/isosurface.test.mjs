// Isosurfaces (issue #370): isosurface.js (the backend's mesh decoded, the
// buffers' packing, new levels, the request, the lines), isosurface-gl.js
// (the normal matrix; buffers sized once, filled with bufferSubData, nothing
// allocated to draw), isosurface-ui.js against a fake backend, and the hand
// menu's Surfaces page (node --test).
import test from 'node:test';
import assert from 'node:assert/strict';
import * as ISO from '../../app/static/vr/js/isosurface.js';
import { IsoSurfaceGL, normalMatrix } from '../../app/static/vr/js/isosurface-gl.js';
import * as HM from '../../app/static/vr/js/hand-menu.js';
import * as M from '../../app/static/vr/js/mat4.js';

// A tetrahedron (and one vertex of no triangle) as app/isosurface.py
// encode_mesh wrote it: points (0.25..0.75) in box coordinates, normals of
// its vertices (-1,-1,-1)/√3, +x, +y, +z, +z.
const FIXTURE = 'VklTTwEAAAAFAAAABAAAAAAAgD4AAIA+AACAPgAAQD8AAEA/AABAPwAAAAAAAP//AAAAAAAA//8AAAAAAAD//wCAAIAzswAAt7e3AH8AAAAAfwAAAAB/AAAAfwAAAAAAAgAAAAEAAAAAAAAAAQAAAAMAAAAAAAAAAwAAAAIAAAABAAAAAgAAAAMAAAA=';
const fixture = () => { const b = Buffer.from(FIXTURE, 'base64'); return b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength); };

/** A mesh in the wire format (as encode_mesh documents it). */
function encode(points, normals, tris) {
  const n = points.length, m = tris.length;
  const lo = [0, 1, 2].map((a) => Math.min(...points.map((p) => p[a])));
  const hi = [0, 1, 2].map((a) => Math.max(...points.map((p) => p[a])));
  const nrmAt = 40 + 6 * n + ((4 - ((6 * n) % 4)) % 4);
  const buf = new ArrayBuffer(nrmAt + 4 * n + 12 * m);
  const dv = new DataView(buf);
  dv.setUint32(0, ISO.MAGIC, true); dv.setUint32(4, 1, true); dv.setUint32(8, n, true); dv.setUint32(12, m, true);
  for (let a = 0; a < 3; a++) { dv.setFloat32(16 + 4 * a, lo[a], true); dv.setFloat32(28 + 4 * a, hi[a], true); }
  points.forEach((p, k) => p.forEach((v, a) => dv.setUint16(40 + 6 * k + 2 * a, Math.round(((v - lo[a]) / ((hi[a] - lo[a]) || 1)) * 65535), true)));
  normals.forEach((q, k) => q.forEach((v, a) => dv.setInt8(nrmAt + 4 * k + a, Math.round(v * 127))));
  tris.forEach((t, k) => t.forEach((v, a) => dv.setUint32(nrmAt + 4 * n + 12 * k + 4 * a, v, true)));
  return buf;
}

test('the backend\'s mesh is read as it was written', () => {
  const m = ISO.decodeMesh(fixture());
  assert.equal(m.vertices, 5);
  assert.equal(m.triangles, 4);
  assert.deepEqual(m.lo, [0.25, 0.25, 0.25]);
  assert.deepEqual(m.hi, [0.75, 0.75, 0.75]);
  const want = [[0.25, 0.25, 0.25], [0.75, 0.25, 0.25], [0.25, 0.75, 0.25], [0.25, 0.25, 0.75], [0.5, 0.5, 0.6]];
  want.forEach((p, k) => ISO.vertexAt(m, k).forEach((v, a) => assert.ok(Math.abs(v - p[a]) < 1e-5, `${k} ${a} ${v}`)));
  assert.deepEqual([...m.normals.slice(0, 4)], [-73, -73, -73, 0]);
  assert.deepEqual([...m.normals.slice(4, 8)], [127, 0, 0, 0]);
  assert.deepEqual([...m.indices], [0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3]);
  assert.ok(ISO.indicesValid(m));
  assert.equal(m.bytes, 140);
  // Views, not copies: what is uploaded is the response's own bytes.
  assert.equal(m.positions.byteOffset % 2, 0);
  assert.equal(m.indices.byteOffset % 4, 0);
});

test('a mesh with an odd number of vertices keeps its arrays aligned; bad meshes are refused', () => {
  const pts = [[0, 0, 0], [1, 0, 0], [0, 1, 0]];
  const buf = encode(pts, [[0, 0, 1], [0, 0, 1], [0, 0, 1]], [[0, 1, 2]]);
  const m = ISO.decodeMesh(buf);
  assert.equal(m.normals.byteOffset, 40 + 18 + 2);
  assert.deepEqual([...m.indices], [0, 1, 2]);
  assert.deepEqual(ISO.vertexAt(m, 1), [1, 0, 0]);
  assert.throws(() => ISO.decodeMesh(buf.slice(0, buf.byteLength - 4)), /bytes for 3 vertices/);
  const bad = buf.slice(0); new DataView(bad).setUint32(0, 0, true);
  assert.throws(() => ISO.decodeMesh(bad), /not an isosurface/);
  const v2 = buf.slice(0); new DataView(v2).setUint32(4, 2, true);
  assert.throws(() => ISO.decodeMesh(v2), /version 2/);
  assert.throws(() => ISO.decodeMesh(new ArrayBuffer(8)), /empty/);
  const past = buf.slice(0); new DataView(past).setUint32(past.byteLength - 4, 3, true);
  assert.equal(ISO.indicesValid(ISO.decodeMesh(past)), false);
  // An empty surface (a level outside the values): the header alone.
  const empty = ISO.decodeMesh(encode([], [], []));
  assert.equal(empty.vertices + empty.triangles, 0);
});

test('surfaces are packed in order into the buffers, the ones past them left out', () => {
  const at = ISO.planPacking([{ vertices: 10, triangles: 20 }, null, { vertices: 5, triangles: 7 }, { vertices: 100, triangles: 1 }], 50, 100);
  assert.deepEqual(at, [{ vOff: 0, tOff: 0 }, null, { vOff: 10, tOff: 20 }, null]);
  // Four full surfaces fill the capacity exactly.
  const full = ISO.planPacking(Array(4).fill({ vertices: ISO.SURFACE_TRIANGLES, triangles: ISO.SURFACE_TRIANGLES }));
  assert.ok(full.every(Boolean));
  assert.equal(ISO.MAX_TRIANGLES, 600000);
});

test('new levels start at the threshold, then between it and the colour max', () => {
  assert.equal(ISO.newLevelNorm([], 0.2, 0.8), 0.2);
  assert.ok(Math.abs(ISO.newLevelNorm([0.2], 0.2, 0.8) - 0.5) < 1e-12);
  assert.ok(Math.abs(ISO.newLevelNorm([0.2, 0.5], 0.2, 0.8) - 0.35) < 1e-12);
  // Near every candidate: the farthest from the levels there.
  assert.ok(Math.abs(ISO.newLevelNorm([0.2, 0.5, 0.35, 0.66, 0.27, 0.73], 0.2, 0.8) - 0.65) < 1e-12);
  assert.equal(ISO.nextColour([]), 'Orange');
  assert.equal(ISO.nextColour(['Orange', 'Magenta']), 'Cyan');
  assert.equal(ISO.cssColour('Orange'), '#ff993d');
});

test('the request: field units, the cube\'s scale, levels a log scale cannot take left out', () => {
  const r = ISO.extractRequest('ds1', [1e-6, 0, 1e-6, -2, 3e-5], 'log');
  assert.deepEqual(r.body, { dataset_id: 'ds1', levels: [1e-6, 3e-5], scale: 'log', lod: -1, max_triangles: ISO.SURFACE_TRIANGLES });
  assert.deepEqual(r.refused, [0, -2]);
  assert.deepEqual(ISO.extractRequest('d', [-2, 0], 'linear').body.levels, [-2, 0]);
  assert.equal(ISO.extractRequest('d', [1], 'auto').body.scale, 'linear');
  assert.equal(ISO.levelText(3.2e-6, 'g/cm3'), '3.20e-6 g/cm3');
  const line = ISO.surfaceLine(0, { value: 1e-6, colour: 'Cyan', made: { triangles: 150000, raw_triangles: 12124604, grid: [512, 512, 512], source: 'pyramid', lod: 2 } });
  assert.equal(line, '1 Cyan: 1.00e-6: 150 k triangles of 12 M, 512×512×512 pyramid level 2');
  assert.match(ISO.surfaceLine(1, { value: 2, colour: 'Lime', pending: true, progress: 0.42 }), /extracting 42%/);
  assert.match(ISO.surfaceLine(1, { value: 2, colour: 'Lime', made: { triangles: 0 } }), /outside the values/);
});

test('the normal matrix is the inverse transpose of the box-to-view map, up to a scale', () => {
  const mv = M.multiply(M.rotation([0.3, 1, 0.2], 0.7), M.scaling(1.7, 1.7, -1.7));   // a mirroring model too
  const extent = [2, 0.5, 1.25];
  const nm = normalMatrix(new Float32Array(9), mv, extent);
  // A tangent t in box coordinates maps to A t; a normal n (n·t = 0) to
  // nm n; their images stay perpendicular.
  const A = (t) => [0, 1, 2].map((r) => mv[r] * extent[0] * t[0] + mv[4 + r] * extent[1] * t[1] + mv[8 + r] * extent[2] * t[2]);
  const N = (n) => [0, 1, 2].map((r) => nm[r] * n[0] + nm[3 + r] * n[1] + nm[6 + r] * n[2]);
  const n = M.normalize([1, -2, 0.5]);
  for (const t of [[2, 1, 0], [0, 0.25, 1], [1, 0.5, 0]]) {
    const tt = M.sub(t, M.scale3(n, M.dot(t, n)));   // in the tangent plane
    assert.ok(Math.abs(M.dot(A(tt), N(n))) < 1e-5 * M.length(A(tt)) * M.length(N(n)));
  }
});

/** A WebGL2 stand-in that counts what allocates. */
function fakeGL() {
  const calls = { bufferData: [], bufferSubData: [], drawElements: [], createBuffer: 0 };
  const gl = new Proxy({
    ARRAY_BUFFER: 1, ELEMENT_ARRAY_BUFFER: 2, STATIC_DRAW: 3, TRIANGLES: 4, UNSIGNED_INT: 5, UNSIGNED_SHORT: 6, BYTE: 7,
    COMPILE_STATUS: 8, LINK_STATUS: 9, VERTEX_SHADER: 10, FRAGMENT_SHADER: 11,
    createBuffer: () => { calls.createBuffer++; return {}; },
    bufferData: (t, size) => calls.bufferData.push([t, size]),
    bufferSubData: (t, off, data) => calls.bufferSubData.push([t, off, data.byteLength]),
    drawElements: (mode, count, type, off) => calls.drawElements.push([count, off]),
    getShaderParameter: () => true, getProgramParameter: () => true, isContextLost: () => false,
    getUniformLocation: (p, n) => n,
  }, { get: (o, k) => (k in o ? o[k] : () => ({})) });
  return { gl, calls };
}

test('the buffers are sized once; surfaces are written with bufferSubData; a draw allocates nothing', () => {
  const { gl, calls } = fakeGL();
  const g = new IsoSurfaceGL(gl);
  assert.equal(g.bytes, 0);
  const mesh = ISO.decodeMesh(fixture());
  g.upload(mesh, { vOff: 0, tOff: 0 });
  g.upload(mesh, { vOff: 5, tOff: 4 });
  assert.equal(calls.createBuffer, 3);
  assert.deepEqual(calls.bufferData.map((c) => c[1]), [ISO.MAX_VERTICES * 6, ISO.MAX_VERTICES * 4, ISO.MAX_TRIANGLES * 12]);
  assert.deepEqual(calls.bufferSubData.slice(3).map((c) => c[1]), [30, 20, 48]);
  assert.equal(g.bytes, 6e5 * 10 + 6e5 * 12);
  g.setSlots([{ at: { vOff: 0, tOff: 0 }, mesh, rgb: [1, 0, 0], opacity: 1, shown: true },
    { at: { vOff: 5, tOff: 4 }, mesh, rgb: [0, 1, 0], opacity: 0.5, shown: true },
    { at: { vOff: 5, tOff: 4 }, mesh, rgb: [0, 1, 0], opacity: 0.5, shown: false }]);
  const before = [calls.bufferData.length, calls.createBuffer];
  g.draw({ proj: M.identity(), view: M.identity() }, M.identity(), { extent: [1, 1, 1], boxMin: [0, 0, 0], boxMax: [1, 1, 1] });
  assert.deepEqual(calls.drawElements, [[12, 0], [12, 48]]);       // opaque first, the hidden one not drawn
  assert.equal(g.triangles, 8);
  assert.deepEqual([calls.bufferData.length, calls.createBuffer], before);
  // The clipping plane (#369): its uniforms set with every draw, mode 0 without one.
  const unis = [];
  const { gl: gl2 } = fakeGL();
  gl2.uniform1i = (loc, v) => unis.push([loc, v]);
  gl2.uniform4f = (loc, ...v) => unis.push([loc, v]);
  const g2 = new IsoSurfaceGL(gl2);
  g2.upload(mesh, { vOff: 0, tOff: 0 });
  g2.setSlots([{ at: { vOff: 0, tOff: 0 }, mesh, rgb: [1, 0, 0], opacity: 1, shown: true }]);
  const p = { extent: [1, 1, 1], boxMin: [0, 0, 0], boxMax: [1, 1, 1] };
  g2.draw({ proj: M.identity(), view: M.identity() }, M.identity(), p);
  assert.deepEqual(unis.filter((x) => x[0] === 'uClipMode'), [['uClipMode', 0]]);
  unis.length = 0;
  g2.draw({ proj: M.identity(), view: M.identity() }, M.identity(), { ...p, clip: { mode: 1, plane: [1, 0, 0, -0.5], half: 0, sectionA: 0.85 } });
  assert.deepEqual(unis.filter((x) => x[0].startsWith('uClip')), [['uClipMode', 1], ['uClipPlane', [1, 0, 0, -0.5]]]);
});

test('the Surfaces page: one press from Display, its controls greyed with a reason', () => {
  const base = { ...HM.defaultContext(), kind: 'cube' };
  assert.ok(!HM.pageLayout('display', base).some((w) => w.id === 'iso-page'));
  const iso = { none: '', surfaces: [], edit: 0, volumeHidden: false, max: 4, busy: false };
  const disp = HM.pageLayout('display', { ...base, iso });
  assert.ok(disp.find((w) => w.id === 'iso-page' && !w.disabled));
  assert.ok(HM.PAGES.includes('iso') && HM.tabOf('iso') === 'display');
  let page = HM.pageLayout('iso', { ...base, iso });
  assert.equal(page.find((w) => w.id === 'iso-add').disabled, '');
  assert.match(page.find((w) => w.id === 'iso-level').disabled, /Add a surface/);
  const two = { ...iso, surfaces: [{ colour: 'Orange', shown: true, made: true }, { colour: 'Cyan', shown: false, made: false }], edit: 1 };
  page = HM.pageLayout('iso', { ...base, iso: two });
  assert.ok(page.find((w) => w.id === 'iso-edit:1').on);
  assert.equal(page.find((w) => w.id === 'iso-shown').label, 'Hidden');
  assert.ok(page.find((w) => w.id === 'iso-colour:Cyan').on);
  assert.match(page.find((w) => w.id === 'iso-export:ply').disabled, /not made yet/);
  const four = { ...two, surfaces: Array(4).fill({ colour: 'Lime', shown: true, made: true }), edit: 0 };
  page = HM.pageLayout('iso', { ...base, iso: four });
  assert.match(page.find((w) => w.id === 'iso-add').disabled, /Four surfaces/);
  assert.equal(page.find((w) => w.id === 'iso-export:glb').disabled, '');
  // Every widget inside the page and on its rows.
  for (const w of page) assert.ok(w.rect[0] >= 0 && w.rect[0] + w.rect[2] <= 1024 && w.row <= 5, w.id);
  // With the overlay drawn, the Surfaces button is not greyed.
  const ov = { fields: [], shown: true, drawing: true };
  const withOv = HM.pageLayout('display', { ...base, simulation: true, vars: { names: ['rho', 'T'], current: 'rho' }, ov, iso });
  assert.equal(withOv.find((w) => w.id === 'iso-page').disabled, '');
  assert.ok(withOv.find((w) => w.id === 'render:0').disabled);
});

test('the page side: a level asked for, the task followed, the mesh fetched and packed', async () => {
  const { createIsosurfaces } = await import('../../app/static/vr/js/isosurface-ui.js');
  const { gl, calls } = fakeGL();
  const posts = [];
  const meshBuf = fixture();
  const backend = {
    headers: () => ({}),
    _json: async (method, path, body) => {
      if (method === 'POST') { posts.push(body); return { task_id: 't1', status: 'running' }; }
      if (path.startsWith('/v1/tasks/')) return { status: 'completed', progress: 1, result: { cache_hits: 0, meshes: body ? [] : posts.at(-1).levels.map((level, k) => ({ mesh_id: `m${k}`, level, triangles: 4, raw_triangles: 4 })) } };
      throw new Error(path);
    },
  };
  globalThis.fetch = async (url) => ({ ok: true, status: 200, arrayBuffer: async () => meshBuf.slice(0), url });
  const state = { cube: { lo: -8, hi: -4, scale: 'log', mode: 'mean', bunit: 'g/cm3' }, open: { dataset_id: 'ds' } };
  let changes = 0;
  const ui = createIsosurfaces({ gl, state, backend: () => backend, thrNorm: () => 0.5, cmaxNorm: () => 1, changed: () => changes++, log: () => {}, toast: () => {} });
  ui.onCube();
  assert.equal(ui.menuContext().surfaces.length, 0);
  assert.ok(ui.press('iso-add', { id: 'iso-add' }));
  assert.equal(calls.bufferData.length, 3);         // the buffers with the first surface
  await ui.run();
  assert.deepEqual(posts[0], { dataset_id: 'ds', levels: [1e-6], scale: 'log', lod: -1, max_triangles: ISO.SURFACE_TRIANGLES });
  const s = ui.iso.surfaces[0];
  assert.equal(s.mesh.triangles, 4);
  assert.deepEqual(s.at, { vOff: 0, tOff: 0 });
  assert.ok(ui.menuContext().surfaces[0].made);
  // The level slider is on the cube's (log) scale.
  ui.slide('iso-level', 0.75);
  assert.ok(Math.abs(Math.log10(s.value) + 5) < 1e-12);
  // Surfaces only: the volume hidden while one is shown.
  ui.press('iso-show:surfaces', { id: 'iso-show:surfaces', group: 'iso-show', value: 'surfaces' });
  assert.equal(ui.hidesVolume(), true);
  ui.press('iso-shown', { id: 'iso-shown' });
  assert.equal(ui.hidesVolume(), false);
  // Another cube: its surfaces go.
  state.open = { dataset_id: 'other' };
  ui.onCube();
  assert.equal(ui.iso.surfaces.length, 0);
  assert.ok(changes > 0);
});

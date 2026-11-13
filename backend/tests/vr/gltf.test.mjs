// The page's own reader of binary glTF (the real controller models).
// node --test backend/tests/vr/gltf.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import * as G from '../../app/static/vr/js/gltf.js';

const MODELS = new URL('../../app/static/vr/models/', import.meta.url);
const read = (p) => readFileSync(new URL(p, MODELS));

test('the Touch Plus model reads: six meshes of indexed, textured triangles in a node tree', () => {
  const g = G.parseGLB(read('meta-quest-touch-plus/right.glb'));
  assert.equal(g.json.asset.version, '2.0');
  assert.ok(!g.json.extensionsRequired && !g.json.extensionsUsed, 'no Draco, no meshopt');
  const m = G.readGLTF(g);
  assert.equal(m.meshes.length, 6);
  assert.equal(m.nodes.length, 31);
  assert.deepEqual(m.roots, [30]);
  for (const prim of m.meshes.flat()) {
    const n = prim.positions.length / 3;
    assert.ok(prim.positions instanceof Float32Array && prim.normals.length === 3 * n && prim.uvs.length === 2 * n);
    assert.ok(prim.indices instanceof Uint16Array && prim.indices.length % 3 === 0);
    assert.ok(prim.indices.every((i) => i < n));
    assert.ok(prim.positions.every(Number.isFinite));
  }
  // One material, its base colour an embedded PNG (2048², resized on upload).
  assert.equal(m.materials.length, 1);
  assert.equal(m.materials[0].texture, 0);
  assert.equal(m.images[0].mime, 'image/png');
  const png = m.images[0].bytes;
  assert.deepEqual([...png.subarray(1, 4)], [0x50, 0x4e, 0x47]);
  // Every node has a parent except the root; parents come before children.
  const order = G.nodeOrder(m);
  assert.equal(order.length, 31);
  const at = new Map(order.map((i, k) => [i, k]));
  m.nodes.forEach((n, i) => { if (n.parent >= 0) assert.ok(at.get(n.parent) < at.get(i)); });
});

test('every vendored asset reads, and is the size of a controller', () => {
  for (const id of ['meta-quest-touch-plus-v2', 'meta-quest-touch-plus', 'meta-quest-touch-pro', 'oculus-touch-v3', 'generic-trigger-squeeze-thumbstick']) {
    for (const h of ['left', 'right']) {
      const m = G.readGLTF(G.parseGLB(read(`${id}/${h}.glb`)));
      const worlds = [];
      const I = new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]);
      for (const i of G.nodeOrder(m)) {
        const n = m.nodes[i];
        const local = n.matrix || G.composeTRS(new Float32Array(16), n.t, n.r, n.s);
        worlds[i] = G.mulInto(new Float32Array(16), n.parent >= 0 ? worlds[n.parent] : I, local);
      }
      const { lo, hi } = G.modelBounds(m, worlds);
      const size = hi.map((v, k) => v - lo[k]);
      // A hand-held controller: 5 to 15 cm on its longest side, nothing over 20 cm.
      assert.ok(Math.max(...size) > 0.05 && Math.max(...size) < 0.2, `${id} ${h}: ${size.map((v) => v.toFixed(3))}`);
      assert.ok(lo.every((v) => v > -0.15) && hi.every((v) => v < 0.15), `${id} ${h}: near the grip origin`);
    }
  }
});

/** A minimal .glb: one triangle, the given extra JSON. */
function tinyGLB(extra = {}, { mode, indexType = 5123 } = {}) {
  const pos = new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]);
  const idx = indexType === 5121 ? new Uint8Array([0, 1, 2, 0]) : new Uint16Array([0, 1, 2, 0]);
  const bin = new Uint8Array(36 + 8);
  bin.set(new Uint8Array(pos.buffer), 0);
  bin.set(new Uint8Array(idx.buffer), 36);
  const json = {
    asset: { version: '2.0' },
    buffers: [{ byteLength: bin.length }],
    bufferViews: [{ buffer: 0, byteOffset: 0, byteLength: 36 }, { buffer: 0, byteOffset: 36, byteLength: indexType === 5121 ? 3 : 6 }],
    accessors: [{ bufferView: 0, componentType: 5126, count: 3, type: 'VEC3' }, { bufferView: 1, componentType: indexType, count: 3, type: 'SCALAR' }],
    meshes: [{ primitives: [{ attributes: { POSITION: 0 }, indices: 1, ...(mode !== undefined ? { mode } : {}) }] }],
    nodes: [{ mesh: 0, translation: [1, 2, 3] }],
    scenes: [{ nodes: [0] }],
    ...extra,
  };
  let js = new TextEncoder().encode(JSON.stringify(json));
  const pad = (4 - (js.length % 4)) % 4;
  js = Uint8Array.from([...js, ...Array(pad).fill(0x20)]);
  const total = 12 + 8 + js.length + 8 + bin.length;
  const out = new Uint8Array(total);
  const dv = new DataView(out.buffer);
  dv.setUint32(0, 0x46546c67, true); dv.setUint32(4, 2, true); dv.setUint32(8, total, true);
  dv.setUint32(12, js.length, true); dv.setUint32(16, 0x4e4f534a, true); out.set(js, 20);
  const b = 20 + js.length;
  dv.setUint32(b, bin.length, true); dv.setUint32(b + 4, 0x004e4942, true); out.set(bin, b + 8);
  return out;
}

test('a minimal file reads: positions, 8-bit indices widened, no material', () => {
  const m = G.readGLTF(G.parseGLB(tinyGLB({}, { indexType: 5121 })));
  const p = m.meshes[0][0];
  assert.deepEqual([...p.indices], [0, 1, 2]);
  assert.ok(p.indices instanceof Uint16Array);
  assert.equal(p.normals, null);
  assert.equal(p.material, -1);
  assert.deepEqual(m.nodes[0].t, [1, 2, 3]);
});

test('what the page cannot draw is refused with the reason', () => {
  assert.throws(() => G.parseGLB(new Uint8Array(40)), /not a binary glTF/);
  assert.throws(() => G.readGLTF(G.parseGLB(tinyGLB({ extensionsRequired: ['KHR_draco_mesh_compression'] }))), /KHR_draco_mesh_compression/);
  assert.throws(() => G.readGLTF(G.parseGLB(tinyGLB({ extensionsRequired: ['EXT_meshopt_compression'] }))), /EXT_meshopt_compression/);
  assert.throws(() => G.readGLTF(G.parseGLB(tinyGLB({}, { mode: 1 }))), /only triangles/);
  assert.throws(() => G.readGLTF(G.parseGLB(tinyGLB({ buffers: [{ byteLength: 44, uri: 'x.bin' }] }))), /outside the file/);
  assert.throws(() => G.readGLTF(G.parseGLB(tinyGLB({ nodes: [{ mesh: 0, children: [0] }] }))), /not a tree/);
  // A truncated file.
  const t = tinyGLB();
  assert.throws(() => G.parseGLB(t.subarray(0, t.length - 20)), /past the end|too short/);
});

test('image sizes from the headers, without decoding; the size to decode to', () => {
  const plus = G.readGLTF(G.parseGLB(read('meta-quest-touch-plus/right.glb')));
  assert.deepEqual(G.imageSize(plus.images[0].bytes), { width: 2048, height: 2048 });
  const generic = G.readGLTF(G.parseGLB(read('generic-trigger-squeeze-thumbstick/left.glb')));
  for (const im of generic.images) assert.deepEqual(G.imageSize(im.bytes), { width: 1024, height: 1024 }, im.mime);
  assert.equal(G.imageSize(new Uint8Array([1, 2, 3, 4])), null);
  assert.deepEqual(G.fitSize({ width: 2048, height: 2048 }, 1024), { width: 1024, height: 1024 });
  assert.deepEqual(G.fitSize({ width: 2048, height: 512 }, 1024), { width: 1024, height: 256 });
  assert.deepEqual(G.fitSize({ width: 512, height: 512 }, 1024), { width: 512, height: 512 });
});

test('node transforms: TRS composition, multiplication, slerp and lerp in place', () => {
  const s = Math.SQRT1_2;
  const m = G.composeTRS(new Float32Array(16), [1, 2, 3], [0, 0, s, s], [2, 2, 2]);   // 90° about z, scale 2
  const p = [m[0] * 1 + m[12], m[1] * 1 + m[13], m[2] * 1 + m[14]];                    // (1,0,0) → (0,2,0) + t
  assert.ok(Math.abs(p[0] - 1) < 1e-6 && Math.abs(p[1] - 4) < 1e-6 && Math.abs(p[2] - 3) < 1e-6);
  const I = G.composeTRS(new Float32Array(16), [0, 0, 0], [0, 0, 0, 1], [1, 1, 1]);
  assert.deepEqual([...G.mulInto(new Float32Array(16), I, m)], [...m]);
  const q = G.slerpInto([0, 0, 0, 0], [0, 0, 0, 1], [0, 0, s, s], 0.5);                 // 45° about z
  assert.ok(Math.abs(q[2] - Math.sin(Math.PI / 8)) < 1e-6 && Math.abs(q[3] - Math.cos(Math.PI / 8)) < 1e-6);
  // The shortest path: −q is the same rotation.
  const q2 = G.slerpInto([0, 0, 0, 0], [0, 0, 0, 1], [0, 0, -s, -s], 0.5);
  assert.ok(Math.abs(Math.abs(q2[2]) - Math.sin(Math.PI / 8)) < 1e-6);
  assert.deepEqual(G.lerpInto([0, 0, 0], [0, 0, 0], [2, 4, 6], 0.25), [0.5, 1, 1.5]);
});

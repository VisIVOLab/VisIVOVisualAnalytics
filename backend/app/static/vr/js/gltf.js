// A small reader of binary glTF 2.0 (.glb) files, for the controller models
// of the WebXR Input Profiles project (backend/app/static/vr/models/). The
// page has no third-party library, so this reads what those assets use and
// nothing more: one JSON chunk and one BIN chunk, meshes of indexed
// triangles (POSITION, NORMAL, TEXCOORD_0 as floats), a node tree with
// translation/rotation/scale or a matrix, and a material's base colour
// (factor and an embedded PNG or JPEG texture). A file that needs an
// extension (Draco or meshopt compression among them), points outside the
// file, or draws anything but triangles is refused with the reason, and the
// page falls back to its own model.
//
// Pure (no DOM, no GL): unit-tested in backend/tests/vr/gltf.test.mjs on the
// vendored files. The node-transform helpers work in place on Float32Arrays
// so the frame loop allocates nothing (controller-profiles.js).

const GLB_MAGIC = 0x46546c67;      // 'glTF'
const CHUNK_JSON = 0x4e4f534a;     // 'JSON'
const CHUNK_BIN = 0x004e4942;      // 'BIN\0'
const FLOAT = 5126;
const COMPONENTS = { SCALAR: 1, VEC2: 2, VEC3: 3, VEC4: 4, MAT4: 16 };
const INDEX_TYPES = { 5121: Uint8Array, 5123: Uint16Array, 5125: Uint32Array };

/** The JSON and the binary chunk of a .glb (ArrayBuffer or a typed view). */
export function parseGLB(input) {
  const buf = input instanceof ArrayBuffer ? input : input.buffer.slice(input.byteOffset, input.byteOffset + input.byteLength);
  if (buf.byteLength < 20) throw new Error('glb: file too short');
  const dv = new DataView(buf);
  if (dv.getUint32(0, true) !== GLB_MAGIC) throw new Error('glb: not a binary glTF file');
  const version = dv.getUint32(4, true);
  if (version !== 2) throw new Error(`glb: version ${version}, only 2 is read`);
  const total = Math.min(dv.getUint32(8, true), buf.byteLength);
  let off = 12, json = null, bin = null;
  while (off + 8 <= total) {
    const len = dv.getUint32(off, true), type = dv.getUint32(off + 4, true);
    const start = off + 8;
    if (start + len > total) throw new Error('glb: a chunk runs past the end of the file');
    if (type === CHUNK_JSON && !json) json = JSON.parse(new TextDecoder().decode(new Uint8Array(buf, start, len)));
    else if (type === CHUNK_BIN && !bin) bin = new Uint8Array(buf, start, len);
    off = start + len + ((4 - (len % 4)) % 4);
  }
  if (!json) throw new Error('glb: no JSON chunk');
  return { json, bin: bin || new Uint8Array(0) };
}

/** The bytes of a buffer view (only the GLB's own buffer 0). */
function viewBytes(json, bin, index) {
  const bv = json.bufferViews && json.bufferViews[index];
  if (!bv) throw new Error(`glb: no buffer view ${index}`);
  if ((bv.buffer || 0) !== 0 || (json.buffers && json.buffers[0] && json.buffers[0].uri)) {
    throw new Error('glb: data outside the file is not read');
  }
  const start = bv.byteOffset || 0;
  if (start + bv.byteLength > bin.byteLength) throw new Error('glb: a buffer view runs past the binary chunk');
  return { bytes: bin.subarray(start, start + bv.byteLength), stride: bv.byteStride || 0 };
}

/** An accessor as a packed typed array (floats for attributes, integers for indices). */
function readAccessor(json, bin, index, { indices = false } = {}) {
  const acc = json.accessors && json.accessors[index];
  if (!acc) throw new Error(`glb: no accessor ${index}`);
  if (acc.sparse) throw new Error('glb: sparse accessors are not read');
  if (acc.bufferView === undefined) throw new Error('glb: an accessor without data');
  const n = COMPONENTS[acc.type];
  if (!n) throw new Error(`glb: accessor type ${acc.type}`);
  const { bytes, stride } = viewBytes(json, bin, acc.bufferView);
  const base = acc.byteOffset || 0;
  let Type;
  if (indices) {
    Type = INDEX_TYPES[acc.componentType];
    if (!Type || n !== 1) throw new Error(`glb: index type ${acc.componentType}`);
  } else {
    if (acc.componentType !== FLOAT) throw new Error(`glb: attribute component type ${acc.componentType} (only floats are read)`);
    Type = Float32Array;
  }
  const size = Type.BYTES_PER_ELEMENT;
  const elem = n * size;
  const step = stride || elem;
  if (acc.count > 0 && base + (acc.count - 1) * step + elem > bytes.byteLength) throw new Error('glb: an accessor runs past its buffer view');
  const out = new Type(acc.count * n);
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  for (let i = 0; i < acc.count; i++) {
    const o = base + i * step;
    for (let k = 0; k < n; k++) {
      const p = o + k * size;
      out[i * n + k] = size === 4 ? (indices ? dv.getUint32(p, true) : dv.getFloat32(p, true))
        : size === 2 ? dv.getUint16(p, true) : dv.getUint8(p);
    }
  }
  return out;
}

/**
 * A glTF document as plain arrays: {nodes, roots, meshes, materials, images}.
 * nodes[i] = {name, mesh (index or -1), children, parent, t, r, s, matrix
 * (a 16-float local matrix or null)}; meshes[i] = [{positions, normals, uvs,
 * indices (Uint16Array or Uint32Array), material}]; materials[i] =
 * {baseColor [r,g,b,a], texture (image index or -1), doubleSided};
 * images[i] = {mime, bytes}. Throws, with the reason, on what it cannot draw.
 */
export function readGLTF({ json, bin }) {
  const req = json.extensionsRequired || [];
  if (req.length) throw new Error(`glb: needs ${req.join(', ')}`);
  const images = (json.images || []).map((im) => {
    if (im.bufferView === undefined) throw new Error('glb: an image outside the file');
    return { mime: im.mimeType || 'image/png', bytes: viewBytes(json, bin, im.bufferView).bytes };
  });
  const texImage = (ti) => {
    if (!ti) return -1;
    const tex = json.textures && json.textures[ti.index];
    if (!tex || tex.source === undefined || !images[tex.source]) return -1;
    if ((ti.texCoord || 0) !== 0) return -1;
    return tex.source;
  };
  const materials = (json.materials || []).map((m) => {
    const pbr = m.pbrMetallicRoughness || {};
    const f = pbr.baseColorFactor || [1, 1, 1, 1];
    return { baseColor: [f[0], f[1], f[2], f[3]], texture: texImage(pbr.baseColorTexture), doubleSided: !!m.doubleSided };
  });
  const meshes = (json.meshes || []).map((mesh) => mesh.primitives.map((p) => {
    if ((p.mode === undefined ? 4 : p.mode) !== 4) throw new Error(`glb: primitive mode ${p.mode} (only triangles)`);
    if (p.extensions && Object.keys(p.extensions).length) throw new Error(`glb: primitive extension ${Object.keys(p.extensions).join(', ')}`);
    const a = p.attributes || {};
    if (a.POSITION === undefined) throw new Error('glb: a primitive without positions');
    const positions = readAccessor(json, bin, a.POSITION);
    const count = positions.length / 3;
    const normals = a.NORMAL !== undefined ? readAccessor(json, bin, a.NORMAL) : null;
    const uvs = a.TEXCOORD_0 !== undefined ? readAccessor(json, bin, a.TEXCOORD_0) : null;
    if (normals && normals.length !== count * 3) throw new Error('glb: normals and positions differ in count');
    if (uvs && uvs.length !== count * 2) throw new Error('glb: texture coordinates and positions differ in count');
    let indices;
    if (p.indices !== undefined) {
      const raw = readAccessor(json, bin, p.indices, { indices: true });
      indices = raw instanceof Uint32Array && count <= 65536 ? Uint16Array.from(raw)
        : raw instanceof Uint8Array ? Uint16Array.from(raw) : raw;
    } else {
      indices = count <= 65536 ? new Uint16Array(count) : new Uint32Array(count);
      for (let i = 0; i < count; i++) indices[i] = i;
    }
    for (let i = 0; i < indices.length; i++) if (indices[i] >= count) throw new Error('glb: an index past the vertices');
    if (indices.length % 3) throw new Error('glb: a triangle list whose length is not a multiple of 3');
    return { positions, normals, uvs, indices, material: p.material === undefined ? -1 : p.material };
  }));
  const nodes = (json.nodes || []).map((n) => ({
    name: n.name || '',
    mesh: n.mesh === undefined ? -1 : n.mesh,
    children: n.children ? n.children.slice() : [],
    parent: -1,
    t: n.translation ? n.translation.slice() : [0, 0, 0],
    r: n.rotation ? n.rotation.slice() : [0, 0, 0, 1],
    s: n.scale ? n.scale.slice() : [1, 1, 1],
    matrix: n.matrix ? new Float32Array(n.matrix) : null,
  }));
  for (let i = 0; i < nodes.length; i++) {
    for (const c of nodes[i].children) {
      if (!nodes[c] || nodes[c].parent !== -1 || c === i) throw new Error('glb: the nodes are not a tree');
      nodes[c].parent = i;
    }
  }
  for (const n of nodes) if (n.mesh >= meshes.length) throw new Error('glb: a node names a missing mesh');
  const scene = json.scenes && json.scenes[json.scene || 0];
  const roots = scene && scene.nodes ? scene.nodes.slice() : nodes.map((n, i) => i).filter((i) => nodes[i].parent === -1);
  return { nodes, roots, meshes, materials, images };
}

/** The nodes in an order where every parent comes before its children (the scene's only). */
export function nodeOrder(model) {
  const order = [];
  const seen = new Set();
  const walk = (i, depth) => {
    if (seen.has(i) || depth > 64) return;
    seen.add(i); order.push(i);
    for (const c of model.nodes[i].children) walk(c, depth + 1);
  };
  for (const r of model.roots) walk(r, 0);
  return order;
}

/**
 * The width and height of an embedded PNG or JPEG from its header, without
 * decoding it ({width, height} or null), so the page can decode it straight
 * to the size it will upload.
 */
export function imageSize(bytes) {
  const b = bytes;
  if (b.length >= 24 && b[0] === 0x89 && b[1] === 0x50 && b[2] === 0x4e && b[3] === 0x47) {
    const dv = new DataView(b.buffer, b.byteOffset, b.byteLength);
    return { width: dv.getUint32(16), height: dv.getUint32(20) };
  }
  if (b.length >= 4 && b[0] === 0xff && b[1] === 0xd8) {
    let k = 2;
    while (k + 9 < b.length) {
      if (b[k] !== 0xff) { k++; continue; }
      const mk = b[k + 1];
      if (mk === 0xff) { k++; continue; }                       // fill byte
      if (mk === 0xd8 || mk === 0x01 || (mk >= 0xd0 && mk <= 0xd7)) { k += 2; continue; }   // no length
      // A start of frame (baseline, progressive, ...; not DHT C4, JPG C8, DAC CC).
      if (mk >= 0xc0 && mk <= 0xcf && mk !== 0xc4 && mk !== 0xc8 && mk !== 0xcc) {
        return { width: (b[k + 7] << 8) | b[k + 8], height: (b[k + 5] << 8) | b[k + 6] };
      }
      k += 2 + ((b[k + 2] << 8) | b[k + 3]);
    }
  }
  return null;
}

/** The size to decode an image of `size` to, at most `max` a side, aspect kept. */
export function fitSize(size, max) {
  const s = Math.min(1, max / Math.max(size.width, size.height));
  return { width: Math.max(1, Math.round(size.width * s)), height: Math.max(1, Math.round(size.height * s)) };
}

/** out = the matrix of translation t, rotation quaternion q (x,y,z,w), scale s (column-major). */
export function composeTRS(out, t, q, s) {
  const [x, y, z, w] = q;
  const x2 = x + x, y2 = y + y, z2 = z + z;
  const xx = x * x2, xy = x * y2, xz = x * z2, yy = y * y2, yz = y * z2, zz = z * z2;
  const wx = w * x2, wy = w * y2, wz = w * z2;
  out[0] = (1 - (yy + zz)) * s[0]; out[1] = (xy + wz) * s[0]; out[2] = (xz - wy) * s[0]; out[3] = 0;
  out[4] = (xy - wz) * s[1]; out[5] = (1 - (xx + zz)) * s[1]; out[6] = (yz + wx) * s[1]; out[7] = 0;
  out[8] = (xz + wy) * s[2]; out[9] = (yz - wx) * s[2]; out[10] = (1 - (xx + yy)) * s[2]; out[11] = 0;
  out[12] = t[0]; out[13] = t[1]; out[14] = t[2]; out[15] = 1;
  return out;
}

/** out = a × b (column-major 4×4); out may not alias a or b. */
export function mulInto(out, a, b) {
  for (let c = 0; c < 4; c++) {
    const b0 = b[c * 4], b1 = b[c * 4 + 1], b2 = b[c * 4 + 2], b3 = b[c * 4 + 3];
    out[c * 4] = a[0] * b0 + a[4] * b1 + a[8] * b2 + a[12] * b3;
    out[c * 4 + 1] = a[1] * b0 + a[5] * b1 + a[9] * b2 + a[13] * b3;
    out[c * 4 + 2] = a[2] * b0 + a[6] * b1 + a[10] * b2 + a[14] * b3;
    out[c * 4 + 3] = a[3] * b0 + a[7] * b1 + a[11] * b2 + a[15] * b3;
  }
  return out;
}

/** out = the shortest-path spherical interpolation of unit quaternions a → b at t. */
export function slerpInto(out, a, b, t) {
  let bx = b[0], by = b[1], bz = b[2], bw = b[3];
  let cos = a[0] * bx + a[1] * by + a[2] * bz + a[3] * bw;
  if (cos < 0) { cos = -cos; bx = -bx; by = -by; bz = -bz; bw = -bw; }
  let k0, k1;
  if (cos > 0.9995) { k0 = 1 - t; k1 = t; } else {
    const th = Math.acos(Math.min(1, cos)), sn = Math.sin(th);
    k0 = Math.sin((1 - t) * th) / sn; k1 = Math.sin(t * th) / sn;
  }
  const x = k0 * a[0] + k1 * bx, y = k0 * a[1] + k1 * by, z = k0 * a[2] + k1 * bz, w = k0 * a[3] + k1 * bw;
  const n = Math.hypot(x, y, z, w) || 1;
  out[0] = x / n; out[1] = y / n; out[2] = z / n; out[3] = w / n;
  return out;
}

/** out = a + (b − a)·t, component-wise over a.length. */
export function lerpInto(out, a, b, t) {
  for (let i = 0; i < a.length; i++) out[i] = a[i] + (b[i] - a[i]) * t;
  return out;
}

/** The axis-aligned bounds of a model's triangles in its root frame (for tests and checks). */
export function modelBounds(model, worlds) {
  const lo = [Infinity, Infinity, Infinity], hi = [-Infinity, -Infinity, -Infinity];
  model.nodes.forEach((n, i) => {
    if (n.mesh < 0) return;
    const m = worlds[i];
    for (const prim of model.meshes[n.mesh]) {
      const p = prim.positions;
      for (let v = 0; v < p.length; v += 3) {
        const x = p[v], y = p[v + 1], z = p[v + 2];
        const q = [m[0] * x + m[4] * y + m[8] * z + m[12], m[1] * x + m[5] * y + m[9] * z + m[13], m[2] * x + m[6] * y + m[10] * z + m[14]];
        for (let k = 0; k < 3; k++) { lo[k] = Math.min(lo[k], q[k]); hi[k] = Math.max(hi[k], q[k]); }
      }
    }
  });
  return { lo, hi };
}

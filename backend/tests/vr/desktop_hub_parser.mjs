// What the desktop reads of the headset's activity (issue #238,
// src/app/VrHeadsetHub.h), mirrored field by field so the page's payloads
// (issue #237) are checked against the same rules: in node's tests
// (headset.test.mjs) and in the browser check (validation/vr_headset_hub.mjs)
// on what the hub actually relayed. Keep in step with VrHeadsetHub.h:
// parseProduct, openIn, parseLook, parseClip, cubeKey, aboutCube.

const str = (v) => (typeof v === 'string' ? v : '');
const obj = (v) => (v && typeof v === 'object' && !Array.isArray(v) ? v : {});

/** VrHub::parseProduct: what the desktop takes from a product event. */
export function parseProduct(e) {
  const p = obj(e.payload);
  const out = {};
  out.kind = str(p.product) || str(p.type);
  out.path = str(p.output_path) || str(p.path);
  out.workspaceFilename = str(p.file) || str(p.workspace_filename);
  if (!out.workspaceFilename && out.path) out.workspaceFilename = out.path.split('/').pop();
  out.datasetId = str(p.dataset_id);
  out.request = obj(p.request);
  out.label = str(p.label);
  const recipe = obj(p.recipe);
  const rp = obj(recipe.product);
  out.recipeId = str(rp.id);
  out.recipeKind = str(rp.kind);
  if (!out.label) out.label = str(rp.label);
  out.replay = (Array.isArray(rp.replay) ? rp.replay : []).filter((s) => str(obj(s).route));
  out.caveat = str(rp.replay_caveat);
  out.parameters = obj(recipe.parameters);
  out.origin = obj(recipe.origin);
  out.values = Array.isArray(p.values) ? p.values : [];
  if (!out.label) out.label = str(e.summary);
  return out;
}

/** VrHub::openIn: where the desktop opens it ('Nothing' when it cannot). */
export function openIn(p) {
  const hasFile = !!p.path;
  if (p.kind === 'moment') return hasFile ? 'ImageViewer' : 'Nothing';
  if (p.kind === 'pv') return hasFile || (Array.isArray(p.request.vertices) && p.request.vertices.length >= 2) ? 'PvViewer' : 'Nothing';
  if (p.kind === 'spectrum') return hasFile || p.values.length ? 'SpectrumPlot' : 'Nothing';
  if (p.kind === 'isolate' || p.kind === 'remove') return hasFile ? 'CubeWindow' : 'Nothing';
  if (p.kind === 'catalogue' || p.kind === 'flow') return hasFile ? 'File' : 'Nothing';
  return 'Nothing';
}

/** VrHub::parseLook: a number only when it is a finite JSON number. */
export function parseLook(payload) {
  const p = obj(payload);
  const num = (...keys) => {
    for (const k of keys) if (typeof p[k] === 'number' && Number.isFinite(p[k])) return p[k];
    return null;
  };
  return {
    colormap: str(p.colormap), threshold: num('threshold'), colourMax: num('colour_max', 'color_max'),
    opacity: num('opacity'), gamma: num('gamma'),
    mode: typeof p.mode === 'number' ? String(Math.trunc(p.mode)) : str(p.mode), stretch: str(p.stretch),
  };
}

/** VrHub::parseClip: payload.box (or the payload) as [[x0, x1], [y0, y1], [z0, z1]],
 *  [x0, x1, y0, y1, z0, z1] or {min, max}; anything else (null) is no clip. */
export function parseClip(payload) {
  const p = obj(payload);
  const box = 'box' in p ? p.box : p;
  const none = { set: false };
  const num = (v) => typeof v === 'number';
  if (box && !Array.isArray(box) && typeof box === 'object') {
    const lo = box.min, hi = box.max;
    if (!Array.isArray(lo) || !Array.isArray(hi) || lo.length !== 3 || hi.length !== 3) return none;
    if (![...lo, ...hi].every(num)) return none;
    return { set: true, min: lo.map((v, i) => Math.min(v, hi[i])), max: lo.map((v, i) => Math.max(v, hi[i])) };
  }
  if (!Array.isArray(box)) return none;
  if (box.length === 6 && box.every(num)) {
    return { set: true, min: [0, 1, 2].map((i) => Math.min(box[2 * i], box[2 * i + 1])), max: [0, 1, 2].map((i) => Math.max(box[2 * i], box[2 * i + 1])) };
  }
  if (box.length === 3 && box.every((r) => Array.isArray(r) && r.length === 2 && r.every(num))) {
    return { set: true, min: box.map((r) => Math.min(...r)), max: box.map((r) => Math.max(...r)) };
  }
  return none;
}

/** VrHub::cubeKey and aboutCube: which cube an event is about. */
export function cubeKey(e) {
  if (str(e.cube_path)) return e.cube_path;
  const id = str(obj(e.payload).dataset_id);
  return id ? `dataset:${id}` : '';
}
export function aboutCube(e, path, datasetId) {
  if (str(e.cube_path)) return e.cube_path === path;
  const id = str(obj(e.payload).dataset_id);
  return !!id && id === datasetId;
}

/** VrHub::parseSpectrumCsv: the first two numeric columns, a header and '#' lines skipped. */
export function parseSpectrumCsv(text) {
  const axis = [], values = [];
  let header = null;
  for (const raw of String(text).split('\n')) {
    const line = raw.trim();
    if (!line || line.startsWith('#')) continue;
    const cells = line.includes(',') ? line.split(',') : line.split(/\s+/);
    if (cells.length < 2) continue;
    const x = Number(cells[0].trim());
    const yc = cells[1].trim();
    let y = Number(yc);
    if (yc === '' || /^nan$/i.test(yc)) y = NaN;
    if (cells[0].trim() === '' || !Number.isFinite(x) || (Number.isNaN(y) && !(yc === '' || /^nan$/i.test(yc)))) {
      if (!header && !axis.length) header = [cells[0].trim(), cells[1].trim()];
      continue;
    }
    axis.push(x);
    values.push(y);
  }
  return { ok: values.length >= 2, axis, values, header };
}

// Catalogue sources over a cube in the headset (issue #190): VLKB compact
// sources and filaments, a cross-match, or a catalogue file, placed in the
// cube's pixels by the backend with the cube's full WCS (/v1/cube/
// catalogue_pixels, app/cube_catalogue.py: CAR, TAN... in the file's own
// frame). Here a pixel only goes to the box: pixel i spans [i - 0.5, i + 0.5],
// box coordinate (i + 0.5) / n (selection.js fullPixelToBox), model
// coordinate (u - 0.5) x extent. Pure functions: backend/tests/vr/layers.test.mjs.

/** Colours of the VLKB bands (the desktop's vlkbBandColor). */
export const BAND_COLOURS = {
  ft: [1, 1, 1], 21: [0.8, 0.3, 0.9], 22: [0.6, 0.35, 0.95], 24: [0.95, 0.35, 0.65],
  70: [1, 0.3, 0.3], 160: [1, 0.6, 0.1], 250: [1, 0.92, 0.25], 350: [0.35, 0.95, 0.45],
  500: [0.45, 0.65, 1], 870: [0.2, 0.85, 0.85], 1100: [0.95, 0.45, 0.95],
};
const MARKER = [1, 0.85, 0.3];
const FILAMENT = [0.4, 1, 0.85];

/** Model coordinate of a pixel coordinate along axis i (0-based pixel, pixel centre at integers). */
export function pixelToModel(px, n, extent) {
  return ((px + 0.5) / n - 0.5) * extent;
}

/** The request for a source kind (the panel's choice). */
export function overlayRequest(datasetId, choice) {
  const r = { dataset_id: datasetId, source: choice.source, max_sources: choice.maxSources || 2000 };
  if (choice.source === 'catalogue') r.catalogue_id = choice.catalogueId;
  if (choice.source === 'vlkb_sources') r.bands = choice.bands && choice.bands.length ? choice.bands : ['ft'];
  if (choice.source === 'crossmatch') {
    r.crossmatch_catalogue = choice.catalogue || 'simbad';
    r.radius_arcsec = choice.radiusArcsec ?? 60;
  }
  return r;
}

/**
 * Segments and labels of an overlay (catalogue_pixels' answer) in the cube's
 * model space. `shape` [nx, ny, nz] full pixels, `extent` the box sides, `clip`
 * {min, max} box fractions (sources outside it are left out). A source with a
 * channel is a cross of three strokes at its voxel, with its ellipse (if any)
 * in that channel's plane; one without is its ellipse (or a cross) on the back
 * face of the clipping box and a line through the whole spectral range, so it
 * reads at every velocity. Contours lie on the back face.
 */
export function overlaySegments(ov, shape, extent, clip, opts = {}) {
  const segs = [], labels = [];
  if (!ov || !ov.valid) return { segs, labels, drawn: 0 };
  const [nx, ny, nz] = shape;
  const mx = (x) => pixelToModel(x, nx, extent[0]);
  const my = (y) => pixelToModel(y, ny, extent[1]);
  const zBack = (clip.min[2] - 0.5) * extent[2], zFront = (clip.max[2] - 0.5) * extent[2];
  const inClip = (x, y, z) => {
    const u = [(x + 0.5) / nx, (y + 0.5) / ny];
    if (u[0] < clip.min[0] || u[0] > clip.max[0] || u[1] < clip.min[1] || u[1] > clip.max[1]) return false;
    if (z === null || z === undefined) return true;
    const w = (z + 0.5) / nz;
    return w >= clip.min[2] && w <= clip.max[2];
  };
  const size = 0.012 * Math.max(...extent);
  const alpha = opts.alpha ?? 0.95;
  let drawn = 0;
  for (const s of ov.sources || []) {
    if (!inClip(s.x, s.y, s.z)) continue;
    const rgb = s.band ? (BAND_COLOURS[s.band] || MARKER) : MARKER;
    const col = [...rgb, alpha];
    const X = mx(s.x), Y = my(s.y);
    const Z = s.z === null || s.z === undefined ? null : pixelToModel(s.z, nz, extent[2]);
    const plane = Z === null ? zBack : Z;
    if (s.ellipse && s.ellipse.length > 1) {
      for (let k = 0; k + 1 < s.ellipse.length; k++) {
        const a = s.ellipse[k], b = s.ellipse[k + 1];
        segs.push({ a: [mx(a[0]), my(a[1]), plane], b: [mx(b[0]), my(b[1]), plane], color: col });
      }
    }
    if (Z === null) {
      if (!s.ellipse) {
        segs.push({ a: [X - size, Y, zBack], b: [X + size, Y, zBack], color: col });
        segs.push({ a: [X, Y - size, zBack], b: [X, Y + size, zBack], color: col });
      }
      if (opts.pins !== false) segs.push({ a: [X, Y, zBack], b: [X, Y, zFront], color: [...rgb, alpha * 0.45] });
    } else {
      segs.push({ a: [X - size, Y, Z], b: [X + size, Y, Z], color: col });
      segs.push({ a: [X, Y - size, Z], b: [X, Y + size, Z], color: col });
      segs.push({ a: [X, Y, Z - size], b: [X, Y, Z + size], color: col });
    }
    if (opts.labels && labels.length < (opts.maxLabels || 40) && s.label) {
      labels.push({ pos: [X + 1.3 * size, Y + 1.3 * size, plane], text: s.label, height: 0.022,
        anchor: 'left', color: `rgb(${rgb.map((c) => Math.round(255 * c)).join(',')})` });
    }
    drawn++;
  }
  for (const p of ov.polylines || []) {
    const col = [...FILAMENT, alpha];
    let any = false;
    for (let k = 0; k + 1 < p.points.length; k++) {
      const a = p.points[k], b = p.points[k + 1];
      if (!inClip(a[0], a[1], null) && !inClip(b[0], b[1], null)) continue;
      segs.push({ a: [mx(a[0]), my(a[1]), zBack], b: [mx(b[0]), my(b[1]), zBack], color: col });
      any = true;
    }
    if (any) drawn++;
  }
  return { segs, labels, drawn };
}

/** One line of the panel and HUD about an overlay. */
export function overlaySummary(ov) {
  if (!ov) return '';
  if (!ov.valid) return `catalogue: ${ov.error || 'failed'}`;
  const parts = [`${ov.name || ov.source}: ${ov.n_sources} sources`];
  if (ov.n_polylines) parts.push(`${ov.n_polylines} contours`);
  if (ov.n_outside) parts.push(`${ov.n_outside} outside the cube`);
  if (ov.spectral && ov.spectral.used) parts.push(`placed in velocity by ${ov.spectral.note}`);
  else parts.push('on the sky only (through every channel)');
  return parts.join(', ');
}

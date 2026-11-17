// Isosurfaces of the cube (issue #370), run by vr_lifecycle.mjs --isosurface
// on the smoke's cube (a Gaussian blob over noise): a surface added at the
// threshold is extracted by the backend (a task) and drawn; its draw covers
// pixels and allocates no GPU buffer after the first surface; moving its
// level asks again, and the same level comes back from the cache; "Surfaces
// only" hides the volume; the mesh is exported as PLY. Cheap: one small cube,
// a few requests.

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

export async function runIsosurfaceChecks({ c, check, waitFor, api, token, results = {} }) {
  const L = 'isosurface';
  const made = (i) => `(() => { const s = window.__vrIso.iso.surfaces[${i}]; return !!(s && s.mesh && !s.pending && !window.__vrIso.iso.running); })()`;
  const t0 = Date.now();
  await c.evaluate('window.__vrIso.add()');
  const first = await waitFor(c, made(0), 30000);
  check(`${L}: a surface at the threshold extracted and fetched`, !!first.value, `${first.ms} ms`);
  if (!first.value) return results;
  const s0 = await c.evaluate(`(() => { const s = window.__vrIso.iso.surfaces[0]; return { value: s.value, made: s.made, err: s.error, bytes: window.__vrIso.gl.bytes }; })()`);
  check(`${L}: the surface has triangles, within the budget`, s0.made.triangles > 100 && s0.made.triangles <= 150000 && !s0.err,
    JSON.stringify({ triangles: s0.made.triangles, raw: s0.made.raw_triangles, lod: s0.made.lod, source: s0.made.source }));
  const px = await c.evaluate('window.__vrIsoPixels({ frames: 10 })');
  check(`${L}: the surface is drawn (pixels covered)`, px.fraction > 0.002 && px.draws === 1 && px.triangles === s0.made.triangles,
    `${(100 * px.fraction).toFixed(2)} % of ${px.w}×${px.h}, ${px.ms.toFixed(2)} ms a draw`);
  results.firstMs = first.ms;
  results.drawMs = +px.ms.toFixed(3);
  results.triangles = s0.made.triangles;

  // A second surface: no buffer allocated again (bufferData counted by the
  // page's own GL, not wrapped here: the buffers' bytes stay the same).
  await c.evaluate('window.__vrIso.add()');
  const second = await waitFor(c, made(1), 30000);
  const s1 = await c.evaluate(`({ bytes: window.__vrIso.gl.bytes, n: window.__vrIso.iso.surfaces.length, at: window.__vrIso.iso.surfaces[1].at })`);
  check(`${L}: a second surface packed after the first, the buffers unchanged`, !!second.value && s1.n === 2 && s1.bytes === s0.bytes && s1.at && s1.at.vOff > 0,
    JSON.stringify(s1));

  // The level moved and moved back: the first answer comes from the cache.
  await c.evaluate(`window.__vrIso.setLevel(0, ${s0.value} * 1.5)`);
  await sleep(100);
  await waitFor(c, made(0), 30000);
  const t1 = Date.now();
  await c.evaluate(`window.__vrIso.setLevel(0, ${s0.value})`);
  await sleep(50);
  await waitFor(c, made(0), 30000);
  const back = await c.evaluate(`window.__vrIso.iso.surfaces[0].made`);
  check(`${L}: the same level again is the same mesh, from the cache`, back.mesh_id === s0.made.mesh_id, `${Date.now() - t1} ms`);

  // Surfaces only: the volume left out of the scene.
  await c.evaluate(`window.__vrIso.press('iso-show:surfaces', { id: 'iso-show:surfaces', group: 'iso-show', value: 'surfaces' })`);
  check(`${L}: Surfaces only hides the volume`, await c.evaluate('window.__vrIso.hidesVolume()'));
  await c.evaluate(`window.__vrIso.press('iso-show:volume', { id: 'iso-show:volume', group: 'iso-show', value: 'volume' })`);

  // The export, through the API (the page saves it in the exports).
  const ply = await api('GET', `/v1/isosurface/mesh/${s0.made.mesh_id}/export`, token);
  check(`${L}: the surface exported as PLY`, ply.status === 200 && ply.body.startsWith('ply\n'), `${ply.status}, ${ply.body.length} chars`);
  const errs = await c.evaluate(`window.__vrShell.diag.items.filter((e) => e.event === 'isosurface-failed').map((e) => e.message)`);
  check(`${L}: no extraction failed`, !errs.length, errs.join(' / '));
  // Left as found, for the checks that follow on this page.
  await c.evaluate(`(() => { const u = window.__vrIso; while (u.iso.surfaces.length) u.remove(0); })()`);
  results.totalMs = Date.now() - t0;
  return results;
}

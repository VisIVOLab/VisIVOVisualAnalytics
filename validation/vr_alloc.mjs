// GPU allocations under fast repeated selections (issue #226, point 8), run
// by vr_lifecycle.mjs --regressions (and --alloc alone) on the desktop page.
//
// The Quest lost its WebGL context after about ten Kinematic Lasso picks and
// spectra in about five seconds, then a mask statistics request. This drives
// the same pattern through the page's own click path (several bursts) and
// measures, after each burst:
//   - the page's live WebGL objects and their bytes, counted by a wrapper on
//     WebGL2RenderingContext installed before the page's scripts (GL_PROBE),
//     so the count does not depend on the page's own bookkeeping;
//   - the bytes uploaded into textures (re-specified storage included);
//   - the 2-D canvases the page made, and the JS heap;
//   - the GPU process's memory footprint, read from the host (macOS
//     `footprint`, else `ps` RSS), when the caller gives Chrome's pid.
// The check: after the first burst, further bursts do not add live WebGL
// objects or texture bytes (a bound, not a slow leak).
import { spawnSync } from 'node:child_process';

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/** Installed with Page.addScriptToEvaluateOnNewDocument (before the page's
 *  modules run): counts WebGL objects, their bytes, and texture uploads. */
export const GL_PROBE = `(() => {
  if (window.__glProbe || typeof WebGL2RenderingContext === 'undefined') return;
  const P = WebGL2RenderingContext.prototype;
  const live = new Map();          // object -> {kind, bytes}
  const stats = { uploads: 0, uploadBytes: 0, respecs: 0, respecBytes: 0, canvases: 0, made: { texture: 0, buffer: 0, renderbuffer: 0, framebuffer: 0 } };
  const unit = new WeakMap();      // gl -> active unit
  const boundTex = new WeakMap();  // gl -> Map(unit:target -> texture)
  const boundBuf = new WeakMap();  // gl -> Map(target -> buffer)
  const bpp = (gl, f, type) => {
    switch (f) {
      case gl.R8: case gl.ALPHA: case gl.LUMINANCE: return 1;
      case gl.R16F: case gl.RG8: return 2;
      case gl.R32F: case gl.RG16F: case gl.RGBA8: case gl.RGBA: case gl.SRGB8_ALPHA8: return 4;
      case gl.RGBA16F: return 8;
      case gl.RGBA32F: return 16;
      default: return 4;
    }
  };
  const wrap = (name, after) => {
    const f = P[name];
    if (!f) return;
    P[name] = function (...a) { const r = f.apply(this, a); try { after.call(this, r, a); } catch (e) { /* counting only */ } return r; };
  };
  const texOf = (gl, target) => (boundTex.get(gl) || new Map()).get((unit.get(gl) || 0) + ':' + target);
  wrap('createTexture', function (t) { if (t) { live.set(t, { kind: 'texture', bytes: 0 }); stats.made.texture++; } });
  wrap('createBuffer', function (b) { if (b) { live.set(b, { kind: 'buffer', bytes: 0 }); stats.made.buffer++; } });
  wrap('createRenderbuffer', function (b) { if (b) { live.set(b, { kind: 'renderbuffer', bytes: 0 }); stats.made.renderbuffer++; } });
  wrap('createFramebuffer', function (b) { if (b) { live.set(b, { kind: 'framebuffer', bytes: 0 }); stats.made.framebuffer++; } });
  for (const n of ['deleteTexture', 'deleteBuffer', 'deleteRenderbuffer', 'deleteFramebuffer']) wrap(n, function (_, a) { live.delete(a[0]); });
  wrap('activeTexture', function (_, a) { unit.set(this, a[0] - this.TEXTURE0); });
  wrap('bindTexture', function (_, a) {
    if (!boundTex.has(this)) boundTex.set(this, new Map());
    boundTex.get(this).set((unit.get(this) || 0) + ':' + a[0], a[1]);
  });
  wrap('bindBuffer', function (_, a) { if (!boundBuf.has(this)) boundBuf.set(this, new Map()); boundBuf.get(this).set(a[0], a[1]); });
  wrap('bufferData', function (_, a) {
    const b = (boundBuf.get(this) || new Map()).get(a[0]);
    const e = b && live.get(b);
    if (e) e.bytes = typeof a[1] === 'number' ? a[1] : (a[1] && a[1].byteLength) || 0;
  });
  const sized = (src) => src && (src.width || src.videoWidth || src.displayWidth || 0) * (src.height || src.videoHeight || src.displayHeight || 0);
  wrap('texImage2D', function (_, a) {
    const t = texOf(this, a[0]); const e = t && live.get(t);
    let n;
    if (a.length >= 8) n = a[3] * a[4] * bpp(this, a[2], a[7]);
    else n = sized(a[5]) * bpp(this, a[2], a[4]);
    if (e) { e.bytes = n; }
    stats.uploads++; stats.uploadBytes += n; stats.respecs++; stats.respecBytes += n;
  });
  wrap('texImage3D', function (_, a) {
    const t = texOf(this, a[0]); const e = t && live.get(t);
    const n = a[3] * a[4] * a[5] * bpp(this, a[2], a[8]);
    if (e) e.bytes = n;
    stats.uploads++; stats.uploadBytes += n; stats.respecs++; stats.respecBytes += n;
  });
  wrap('texStorage2D', function (_, a) { const t = texOf(this, a[0]); const e = t && live.get(t); if (e) e.bytes = a[3] * a[4] * bpp(this, a[2]); });
  wrap('texStorage3D', function (_, a) { const t = texOf(this, a[0]); const e = t && live.get(t); if (e) e.bytes = a[3] * a[4] * a[5] * bpp(this, a[2]); });
  wrap('texSubImage2D', function (_, a) {
    const n = a.length >= 9 ? a[4] * a[5] * 4 : sized(a[6]) * 4;
    stats.uploads++; stats.uploadBytes += n;
  });
  wrap('texSubImage3D', function (_, a) { stats.uploads++; stats.uploadBytes += a[5] * a[6] * a[7]; });
  const mk = Document.prototype.createElement;
  Document.prototype.createElement = function (tag, ...rest) { if (String(tag).toLowerCase() === 'canvas') stats.canvases++; return mk.call(this, tag, ...rest); };
  if (typeof OffscreenCanvas !== 'undefined') {
    const O = OffscreenCanvas;
    window.OffscreenCanvas = function (...a) { stats.canvases++; return new O(...a); };
    window.OffscreenCanvas.prototype = O.prototype;
  }
  window.__glProbe = () => {
    const out = { textures: 0, buffers: 0, renderbuffers: 0, framebuffers: 0, textureBytes: 0, bufferBytes: 0 };
    for (const e of live.values()) {
      out[e.kind + 's']++;
      if (e.kind === 'texture') out.textureBytes += e.bytes;
      else out.bufferBytes += e.bytes;
    }
    return { ...out, uploads: stats.uploads, uploadBytes: stats.uploadBytes, respecs: stats.respecs, respecBytes: stats.respecBytes, canvases: stats.canvases,
      made: { ...stats.made }, heapMB: performance.memory ? performance.memory.usedJSHeapSize / 1e6 : null };
  };
  // A lost context frees everything.
  addEventListener('webglcontextlost', () => live.clear(), true);
})();`;

/** The GPU process of Chrome `pid`: its memory footprint (MB), from the host. */
export function gpuProcessMB(chromePid) {
  if (!chromePid) return null;
  const ps = spawnSync('ps', ['-axo', 'pid=,ppid=,rss=,command=']);
  if (ps.status !== 0) return null;
  const rows = String(ps.stdout).split('\n').map((l) => l.trim().match(/^(\d+)\s+(\d+)\s+(\d+)\s+(.*)$/)).filter(Boolean);
  const gpu = rows.find((r) => Number(r[2]) === chromePid && /--type=gpu-process/.test(r[4]));
  if (!gpu) return null;
  const pid = Number(gpu[1]);
  const out = { pid, rssMB: Number(gpu[3]) / 1024, footprintMB: null };
  if (process.platform === 'darwin') {
    const fp = spawnSync('footprint', ['-p', String(pid)], { timeout: 10000 });
    const m = String(fp.stdout || '').match(/Footprint:\s*([\d.]+)\s*(KB|MB|GB)/i);
    if (m) out.footprintMB = Number(m[1]) * ({ KB: 1 / 1024, MB: 1, GB: 1024 })[m[2].toUpperCase()];
  }
  return out;
}

/**
 * The repeated-selection pattern: `bursts` × (alternate Kinematic Lasso picks
 * and spectra, `picks` of them over `burstMs`, then a mask statistics request
 * of the last lasso), measured after each burst.
 */
export async function runAllocChecks({ c, check, waitFor, results, chromePid = 0, bursts = 4, picks = 20, burstMs = 5000, assert = true }) {
  const say = (m) => console.log(`     alloc: ${m}`);
  const have = await c.evaluate('typeof window.__glProbe === "function"');
  if (!have) { check('alloc: the GL probe is installed', false, 'Page.addScriptToEvaluateOnNewDocument did not run'); return null; }
  const sample = async (label) => {
    await sleep(1500);                     // the replies of the burst have landed
    const s = await c.evaluate('window.__glProbe()');
    const gp = gpuProcessMB(chromePid);
    const row = { label, ...s, gpuProcess: gp };
    say(`${label}: ${s.textures} textures ${(s.textureBytes / 1e6).toFixed(2)} MB, ${s.buffers} buffers ${(s.bufferBytes / 1e6).toFixed(2)} MB, `
      + `uploads ${s.uploads} (${(s.uploadBytes / 1e6).toFixed(1)} MB), re-specified ${s.respecs} (${(s.respecBytes / 1e6).toFixed(1)} MB), canvases ${s.canvases}, heap ${s.heapMB ? s.heapMB.toFixed(1) : 'n/a'} MB`
      + (gp ? `, GPU process ${gp.footprintMB !== null ? `${gp.footprintMB.toFixed(1)} MB footprint, ` : ''}${gp.rssMB.toFixed(1)} MB RSS` : ''));
    return row;
  };
  // The emission: points of the source near the box centre, through the page's projection.
  const spots = [[0.5, 0.47, 0.5], [0.52, 0.47, 0.48], [0.48, 0.46, 0.52], [0.5, 0.5, 0.5], [0.53, 0.45, 0.5]];
  const rows = [await sample('before')];
  for (let b = 0; b < bursts; b++) {
    const t0 = Date.now();
    for (let i = 0; i < picks; i++) {
      const tool = i % 2 ? 'probe' : 'lasso';
      const [x, y, z] = spots[i % spots.length];
      await c.evaluate(`(() => { const s = window.__vrSelection; if (s.sel.tool !== '${tool}') s.setTool('${tool}');
        const [px, py] = window.__vrProject([${x}, ${y}, ${z}]); window.__vrClick(px, py); })()`);
      const wait = t0 + ((i + 1) * burstMs) / picks - Date.now();
      if (wait > 0) await sleep(wait);
    }
    // The last lasso of the burst, then its statistics again (the headset's
    // mask statistics request).
    await c.evaluate(`window.__vrSelection.setTool('lasso')`);
    const [x, y] = await c.evaluate('window.__vrProject([0.5, 0.47, 0.5])');
    await c.evaluate(`window.__vrClick(${x}, ${y})`);
    await waitFor(c, '!!(window.__vrSelection.sel.lasso || window.__vrSelection.sel.error) && !window.__vrSelection.sel.busy', 30000);
    await c.evaluate(`(async () => { const L = window.__vrSelection.sel.lasso; const b = window.__vrRefine.refiner.backend();
      if (L && b) await b.maskStats(window.__vrState.open.dataset_id, L.bytes, L.origin, L.dims); })()`);
    rows.push(await sample(`burst ${b + 1}`));
  }
  if (results) results.alloc = rows;
  if (assert) {
    const first = rows[1], last = rows[rows.length - 1];
    const lost = await c.evaluate('window.__vrGL.isContextLost()');
    check('alloc: no WebGL context loss under repeated selections', !lost);
    check('alloc: repeated selections add no live WebGL objects after the first burst',
      last.textures <= first.textures && last.buffers <= first.buffers && last.renderbuffers <= first.renderbuffers,
      `textures ${first.textures}→${last.textures}, buffers ${first.buffers}→${last.buffers}`);
    check('alloc: texture bytes bounded under repeated selections', last.textureBytes <= first.textureBytes * 1.05 + 65536,
      `${(first.textureBytes / 1e6).toFixed(2)} → ${(last.textureBytes / 1e6).toFixed(2)} MB`);
    // The panels, the label atlas and the selection mask keep their storage:
    // a redraw or a new selection writes into it (texSubImage), it does not
    // allocate again. A few re-specifications (a mask grown once) are allowed.
    const respec = last.respecs - first.respecs;
    check('alloc: later bursts re-specify (almost) no texture storage', respec <= 3,
      `${respec} re-specifications, ${((last.respecBytes - first.respecBytes) / 1e6).toFixed(1)} MB, over ${rows.length - 2} bursts`);
  }
  return rows;
}

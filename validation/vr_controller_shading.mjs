// The real controller models' shading, offscreen (run by vr_lifecycle.mjs
// --regressions; also alone with `node validation/vr_controller_shading.mjs`,
// which serves the static files itself and saves the PNGs).
//
// On a Quest 3 the Touch Plus models looked grainy and their black plastic
// turned white when looked at (the effect followed the view direction). This
// renders each vendored Touch Plus model (v1 and v2, left and right) with the
// page's own pipeline (Renderer.uploadModel / drawModels, the textures decoded
// as the page decodes them) from several view directions, into an offscreen
// WebGL2 canvas made as the page makes its own (alpha, premultiplied), and
// reads the pixels back:
//   - the dark plastic (pixels whose unlit base colour is dark) stays dark
//     from every direction, and the light plastic is lit but not clipped;
//   - the model is opaque: every pixel it covers has alpha 255;
//   - blending, depth test and culling are left as drawModels found them.
// Pure page-side code in PROBE (a string evaluated in the page), so the same
// measurement runs in the lifecycle page and in the standalone driver.

import { createServer } from 'node:http';
import { spawn } from 'node:child_process';
import { readFileSync, writeFileSync, mkdirSync, mkdtempSync, existsSync, statSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, dirname, extname, normalize, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

/** The models drawn: both Touch Plus (all double-sided), the Quest 2's (single-sided) and the generic one (single-sided, under a mirroring node). */
export const PROFILES = ['meta-quest-touch-plus', 'meta-quest-touch-plus-v2', 'oculus-touch-v3', 'generic-trigger-squeeze-thumbstick'];

/** The view directions (from the model's centre towards the eye), named. */
export const DIRECTIONS = {
  front: [0, 0, 1], back: [0, 0, -1], left: [-1, 0, 0], right: [1, 0, 0],
  top: [0, 1, 0], bottom: [0, -1, 0], 'front-top': [0, 0.7, 0.7], 'grip-side': [0.6, -0.3, 0.75],
};

/**
 * The page-side probe: `jsBase` the URL of the page's js/ directory (by
 * default the one the page's own module script comes from); `size` the canvas side; `images` true returns
 * a PNG data URL per view. Returns {views: [{profile, hand, dir, covered,
 * darkN, darkMedian, darkP95, darkWhite, lightN, lightMedian, lightP10,
 * lightP90, lightClipped, opaque, png}], stateKept, maxAniso, bytes}. Each
 * view is drawn twice: unlit base colour (which pixels are dark plastic),
 * then with drawModels.
 */
export function probeSource({ jsBase = null, size = 320, images = false, profiles = PROFILES } = {}) {
  return `(async () => {
  // The page's own modules (its versioned address), else the given directory.
  const page = document.querySelector('script[type=module]');
  const base = new URL(${jsBase ? JSON.stringify(jsBase) : "page ? new URL('.', page.src).href : '/vr/js/'"}, location.href);
  const J = (f) => new URL(f, base).href;
  const R_ = await import(J('renderer.js'));
  const CM = await import(J('controller-models.js'));
  const CP = await import(J('controller-profiles.js'));
  const G = await import(J('gltf.js'));
  const S = ${size};
  const canvas = document.createElement('canvas');
  canvas.width = S; canvas.height = S;
  const gl = canvas.getContext('webgl2', { antialias: false, alpha: true, premultipliedAlpha: true, preserveDrawingBuffer: true });
  if (!gl) return { error: 'no WebGL2' };
  const R = new R_.Renderer(gl);
  const cm = new CM.ControllerModels(R);         // its models/ beside its js/, as on the page
  const dirs = ${JSON.stringify(DIRECTIONS)};
  const norm = (v) => { const l = Math.hypot(...v) || 1; return v.map((x) => x / l); };
  const cross = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
  const lookAt = (eye, c, up) => {
    const f = norm([c[0] - eye[0], c[1] - eye[1], c[2] - eye[2]]);
    let s = cross(f, up); if (Math.hypot(...s) < 1e-6) s = cross(f, [0, 0, 1]); s = norm(s);
    const u = cross(s, f);
    const m = new Float32Array(16);
    m[0] = s[0]; m[4] = s[1]; m[8] = s[2];
    m[1] = u[0]; m[5] = u[1]; m[9] = u[2];
    m[2] = -f[0]; m[6] = -f[1]; m[10] = -f[2];
    m[12] = -(s[0] * eye[0] + s[1] * eye[1] + s[2] * eye[2]);
    m[13] = -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]);
    m[14] = f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2];
    m[15] = 1;
    return m;
  };
  const persp = (fovy, near, far) => {
    const f = 1 / Math.tan(fovy / 2), m = new Float32Array(16);
    m[0] = f; m[5] = f; m[10] = (far + near) / (near - far); m[11] = -1; m[14] = 2 * far * near / (near - far);
    return m;
  };
  const ident = new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]);
  const px = new Uint8Array(S * S * 4), alb = new Uint8Array(S * S * 4), both = new Uint8Array(S * S * 4);
  // The albedo pass: the same geometry and base-colour texture, unlit, so each
  // pixel is known to be dark plastic (or not) whatever the shading does.
  const albedo = (() => {
    const vs = '#version 300 es\\nprecision highp float;in vec3 aPos;in vec2 aUV;uniform mat4 uVP;uniform mat4 uModel;out vec2 vUV;void main(){vUV=aUV;gl_Position=uVP*(uModel*vec4(aPos,1.0));}';
    const fs = '#version 300 es\\nprecision highp float;in vec2 vUV;uniform sampler2D uTex;uniform vec4 uColor;out vec4 o;void main(){o=vec4(texture(uTex,vUV).rgb*uColor.rgb,1.0);}';
    const p = gl.createProgram();
    for (const [t, src] of [[gl.VERTEX_SHADER, vs], [gl.FRAGMENT_SHADER, fs]]) {
      const sh = gl.createShader(t); gl.shaderSource(sh, src); gl.compileShader(sh); gl.attachShader(p, sh);
    }
    for (const nm of ['aPos', 'aUV']) gl.bindAttribLocation(p, gl.getAttribLocation(R.modelProg.p, nm), nm);
    gl.linkProgram(p);
    const u = {}; for (const nm of ['uVP', 'uModel', 'uTex', 'uColor']) u[nm] = gl.getUniformLocation(p, nm);
    return { p, u };
  })();
  const drawAlbedo = (handle, rig, vp) => {
    gl.useProgram(albedo.p);
    gl.uniformMatrix4fv(albedo.u.uVP, false, vp);
    gl.uniform1i(albedo.u.uTex, 0);
    gl.activeTexture(gl.TEXTURE0);
    gl.enable(gl.DEPTH_TEST); gl.depthFunc(gl.LEQUAL); gl.disable(gl.BLEND); gl.disable(gl.CULL_FACE);
    gl.bindVertexArray(handle.vao);
    for (const i of rig.order) {
      const mi = rig.model.nodes[i].mesh;
      if (mi < 0 || !rig.drawn[i]) continue;
      gl.uniformMatrix4fv(albedo.u.uModel, false, rig.world[i]);
      for (const p of handle.meshes[mi]) {
        gl.uniform4fv(albedo.u.uColor, p.color);
        const t = p.texture >= 0 && handle.textures[p.texture];
        gl.bindTexture(gl.TEXTURE_2D, (t && (t.srgb || t)) || R.whiteTex);
        gl.drawElements(gl.TRIANGLES, p.count, p.type, p.offset);
      }
    }
    gl.bindVertexArray(null);
  };
  const views = [];
  let stateKept = true;
  for (const id of ${JSON.stringify(profiles)}) {
    for (const hand of ['left', 'right']) {
      const { model, handle, layout } = await cm._asset(id, hand);
      const rig = CP.makeRig(model, layout);
      CP.applyResponses(rig, null);
      CP.updateWorld(rig, ident);
      const b = G.modelBounds(model, rig.world);
      const c = [0, 1, 2].map((k) => (b.lo[k] + b.hi[k]) / 2);
      const r = Math.hypot(...[0, 1, 2].map((k) => b.hi[k] - b.lo[k])) / 2;
      for (const [name, d0] of Object.entries(dirs)) {
        const d = norm(d0);
        const dist = r / Math.sin(0.5 * 0.6) * 1.05;
        const eye = c.map((x, k) => x + d[k] * dist);
        const up = Math.abs(d[1]) > 0.9 ? [0, 0, -1] : [0, 1, 0];
        const proj = persp(0.6, dist * 0.1, dist * 4), view = lookAt(eye, c, up);
        const clear = () => {
          gl.bindFramebuffer(gl.FRAMEBUFFER, null);
          gl.viewport(0, 0, S, S);
          gl.disable(gl.SCISSOR_TEST);
          gl.colorMask(true, true, true, true);
          gl.depthMask(true);
          gl.clearColor(0, 0, 0, 0);
          gl.clearDepth(1);
          gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
        };
        clear();
        const vp = new Float32Array(16);
        G.mulInto(vp, proj, view);
        drawAlbedo(handle, rig, vp);
        gl.readPixels(0, 0, S, S, gl.RGBA, gl.UNSIGNED_BYTE, alb);
        clear();
        // The page draws its other layers with blending on and no depth test;
        // culling is seeded with values the page does not use, which must
        // neither hide the models nor be changed by them.
        gl.enable(gl.BLEND);
        gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA);
        gl.disable(gl.DEPTH_TEST);
        gl.enable(gl.CULL_FACE);
        gl.cullFace(gl.FRONT);
        gl.frontFace(gl.CW);
        R.drawModels({ proj, view }, [{ handle, rig }], 1);
        stateKept = stateKept && gl.isEnabled(gl.BLEND) && !gl.isEnabled(gl.DEPTH_TEST) && gl.isEnabled(gl.CULL_FACE)
          && gl.getParameter(gl.CULL_FACE_MODE) === gl.FRONT && gl.getParameter(gl.FRONT_FACE) === gl.CW;
        gl.readPixels(0, 0, S, S, gl.RGBA, gl.UNSIGNED_BYTE, px);
        const png = ${images ? 'canvas.toDataURL()' : 'null'};
        // Single-sided models: drawn again with both sides, culling off. For a
        // closed model the front faces are the nearest, so the image must not
        // change; culling the wrong side (a mirroring node's winding) shows
        // the far side instead.
        let cullDiff = -1;
        if (handle.singleSided) {
          const keep = handle.meshes.map((prs) => prs.map((p) => p.doubleSided));
          handle.singleSided = false;
          handle.meshes.forEach((prs) => prs.forEach((p) => { p.doubleSided = true; }));
          clear();
          gl.disable(gl.CULL_FACE);
          R.drawModels({ proj, view }, [{ handle, rig }], 1);
          gl.readPixels(0, 0, S, S, gl.RGBA, gl.UNSIGNED_BYTE, both);
          handle.meshes.forEach((prs, a) => prs.forEach((p, b) => { p.doubleSided = keep[a][b]; }));
          handle.singleSided = true;
          let n = 0, differ = 0;
          for (let i = 0; i < S * S; i++) {
            if (!px[4 * i + 3] && !both[4 * i + 3]) continue;
            n++;
            if (Math.abs(px[4 * i] - both[4 * i]) + Math.abs(px[4 * i + 1] - both[4 * i + 1]) + Math.abs(px[4 * i + 2] - both[4 * i + 2]) > 48
              || px[4 * i + 3] !== both[4 * i + 3]) differ++;
          }
          cullDiff = n ? differ / n : 0;
        }
        const dark = [], light = [];
        let covered = 0, opaque = 0, clipped = 0;
        const L = (a, i) => 0.2126 * a[4 * i] + 0.7152 * a[4 * i + 1] + 0.0722 * a[4 * i + 2];
        for (let i = 0; i < S * S; i++) {
          const a = px[4 * i + 3];
          if (!a) continue;
          covered++;
          if (a === 255) opaque++;
          const l = L(px, i);
          // Dark plastic: an albedo below 60 of 255 (sRGB 35 is 4 in linear, either way below).
          if (L(alb, i) < 60) dark.push(l); else { light.push(l); if (l >= 254) clipped++; }
        }
        const q = (arr, p) => (arr.length ? arr[Math.min(arr.length - 1, Math.floor(p * arr.length))] : 0);
        dark.sort((x, y) => x - y); light.sort((x, y) => x - y);
        views.push({ profile: id, hand, dir: name, covered, darkN: dark.length, darkMedian: q(dark, 0.5), darkP95: q(dark, 0.95),
          darkWhite: dark.length ? dark.filter((v) => v > 128).length / dark.length : 0,
          lightN: light.length, lightMedian: q(light, 0.5), lightP10: q(light, 0.1), lightP90: q(light, 0.9),
          lightClipped: light.length ? clipped / light.length : 0,
          opaque: covered ? opaque / covered : 1, cullDiff, png });
      }
    }
  }
  // Mixed models in one drawModels call, both orders: a single-sided model
  // (culling on) before or after a double-sided one must not change how the
  // double-sided one is drawn. The single-sided one is placed out of view.
  const mixed = {};
  if (${JSON.stringify(profiles)}.includes('oculus-touch-v3') && ${JSON.stringify(profiles)}.includes('meta-quest-touch-plus')) {
    const a = await cm._asset('meta-quest-touch-plus', 'left'), b = await cm._asset('oculus-touch-v3', 'left');
    const ra = CP.makeRig(a.model, a.layout), rb = CP.makeRig(b.model, b.layout);
    CP.applyResponses(ra, null); CP.applyResponses(rb, null);
    CP.updateWorld(ra, ident);
    CP.updateWorld(rb, new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 50, 0, 0, 1]));
    const bb = G.modelBounds(a.model, ra.world);
    const c = [0, 1, 2].map((k) => (bb.lo[k] + bb.hi[k]) / 2);
    const proj = persp(0.6, 0.01, 10), view = lookAt([c[0] + 0.2, c[1] + 0.1, c[2] + 0.3], c, [0, 1, 0]);
    const draw = (list, out) => {
      gl.viewport(0, 0, S, S); gl.clearColor(0, 0, 0, 0); gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
      gl.disable(gl.CULL_FACE); gl.frontFace(gl.CCW);
      R.drawModels({ proj, view }, list, list.length);
      gl.readPixels(0, 0, S, S, gl.RGBA, gl.UNSIGNED_BYTE, out);
    };
    const alone = new Uint8Array(S * S * 4);
    draw([{ handle: a.handle, rig: ra }], alone);
    for (const [name, list] of [['single-sided first', [{ handle: b.handle, rig: rb }, { handle: a.handle, rig: ra }]],
      ['single-sided last', [{ handle: a.handle, rig: ra }, { handle: b.handle, rig: rb }]]]) {
      draw(list, both);
      let n = 0, differ = 0;
      for (let i = 0; i < S * S; i++) {
        if (!alone[4 * i + 3] && !both[4 * i + 3]) continue;
        n++;
        if (alone[4 * i] !== both[4 * i] || alone[4 * i + 1] !== both[4 * i + 1] || alone[4 * i + 2] !== both[4 * i + 2] || alone[4 * i + 3] !== both[4 * i + 3]) differ++;
      }
      mixed[name] = { pixels: n, differ };
    }
  }
  const ext = gl.getExtension('EXT_texture_filter_anisotropic');
  const out = { views, stateKept, mixed, maxAniso: ext ? gl.getParameter(ext.MAX_TEXTURE_MAX_ANISOTROPY_EXT) : 0,
    bytes: [...R._models].reduce((a, h) => a + h.bytes, 0), renderer: gl.getParameter(gl.RENDERER) };
  // This context's textures go now, not when it is collected: the page's
  // later checks (timed, on a software GPU too) run without them.
  const lose = gl.getExtension('WEBGL_lose_context');
  if (lose) lose.loseContext();
  return out;
})()`;
}

/**
 * The thresholds of the checks (8-bit sRGB luminance of the model's pixels).
 * Dark plastic (albedo sRGB 35 on the Touch Plus): its 95th percentile under
 * 110 and at most 3 % of it brighter than 128 (a highlight, not a white
 * patch), from every direction, and its median at least 3 (lit, not black: the generic model is near black).
 * Light plastic (albedo 226): median at least 90 and at most 25 % of it
 * clipped at 255 (lit, with its shape, not flat white). A single-sided model
 * drawn with culling differs from the same drawn with both sides on at most
 * 5 % of its pixels.
 */
export const LIMITS = { darkP95: 110, darkWhite: 0.03, darkMedianMin: 3, lightMedianMin: 90, lightClipped: 0.25,
  coveredMin: 2000, darkMin: 300, cullDiff: 0.05 };

/** The checks on a probe's result; `check(name, ok, detail)`. */
export function checkShading(res, check, label = '') {
  const pre = label ? `${label}: ` : '';
  if (res.error) { check(`${pre}controller shading probe runs`, false, res.error); return; }
  const by = {};
  for (const v of res.views) (by[`${v.profile}/${v.hand}`] ||= []).push(v);
  for (const [key, vs] of Object.entries(by)) {
    const drawn = vs.every((v) => v.covered >= LIMITS.coveredMin);
    check(`${pre}controller ${key}: drawn from every direction`, drawn, vs.map((v) => `${v.dir} ${v.covered}`).join(', '));
    const seen = vs.filter((v) => v.darkN >= LIMITS.darkMin);
    const lit = vs.filter((v) => v.lightN >= LIMITS.darkMin);
    check(`${pre}controller ${key}: some of it is seen as dark or light plastic`, seen.length + lit.length > 0);
    check(`${pre}controller ${key}: the dark plastic stays dark from every direction, and is lit`,
      seen.every((v) => v.darkP95 <= LIMITS.darkP95 && v.darkWhite <= LIMITS.darkWhite && v.darkMedian >= LIMITS.darkMedianMin),
      seen.length ? seen.map((v) => `${v.dir} median ${v.darkMedian.toFixed(0)} p95 ${v.darkP95.toFixed(0)} (${(100 * v.darkWhite).toFixed(1)} % >128)`).join(', ') : 'no dark plastic');
    check(`${pre}controller ${key}: the light plastic is lit, not black and not clipped to white`,
      lit.every((v) => v.lightMedian >= LIMITS.lightMedianMin && v.lightClipped <= LIMITS.lightClipped),
      lit.length ? lit.map((v) => `${v.dir} median ${v.lightMedian.toFixed(0)} p10-p90 ${v.lightP10.toFixed(0)}-${v.lightP90.toFixed(0)} clipped ${(100 * v.lightClipped).toFixed(0)} %`).join(', ') : 'no light plastic');
    const culled = vs.filter((v) => v.cullDiff >= 0);
    if (culled.length) {
      check(`${pre}controller ${key}: single-sided, the side culled is the back`, culled.every((v) => v.cullDiff <= LIMITS.cullDiff),
        culled.map((v) => `${v.dir} ${(100 * v.cullDiff).toFixed(1)} %`).join(', '));
    }
    check(`${pre}controller ${key}: opaque (alpha 1 wherever it is drawn)`, vs.every((v) => v.opaque === 1),
      vs.map((v) => `${v.dir} ${(100 * v.opaque).toFixed(1)} %`).join(', '));
  }
  check(`${pre}controller models leave blending, depth test, culling, cull mode and front face as they found them`, res.stateKept);
  for (const [order, m] of Object.entries(res.mixed || {})) {
    check(`${pre}controller models: a single-sided model drawn ${order.replace('single-sided ', '')} in the same call does not change a double-sided one`,
      m.pixels > 2000 && m.differ === 0, `${m.differ} of ${m.pixels} pixels differ`);
  }
}

/** In the lifecycle page (vr_lifecycle.mjs --regressions): the probe, then the checks. */
export async function runControllerShadingChecks({ c, check, say = console.log }) {
  const res = await c.evaluate(probeSource(), 60000);
  if (!res.error) say(`controller models: ${res.renderer}; ${(res.bytes / 1048576).toFixed(1)} MB on the GPU for the ${res.views.length / Object.keys(DIRECTIONS).length} models; max anisotropy ${res.maxAniso}`);
  checkShading(res, check);
  return res;
}

// ── Standalone: serve the static files, run Chrome, save the PNGs ──────────
async function main() {
  const HERE = dirname(fileURLToPath(import.meta.url));
  const args = Object.fromEntries(process.argv.slice(2).reduce((a, v, i, all) => {
    if (v.startsWith('--')) a.push([v.slice(2), all[i + 1] && !all[i + 1].startsWith('--') ? all[i + 1] : true]);
    return a;
  }, []));
  // --static DIR: another copy of backend/app/static (e.g. an older build, for a before/after).
  const STATIC = args.static ? resolve(args.static) : join(HERE, '..', 'backend', 'app', 'static');
  const PORT = Number(args.port || 8893), CDP = Number(args['cdp-port'] || 9493);
  const GL = args.gl || 'metal';
  const OUT = args.out || join(HERE, 'out', 'vr_controller_shading');
  mkdirSync(OUT, { recursive: true });
  const types = { '.js': 'text/javascript', '.mjs': 'text/javascript', '.json': 'application/json', '.html': 'text/html', '.glb': 'model/gltf-binary' };
  const server = createServer((req, res) => {
    const p = decodeURIComponent(new URL(req.url, 'http://x').pathname);
    if (p === '/' || p === '/blank.html') { res.writeHead(200, { 'content-type': 'text/html' }); res.end('<!doctype html><title>probe</title>'); return; }
    const f = normalize(join(STATIC, p));
    if (!f.startsWith(STATIC) || !existsSync(f) || !statSync(f).isFile()) { res.writeHead(404); res.end(); return; }
    res.writeHead(200, { 'content-type': types[extname(f)] || 'application/octet-stream' });
    res.end(readFileSync(f));
  });
  await new Promise((r) => server.listen(PORT, 'localhost', r));
  const CHROME = args.chrome || process.env.CHROME || '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
  const gl = GL === 'metal' ? ['--use-angle=metal'] : ['--use-angle=swiftshader', '--enable-unsafe-swiftshader', '--use-gl=angle'];
  const chrome = spawn(CHROME, ['--headless=new', ...gl, `--remote-debugging-port=${CDP}`, `--user-data-dir=${mkdtempSync(join(tmpdir(), 'vr-shading-'))}`,
    '--no-first-run', '--no-default-browser-check', `http://localhost:${PORT}/blank.html`], { stdio: 'ignore' });
  let code = 1;
  try {
    let ws = null;
    for (let i = 0; i < 300 && !ws; i++) {
      try {
        const list = await (await fetch(`http://127.0.0.1:${CDP}/json/list`)).json();
        const page = list.find((t) => t.type === 'page' && t.url.includes('blank'));
        if (page) ws = page.webSocketDebuggerUrl;
      } catch { /* not up */ }
      if (!ws) await new Promise((r) => setTimeout(r, 100));
    }
    if (!ws) throw new Error('Chrome did not start');
    const sock = new WebSocket(ws);
    await new Promise((r) => sock.addEventListener('open', r));
    let id = 0; const waiting = new Map();
    sock.addEventListener('message', (e) => { const m = JSON.parse(e.data); if (waiting.has(m.id)) { waiting.get(m.id)(m); waiting.delete(m.id); } });
    const send = (method, params) => new Promise((r) => { const i = ++id; waiting.set(i, r); sock.send(JSON.stringify({ id: i, method, params })); });
    // The blank page loaded (an evaluation during the navigation loses its context).
    for (let i = 0; i < 100; i++) {
      const s = await send('Runtime.evaluate', { expression: "location.pathname === '/blank.html' && document.readyState", returnByValue: true });
      if (s.result && s.result.result && s.result.result.value === 'complete') break;
      await new Promise((res) => setTimeout(res, 100));
    }
    const r = await send('Runtime.evaluate', { expression: probeSource({ jsBase: '/vr/js/', images: !args['no-images'], size: Number(args.size || 320) }), awaitPromise: true, returnByValue: true });
    if (r.result.exceptionDetails) throw new Error(r.result.exceptionDetails.exception?.description || r.result.exceptionDetails.text);
    const res = r.result.result.value;
    let failed = 0;
    const check = (name, ok, detail = '') => { if (!ok) failed++; console.log(`${ok ? 'ok  ' : 'FAIL'} ${name}${detail ? ` — ${detail}` : ''}`); };
    if (!res.error) {
      console.log(`${res.renderer}; ${(res.bytes / 1048576).toFixed(2)} MB on the GPU; max anisotropy ${res.maxAniso}`);
      for (const v of res.views) {
        if (v.png) writeFileSync(join(OUT, `${args.tag ? `${args.tag}_` : ''}${v.profile}_${v.hand}_${v.dir}.png`), Buffer.from(v.png.split(',')[1], 'base64'));
        v.png = undefined;
      }
      writeFileSync(join(OUT, `${args.tag || 'result'}.json`), JSON.stringify(res, null, 1));
    }
    checkShading(res, check, GL);
    code = failed ? 1 : 0;
    sock.close();
  } finally {
    chrome.kill();
    server.close();
  }
  process.exit(code);
}

if (process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1]) main().catch((e) => { console.error(e); process.exit(2); });

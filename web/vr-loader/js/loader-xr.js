// The loader's own notice in VR (issue #383). In the VisIVO VR headset app
// the loader is never shown as a 2-D page: the browser's immersive part shows
// only a VR session, and before one starts the user sees an empty room. When
// the loader cannot pair (an expired code, no computer answering) it starts a
// session of its own (the app's browser grants one with no press) and shows
// one card in front of the user. A trigger or a pinch, or VR_NOTICE_MS, ends
// the session: the browser then closes its part of the app and the app's
// scanner comes back. Small on purpose: one textured quad, no input but
// `select`. The pure parts (wrapping, placement) are in loader-core.js.

import { wrapText, panelModel, VR_NOTICE_MS } from './loader-core.js';

const VS = `#version 300 es
precision highp float;
in vec2 pos;
uniform mat4 mvp;
out vec2 uv;
void main() {
  uv = vec2(pos.x + 0.5, 0.5 - pos.y);
  gl_Position = mvp * vec4(pos, 0.0, 1.0);
}`;

const FS = `#version 300 es
precision highp float;
in vec2 uv;
uniform sampler2D tex;
out vec4 color;
void main() { color = texture(tex, uv); }`;

const W = 1024, H = 512;

/** The card: the notice's title and lines on the loader's colours. */
function drawCard(notice) {
  const c = Object.assign(document.createElement('canvas'), { width: W, height: H });
  const g = c.getContext('2d');
  g.fillStyle = '#101418';
  g.fillRect(0, 0, W, H);
  g.strokeStyle = '#2f3a45';
  g.lineWidth = 6;
  g.strokeRect(3, 3, W - 6, H - 6);
  let y = 92;
  g.fillStyle = '#ff9c8f';
  g.font = 'bold 54px system-ui, sans-serif';
  g.fillText(String(notice.title || ''), 48, y);
  y += 30;
  g.font = '34px system-ui, sans-serif';
  for (const [i, text] of (notice.lines || []).entries()) {
    g.fillStyle = i === notice.lines.length - 1 ? '#8b98a5' : '#e6edf3';
    for (const line of wrapText((s) => g.measureText(s).width, text, W - 96)) {
      y += 46;
      if (y > H - 24) return c;
      g.fillText(line, 48, y);
    }
    y += 14;
  }
  return c;
}

function mul(a, b) {
  const o = new Float32Array(16);
  for (let i = 0; i < 4; i++) {
    for (let j = 0; j < 4; j++) {
      let s = 0;
      for (let k = 0; k < 4; k++) s += a[k * 4 + j] * b[i * 4 + k];
      o[i * 4 + j] = s;
    }
  }
  return o;
}

function program(gl) {
  const shader = (type, src) => {
    const s = gl.createShader(type);
    gl.shaderSource(s, src);
    gl.compileShader(s);
    if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(s) || 'shader');
    return s;
  };
  const p = gl.createProgram();
  gl.attachShader(p, shader(gl.VERTEX_SHADER, VS));
  gl.attachShader(p, shader(gl.FRAGMENT_SHADER, FS));
  gl.bindAttribLocation(p, 0, 'pos');
  gl.linkProgram(p);
  if (!gl.getProgramParameter(p, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(p) || 'program');
  return p;
}

/**
 * Shows `notice` ({title, lines}) in VR until a select (trigger, pinch) or
 * `timeoutMs`, then ends the session. Resolves true once the session ended,
 * false when no session could be had (no WebXR, no WebGL 2, refused).
 */
export async function showInVR(notice, { timeoutMs = VR_NOTICE_MS } = {}) {
  if (!navigator.xr || typeof navigator.xr.requestSession !== 'function') return false;
  const canvas = document.createElement('canvas');
  // Always with alpha: an opaque XR layer is black on the Quest (#225).
  const gl = canvas.getContext('webgl2', { xrCompatible: true, alpha: true, antialias: true });
  if (!gl) return false;
  let prog, tex, vao;
  try {
    prog = program(gl);
    // Everything uploaded before the session (#229: no allocations in VR).
    tex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, tex);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, drawCard(notice));
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    vao = gl.createVertexArray();
    gl.bindVertexArray(vao);
    const buf = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, buf);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-0.5, -0.5, 0.5, -0.5, -0.5, 0.5, 0.5, 0.5]), gl.STATIC_DRAW);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
  } catch (err) {
    console.warn('[vr-loader] the notice could not be drawn:', err);
    return false;
  }
  let session;
  try {
    session = await navigator.xr.requestSession('immersive-vr', { optionalFeatures: ['local-floor'] });
  } catch (err) {
    console.warn('[vr-loader] no VR session for the notice:', err);
    return false;
  }
  const mvpAt = gl.getUniformLocation(prog, 'mvp');
  const texAt = gl.getUniformLocation(prog, 'tex');
  return new Promise((resolve) => {
    const end = () => { try { session.end().catch(() => {}); } catch { /* ended */ } };
    const timer = setTimeout(end, timeoutMs);
    session.addEventListener('end', () => { clearTimeout(timer); resolve(true); });
    session.addEventListener('select', end);
    (async () => {
      let ref;
      try {
        session.updateRenderState({ baseLayer: new XRWebGLLayer(session, gl, { alpha: true, antialias: true }) });
        ref = await session.requestReferenceSpace('local');
      } catch (err) {
        console.warn('[vr-loader] the notice\'s session could not be set up:', err);
        end();
        return;
      }
      let model = null;
      const frame = (t, f) => {
        session.requestAnimationFrame(frame);
        const pose = f.getViewerPose(ref);
        const layer = session.renderState.baseLayer;
        if (!pose || !layer) return;
        if (!model) model = panelModel(pose.transform.matrix);
        gl.bindFramebuffer(gl.FRAMEBUFFER, layer.framebuffer);
        gl.clearColor(0.02, 0.025, 0.03, 1);
        gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
        gl.useProgram(prog);
        gl.bindVertexArray(vao);
        gl.activeTexture(gl.TEXTURE0);
        gl.bindTexture(gl.TEXTURE_2D, tex);
        gl.uniform1i(texAt, 0);
        for (const v of pose.views) {
          const vp = layer.getViewport(v);
          gl.viewport(vp.x, vp.y, vp.width, vp.height);
          gl.uniformMatrix4fv(mvpAt, false, mul(v.projectionMatrix, mul(v.transform.inverse.matrix, model)));
          gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
        }
      };
      session.requestAnimationFrame(frame);
    })();
  });
}

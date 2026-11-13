// No 3-D texture is allocated while a WebXR session runs (issue #229).
//
// On a Quest 3 (Quest Browser 154, Adreno 740) a texImage3D issued during
// an immersive session makes the GPU process's command decoder fail
// ("Error: 5 for Command kTexImage3D", CommandBufferStub::OnParseError):
// the WebGL context is lost and the GPU process restarts. texSubImage3D into
// storage allocated on the 2-D page is safe (eight lassos in VR, no loss).
// texStorage3D during a session has not been proven safe either, so the
// page allocates every 3-D texture before entering VR and writes into it
// with texSubImage3D in VR; this guard makes any allocation in VR an error
// the page sees (a thrown Error, logged as `tex3d-in-xr`) instead of a GPU
// process restart on the headset.

/** The WebGL calls that (re)specify a 3-D texture's storage. */
export const TEX3D_ALLOCATIONS = ['texImage3D', 'texStorage3D'];

/** The Error a guarded call throws: `code` 'tex3d-in-xr', `call` its name. */
export function tex3dInXRError(name) {
  const e = new Error(`tex3d-in-xr: ${name} while a WebXR session is running (allocate 3-D textures before entering VR)`);
  e.code = 'tex3d-in-xr';
  e.call = name;
  return e;
}

/**
 * Wraps `gl`'s 3-D allocations on the instance: while `isActive()` is true
 * a call is refused (onViolation(name) first, then the Error thrown) and
 * never reaches the GPU. Counts every call, and those refused, in the
 * returned record. Installing it twice on one context wraps once.
 */
export function guardTex3D(gl, isActive, onViolation = () => {}) {
  if (gl.__tex3dGuard && typeof gl.__tex3dGuard === 'object') return gl.__tex3dGuard;
  const record = { calls: { texImage3D: 0, texStorage3D: 0 }, refused: 0 };
  for (const name of TEX3D_ALLOCATIONS) {
    const orig = gl[name];
    if (typeof orig !== 'function') continue;
    const wrapped = function (...a) {
      record.calls[name]++;
      if (isActive()) {
        record.refused++;
        try { onViolation(name); } catch { /* the log must not mask the refusal */ }
        throw tex3dInXRError(name);
      }
      return orig.apply(gl, a);
    };
    try { gl[name] = wrapped; } catch { /* a frozen stand-in: left unguarded */ }
  }
  try { gl.__tex3dGuard = record; } catch { /* as above */ }
  return record;
}

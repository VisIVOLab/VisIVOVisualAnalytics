// The pairing link from the desktop (issue #172): "Open in VR Headset…" opens
// /vr/#token=…&session=…&dataset=…&path=… (src/app/VrLink.h); for a dataset
// that is not a cube (issue #190) also &kind=points|velocity, and a velocity
// field's &box= (its box size in Mpc, to open its path again). Every value is
// percent-encoded once, so it is decoded once with decodeURIComponent ('+'
// stays a plus). Pure functions: unit-tested in backend/tests/vr/pairing.test.mjs.

/** The keys the desktop writes; all of them are removed from the address. */
export const PAIRING_KEYS = ['token', 'session', 'dataset', 'path', 'kind', 'box'];

/**
 * Parses a location hash ("#a=1&b=2", or without the "#").
 * Returns {token, session, dataset, path, kind, box, errors, rest, present}:
 * - a key missing, empty or malformed is '' (a malformed %-escape is listed
 *   in `errors` by key name; the value itself is never echoed);
 * - a key given twice keeps its first value;
 * - `rest` holds the parameters that are not pairing keys, as written, for
 *   the address bar ('' if none);
 * - `present` is true when any pairing key was in the hash.
 */
export function parsePairing(hash) {
  const out = { token: '', session: '', dataset: '', path: '', kind: '', box: '', errors: [], rest: '', present: false };
  const text = String(hash || '').replace(/^#/, '');
  if (!text) return out;
  const keep = [];
  const seen = new Set();
  for (const part of text.split('&')) {
    if (!part) continue;
    const eq = part.indexOf('=');
    const key = eq < 0 ? part : part.slice(0, eq);
    const raw = eq < 0 ? '' : part.slice(eq + 1);
    if (!PAIRING_KEYS.includes(key)) { keep.push(part); continue; }
    out.present = true;
    if (seen.has(key)) continue;
    seen.add(key);
    try {
      out[key] = decodeURIComponent(raw);
    } catch {
      out[key] = '';
      out.errors.push(key);
    }
  }
  out.rest = keep.join('&');
  return out;
}

/** The address without the pairing keys: path + query + the other fragment parameters. */
export function strippedAddress(pathname, search, hash) {
  const { rest } = parsePairing(hash);
  return `${pathname}${search || ''}${rest ? `#${rest}` : ''}`;
}

/**
 * What to do with a pairing once the session is adopted, from the answer to
 * the dataset check (reduced_info with the pairing's dataset id):
 * 'use' the dataset; 'open' the path in this session (the session holds no
 * such dataset, e.g. the backend restarted); 'new-session' then open the
 * path (the session is not ours, unknown in strict mode, or refused);
 * 'fail' when there is no path to fall back on.
 * `err` is null on success, else the Backend's Error ("<status>: <detail>").
 */
export function pairingFallback(err, hasPath) {
  if (!err) return 'use';
  const m = String(err.message || err).match(/^(\d{3}):\s*(.*)$/s);
  const code = m ? Number(m[1]) : 0;
  const detail = m ? m[2] : '';
  let next;
  if (code === 404 && /session not found/i.test(detail)) next = 'new-session';
  else if (code === 400 && /session/i.test(detail)) next = 'new-session';
  else if (code === 404) next = 'open';
  else return 'fail';        // the token refused (401), the backend failing: say so
  return hasPath ? next : 'fail';
}

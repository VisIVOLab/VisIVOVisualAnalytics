// Where the backend's API is, seen from the page (issue #187).
//
// The page is served at <prefix>/vr/ and the API at <prefix>/v1/...: the
// prefix is empty for a backend at the root of its address (the local one, or
// a cluster's reached through an SSH tunnel), and a path such as /visivo or
// /node/r33/proxy/8000 for a backend behind a reverse proxy that strips it
// (an HPC portal). The page finds the prefix in its own address, so the same
// files work both ways and no server-side setting is needed. Pure functions:
// unit-tested in backend/tests/vr/base-path.test.mjs.

/**
 * The prefix of a page address's path: everything before its last "/vr"
 * segment ('/visivo/vr/' and '/visivo/vr/index.html' give '/visivo'; '/vr/'
 * gives ''). A path without a "/vr" segment gives '' (the page served at the
 * root, as in tests).
 */
export function basePrefix(pathname) {
  // Repeated slashes collapsed first: '//host/vr/' must never give a prefix
  // that starts with '//', which fetch() would read as another host.
  const path = String(pathname || '').replace(/\/{2,}/g, '/');
  const m = /^(.*)\/vr(?:\/[^/]*)?$/.exec(path);
  const prefix = m ? m[1].replace(/\/+$/, '') : '';
  return prefix.startsWith('/') ? prefix : '';
}

function here() {
  const loc = (typeof window !== 'undefined' && window.location) || {};
  return { pathname: loc.pathname || '', protocol: loc.protocol || 'http:', host: loc.host || '' };
}

/** An API path ('/v1/...') as the page must fetch it. */
export function apiUrl(path, loc = here()) {
  return `${basePrefix(loc.pathname)}${path}`;
}

/** A WebSocket address for an API path, on the page's own host. It adds
 *  nothing to the path: no token (a portal's proxy logs URLs; the token
 *  travels as a WebSocket subprotocol, or the caller's query on a backend
 *  without that). */
export function wsUrl(path, loc = here()) {
  const proto = loc.protocol === 'https:' ? 'wss' : 'ws';
  return `${proto}://${loc.host}${apiUrl(path, loc)}`;
}

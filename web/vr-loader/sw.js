// The public loader's service worker (issue #213). Its scope is the loader's
// own directory, whatever path the web server gives it.
//
// - app/<tag>/<path>: the VR page's files, from the user's backend. The worker
//   asks the loader window that owns <tag> (it holds the WebRTC link) to GET
//   /<path> over the `api` channel. Files under vr/b/<build>/ are versioned by
//   the backend's page build: they are kept in the Cache Storage named
//   "visivo-vr-<build>" and served from it afterwards. The page itself
//   (vr/, vr/index.html) is always asked for afresh.
// - Everything else (the loader's own files): the network first, the cache
//   when offline, so the installed app opens and says what is wrong.

const SCOPE = self.registration.scope;          // ends with '/'
const APP = `${SCOPE}app/`;
const SHELL_CACHE = 'visivo-loader-shell-v1';
const BUILD_CACHE = 'visivo-vr-';
const SHELL = ['./', 'index.html', 'style.css', 'manifest.webmanifest', 'icon-192.png', 'icon-512.png',
  'js/loader.js', 'js/loader-core.js', 'js/pair-wire.js', 'js/pair-transport.js'];
const ASK_TIMEOUT_MS = 30000;

self.addEventListener('install', (event) => {
  event.waitUntil(caches.open(SHELL_CACHE).then((c) => c.addAll(SHELL)).catch(() => {}).then(() => self.skipWaiting()));
});

self.addEventListener('activate', (event) => {
  event.waitUntil(self.clients.claim());
});

self.addEventListener('message', (event) => {
  const m = event.data || {};
  if (m.t === 'keep-build' && typeof m.build === 'string') {
    // Older builds' files are not needed by this backend's page any more.
    event.waitUntil(caches.keys().then((keys) => Promise.all(keys
      .filter((k) => k.startsWith(BUILD_CACHE) && k !== `${BUILD_CACHE}${m.build}`)
      .map((k) => caches.delete(k)))));
  }
});

self.addEventListener('fetch', (event) => {
  const url = event.request.url;
  if (url.startsWith(APP)) { event.respondWith(appFile(event.request)); return; }
  if (!url.startsWith(SCOPE) || event.request.method !== 'GET') return;
  event.respondWith(shellFile(event.request));
});

async function shellFile(request) {
  try {
    const resp = await fetch(request);
    if (resp.ok && !request.url.endsWith('/config.json')) {
      const copy = resp.clone();
      caches.open(SHELL_CACHE).then((c) => c.put(request, copy)).catch(() => {});
    }
    return resp;
  } catch (err) {
    const hit = await caches.match(request, { cacheName: SHELL_CACHE });
    if (hit) return hit;
    throw err;
  }
}

function plain(status, text) {
  return new Response(text, { status, headers: { 'Content-Type': 'text/plain; charset=utf-8', 'Cache-Control': 'no-store' } });
}

/** {tag, path}: app/<tag>/<rest> → the backend path /<rest> (query kept). */
function appTarget(url) {
  const rest = url.slice(APP.length);
  const slash = rest.indexOf('/');
  if (slash <= 0) return null;
  const tag = rest.slice(0, slash);
  if (!/^[a-z0-9]{4,32}$/.test(tag)) return null;
  const u = new URL(url);
  const pathname = u.pathname.slice(new URL(APP).pathname.length + slash);
  return { tag, path: `${pathname}${u.search}` };
}

async function appFile(request) {
  if (request.method !== 'GET' && request.method !== 'HEAD') return plain(405, 'only the page\'s files go through the loader');
  const t = appTarget(request.url);
  if (!t) return plain(404, 'not a file of the page');
  const m = /^\/vr\/b\/([A-Za-z0-9_-]{4,64})\//.exec(t.path);
  const key = m ? `${SCOPE}__build__${t.path.split('?')[0]}` : '';
  if (m) {
    const hit = await caches.match(key, { cacheName: `${BUILD_CACHE}${m[1]}` });
    if (hit) return hit;
  }
  const got = await askLoader(t.tag, t.path);
  if (!got) return plain(504, 'the loader holding the link to the computer did not answer');
  const resp = new Response(got.status === 204 || got.status === 304 ? null : got.body,
    { status: got.status >= 200 && got.status <= 599 ? got.status : 502, headers: got.headers || [] });
  // Kept for good only when the backend says the bytes are this build's
  // (immutable); a file that changed under the build's address is no-cache.
  const cc = resp.headers.get('cache-control') || '';
  if (m && got.status === 200 && /\bimmutable\b/i.test(cc) && !/no-(cache|store)/i.test(cc)) {
    const copy = resp.clone();
    caches.open(`${BUILD_CACHE}${m[1]}`).then((c) => c.put(key, copy)).catch(() => {});
  }
  return resp;
}

/** Asks every loader window; the one that owns `tag` answers with the file. */
async function askLoader(tag, path) {
  const all = await self.clients.matchAll({ type: 'window', includeUncontrolled: true });
  const loaders = all.filter((c) => !c.url.startsWith(APP));
  if (!loaders.length) return null;
  return new Promise((resolve) => {
    let left = loaders.length;
    const timer = setTimeout(() => resolve(null), ASK_TIMEOUT_MS);
    for (const c of loaders) {
      const mc = new MessageChannel();
      mc.port1.onmessage = (e) => {
        const r = e.data || {};
        mc.port1.close();
        if (r.notMine) {
          if (--left === 0) { clearTimeout(timer); resolve(null); }
          return;
        }
        clearTimeout(timer);
        resolve(r);
      };
      c.postMessage({ t: 'get', tag, path }, [mc.port2]);
    }
  });
}

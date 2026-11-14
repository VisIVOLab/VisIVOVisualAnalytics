// The module Web Worker of cube-prep.js (issue #246): a cube's skip grid,
// colour-range percentiles and apron, computed off the page's main thread.
// The voxels arrive transferred and always go back the same way, with the
// results' buffers, even when the computation fails.

import { prepareCube, preparedBuffers } from './cube-prep.js';

self.onmessage = (e) => {
  const { id, data, dims, encoding, opts } = e.data || {};
  try {
    const prep = prepareCube(data, dims, encoding, opts);
    self.postMessage({ id, data, prep }, [data.buffer, ...preparedBuffers(prep)]);
  } catch (err) {
    self.postMessage({ id, data, error: String((err && err.message) || err) }, data && data.buffer ? [data.buffer] : []);
  }
};
self.postMessage({ type: 'ready' });

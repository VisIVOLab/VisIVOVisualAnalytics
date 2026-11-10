// Colour maps as control points (matplotlib's viridis/inferno/magma sampled at
// nine stops), expanded to a 256-entry RGBA table for a 1-D texture.

const STOPS = {
  viridis: ['#440154', '#472d7b', '#3b528b', '#2c728e', '#21918c', '#28ae80', '#5ec962', '#addc30', '#fde725'],
  inferno: ['#000004', '#1b0c41', '#4a0c6b', '#781c6d', '#a52c60', '#cf4446', '#ed6925', '#fb9b06', '#fcffa4'],
  magma: ['#000004', '#1c1044', '#4f127b', '#812581', '#b5367a', '#e55964', '#fb8761', '#fec287', '#fcfdbf'],
  hot: ['#0b0000', '#600000', '#b40000', '#ff1000', '#ff6a00', '#ffc000', '#ffff20', '#ffff90', '#ffffff'],
  grey: ['#000000', '#ffffff'],
};

export const COLORMAP_NAMES = Object.keys(STOPS);

function hex(c) {
  return [1, 3, 5].map((i) => parseInt(c.slice(i, i + 2), 16));
}

/** 256 x RGBA8 table of the named map. */
export function colormapTable(name) {
  const stops = (STOPS[name] || STOPS.viridis).map(hex);
  const out = new Uint8Array(256 * 4);
  const n = stops.length - 1;
  for (let i = 0; i < 256; i++) {
    const x = (i / 255) * n, k = Math.min(n - 1, Math.floor(x)), f = x - k;
    for (let ch = 0; ch < 3; ch++) {
      out[i * 4 + ch] = Math.round(stops[k][ch] + (stops[k + 1][ch] - stops[k][ch]) * f);
    }
    out[i * 4 + 3] = 255;
  }
  return out;
}

// The standard HI measurements of a selection (issue #280) as text: the
// selection window's lines, the wrist card's and the record's labels. The
// numbers are the backend's (mask_stats_bin's `hi`, the definitions in
// app/compute/hi_measures.py); nothing is computed here. Pure: node runs the tests.

function fmt(v, digits = 3) {
  if (v === null || v === undefined || !Number.isFinite(v)) return 'n/a';
  const a = Math.abs(v);
  if (a !== 0 && (a < 1e-3 || a >= 1e5)) return v.toExponential(digits - 1);
  return v.toPrecision(digits);
}
const fixed = (v, d = 1) => (Number.isFinite(v) ? v.toFixed(d) : 'n/a');

/** Degrees as sexagesimal: hours for RA (2 decimals of a second), degrees
 *  for Dec (1 decimal of an arcsecond, signed). */
export function sexagesimal(deg, hours = false) {
  if (!Number.isFinite(deg)) return 'n/a';
  const places = hours ? 2 : 1;
  const unit = 10 ** places;
  const v = hours ? (((deg % 360) + 360) % 360) / 15 : Math.abs(deg);
  // Counted in the last digit shown, rounded once: 59.999 s carries into the minute.
  let n = Math.round(v * 3600 * unit);
  if (hours) n %= 24 * 3600 * unit;
  const s = n % (60 * unit);
  n = (n - s) / (60 * unit);
  const m = n % 60, h = (n - m) / 60;
  const pad2 = (k) => String(k).padStart(2, '0');
  const sign = hours ? '' : (deg < 0 ? '-' : '+');
  return `${sign}${pad2(h)}:${pad2(m)}:${(s / unit).toFixed(places).padStart(3 + places, '0')}`;
}

/** The `hi` object of a statistics answer, or null (an older backend). */
export function hiOf(stats) {
  return stats && stats.hi && typeof stats.hi === 'object' ? stats.hi : null;
}

/** 'flux 3.21 ± 0.04 Jy km/s' (the uncertainty when the noise was measured). */
export function fluxText(stats) {
  const hi = hiOf(stats);
  const f = stats ? stats.flux_jy_kms : null;
  const err = hi ? hi.flux_err_jy_kms : null;
  return `flux ${fmt(f)}${Number.isFinite(err) ? ` ± ${fmt(err, 2)}` : ''} Jy km/s`;
}

/** 'PA 44° E of N' (east of north, from the WCS), else the pixel grid's
 *  angle named as such, else '' (a round or unresolved footprint). */
export function paText(hi) {
  if (Number.isFinite(hi.pa_deg)) return `PA ${fixed(hi.pa_deg, 0)}° E of N`;
  if (Number.isFinite(hi.pa_grid_deg)) return `PA ${fixed(hi.pa_grid_deg, 0)}° on the pixel grid`;
  return '';
}

/** The selection window's lines after the flux: the channel correlation of
 *  its error, peak S/N and integrated S/N, widths and systemic velocity with
 *  the convention, centroid, size, the noise used, and the backend's notes. */
export function panelLines(stats) {
  const hi = hiOf(stats);
  if (!hi) return [];
  const out = [];
  if (hi.channel_correlation) out.push(hi.channel_correlation);
  out.push(`peak ${fmt(hi.peak)} ${hi.peak_unit || ''}`
    + `${Number.isFinite(hi.peak_snr) ? `, peak S/N ${fixed(hi.peak_snr)}` : ''}`
    + `${Number.isFinite(hi.snr_integrated) ? `, integrated S/N ${fixed(hi.snr_integrated)}` : ''}`);
  if (Number.isFinite(hi.w20_kms)) {
    out.push(`W20 ${fixed(hi.w20_kms)}  W50 ${fixed(hi.w50_kms)}  Vsys ${fixed(hi.vsys_kms)} km/s`);
    out.push(`velocity: ${hi.velocity_convention || 'convention unknown'}${hi.velocity_frame ? `, ${hi.velocity_frame}` : ''}`);
  }
  if (Number.isFinite(hi.centroid_ra_deg)) {
    out.push(`centroid ${sexagesimal(hi.centroid_ra_deg, true)} ${sexagesimal(hi.centroid_dec_deg)}`
      + `${Number.isFinite(hi.centroid_velocity_kms) ? `, ${fixed(hi.centroid_velocity_kms)} km/s` : ''}`);
  }
  if (Number.isFinite(hi.major_axis_arcsec)) {
    const pa = paText(hi);
    out.push(`size ${fixed(hi.major_axis_arcsec)}" × ${fixed(hi.minor_axis_arcsec)}" (semi-axes)${pa ? `, ${pa}` : ''}`);
  }
  if (Number.isFinite(hi.noise_sigma)) {
    out.push(`σ ${fmt(hi.noise_sigma)} ${hi.noise_unit || ''} (${String(hi.noise_source || '').split(':')[0].split(' (')[0]})`);
  }
  for (const n of hi.notes || []) out.push(n);
  return out;
}

/** The wrist card's second and third lines for a selection. */
export function wristLines(stats) {
  const hi = hiOf(stats);
  if (!hi) return null;
  const second = Number.isFinite(hi.vsys_kms)
    ? `Vsys ${fixed(hi.vsys_kms)} W50 ${fixed(hi.w50_kms, 0)} km/s`
    : `peak S/N ${fixed(hi.peak_snr)}`;
  return [fluxText(stats), second];
}

/** The record's Provenance labels (the desktop shows them with the recipe). */
export function recordParams(stats) {
  const hi = hiOf(stats);
  if (!hi) return {};
  const p = {};
  p['Flux (Jy km/s)'] = Number.isFinite(hi.flux_err_jy_kms)
    ? `${fmt(hi.flux_jy_kms, 4)} ± ${fmt(hi.flux_err_jy_kms, 2)}` : fmt(hi.flux_jy_kms, 4);
  if (Number.isFinite(hi.flux_err_indep_jy_kms)) {
    p['Flux error'] = `${hi.channel_correlation}; independent channels ± ${fmt(hi.flux_err_indep_jy_kms, 2)}`;
  }
  if (Number.isFinite(hi.noise_sigma)) p['Noise σ'] = `${fmt(hi.noise_sigma, 4)} ${hi.noise_unit || ''} (${hi.noise_source})`;
  p.Peak = `${fmt(hi.peak, 4)} ${hi.peak_unit || ''}`.trim();
  if (Number.isFinite(hi.peak_snr)) p['Peak S/N'] = fixed(hi.peak_snr);
  if (Number.isFinite(hi.snr_integrated)) p['Integrated S/N'] = fixed(hi.snr_integrated);
  if (Number.isFinite(hi.w20_kms)) {
    p['W20 / W50 (km/s)'] = `${fixed(hi.w20_kms, 2)} / ${fixed(hi.w50_kms, 2)}`;
    p['Systemic velocity (km/s)'] = `${fixed(hi.vsys_kms, 2)} (${hi.vsys_definition}; ${hi.velocity_convention}${hi.velocity_frame ? `, ${hi.velocity_frame}` : ''})`;
  }
  if (hi.notes && hi.notes.length) p['Measurement notes'] = hi.notes.join('; ');
  if (Number.isFinite(hi.centroid_ra_deg)) {
    p['Centroid (RA, Dec)'] = `${sexagesimal(hi.centroid_ra_deg, true)}, ${sexagesimal(hi.centroid_dec_deg)} (${hi.centroid_frame}; ${fixed(hi.centroid_ra_deg, 6)}°, ${fixed(hi.centroid_dec_deg, 6)}°)`;
  }
  if (Number.isFinite(hi.centroid_velocity_kms)) p['Centroid velocity (km/s)'] = fixed(hi.centroid_velocity_kms, 2);
  if (Number.isFinite(hi.major_axis_arcsec)) {
    const pa = Number.isFinite(hi.pa_deg) ? `, PA ${fixed(hi.pa_deg, 1)}° east of north`
      : (Number.isFinite(hi.pa_grid_deg) ? `, PA ${fixed(hi.pa_grid_deg, 1)}° on the pixel grid` : '');
    p['Size (arcsec)'] = `${fixed(hi.major_axis_arcsec, 2)} × ${fixed(hi.minor_axis_arcsec, 2)}${pa} (${hi.size_definition})`;
  }
  return p;
}

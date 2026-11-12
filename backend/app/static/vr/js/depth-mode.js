// How a refined frame may be warped to the current pose (issue #187).
//
// A backend whose GL cannot read depth back (GLES, some Mesa or cluster EGL
// stacks) sends colour-only stereo frames; their header says
// "reprojection": "pose" and why ("depth_unavailable"). Such a frame is warped
// by the pose alone: through a plane at the cube's centre, which is exact for
// a turn of the head (a rotation moves every depth alike) and wrong by the
// parallax of the cube's depth for a step sideways. That error is
// f x step x |1/d - 1/d_centre|: for a 0.5 m cube 0.75 m away on a Quest 3
// eye (f ~ 800 px), about 5 px at the cube's near face for a 1 cm step and
// ~21 px for 4 cm (the step a frame WITH depth may be shown for, where the
// depth removes this error). So a frame without depth is shown only while the
// eye has moved less than POSE_ONLY_MAX_METRES since its pose. The error
// grows sharply as the eye nears the cube (1/d grows without bound): leaning
// into the cube, or with the eye inside it, frames without depth misplace
// the near emission much more. Pure: backend/tests/vr/depth-mode.test.mjs.

export const POSE_ONLY_MAX_METRES = 0.01;

/** {mode: 'depth' | 'pose', reason} of a frame header ('' when it has depth).
 *  A backend older than the "reprojection" field: from the depth alone. */
export function frameReprojection(header) {
  const h = header || {};
  const hasDepth = !!h.depth;
  const mode = h.reprojection === 'pose' || !hasDepth ? 'pose' : 'depth';
  const reason = mode === 'depth' ? '' : String(h.depth_unavailable || 'the frame carries no depth');
  return { mode, reason };
}

/** The limits a shown frame is held to (refine.js SHOW_DEFAULTS), tightened
 *  for a frame without depth. */
export function showLimits(header, base) {
  if (frameReprojection(header).mode === 'depth') return base;
  return { ...base, maxMetres: Math.min(base.maxMetres, POSE_ONLY_MAX_METRES) };
}

/** The HUD's lines for the frame on show: none with depth, else what the
 *  warp is and why (the reason over as many lines as it takes, at most 3). */
export function reprojectionHud(header, width = 60) {
  const r = frameReprojection(header);
  if (r.mode === 'depth') return [];
  const lines = ['  reprojection: pose only (no depth)'];
  let rest = `why: ${r.reason}`;
  while (rest && lines.length < 4) {
    let cut = rest.length <= width - 4 ? rest.length : rest.lastIndexOf(' ', width - 4);
    if (cut <= 0) cut = width - 4;
    lines.push(`    ${rest.slice(0, cut)}`);
    rest = rest.slice(cut).trimStart();
  }
  return lines;
}

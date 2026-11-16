// The clipping plane of a simulation volume (issue #369), run by
// vr_simulation.mjs while its density is on show (vr_lifecycle.mjs
// --clip-plane, and --regressions): the Range page's plane buttons turn the
// plane on, flip it, make it a slab and reset it; each changes what is drawn
// as it should (about half of the volume kept by a plane through its centre,
// less by a slab, all of it again with the plane off), the command for the
// render session follows, the gizmo's lines are drawn with the tool in use,
// and an undo step gives the plane back. A few draws: cheap enough for CI.

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

export async function runClipPlaneChecks({ c, check, results = {} }) {
  const L = 'clip plane';
  const drawn = async () => { await sleep(150); return (await c.evaluate('window.__vrDrawnFraction()')).fraction; };
  const get = () => c.evaluate('window.__vrPlane.get()');
  const press = (id) => c.evaluate(`window.__vrMenu.act({ type: 'press', id: ${JSON.stringify(id)} })`);
  await c.evaluate(`(() => { window.__vrMenu.ui.page = 'range'; window.__vrMenu.layout(); })()`);
  const s0 = await get();
  check(`${L}: offered on a simulation volume, off to begin with`, s0.available && s0.plane && !s0.plane.on && !s0.shader
    && s0.command.plane === null, JSON.stringify({ available: s0.available, on: s0.plane && s0.plane.on }));
  const whole = await drawn();
  await press('plane:on');
  const half = await drawn();
  const s1 = await get();
  check(`${L}: on, through the centre across x: about half the volume drawn, the command sent with it`,
    s1.plane.on && s1.shader && s1.shader.mode === 1 && s1.command.plane && s1.command.plane.normal.join() === '1,0,0'
    && half > 0.25 * whole && half < 0.8 * whole,
    `${(100 * whole).toFixed(2)} % → ${(100 * half).toFixed(2)} % of the canvas; origin ${s1.plane.origin.map((v) => v.toFixed(1)).join(', ')}`);
  await press('plane:flip');
  const other = await drawn();
  check(`${L}: flipped: the other half (normal -x)`, (await get()).command.plane.normal[0] === -1 && other > 0.25 * whole && other < 0.8 * whole,
    `${(100 * other).toFixed(2)} %`);
  // 45° across x and y, through the centre (the issue's case), set as the grab would leave it.
  const tilted = await c.evaluate(`(() => { const p = window.__vrPlane.set({ normal: [Math.SQRT1_2, Math.SQRT1_2, 0] });
    return { normal: p.normal, cmd: window.__vrPlane.get().command.plane.normal }; })()`);
  const at45 = await drawn();
  check(`${L}: at 45°: drawn, the command's normal (0.71, 0.71, 0)`, at45 > 0.1 * whole && at45 < 0.9 * whole
    && Math.abs(tilted.cmd[0] - Math.SQRT1_2) < 1e-9 && Math.abs(tilted.cmd[1] - Math.SQRT1_2) < 1e-9, `${(100 * at45).toFixed(2)} %`);
  await press('plane:snap');
  check(`${L}: snapped to the nearest axis`, (await get()).plane.normal.join() === '1,0,0', JSON.stringify((await get()).plane.normal));
  await press('plane:slab');
  const slab = await drawn();
  const s2 = await get();
  check(`${L}: a slab: less drawn than the half, the thickness sent`, s2.shader.mode === 2 && s2.command.plane.thickness > 0
    && slab > 0 && slab < half, `${(100 * slab).toFixed(2)} %, ${s2.command.plane.thickness} px thick`);
  // The tool in use: the gizmo's lines (the section's outline, both faces of the slab).
  await c.evaluate('window.__vrPlane.tool()');
  await sleep(200);
  const s3 = await get();
  check(`${L}: the Clipping plane tool draws the gizmo`, s3.toolOn && s3.lines > 0, `${s3.lines} line vertices`);
  // Undo gives back the plane before the slab.
  await c.evaluate(`window.__vrMenu.act({ type: 'press', id: 'undo' })`);
  const s4 = await get();
  check(`${L}: undo gives back the plane before the slab`, s4.plane.on && !s4.plane.slab, JSON.stringify({ on: s4.plane.on, slab: s4.plane.slab }));
  await press('plane:reset');
  await press('plane:on');
  const back = await drawn();
  const s5 = await get();
  check(`${L}: off: the whole volume again, the command a null plane`, !s5.plane.on && s5.command.plane === null
    && Math.abs(back - whole) < 0.05 * whole + 1e-4, `${(100 * back).toFixed(2)} % vs ${(100 * whole).toFixed(2)} %`);
  results.fractions = { whole, half, other, at45, slab, back };
  return results;
}

// The page's scrubbing (epic #166): the same cases as the backend's scrub()
// (backend/tests/vr/scrub_cases.json, also read by test_vr_lifecycle.py).
// node --test backend/tests/vr/diagnostics.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { scrubText } from '../../app/static/vr/js/diagnostics.js';

const { cases } = JSON.parse(readFileSync(new URL('./scrub_cases.json', import.meta.url), 'utf8'));

for (const c of cases) {
  test(`scrub: ${c.name}`, () => {
    const out = scrubText(c.input, c.secrets || []);
    for (const a of c.absent || []) assert.ok(!out.includes(a), `${JSON.stringify(a)} left in ${JSON.stringify(out)}`);
    for (const p of c.present || []) assert.ok(out.includes(p), `${JSON.stringify(p)} missing from ${JSON.stringify(out)}`);
    if (c.expected !== undefined) assert.equal(out, c.expected);
  });
}

test('scrubbing stays fast on adversarial inputs (no catastrophic backtracking)', () => {
  const { adversarial } = JSON.parse(readFileSync(new URL('./scrub_cases.json', import.meta.url), 'utf8'));
  const worst = [];
  for (const unit of adversarial.units) {
    const text = unit.repeat(Math.ceil(adversarial.length / unit.length)).slice(0, adversarial.length);
    scrubText(text.slice(0, 50));                     // warm the regexes up
    const t0 = performance.now();
    scrubText(text, ['secret-token-xyz']);
    const ms = performance.now() - t0;
    worst.push([unit, ms]);
    assert.ok(ms < adversarial.max_ms, `${JSON.stringify(unit)}×n: ${ms.toFixed(1)} ms`);
  }
});

test('known secrets are replaced over the whole text, before the cut', () => {
  const secret = 'Zx81secretTOKEN';
  const long = `${'x'.repeat(3000)} ${secret}`;
  assert.ok(!scrubText(long, [secret]).includes(secret));
  assert.ok(scrubText(long, [secret]).length <= 1200);
});

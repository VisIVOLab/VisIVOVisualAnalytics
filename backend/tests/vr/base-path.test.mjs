// The page under a path prefix (issue #187):
// node --test backend/tests/vr/base-path.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';

import { basePrefix, apiUrl, wsUrl } from '../../app/static/vr/js/base-path.js';

test('the prefix is everything before the page\'s /vr segment', () => {
  assert.equal(basePrefix('/vr/'), '');
  assert.equal(basePrefix('/vr'), '');
  assert.equal(basePrefix('/vr/index.html'), '');
  assert.equal(basePrefix('/visivo/vr/'), '/visivo');
  assert.equal(basePrefix('/node/r33c1s02/proxy/8000/vr/index.html'), '/node/r33c1s02/proxy/8000');
  assert.equal(basePrefix('/a%20b/vr/'), '/a%20b');
  // The last /vr segment counts: a prefix that has one of its own.
  assert.equal(basePrefix('/vr/x/vr/'), '/vr/x');
  // Not a /vr page (tests, a page served elsewhere): the root.
  assert.equal(basePrefix('/'), '');
  assert.equal(basePrefix(''), '');
  assert.equal(basePrefix(undefined), '');
  assert.equal(basePrefix('/vrx/'), '');
  assert.equal(basePrefix('//vr/'), '');
});

test('a protocol-relative path never becomes another host', () => {
  // '//evil.com/vr/' as a prefix would make fetch('//evil.com/v1/...') go to
  // evil.com: slashes are collapsed, and the prefix always starts with one '/'.
  assert.equal(basePrefix('//evil.com/vr/'), '/evil.com');
  assert.equal(basePrefix('///evil.com//vr/'), '/evil.com');
  assert.equal(basePrefix('/a//b/vr/'), '/a/b');
  const loc = { pathname: '//evil.com/vr/', protocol: 'http:', host: 'localhost:8000' };
  assert.ok(!apiUrl('/v1/ping', loc).startsWith('//'));
  assert.equal(wsUrl('/v1/x', loc), 'ws://localhost:8000/evil.com/v1/x');
});

test('the WebSocket address carries no token of its own', () => {
  const loc = { pathname: '/p/vr/', protocol: 'http:', host: 'localhost:1' };
  assert.equal(wsUrl('/v1/render/s/ws', loc), 'ws://localhost:1/p/v1/render/s/ws');
});

test('API and WebSocket addresses carry the prefix', () => {
  const loc = { pathname: '/visivo/vr/', protocol: 'http:', host: 'localhost:18900' };
  assert.equal(apiUrl('/v1/ping', loc), '/visivo/v1/ping');
  assert.equal(wsUrl('/v1/render/s/ws?token=t', loc), 'ws://localhost:18900/visivo/v1/render/s/ws?token=t');
  const root = { pathname: '/vr/', protocol: 'https:', host: 'localhost:8443' };
  assert.equal(apiUrl('/v1/ping', root), '/v1/ping');
  assert.equal(wsUrl('/v1/x', root), 'wss://localhost:8443/v1/x');
});

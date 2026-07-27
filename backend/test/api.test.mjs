/*
 * Runs the handlers directly with a stub req/res. No server, no network — the
 * point is the approved-only filter and the 404-for-hidden behaviour, both of
 * which are policy, not plumbing.
 */
import assert from 'node:assert/strict';
import listMods from '../api/mods.js';
import getMod from '../api/mods/[id].js';
import registry from '../registry/mods.js';

function stubRes() {
  const res = {
    headers: {},
    statusCode: 0,
    body: null,
    setHeader(k, v) { this.headers[k] = v; },
    status(code) { this.statusCode = code; return this; },
    send(payload) { this.body = payload; return this; },
  };
  return res;
}

async function call(handler, { method = 'GET', query = {} } = {}) {
  const res = stubRes();
  await handler({ method, query }, res);
  return { res, json: res.body ? JSON.parse(res.body) : null };
}

let failures = 0;
async function test(name, fn) {
  try { await fn(); console.log(`  ok   ${name}`); }
  catch (error) { failures++; console.log(`  FAIL ${name}\n       ${error.message}`); }
}

console.log('registry API');

await test('lists only approved mods', async () => {
  const { res, json } = await call(listMods);
  assert.equal(res.statusCode, 200);
  const expected = registry.mods.filter((m) => m.review?.status === 'approved');
  assert.equal(json.count, expected.length);
  assert.equal(json.mods.length, expected.length);
});

await test('a pending mod is invisible in the list and 404 by id', async () => {
  // Mutate a copy of the live data so the assertion covers the real filter.
  const victim = registry.mods[0];
  const original = victim.review.status;
  victim.review.status = 'pending';
  try {
    const { json } = await call(listMods);
    assert.ok(
      !json.mods.some((m) => m.id === victim.id),
      'pending mod leaked into the list'
    );
    const detail = await call(getMod, { query: { id: victim.id } });
    assert.equal(detail.res.statusCode, 404, 'pending mod reachable by id');
  } finally {
    victim.review.status = original;
  }
});

await test('never exposes reviewer notes to clients', async () => {
  const { json } = await call(listMods);
  for (const mod of json.mods) {
    assert.equal(mod.review, undefined, `${mod.id} exposed its review block`);
  }
});

await test('detail returns the requested mod', async () => {
  const { res, json } = await call(getMod, { query: { id: 'night-mode' } });
  assert.equal(res.statusCode, 200);
  assert.equal(json.id, 'night-mode');
  assert.ok(json.long.length > 0);
});

await test('unknown id is 404', async () => {
  const { res } = await call(getMod, { query: { id: 'nope' } });
  assert.equal(res.statusCode, 404);
});

await test('type filter narrows the list', async () => {
  const { json } = await call(listMods, { query: { type: 'native' } });
  assert.ok(json.mods.length > 0);
  assert.ok(json.mods.every((m) => m.type === 'native'));
});

await test('search matches name, description and author', async () => {
  const { json } = await call(listMods, { query: { q: 'дождь' } });
  assert.equal(json.mods.length, 1);
  assert.equal(json.mods[0].id, 'screen-rain');
});

await test('non-GET is rejected', async () => {
  const { res } = await call(listMods, { method: 'POST' });
  assert.equal(res.statusCode, 405);
});

await test('responses are CORS-readable by the game client', async () => {
  const { res } = await call(listMods);
  assert.equal(res.headers['Access-Control-Allow-Origin'], '*');
});

console.log(failures ? `\n${failures} failing` : '\nall passing');
process.exit(failures ? 1 : 0);

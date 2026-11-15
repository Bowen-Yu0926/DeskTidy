/**
 * Runs the Pages Function against an in-memory SQLite DB to prove the API
 * contract before deploying. Node 22+ strips the TS types on import.
 *
 *   node server/d1/test/vault_api_test.mjs
 */
import { DatabaseSync } from 'node:sqlite';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { pathToFileURL } from 'node:url';

// Run from the repo root: node server/d1/test/vault_api_test.mjs
const mod = await import(
  pathToFileURL(join(process.cwd(), 'src/vault_worker.ts')).href
);
const onRequest = mod.default.fetch;

const db = new DatabaseSync(':memory:');
db.exec(readFileSync('server/d1/schema.sql', 'utf8'));
db.exec(readFileSync('server/d1/seed.sql', 'utf8'));

// Minimal D1 shim over node:sqlite.
function stmt(sql) {
  const s = db.prepare(sql);
  return {
    bind(...args) {
      return {
        async first() {
          const row = s.get(...args);
          return row === undefined ? null : row;
        },
        async all() {
          return { results: s.all(...args) };
        },
        async run() {
          const r = s.run(...args);
          return { meta: { last_row_id: Number(r.lastInsertRowid), changes: Number(r.changes) } };
        },
      };
    },
    async first() { const r = s.get(); return r === undefined ? null : r; },
    async all() { return { results: s.all() }; },
    async run() { const r = s.run(); return { meta: { last_row_id: Number(r.lastInsertRowid), changes: Number(r.changes) } }; },
  };
}
const DB = { prepare: stmt, async batch(list) { return Promise.all(list.map((x) => x.run())); } };

import { readFileSync as rf } from 'node:fs';
const env = { DB, VAULT_DATA_KEY: rf('.vault_key','utf8').trim() };

let pass = 0, fail = 0;
function check(name, cond, extra) {
  if (cond) { pass++; console.log('  ok   ' + name); }
  else { fail++; console.log('  FAIL ' + name + (extra ? '  ' + JSON.stringify(extra) : '')); }
}

async function call(method, path, { body, token } = {}) {
  const headers = {};
  if (body) headers['Content-Type'] = 'application/json';
  if (token) headers['Authorization'] = 'Bearer ' + token;
  const req = new Request('https://x.pages.dev/api/' + path, {
    method, headers, body: body ? JSON.stringify(body) : undefined,
  });
  const res = await onRequest(req, env, {});
  return { status: res.status, json: await res.json() };
}

// Values come from the seed you generated with migrate_vault_to_d1.mjs.
// Supply them via the environment so no real login or credential is committed:
//
//   TEST_LOGIN=... TEST_PASSWORD=... TEST_DISPLAY=... \
//   TEST_CRED_USER=... TEST_CRED_PASS=... TEST_NOTE_TEXT=... \
//   node server/d1/test/vault_api_test.mjs
const PASSWORD = process.env.TEST_PASSWORD || '';
const LOGIN = process.env.TEST_LOGIN || '';
const DISPLAY = process.env.TEST_DISPLAY || '';
const CRED_USER = process.env.TEST_CRED_USER || '';
const CRED_PASS = process.env.TEST_CRED_PASS || '';
const NOTE_TEXT = process.env.TEST_NOTE_TEXT || '';

if (!LOGIN || !PASSWORD) {
  console.error(
    'set TEST_LOGIN and TEST_PASSWORD (plus TEST_DISPLAY, TEST_CRED_USER,\n' +
    'TEST_CRED_PASS, TEST_NOTE_TEXT) to match the seed you generated.',
  );
  process.exit(1);
}

console.log('\n-- health --');
let r = await call('GET', '');
check('health ok', r.json.ok === true && r.json.data.runtime === 'workers-d1', r.json);

console.log('\n-- login --');
r = await call('POST', 'auth/login', { body: { login: LOGIN, password: 'wrong' } });
check('wrong password rejected', r.status === 401 && r.json.error === 'bad_credentials', r.json);

r = await call('POST', 'auth/login', { body: { login: LOGIN, password: PASSWORD } });
check('correct password accepted', r.json.ok === true && !!r.json.data.token, r.json);
const token = r.json.data?.token;
check('display_name preserved', r.json.data?.display_name === DISPLAY, r.json.data);

console.log('\n-- credentials --');
r = await call('GET', 'credentials', { token });
check('list ok', r.json.ok === true, r.json);
check('18 credentials migrated', r.json.data?.items?.length === 18, { got: r.json.data?.items?.length });
const first = r.json.data?.items?.[0];
check('first is pinned DeskTidy服务器',
  first?.title === 'DeskTidy服务器' && first?.pinned === true, first);
check('plaintext username round-trips', first?.username === CRED_USER, first);
check('plaintext password round-trips', first?.password === CRED_PASS, first);

console.log('\n-- notes --');
r = await call('GET', 'notes', { token });
check('note list ok', r.json.data?.items?.length === 1, r.json.data);
const noteId = r.json.data?.items?.[0]?.id;
r = await call('GET', 'notes/' + noteId, { token });
check('note body intact', (r.json.data?.body || '').includes(NOTE_TEXT), r.json.data);
check('note multiline preserved', (r.json.data?.body || '').split('\n').length > 4, r.json.data);

console.log('\n-- create / update / delete credential --');
r = await call('POST', 'credentials', {
  token, body: { title: '测试条目', username: 'u1', password: 'p1', url: 'http://x', note: 'n1', pinned: false },
});
check('create ok', r.json.ok === true && r.json.data?.id > 18, r.json);
const newId = r.json.data?.id;
check('sort_order auto-incremented', r.json.data?.sort_order === 18, r.json.data);

r = await call('PUT', 'credentials/' + newId, { token, body: { title: '改过', username: 'u2', password: 'p2', url: '', note: '' } });
check('update ok', r.json.data?.title === '改过' && r.json.data?.username === 'u2', r.json.data);

r = await call('PUT', 'credentials/' + newId, { token, body: { pinned: true } });
check('pin-only update keeps title', r.json.data?.title === '改过' && r.json.data?.pinned === true, r.json.data);

r = await call('DELETE', 'credentials/' + newId, { token });
check('delete ok', r.json.data?.deleted === true, r.json);

console.log('\n-- reorder --');
const ids = (await call('GET', 'credentials', { token })).json.data.items.map((i) => i.id);
r = await call('POST', 'credentials/reorder', { token, body: { ids: [...ids].reverse() } });
check('reorder ok', r.json.data?.reordered === true, r.json);
r = await call('GET', 'credentials', { token });
const after = r.json.data.items.filter((i) => !i.pinned).map((i) => i.id);
check('reorder applied to unpinned', after[0] === ids[ids.length - 1], { after: after.slice(0, 3) });

console.log('\n-- auth guards --');
r = await call('GET', 'credentials');
check('no token -> 401', r.status === 401 && r.json.error === 'unauthorized', r.json);
r = await call('GET', 'credentials', { token: 'deadbeef' });
check('bad token -> 401', r.status === 401, r.json);
r = await call('GET', 'nope');
check('unknown route -> 404', r.status === 404, r.json);

console.log('\n-- register --');
r = await call('POST', 'auth/register', { body: { login: LOGIN, password: 'abc123', display_name: '重复' } });
check('duplicate login -> 409', r.status === 409 && r.json.error === 'login_taken', r.json);
r = await call('POST', 'auth/register', { body: { login: 'new@example.com', password: 'abc123', display_name: '新用户' } });
check('new account created', r.json.ok === true && !!r.json.data.token, r.json);
const t2 = r.json.data?.token;
r = await call('GET', 'credentials', { token: t2 });
check('new account sees no credentials', r.json.data?.items?.length === 0, r.json.data);

console.log('\n-- logout --');
r = await call('POST', 'auth/logout', { token: t2 });
check('logout ok', r.json.data?.logged_out === true, r.json);
r = await call('GET', 'credentials', { token: t2 });
check('token revoked', r.status === 401, r.json);

console.log('\n================');
console.log(`pass ${pass}  fail ${fail}`);
process.exit(fail ? 1 : 0);

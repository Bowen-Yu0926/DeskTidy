/**
 * DeskTidy vault API — Cloudflare Pages Functions + D1.
 *
 * Drop-in replacement for the PHP backend in server/desktidy-vault/. The client
 * (native/src/vault_http.cpp) talks to the same paths and the same response
 * envelope, so nothing on the client changes except the API base URL:
 *
 *   success: { "ok": true, "data": { ... } }
 *   failure: { "ok": false, "error": "<code>", "message": "<text>" }
 *
 * Route: /api/<rest> — see route() at the bottom.
 *
 * Storage of the four *_enc columns:
 *   enc_ver = 0  server-side AES-256-GCM, key from VAULT_DATA_KEY (32 bytes,
 *                base64). This is the default: the database never holds
 *                plaintext credentials.
 *   enc_ver = 1  client-side E2E ciphertext ("v1:..."), passed through
 *                verbatim — the server cannot read it even with the key.
 *
 * Packed layout for enc_ver=0 is base64(iv[12] | tag[16] | ciphertext), the
 * same shape the repo's PHP backend (server/desktidy-vault/lib/crypto.php)
 * produced, so either backend can read the other's rows.
 */

const TOKEN_TTL_DAYS = 90;
const MAX_NOTE_BODY = 2_000_000;

// ---------------------------------------------------------------- responses

const CORS = {
  'Access-Control-Allow-Origin': '*',
  'Access-Control-Allow-Headers': 'Authorization, Content-Type, X-Vault-Token',
  'Access-Control-Allow-Methods': 'GET, POST, PUT, PATCH, DELETE, OPTIONS',
};

function ok(data, code = 200) {
  const payload = data === undefined ? { ok: true } : { ok: true, data };
  return new Response(JSON.stringify(payload), {
    status: code,
    headers: { 'Content-Type': 'application/json; charset=utf-8', ...CORS },
  });
}

function fail(error, message, code = 400) {
  return new Response(
    JSON.stringify({ ok: false, error, message }),
    {
      status: code,
      headers: { 'Content-Type': 'application/json; charset=utf-8', ...CORS },
    },
  );
}

// ---------------------------------------------------------------- helpers

const enc = new TextEncoder();
const dec = new TextDecoder();

function b64encode(bytes) {
  let s = '';
  const u8 = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
  for (let i = 0; i < u8.length; i++) s += String.fromCharCode(u8[i]);
  return btoa(s);
}

function b64decode(str) {
  const bin = atob(str);
  const out = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
  return out;
}

function nowUtc() {
  // "YYYY-MM-DD HH:MM:SS" — same shape the PHP backend wrote, so clients that
  // compare these strings keep working.
  return new Date().toISOString().replace('T', ' ').slice(0, 19);
}

async function sha256Hex(text) {
  const d = await crypto.subtle.digest('SHA-256', enc.encode(text));
  return [...new Uint8Array(d)].map((b) => b.toString(16).padStart(2, '0')).join('');
}

async function readJson(request) {
  try {
    const text = await request.text();
    if (!text.trim()) return {};
    const v = JSON.parse(text);
    return v && typeof v === 'object' ? v : {};
  } catch {
    return {};
  }
}

// ------------------------------------------------------- password hashing
//
// The PHP backend used password_hash()/password_verify(), which is bcrypt.
// Web Crypto has no bcrypt, so this backend stores its own PBKDF2 format:
//
//   pbkdf2$<iterations>$<salt-b64>$<hash-b64>
//
// verifyPassword() also recognises legacy bcrypt hashes and rejects them with a
// clear error rather than a silent "wrong password" — those accounts must reset.

const PBKDF2_ITERATIONS = 210_000;

async function pbkdf2(password, salt, iterations) {
  const base = await crypto.subtle.importKey(
    'raw', enc.encode(password), 'PBKDF2', false, ['deriveBits'],
  );
  const bits = await crypto.subtle.deriveBits(
    { name: 'PBKDF2', salt, iterations, hash: 'SHA-256' }, base, 256,
  );
  return new Uint8Array(bits);
}

// Format: pbkdf2:<iterations>:<salt-b64>:<hash-b64>
//
// ':' rather than '$' on purpose. The seed script is applied through the D1
// dashboard console, which treats '$' as a bound-parameter placeholder and
// rejects the statement ("unrecognized token: \"$\""). SQLite itself is fine
// with either; ':' keeps the same hash readable by any SQL tool.
const HASH_PREFIX = 'pbkdf2:';

async function hashPassword(password) {
  const salt = crypto.getRandomValues(new Uint8Array(16));
  const hash = await pbkdf2(password, salt, PBKDF2_ITERATIONS);
  return `${HASH_PREFIX}${PBKDF2_ITERATIONS}:${b64encode(salt)}:${b64encode(hash)}`;
}

/** @returns {'ok'|'mismatch'|'legacy'} */
async function verifyPassword(password, stored) {
  if (!stored) return 'mismatch';
  if (stored.startsWith('$2y$') || stored.startsWith('$2a$') ||
      stored.startsWith('$2b$')) {
    return 'legacy';  // bcrypt from the PHP backend; cannot be verified here
  }
  const parts = stored.split(':');
  if (parts.length !== 4 || parts[0] !== 'pbkdf2') return 'mismatch';
  const iterations = parseInt(parts[1], 10);
  if (!Number.isFinite(iterations) || iterations < 1) return 'mismatch';
  const salt = b64decode(parts[2]);
  const want = b64decode(parts[3]);
  const got = await pbkdf2(password, salt, iterations);
  if (got.length !== want.length) return 'mismatch';
  let diff = 0;
  for (let i = 0; i < got.length; i++) diff |= got[i] ^ want[i];
  return diff === 0 ? 'ok' : 'mismatch';
}

// ------------------------------------------------ server-side AES-256-GCM
//
// Only for enc_ver=0 rows (data written by the old PHP backend, or by a client
// that has not enabled zero-knowledge mode). Format: base64(iv|tag|cipher).

async function dataKey(env) {
  const raw = env.VAULT_DATA_KEY;
  if (!raw) throw new Error('VAULT_DATA_KEY is not configured');
  const bytes = b64decode(raw);
  if (bytes.length !== 32) throw new Error('VAULT_DATA_KEY must be 32 bytes');
  return crypto.subtle.importKey('raw', bytes, 'AES-GCM', false, [
    'encrypt', 'decrypt',
  ]);
}

async function aesEncrypt(env, plain) {
  const key = await dataKey(env);
  const iv = crypto.getRandomValues(new Uint8Array(12));
  const cipher = new Uint8Array(
    await crypto.subtle.encrypt(
      { name: 'AES-GCM', iv, tagLength: 128 }, key, enc.encode(plain),
    ),
  );
  // WebCrypto appends the tag to the ciphertext; the PHP layout puts it before.
  const body = cipher.slice(0, cipher.length - 16);
  const tag = cipher.slice(cipher.length - 16);
  const packed = new Uint8Array(12 + 16 + body.length);
  packed.set(iv, 0);
  packed.set(tag, 12);
  packed.set(body, 28);
  return b64encode(packed);
}

async function aesDecrypt(env, encoded) {
  let raw;
  try {
    raw = b64decode(encoded);
  } catch {
    return '';
  }
  if (raw.length < 28) return '';
  const key = await dataKey(env);
  const iv = raw.slice(0, 12);
  const tag = raw.slice(12, 28);
  const body = raw.slice(28);
  const joined = new Uint8Array(body.length + tag.length);
  joined.set(body, 0);
  joined.set(tag, body.length);
  try {
    const plain = await crypto.subtle.decrypt(
      { name: 'AES-GCM', iv, tagLength: 128 }, key, joined,
    );
    return dec.decode(plain);
  } catch {
    return '';
  }
}

// ---------------------------------------------------------------- auth

function bearerToken(request) {
  const h = request.headers.get('Authorization') || '';
  const m = /^\s*Bearer\s+(\S+)\s*$/i.exec(h);
  if (m) return m[1];
  const alt = request.headers.get('X-Vault-Token');
  return alt || null;
}

async function issueToken(env, userId) {
  const token = [...crypto.getRandomValues(new Uint8Array(32))]
    .map((b) => b.toString(16).padStart(2, '0')).join('');
  const hash = await sha256Hex(token);
  const expires = new Date(Date.now() + TOKEN_TTL_DAYS * 86400_000)
    .toISOString().replace('T', ' ').slice(0, 19);
  await env.DB.prepare(
    'INSERT INTO vault_tokens (user_id, token_hash, expires_at) VALUES (?, ?, ?)',
  ).bind(userId, hash, expires).run();
  return token;
}

/** Resolves the caller, or throws an ApiError carrying the response. */
async function requireUser(request, env) {
  const token = bearerToken(request);
  if (!token) throw new ApiError(fail('unauthorized', '请先登录', 401));
  const hash = await sha256Hex(token);
  const row = await env.DB.prepare(
    `SELECT u.id, u.login, u.login_type, u.display_name, t.id AS token_id
       FROM vault_tokens t
       JOIN vault_users u ON u.id = t.user_id
      WHERE t.token_hash = ? AND t.expires_at > ?
      LIMIT 1`,
  ).bind(hash, nowUtc()).first();
  if (!row) {
    throw new ApiError(fail('unauthorized', '登录已失效，请重新登录', 401));
  }
  return {
    id: row.id,
    login: row.login,
    login_type: row.login_type,
    display_name: row.display_name || '',
    token_id: row.token_id,
    token_hash: hash,
  };
}

class ApiError extends Error {
  constructor(response) {
    super('api');
    this.response = response;
  }
}

// ---------------------------------------------------------------- validation

function normalizeLogin(raw) {
  const login = String(raw || '').trim();
  if (!login) throw new ApiError(fail('validation', '请输入手机号或邮箱', 422));
  if (/^1\d{10}$/.test(login)) return [login, 'phone'];
  const lower = login.toLowerCase();
  if (/^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(lower)) return [lower, 'email'];
  throw new ApiError(fail('invalid_login', '请使用有效的手机号或邮箱', 422));
}

function normalizeDisplayName(raw) {
  const name = String(raw || '').trim().replace(/\s+/g, ' ');
  const len = [...name].length;
  if (len < 2) throw new ApiError(fail('validation', '昵称至少 2 个字', 422));
  if (len > 20) throw new ApiError(fail('validation', '昵称最多 20 个字', 422));
  return name;
}

function boolish(v) {
  if (v === true || v === 1) return true;
  if (v === false || v === 0 || v === null || v === undefined) return false;
  if (typeof v === 'string') {
    return ['1', 'true', 'yes', 'on'].includes(v.trim().toLowerCase());
  }
  return false;
}

function authPayload(userId, login, token, displayName, kdf) {
  return {
    user_id: userId,
    login,
    display_name: displayName,
    token,
    kdf_salt: kdf?.kdf_salt || '',
    kdf_iterations: kdf?.kdf_iterations || 0,
    kdf_algo: kdf?.kdf_algo || 'pbkdf2',
    key_check: kdf?.key_check || '',
  };
}

// ---------------------------------------------------------------- handlers

async function handleRegister(request, env) {
  const body = await readJson(request);
  const password = String(body.password || '');
  if (password.length < 6) {
    return fail('validation', '密码至少 6 位', 422);
  }
  const [login, type] = normalizeLogin(body.login);
  const display = normalizeDisplayName(body.display_name ?? body.nickname);

  const exists = await env.DB.prepare(
    'SELECT id FROM vault_users WHERE login = ? LIMIT 1',
  ).bind(login).first();
  if (exists) {
    return fail('login_taken', '该账号已注册，请直接登录', 409);
  }

  const hash = await hashPassword(password);
  const res = await env.DB.prepare(
    `INSERT INTO vault_users (login, login_type, display_name, password_hash)
     VALUES (?, ?, ?, ?)`,
  ).bind(login, type, display, hash).run();
  const userId = res.meta.last_row_id;
  const token = await issueToken(env, userId);
  return ok(authPayload(userId, login, token, display, null), 201);
}

async function handleLogin(request, env) {
  const body = await readJson(request);
  const password = String(body.password || '');
  const [login] = normalizeLogin(body.login);

  const row = await env.DB.prepare(
    `SELECT id, password_hash, display_name, kdf_salt, kdf_iterations,
            kdf_algo, key_check
       FROM vault_users WHERE login = ? LIMIT 1`,
  ).bind(login).first();

  if (!row) return fail('bad_credentials', '账号或密码错误', 401);

  const verdict = await verifyPassword(password, row.password_hash);
  if (verdict === 'legacy') {
    // The row still carries the PHP backend's bcrypt hash. Workers cannot
    // verify bcrypt, so say so instead of pretending the password was wrong.
    return fail(
      'password_reset_required',
      '该账号的密码来自旧服务器，请在 Cloudflare 版重置密码后登录',
      409,
    );
  }
  if (verdict !== 'ok') {
    return fail('bad_credentials', '账号或密码错误', 401);
  }

  const token = await issueToken(env, row.id);
  return ok(authPayload(row.id, login, token, row.display_name || '', row));
}

async function handleLogout(request, env) {
  const user = await requireUser(request, env);
  await env.DB.prepare('DELETE FROM vault_tokens WHERE id = ?')
    .bind(user.token_id).run();
  return ok({ logged_out: true });
}

async function handleSetMaster(request, env) {
  const user = await requireUser(request, env);
  const body = await readJson(request);
  let salt = String(body.kdf_salt || '');
  let algo = String(body.kdf_algo || 'pbkdf2');
  if (algo !== 'argon2id' && algo !== 'pbkdf2') algo = 'pbkdf2';
  let check = String(body.key_check || '');
  let iterations = parseInt(body.kdf_iterations, 10);
  if (!Number.isFinite(iterations) || iterations < 1) iterations = 0;
  if (salt.length > 64) salt = salt.slice(0, 64);
  if (check.length > 255) check = check.slice(0, 255);

  await env.DB.prepare(
    `UPDATE vault_users
        SET kdf_salt = ?, kdf_iterations = ?, kdf_algo = ?, key_check = ?,
            updated_at = ?
      WHERE id = ?`,
  ).bind(salt, iterations, algo, check, nowUtc(), user.id).run();
  return ok({ saved: true });
}

// ---------------------------------------------------------------- credentials

const CRED_COLS = `id, title, username_enc, password_enc, url_enc, note_enc,
                   enc_ver, pinned, sort_order, created_at, updated_at`;

async function credToPublic(env, row) {
  const encVer = row.enc_ver | 0;
  if (encVer === 1) {
    return {
      id: row.id,
      title: row.title,
      username: row.username_enc,
      password: row.password_enc,
      url: row.url_enc,
      note: row.note_enc,
      pinned: !!row.pinned,
      sort_order: row.sort_order | 0,
      enc_ver: 1,
      created_at: row.created_at,
      updated_at: row.updated_at,
    };
  }
  return {
    id: row.id,
    title: row.title,
    username: await aesDecrypt(env, row.username_enc),
    password: await aesDecrypt(env, row.password_enc),
    url: await aesDecrypt(env, row.url_enc),
    note: await aesDecrypt(env, row.note_enc),
    pinned: !!row.pinned,
    sort_order: row.sort_order | 0,
    enc_ver: 0,
    created_at: row.created_at,
    updated_at: row.updated_at,
  };
}

function parseCredentialBody(body, requireTitle) {
  const title = String(body.title || '').trim();
  if (requireTitle && !title) {
    throw new ApiError(fail('validation', '名称不能为空', 422));
  }
  if ([...title].length > 200) {
    throw new ApiError(fail('validation', '名称过长', 422));
  }
  const pinned = Object.prototype.hasOwnProperty.call(body, 'pinned')
    ? (boolish(body.pinned) ? 1 : 0)
    : null;
  let encVer = parseInt(body.enc_ver, 10);
  if (encVer !== 1) encVer = 0;
  return {
    title,
    username: String(body.username ?? '').trim(),
    password: String(body.password ?? '').trim(),
    url: String(body.url ?? '').trim(),
    note: String(body.note ?? '').trim(),
    pinned,
    enc_ver: encVer,
  };
}

async function credFieldsForStorage(env, f) {
  if (f.enc_ver === 1) {
    // Client-side E2E: ciphertext goes in untouched; the server cannot read it.
    return {
      u: f.username, p: f.password, url: f.url, n: f.note,
    };
  }
  return {
    u: await aesEncrypt(env, f.username),
    p: await aesEncrypt(env, f.password),
    url: await aesEncrypt(env, f.url),
    n: await aesEncrypt(env, f.note),
  };
}

async function handleCredentialsList(request, env) {
  const user = await requireUser(request, env);
  const { results } = await env.DB.prepare(
    `SELECT ${CRED_COLS} FROM vault_credentials
      WHERE user_id = ? ORDER BY pinned DESC, sort_order ASC, id ASC`,
  ).bind(user.id).all();
  const items = [];
  for (const row of results || []) items.push(await credToPublic(env, row));
  return ok({ items });
}

async function handleCredentialsCreate(request, env) {
  const user = await requireUser(request, env);
  const f = parseCredentialBody(await readJson(request), true);
  const s = await credFieldsForStorage(env, f);
  const maxRow = await env.DB.prepare(
    'SELECT COALESCE(MAX(sort_order), 0) + 1 AS n FROM vault_credentials WHERE user_id = ?',
  ).bind(user.id).first();
  const sort = maxRow?.n || 1;
  const pinned = f.pinned === null ? 0 : f.pinned;
  const ts = nowUtc();

  const res = await env.DB.prepare(
    `INSERT INTO vault_credentials
       (user_id, title, username_enc, password_enc, url_enc, note_enc,
        enc_ver, pinned, sort_order, created_at, updated_at)
     VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
  ).bind(
    user.id, f.title, s.u, s.p, s.url, s.n, f.enc_ver, pinned, sort, ts, ts,
  ).run();

  const row = await env.DB.prepare(
    `SELECT ${CRED_COLS} FROM vault_credentials WHERE id = ? AND user_id = ?`,
  ).bind(res.meta.last_row_id, user.id).first();
  return ok(await credToPublic(env, row), 201);
}

async function handleCredentialsUpdate(request, env, id) {
  const user = await requireUser(request, env);
  const body = await readJson(request);
  const existing = await env.DB.prepare(
    `SELECT ${CRED_COLS} FROM vault_credentials WHERE id = ? AND user_id = ?`,
  ).bind(id, user.id).first();
  if (!existing) return fail('not_found', '条目不存在', 404);

  // Pin-only update: { "pinned": true } with no title.
  if (Object.prototype.hasOwnProperty.call(body, 'pinned') &&
      !Object.prototype.hasOwnProperty.call(body, 'title')) {
    await env.DB.prepare(
      'UPDATE vault_credentials SET pinned = ?, updated_at = ? WHERE id = ? AND user_id = ?',
    ).bind(boolish(body.pinned) ? 1 : 0, nowUtc(), id, user.id).run();
    const row = await env.DB.prepare(
      `SELECT ${CRED_COLS} FROM vault_credentials WHERE id = ? AND user_id = ?`,
    ).bind(id, user.id).first();
    return ok(await credToPublic(env, row));
  }

  const f = parseCredentialBody(body, true);
  const s = await credFieldsForStorage(env, f);
  const pinned = f.pinned === null ? (existing.pinned ? 1 : 0) : f.pinned;
  await env.DB.prepare(
    `UPDATE vault_credentials
        SET title = ?, username_enc = ?, password_enc = ?, url_enc = ?,
            note_enc = ?, enc_ver = ?, pinned = ?, updated_at = ?
      WHERE id = ? AND user_id = ?`,
  ).bind(
    f.title, s.u, s.p, s.url, s.n, f.enc_ver, pinned, nowUtc(), id, user.id,
  ).run();

  const row = await env.DB.prepare(
    `SELECT ${CRED_COLS} FROM vault_credentials WHERE id = ? AND user_id = ?`,
  ).bind(id, user.id).first();
  if (!row) return fail('not_found', '条目不存在', 404);
  return ok(await credToPublic(env, row));
}

async function handleCredentialsReorder(request, env) {
  const user = await requireUser(request, env);
  const body = await readJson(request);
  const raw = body.ids;
  if (!Array.isArray(raw) || raw.length === 0) {
    return fail('validation', 'ids 不能为空', 422);
  }
  const ids = raw.map((v) => parseInt(v, 10));
  if (ids.some((v) => !Number.isFinite(v) || v < 1)) {
    return fail('validation', 'ids 无效', 422);
  }
  if (new Set(ids).size !== ids.length) {
    return fail('validation', 'ids 不能重复', 422);
  }

  const { results } = await env.DB.prepare(
    'SELECT id FROM vault_credentials WHERE user_id = ?',
  ).bind(user.id).all();
  const owned = new Set((results || []).map((r) => r.id));
  if (owned.size !== ids.length) {
    return fail('validation', '须提交当前账号下的全部条目 id', 422);
  }
  for (const id of ids) {
    if (!owned.has(id)) return fail('validation', '包含无权条目', 403);
  }

  const stmts = ids.map((id, i) => env.DB.prepare(
    'UPDATE vault_credentials SET sort_order = ? WHERE id = ? AND user_id = ?',
  ).bind(i, id, user.id));
  await env.DB.batch(stmts);
  return ok({ reordered: true, count: ids.length });
}

async function handleCredentialsDelete(request, env, id) {
  const user = await requireUser(request, env);
  const res = await env.DB.prepare(
    'DELETE FROM vault_credentials WHERE id = ? AND user_id = ?',
  ).bind(id, user.id).run();
  if (!res.meta.changes) return fail('not_found', '条目不存在', 404);
  return ok({ deleted: true });
}

// ---------------------------------------------------------------- notes

const NOTE_COLS = `id, title, body_enc, enc_ver, created_at, updated_at`;

async function noteToPublic(env, row, includeBody) {
  const encVer = row.enc_ver | 0;
  const out = {
    id: row.id,
    title: row.title,
    enc_ver: encVer,
    created_at: row.created_at,
    updated_at: row.updated_at,
  };
  if (includeBody) {
    out.body = encVer === 1 ? row.body_enc : await aesDecrypt(env, row.body_enc);
  }
  return out;
}

function parseNoteBody(body, requireTitle) {
  const title = String(body.title || '').trim();
  if (requireTitle && !title) {
    throw new ApiError(fail('validation', '标题不能为空', 422));
  }
  if ([...title].length > 200) {
    throw new ApiError(fail('validation', '标题过长', 422));
  }
  const text = String(body.body ?? '');
  if (text.length > MAX_NOTE_BODY) {
    throw new ApiError(fail('validation', '正文过长', 422));
  }
  let encVer = parseInt(body.enc_ver, 10);
  if (encVer !== 1) encVer = 0;
  return { title: title || '未命名', body: text, enc_ver: encVer };
}

async function handleNotesList(request, env) {
  const user = await requireUser(request, env);
  const { results } = await env.DB.prepare(
    `SELECT ${NOTE_COLS} FROM vault_notes
      WHERE user_id = ? ORDER BY updated_at DESC, id DESC`,
  ).bind(user.id).all();
  const items = [];
  for (const row of results || []) items.push(await noteToPublic(env, row, false));
  return ok({ items });
}

async function handleNotesGet(request, env, id) {
  const user = await requireUser(request, env);
  const row = await env.DB.prepare(
    `SELECT ${NOTE_COLS} FROM vault_notes WHERE id = ? AND user_id = ?`,
  ).bind(id, user.id).first();
  if (!row) return fail('not_found', '笔记不存在', 404);
  return ok(await noteToPublic(env, row, true));
}

async function handleNotesCreate(request, env) {
  const user = await requireUser(request, env);
  const f = parseNoteBody(await readJson(request), true);
  const bodyEnc = f.enc_ver === 1 ? f.body : await aesEncrypt(env, f.body);
  const ts = nowUtc();
  const res = await env.DB.prepare(
    `INSERT INTO vault_notes (user_id, title, body_enc, enc_ver, created_at, updated_at)
     VALUES (?, ?, ?, ?, ?, ?)`,
  ).bind(user.id, f.title, bodyEnc, f.enc_ver, ts, ts).run();
  const row = await env.DB.prepare(
    `SELECT ${NOTE_COLS} FROM vault_notes WHERE id = ? AND user_id = ?`,
  ).bind(res.meta.last_row_id, user.id).first();
  return ok(await noteToPublic(env, row, true), 201);
}

async function handleNotesUpdate(request, env, id) {
  const user = await requireUser(request, env);
  const f = parseNoteBody(await readJson(request), false);
  const existing = await env.DB.prepare(
    `SELECT ${NOTE_COLS} FROM vault_notes WHERE id = ? AND user_id = ?`,
  ).bind(id, user.id).first();
  if (!existing) return fail('not_found', '笔记不存在', 404);

  const title = String(f.title || '').trim() === ''
    ? existing.title
    : f.title;
  const bodyEnc = f.enc_ver === 1 ? f.body : await aesEncrypt(env, f.body);
  await env.DB.prepare(
    `UPDATE vault_notes
        SET title = ?, body_enc = ?, enc_ver = ?, updated_at = ?
      WHERE id = ? AND user_id = ?`,
  ).bind(title, bodyEnc, f.enc_ver, nowUtc(), id, user.id).run();

  const row = await env.DB.prepare(
    `SELECT ${NOTE_COLS} FROM vault_notes WHERE id = ? AND user_id = ?`,
  ).bind(id, user.id).first();
  return ok(await noteToPublic(env, row, true));
}

async function handleNotesDelete(request, env, id) {
  const user = await requireUser(request, env);
  const res = await env.DB.prepare(
    'DELETE FROM vault_notes WHERE id = ? AND user_id = ?',
  ).bind(id, user.id).run();
  if (!res.meta.changes) return fail('not_found', '笔记不存在', 404);
  return ok({ deleted: true });
}

// ---------------------------------------------------------------- router

async function route(request, env, path, method) {
  if (path === '' || path === 'health') {
    return ok({ service: 'desktidy-vault', version: 1, runtime: 'workers-d1' });
  }

  if (path === 'auth/register' && method === 'POST') return handleRegister(request, env);
  if (path === 'auth/login' && method === 'POST') return handleLogin(request, env);
  if (path === 'auth/logout' && method === 'POST') return handleLogout(request, env);
  if (path === 'auth/master' && (method === 'PUT' || method === 'POST')) {
    return handleSetMaster(request, env);
  }

  if (path === 'credentials' && method === 'GET') {
    return handleCredentialsList(request, env);
  }
  if (path === 'credentials' && method === 'POST') {
    return handleCredentialsCreate(request, env);
  }
  if (path === 'credentials/reorder' && (method === 'PUT' || method === 'POST')) {
    return handleCredentialsReorder(request, env);
  }

  let m = /^credentials\/(\d+)$/.exec(path);
  if (m) {
    const id = parseInt(m[1], 10);
    if (method === 'PUT' || method === 'PATCH') {
      return handleCredentialsUpdate(request, env, id);
    }
    if (method === 'DELETE') return handleCredentialsDelete(request, env, id);
  }

  if (path === 'notes' && method === 'GET') return handleNotesList(request, env);
  if (path === 'notes' && method === 'POST') return handleNotesCreate(request, env);

  m = /^notes\/(\d+)$/.exec(path);
  if (m) {
    const id = parseInt(m[1], 10);
    if (method === 'GET') return handleNotesGet(request, env, id);
    if (method === 'PUT' || method === 'PATCH') {
      return handleNotesUpdate(request, env, id);
    }
    if (method === 'DELETE') return handleNotesDelete(request, env, id);
  }

  return fail('not_found', `未知接口: ${method} ${path}`, 404);
}

export async function onRequest(context) {
  const { request, env } = context;
  const method = request.method.toUpperCase();

  if (method === 'OPTIONS') {
    return new Response(null, { status: 204, headers: CORS });
  }

  // /api/<rest> — the client's base URL ends in "/api/".
  const url = new URL(request.url);
  const path = url.pathname.replace(/^\/api\/?/, '').replace(/\/+$/, '');

  try {
    return await route(request, env, path, method);
  } catch (e) {
    if (e instanceof ApiError) return e.response;
    return fail('server', `服务器错误: ${e.message || e}`, 500);
  }
}

#!/usr/bin/env node
/**
 * Convert the old vault's phpMyAdmin CSV exports into a D1 seed script.
 *
 *   node scripts/migrate_vault_to_d1.mjs \
 *     --users ~/Downloads/export.csv \
 *     --creds "$HOME/Downloads/export (1).csv" \
 *     --notes "$HOME/Downloads/export (2).csv" \
 *     --login '<your login>' \
 *     --password '<new password>' \
 *     --key-file .vault_key \
 *     --out server/d1/seed.sql
 *
 * Generate the key once, then use the same value for the Worker's
 * VAULT_DATA_KEY environment variable:
 *
 *   node -e "console.log(require('crypto').randomBytes(32).toString('base64'))" > .vault_key
 *
 * Why a new password: the old rows carry a hash format this repo's PHP never
 * produced (16-char salt + "$" + sha256 hex), and Workers has no bcrypt either.
 * The seed re-hashes with the same PBKDF2 scheme functions/api/[[path]].ts
 * verifies. The login name and every credential/note row are preserved; only
 * the password changes.
 *
 * Encryption: the source rows are plaintext, but the D1 backend stores
 * enc_ver=0 as AES-256-GCM, so every field is encrypted here. The database
 * never holds plaintext credentials or note bodies.
 */

import { createCipheriv, createHash, pbkdf2Sync, randomBytes } from 'node:crypto';
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { homedir } from 'node:os';

const PBKDF2_ITERATIONS = 210_000;

// ------------------------------------------------------------------ args

function parseArgs(argv) {
  const out = { out: 'server/d1/seed.sql' };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (!a.startsWith('--')) throw new Error(`unexpected argument: ${a}`);
    const key = a.slice(2).replace(/-([a-z])/g, (_, c) => c.toUpperCase());
    const val = argv[++i];
    if (val === undefined) throw new Error(`missing value for ${a}`);
    out[key] = val;
  }
  return out;
}

const args = parseArgs(process.argv.slice(2));
for (const req of ['users', 'creds', 'notes', 'login', 'password']) {
  if (!args[req]) {
    console.error(`missing --${req}`);
    process.exit(1);
  }
}

function expand(p) {
  return p.startsWith('~') ? resolve(homedir(), p.slice(1).replace(/^[/\\]/, '')) : resolve(p);
}

// ------------------------------------------------------------------ key

function loadKey(keyFile) {
  let raw = '';
  if (keyFile) raw = readFileSync(expand(keyFile), 'utf8').trim();
  if (!raw) raw = (process.env.VAULT_DATA_KEY || '').trim();
  if (!raw) {
    console.error('missing vault key: pass --key-file or set VAULT_DATA_KEY');
    console.error('  node -e "console.log(require(\'crypto\').randomBytes(32).toString(\'base64\'))"');
    process.exit(1);
  }
  const key = Buffer.from(raw, 'base64');
  if (key.length !== 32) {
    console.error(`VAULT_DATA_KEY must be 32 bytes, got ${key.length}`);
    process.exit(1);
  }
  return key;
}

const KEY = loadKey(args.keyFile);

/** base64(iv[12] | tag[16] | ciphertext) — matches aesEncrypt() in the Worker. */
function encryptField(plain) {
  if (plain === '') return '';
  const iv = randomBytes(12);
  const cipher = createCipheriv('aes-256-gcm', KEY, iv);
  const body = Buffer.concat([cipher.update(plain, 'utf8'), cipher.final()]);
  return Buffer.concat([iv, cipher.getAuthTag(), body]).toString('base64');
}

// ------------------------------------------------------------------ csv

function readCsv(path) {
  const text = readFileSync(expand(path), 'utf8').replace(/^﻿/, '');
  const rows = [];
  let row = [];
  let field = '';
  let quoted = false;

  for (let i = 0; i < text.length; i++) {
    const c = text[i];
    if (quoted) {
      if (c === '"') {
        if (text[i + 1] === '"') { field += '"'; i++; } else { quoted = false; }
      } else {
        field += c;
      }
      continue;
    }
    if (c === '"') { quoted = true; continue; }
    if (c === ',') { row.push(field); field = ''; continue; }
    if (c === '\r') continue;
    if (c === '\n') { row.push(field); rows.push(row); row = []; field = ''; continue; }
    field += c;
  }
  if (field !== '' || row.length) { row.push(field); rows.push(row); }

  const nonEmpty = rows.filter((r) => r.some((v) => v.trim() !== ''));
  if (nonEmpty.length === 0) return [];
  const header = nonEmpty[0].map((h) => h.trim());
  return nonEmpty.slice(1).map((r) => {
    const o = {};
    header.forEach((h, i) => { o[h] = r[i] ?? ''; });
    return o;
  });
}

// ------------------------------------------------------------------ sql

function sqlStr(v) {
  if (v === null || v === undefined) return 'NULL';
  return "'" + String(v).replace(/'/g, "''") + "'";
}

/** Both legacy shapes -> 'YYYY-MM-DD HH:MM:SS' (UTC). */
function normTime(raw) {
  let v = (raw || '').trim();
  if (!v) return '';
  v = v.replace('T', ' ');
  if (v.endsWith('Z')) v = v.slice(0, -1);
  if (v.includes('.')) v = v.split('.')[0];
  return v.slice(0, 19);
}

function hashPassword(password) {
  const salt = randomBytes(16);
  const dk = pbkdf2Sync(password, salt, PBKDF2_ITERATIONS, 32, 'sha256');
  // ':' separators, not '$' — the D1 dashboard console treats '$' as a bound
  // parameter and refuses the statement. Must match verifyPassword().
  return `pbkdf2:${PBKDF2_ITERATIONS}:${salt.toString('base64')}:${dk.toString('base64')}`;
}

// ------------------------------------------------------------------ main

const users = readCsv(args.users);
const creds = readCsv(args.creds);
const notes = readCsv(args.notes);

if (users.length !== 1) {
  console.error(`expected exactly 1 user, got ${users.length}`);
  process.exit(1);
}
const user = users[0];
const uid = parseInt(user.id, 10);
if (user.login !== args.login) {
  console.error(`login mismatch: csv has ${JSON.stringify(user.login)}, --login ${JSON.stringify(args.login)}`);
  process.exit(1);
}

const login = user.login;
const loginType = /^1\d{10}$/.test(login) ? 'phone' : 'email';
const display = (user.display_name || '').trim();
const pwHash = hashPassword(args.password);

const L = [];
L.push('-- Generated by scripts/migrate_vault_to_d1.mjs — do not edit by hand.');
L.push('-- Apply with:');
L.push('--   wrangler d1 execute desktidy-vault --remote --file=server/d1/seed.sql');
L.push('--');
L.push('-- All *_enc columns are AES-256-GCM (enc_ver=0). The Worker must be');
L.push('-- configured with the same VAULT_DATA_KEY or these rows read back empty.');
L.push('');
L.push('PRAGMA foreign_keys = OFF;');
L.push('BEGIN;');
L.push('');
L.push('-- Re-runnable: clear the previous seed for this login.');
L.push(`DELETE FROM vault_tokens WHERE user_id IN (SELECT id FROM vault_users WHERE login = ${sqlStr(login)});`);
L.push(`DELETE FROM vault_credentials WHERE user_id IN (SELECT id FROM vault_users WHERE login = ${sqlStr(login)});`);
L.push(`DELETE FROM vault_notes WHERE user_id IN (SELECT id FROM vault_users WHERE login = ${sqlStr(login)});`);
L.push(`DELETE FROM vault_users WHERE login = ${sqlStr(login)};`);
L.push('');

L.push('INSERT INTO vault_users');
L.push('  (id, login, login_type, display_name, password_hash, created_at, updated_at)');
L.push(`VALUES (${uid}, ${sqlStr(login)}, ${sqlStr(loginType)}, ${sqlStr(display)}, ${sqlStr(pwHash)}, ${sqlStr(normTime(user.created_at) || '2026-11-12 00:00:00')}, ${sqlStr(normTime(user.updated_at) || '2026-11-12 00:00:00')});`);
L.push('');

L.push(`-- ${creds.length} credentials (AES-256-GCM, enc_ver=0)`);
for (const r of creds) {
  L.push('INSERT INTO vault_credentials');
  L.push('  (id, user_id, title, username_enc, password_enc, url_enc, note_enc, enc_ver, pinned, sort_order, created_at, updated_at)');
  L.push(
    `VALUES (${parseInt(r.id, 10)}, ${uid}, ${sqlStr(r.title || '')}, ` +
    `${sqlStr(encryptField(r.username_enc || ''))}, ${sqlStr(encryptField(r.password_enc || ''))}, ` +
    `${sqlStr(encryptField(r.url_enc || ''))}, ${sqlStr(encryptField(r.note_enc || ''))}, ` +
    `0, ${['1', 'true'].includes((r.pinned || '0').trim()) ? 1 : 0}, ` +
    `${parseInt((r.sort_order || '0').trim() || '0', 10) || 0}, ` +
    `${sqlStr(normTime(r.created_at) || '2026-11-12 00:00:00')}, ` +
    `${sqlStr(normTime(r.updated_at) || '2026-11-12 00:00:00')});`,
  );
}
L.push('');

L.push(`-- ${notes.length} notes (AES-256-GCM, enc_ver=0)`);
for (const r of notes) {
  L.push('INSERT INTO vault_notes');
  L.push('  (id, user_id, title, body_enc, enc_ver, created_at, updated_at)');
  L.push(
    `VALUES (${parseInt(r.id, 10)}, ${uid}, ${sqlStr(r.title || '')}, ` +
    `${sqlStr(encryptField(r.body_enc || ''))}, 0, ` +
    `${sqlStr(normTime(r.created_at) || '2026-11-12 00:00:00')}, ` +
    `${sqlStr(normTime(r.updated_at) || '2026-11-12 00:00:00')});`,
  );
}
L.push('');
L.push('COMMIT;');
L.push('PRAGMA foreign_keys = ON;');
L.push('');

const outPath = expand(args.out);
mkdirSync(dirname(outPath), { recursive: true });
writeFileSync(outPath, L.join('\n'), 'utf8');

console.log(`wrote ${outPath}`);
console.log(`  user    : ${login} (${loginType}, display=${JSON.stringify(display)})`);
console.log(`  creds   : ${creds.length}`);
console.log(`  notes   : ${notes.length}`);
console.log(`  key sha  : ${createHash('sha256').update(KEY).digest('hex').slice(0, 16)}…`);
console.log(`  password: PBKDF2-SHA256, ${PBKDF2_ITERATIONS} iterations`);

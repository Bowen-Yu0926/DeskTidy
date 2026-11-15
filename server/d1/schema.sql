-- DeskTidy vault schema for Cloudflare D1 (SQLite).
-- Apply with:  wrangler d1 execute desktidy-vault --remote --file=server/d1/schema.sql
--
-- Mirrors server/desktidy-vault/schema.sql (MySQL) with SQLite types. Timestamps
-- are TEXT in "YYYY-MM-DD HH:MM:SS" UTC, the same shape the PHP backend produced,
-- so clients parse them unchanged.

CREATE TABLE IF NOT EXISTS vault_users (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  login TEXT NOT NULL,
  login_type TEXT NOT NULL CHECK (login_type IN ('phone', 'email')),
  display_name TEXT NOT NULL DEFAULT '',
  -- "pbkdf2:<iterations>:<salt-b64>:<hash-b64>"; see functions/api/[[path]].ts.
  -- Colon separators: the D1 dashboard console reads a dollar sign as a
  -- bound parameter and rejects the whole statement.
  password_hash TEXT NOT NULL,
  kdf_salt TEXT NOT NULL DEFAULT '',
  kdf_iterations INTEGER NOT NULL DEFAULT 0,
  kdf_algo TEXT NOT NULL DEFAULT 'pbkdf2',
  key_check TEXT NOT NULL DEFAULT '',
  created_at TEXT NOT NULL DEFAULT (datetime('now')),
  updated_at TEXT NOT NULL DEFAULT (datetime('now'))
);
CREATE UNIQUE INDEX IF NOT EXISTS uq_vault_users_login ON vault_users (login);

CREATE TABLE IF NOT EXISTS vault_tokens (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  user_id INTEGER NOT NULL REFERENCES vault_users (id) ON DELETE CASCADE,
  token_hash TEXT NOT NULL,
  expires_at TEXT NOT NULL,
  created_at TEXT NOT NULL DEFAULT (datetime('now'))
);
CREATE UNIQUE INDEX IF NOT EXISTS uq_vault_tokens_hash ON vault_tokens (token_hash);
CREATE INDEX IF NOT EXISTS idx_vault_tokens_user ON vault_tokens (user_id);

CREATE TABLE IF NOT EXISTS vault_credentials (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  user_id INTEGER NOT NULL REFERENCES vault_users (id) ON DELETE CASCADE,
  title TEXT NOT NULL,
  username_enc TEXT NOT NULL,
  password_enc TEXT NOT NULL,
  url_enc TEXT NOT NULL,
  note_enc TEXT NOT NULL,
  -- 1 = client-side E2E ciphertext stored verbatim; 0 = server-side AES-GCM.
  enc_ver INTEGER NOT NULL DEFAULT 0,
  pinned INTEGER NOT NULL DEFAULT 0,
  sort_order INTEGER NOT NULL DEFAULT 0,
  created_at TEXT NOT NULL DEFAULT (datetime('now')),
  updated_at TEXT NOT NULL DEFAULT (datetime('now'))
);
CREATE INDEX IF NOT EXISTS idx_vault_credentials_user ON vault_credentials (user_id);
CREATE INDEX IF NOT EXISTS idx_vault_credentials_pinned
  ON vault_credentials (user_id, pinned);
CREATE INDEX IF NOT EXISTS idx_vault_credentials_sort
  ON vault_credentials (user_id, pinned, sort_order);

CREATE TABLE IF NOT EXISTS vault_notes (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  user_id INTEGER NOT NULL REFERENCES vault_users (id) ON DELETE CASCADE,
  title TEXT NOT NULL,
  body_enc TEXT NOT NULL,
  enc_ver INTEGER NOT NULL DEFAULT 0,
  created_at TEXT NOT NULL DEFAULT (datetime('now')),
  updated_at TEXT NOT NULL DEFAULT (datetime('now'))
);
CREATE INDEX IF NOT EXISTS idx_vault_notes_user ON vault_notes (user_id);
CREATE INDEX IF NOT EXISTS idx_vault_notes_updated ON vault_notes (user_id, updated_at);

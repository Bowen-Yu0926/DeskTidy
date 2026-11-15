# DeskTidy Vault on Cloudflare (Pages Functions + D1)

Replaces the PHP backend in `server/desktidy-vault/`. Same paths, same response
envelope, so the client only needs a different API base URL.

## What gets stored

| Column | Storage |
|---|---|
| `vault_credentials.username_enc` / `password_enc` / `url_enc` / `note_enc` | AES-256-GCM (`enc_ver=0`) |
| `vault_notes.body_enc` | AES-256-GCM (`enc_ver=0`) |
| `vault_credentials.title`, `vault_notes.title` | plaintext — the rail lists and searches by title without a key |
| `vault_users.password_hash` | PBKDF2-SHA256, 210 000 iterations, per-user salt |

`enc_ver=1` rows are client-side E2E ciphertext and are passed through untouched.

The AES key lives only in the Worker's `VAULT_DATA_KEY` environment variable.
Losing it makes every `enc_ver=0` row unreadable — back it up somewhere safe.

## One-time setup

1. **Create the D1 database** (dashboard → Workers & Pages → D1), then apply
   the schema and the seed:

   ```bash
   wrangler d1 execute desktidy-vault --remote --file=server/d1/schema.sql
   wrangler d1 execute desktidy-vault --remote --file=server/d1/seed.sql
   ```

2. **Bind it to the Pages project** — Settings → Functions → D1 database
   bindings. Variable name must be `DB`. This is account-specific and cannot be
   committed.

3. **Set `VAULT_DATA_KEY`** — Settings → Environment variables, encrypted.
   Use the same value that produced `server/d1/seed.sql`:

   ```bash
   cat .vault_key
   ```

   (`.vault_key` is gitignored. If you lose it, re-run the migration below with a
   fresh key.)

4. **Point the client at the new base** — the client reads `api_base` from
   `%LOCALAPPDATA%\DeskTidyNative\account_vault_session.json`. Either set it in
   DeskTidy's settings, or edit the file to
   `https://<pages-project>.pages.dev/api/`.

## Re-running the migration

`scripts/migrate_vault_to_d1.mjs` turns the old phpMyAdmin CSV exports into an
encrypted seed. It is idempotent — it deletes the previous seed for the same
login before inserting.

```bash
node scripts/migrate_vault_to_d1.mjs \
  --users  ~/Downloads/export.csv \
  --creds  "$HOME/Downloads/export (1).csv" \
  --notes  "$HOME/Downloads/export (2).csv" \
  --login  '<your login>' \
  --password '<new login password>' \
  --key-file .vault_key \
  --out server/d1/seed.sql
```

The login password must be set fresh: the old rows carry a hash format
(16-char salt + `$` + sha256 hex) that neither this repo's PHP nor Workers can
verify. The login name, all 18 credentials and the note are preserved.

## Verifying before deploying

`.scratch/test_vault_api.mjs` runs the Function against an in-memory SQLite copy
of the schema and seed, exercising all 15 routes:

```bash
node .scratch/test_vault_api.mjs
```

Expected: `pass 27  fail 0`.

## API

Identical to `server/desktidy-vault/README.md`:

| Method | Path |
|---|---|
| POST | `auth/register`, `auth/login`, `auth/logout` |
| PUT/POST | `auth/master` |
| GET/POST | `credentials` |
| PUT/PATCH/DELETE | `credentials/{id}` |
| PUT/POST | `credentials/reorder` |
| GET/POST | `notes` |
| GET/PUT/PATCH/DELETE | `notes/{id}` |

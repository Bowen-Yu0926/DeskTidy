# DeskTidy Account Vault API (byethost PHP + MySQL)

## Deploy

1. In byethost panel, create a MySQL database (e.g. `b10_42906238_vault`).
2. Import `schema.sql` via phpMyAdmin.
3. Copy `config.example.php` → `config.local.php` **on the server** (FTP upload separately; never commit).
4. Set `db_name`, `db_pass`, and generate `vault_data_key_hex`:

```bash
php -r "echo bin2hex(random_bytes(32)), PHP_EOL;"
```

5. Upload API files (skips secrets):

```bash
python scripts/upload_vault_api.py
```

6. Health check: `https://bowen-yu0926.byethost10.com/desktidy-vault/`  
   DeskTidy 客户端已自动处理 byethost `__test` 防机器人 Cookie。

**Never commit `config.local.php`.**

## API

Base: `https://bowen-yu0926.byethost10.com/desktidy-vault/`

| Method | Path | Auth | Body |
|--------|------|------|------|
| POST | `auth/register` | no | `{login,password}` |
| POST | `auth/login` | no | `{login,password}` |
| POST | `auth/logout` | Bearer | |
| GET | `credentials` | Bearer | |
| POST | `credentials` | Bearer | `{title,username,password,url,note}` |
| PUT | `credentials/{id}` | Bearer | same |
| DELETE | `credentials/{id}` | Bearer | |

Fallback without rewrite: `index.php?r=auth/login`

## Curl smoke

```bash
curl -s -X POST https://bowen-yu0926.byethost10.com/desktidy-vault/auth/register \
  -H "Content-Type: application/json" \
  -d "{\"login\":\"you@example.com\",\"password\":\"secret1\"}"
```

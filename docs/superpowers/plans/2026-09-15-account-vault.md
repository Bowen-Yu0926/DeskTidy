# Account Vault Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship DeskTidy cloud account vault: PHP/MySQL API on byethost + PyQt6 panel with copy/CRUD and DeskNote note linkage.

**Architecture:** PHP REST under `server/desktidy-vault/` (AES-encrypted credential fields, hashed login + bearer tokens). Client modules mirror `todos.py` / `DesktopTodoWidget`: settings switch, page-indicator chip, floating panel, HTTP JSON client, session file under `%USERPROFILE%\.desktidy\`.

**Tech Stack:** PHP 7.4+/8 + mysqli, MySQL, PyQt6, urllib/stdlib HTTP (no new pip dep), existing DeskNote launch helpers.

**Spec:** `docs/superpowers/specs/2026-09-15-account-vault-design.md`

## Global Constraints

- No OTP; register/login with phone **or** email + password (≥6).
- Fields: title, username, password, url, optional note.
- Default API base: `https://bowen-yu0926.byethost10.com/desktidy-vault/`
- Secrets only in `config.local.php` / `.env.byethost` — never commit.
- Follow existing extension / page-indicator / todo panel patterns.
- Selftests under `scripts/selftest_*.py` (project convention, not pytest).

## File map

| Path | Responsibility |
|------|----------------|
| `server/desktidy-vault/schema.sql` | Tables |
| `server/desktidy-vault/config.example.php` | Template for DB + VAULT_DATA_KEY |
| `server/desktidy-vault/config.local.php` | Local secrets (gitignored) |
| `server/desktidy-vault/lib/*.php` | db, crypto, http helpers |
| `server/desktidy-vault/index.php` | Router |
| `server/desktidy-vault/.htaccess` | Rewrite to index.php |
| `server/desktidy-vault/README.md` | Deploy steps |
| `src/account_vault.py` | settings helpers, session I/O, login normalize, note path |
| `src/account_vault_api.py` | HTTP client |
| `src/ui/account_vault_widget.py` | Floating panel + dialogs |
| `src/ui/extensions_widget.py` | Enable + api_base + logout UI |
| `src/ui/page_indicator.py` | 「账号」chip + signal |
| `src/app.py` | Wire panel like todos |
| `config/default_settings.json` | `account_vault` defaults |
| `scripts/selftest_account_vault.py` | Unit/selftests |
| `.gitignore` | ignore `config.local.php` |

---

### Task 1: PHP schema + crypto + auth API

**Files:**
- Create: `server/desktidy-vault/schema.sql`
- Create: `server/desktidy-vault/config.example.php`
- Create: `server/desktidy-vault/lib/db.php`, `crypto.php`, `http.php`, `auth_lib.php`
- Create: `server/desktidy-vault/index.php`, `.htaccess`, `README.md`
- Modify: `.gitignore` (add `server/desktidy-vault/config.local.php`)

**Interfaces:**
- Produces: HTTP routes `POST /auth/register`, `POST /auth/login`, `POST /auth/logout`; JSON `{ok,data|error,message}`

- [ ] **Step 1:** Add `schema.sql` with `vault_users`, `vault_tokens`, `vault_credentials` as in spec.
- [ ] **Step 2:** Implement `encrypt_field` / `decrypt_field` (AES-256-GCM, key from config).
- [ ] **Step 3:** Implement register/login/logout + token hash storage (SHA-256), 90-day expiry.
- [ ] **Step 4:** Smoke with local PHP if available, or document curl examples in README.
- [ ] **Step 5:** Commit `feat(vault): add PHP auth API scaffold`

### Task 2: PHP credentials CRUD

**Files:**
- Modify: `server/desktidy-vault/index.php` (+ credentials handlers in `lib/credentials.php`)

**Interfaces:**
- Consumes: `require_user()` from auth
- Produces: `GET/POST /credentials`, `PUT/DELETE /credentials/{id}` returning decrypted fields

- [ ] **Step 1:** Implement list/create/update/delete scoped by `user_id`.
- [ ] **Step 2:** Commit `feat(vault): add credentials CRUD API`

### Task 3: Client session + API + settings helpers

**Files:**
- Create: `src/account_vault.py`, `src/account_vault_api.py`
- Modify: `config/default_settings.json`
- Test: `scripts/selftest_account_vault.py`

**Interfaces:**
- Produces:
  - `account_vault_settings(settings) -> dict`
  - `account_vault_enabled(settings) -> bool`
  - `DEFAULT_API_BASE: str`
  - `load_session() / save_session() / clear_session()`
  - `normalize_login(login: str) -> tuple[str, str]  # (normalized, phone|email)`
  - `VaultApiClient(api_base, token)` with `register/login/logout/list/create/update/delete`
  - `credential_note_path(notes_folder, cred_id, title) -> Path`
  - `ensure_credential_note(path, meta: dict) -> Path`

- [ ] **Step 1:** Write selftest for normalize_login, session roundtrip, note path sanitization, API JSON parse with mock/handler.
- [ ] **Step 2:** Implement modules until selftest passes.
- [ ] **Step 3:** Commit `feat(vault): add client API and session helpers`

### Task 4: Extensions panel + default settings

**Files:**
- Modify: `src/ui/extensions_widget.py`
- Modify: `src/settings.py` if needed for known keys list (~line 565)
- Modify: `config/default_settings.json`

- [ ] **Step 1:** Add「账号管理」card: enabled checkbox, api_base edit, masked login label, logout button.
- [ ] **Step 2:** Emit `extensions_changed` on toggle like todos.
- [ ] **Step 3:** Commit `feat(vault): add account vault extension settings`

### Task 5: Floating UI + page indicator + app wiring

**Files:**
- Create: `src/ui/account_vault_widget.py`
- Modify: `src/ui/page_indicator.py` (button + `vault_requested` signal)
- Modify: `src/app.py` (`_setup_account_vault_panel`, connect signal)
- Modify: `src/desktop_pet.py` if chip visibility map needs `vault` key (optional mirror of todo)

**Interfaces:**
- Consumes: `VaultApiClient`, session helpers, `account_vault_enabled`
- Produces: `AccountVaultWidget` with show/raise like `DesktopTodoWidget`

- [ ] **Step 1:** Build panel: search, list rows with copy icons (use `make_action_icon("copy")` if exists else text), edit/delete, login dialog, CRUD dialog.
- [ ] **Step 2:** Wire page chip「账号」and app setup/teardown on extensions_changed.
- [ ] **Step 3:** Extend selftest for enabled helper + note open naming.
- [ ] **Step 4:** Commit `feat(vault): add account vault panel and page chip`

### Task 6: DeskNote note linkage

**Files:**
- Modify: `src/ui/account_vault_widget.py`
- Modify: `src/account_vault.py` (ensure note template)
- Use: `src/desknote_launch.py` (`launch_desknote` / open files API)

- [ ] **Step 1:** On「DeskNote 备注」, `ensure_credential_note` then launch DeskNote with that path.
- [ ] **Step 2:** Selftest template creation without launching GUI.
- [ ] **Step 3:** Commit `feat(vault): link credential notes to DeskNote`

### Task 7: Deploy helpers + docs touch

**Files:**
- Modify: `scripts/upload_byethost.py` OR add `scripts/upload_vault_api.py` to upload `server/desktidy-vault/` (excluding `config.local.php`)
- Modify: `server/desktidy-vault/README.md` with create-DB + import schema + config steps

- [ ] **Step 1:** Add upload script targeting `htdocs/desktidy-vault/`.
- [ ] **Step 2:** Commit `chore(vault): add byethost upload helper for API`

---

## Spec coverage checklist

| Spec item | Task |
|-----------|------|
| PHP + MySQL API | 1–2 |
| Register/login no OTP | 1 |
| Encrypted credential fields | 1–2 |
| Extension enable + api_base | 4 |
| Page bar entry | 5 |
| List + copy + CRUD UI | 5 |
| Session file | 3 |
| DeskNote note md | 6 |
| Deploy to byethost | 7 |
| Selftest | 3, 5, 6 |

## Execution note

User directed「做吧」twice → **inline execution** in this session (executing-plans style), not waiting for subagent choice.

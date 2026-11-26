#pragma once

#include <Windows.h>

#include <string>
#include <vector>

// Cloud access is governed solely by the session token (VaultHttpLoggedIn).
// The old `cloud_enabled.json` master switch is gone entirely: it was a second
// gate on the same feature and made logged-in users see "云服务未启用".

// WinHTTP client for DeskTidy vault API (enc_ver:0 plaintext + enc_ver:1 ZK).
// Default base: https://chromhome.kdns.fr/api/ (Cloudflare-fronted).
// Session: %LocalAppData%\DeskTidy\account_vault_session.json (NativeDataDir).

struct VaultHttpCredential {
  int id = 0;
  std::wstring title;
  std::wstring username;
  std::wstring password;
  std::wstring url;
  std::wstring note;
  bool pinned = false;
  int sort_order = 0;
  int enc_ver = 0;  // 0 plaintext, 1 client-side ZK ciphertext.
};

// Persist preferred API base into the session file (keeps token if any).
void VaultHttpSetApiBase(const std::wstring& api_base);
std::wstring VaultHttpGetApiBase();
bool VaultHttpLoggedIn();
// Session login name (empty if never logged in / cleared).
std::wstring VaultHttpSessionLogin();
// If native session has no token, copy Python ~/.desktidy/account_vault_session.json.
// Returns true when a session token is available afterwards.
bool VaultHttpImportPythonSessionIfNeeded();

bool VaultHttpHealth();
bool VaultHttpLogin(const std::wstring& login, const std::wstring& password);
// Register (auth/register). Returns false with *error_msg set on failure.
bool VaultHttpRegister(const std::wstring& login, const std::wstring& password,
                       const std::wstring& display_name,
                       std::wstring* error_msg);
bool VaultHttpLogout();
std::vector<VaultHttpCredential> VaultHttpListCredentials();
bool VaultHttpUpsertCredential(const std::wstring& title,
                               const std::wstring& user,
                               const std::wstring& pass,
                               const std::wstring& url,
                               const std::wstring& note, bool pinned,
                               int sort_order);
// DELETE a remote credential by id.
bool VaultHttpDeleteCredential(int id);
// Push local entries to cloud (create missing / update matched by title+user).
bool VaultHttpPushAll(const std::vector<VaultHttpCredential>& entries);
// Push using in-memory master key when present (ZK), else plaintext PushAll.
bool VaultHttpPushLocalMirror(const std::vector<VaultHttpCredential>& entries);
// Pull remote list and merge into *entries (remote wins on title+user clash).
// enc_ver=1 rows are decrypted when a master key is in process memory.
bool VaultHttpPullMerge(std::vector<VaultHttpCredential>* entries);

// Zero-knowledge path: encrypt fields as enc_ver=1 before upload; decrypt on pull.
bool VaultHttpPushAllZk(const std::vector<VaultHttpCredential>& entries,
                        const std::vector<BYTE>& key);
bool VaultHttpPullMergeZk(std::vector<VaultHttpCredential>* entries,
                          const std::vector<BYTE>& key);

// Master key (derived) kept in process memory only — never persisted.
void VaultHttpSetMasterKey(const std::vector<BYTE>& key);
void VaultHttpClearMasterKey();
bool VaultHttpHasMasterKey();
// Server KDF params adopted at login (or persisted locally when first set).
bool VaultHttpGetKdfParams(std::wstring* salt_b64, int* iterations,
                           std::wstring* key_check, std::wstring* algo);
void VaultHttpSetKdfParams(const std::wstring& salt_b64, int iterations,
                           const std::wstring& key_check,
                           const std::wstring& algo);
// Upload master KDF params (base64 salt). Returns true when server accepted.
bool VaultHttpEnsureMaster(const std::wstring& salt_b64, int iterations,
                           const std::wstring& key_check,
                           const std::wstring& algo);
// Re-encrypt legacy enc_ver=0 rows to enc_ver=1 under *key. Returns count.
int VaultHttpMigrateLegacyToE2e(const std::vector<BYTE>& key);

// Online notes (same Bearer session as credentials). List omits body.
struct VaultHttpNote {
  int id = 0;
  std::wstring title;
  std::wstring body;
  std::wstring updated_at;
  std::wstring created_at;
  int enc_ver = 0;
};

bool VaultHttpListNotes(std::vector<VaultHttpNote>* out);
bool VaultHttpGetNote(int id, VaultHttpNote* out);
bool VaultHttpCreateNote(const std::wstring& title, const std::wstring& body,
                         VaultHttpNote* created);
bool VaultHttpUpdateNote(int id, const std::wstring& title,
                         const std::wstring& body, VaultHttpNote* updated);
bool VaultHttpDeleteNote(int id);

// Online-note image assets. Bodies only carry `assets/<name>` references; the
// binaries are stored separately on the server so other devices can render
// them. `data_b64` is standard base64 (no URL-safe alphabet).
bool VaultHttpPutNoteAsset(int note_id, const std::wstring& name,
                           const std::wstring& mime,
                           const std::string& data_b64);
// Returns raw bytes on success (mime is inferred from the filename on client).
bool VaultHttpGetNoteAsset(int note_id, const std::wstring& name,
                           std::vector<BYTE>* out_bytes);

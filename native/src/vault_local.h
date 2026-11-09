#pragma once

#include <string>
#include <vector>

// Local vault.dpapi + optional cloud upsert for DeskNote account mirror.
// Market: password managers edit locally then sync when signed in.

struct VaultLocalEntry {
  std::wstring title;
  std::wstring user;
  std::wstring pass;
  std::wstring url;
  std::wstring note;
  bool pinned = false;
};

struct VaultLocalSaveResult {
  bool ok = false;
  bool cloud_ok = false;
  bool cloud_attempted = false;
  int index = -1;  // final index after save
  std::wstring error;
};

std::wstring VaultAccountsNoteFileName();
bool IsVaultAccountsNotePath(const std::wstring& path);

std::vector<VaultLocalEntry> LoadVaultLocalEntries();
bool SaveVaultLocalEntries(const std::vector<VaultLocalEntry>& entries);

// Masked markdown mirror of the vault (no plaintext passwords on disk).
std::wstring BuildVaultAccountsMarkdown(const std::vector<VaultLocalEntry>& entries);
// JSON array for DeskNote HTML UI (masked passwords only).
std::wstring BuildVaultAccountsUiJson(const std::vector<VaultLocalEntry>& entries);
std::wstring BuildVaultAccountsUiJson();

// When vault_enabled: write/update 账号库.md under the notes library and return
// its path. When disabled: return empty (caller should drop the tab).
std::wstring SyncVaultAccountsNote(bool vault_enabled);

// Look up a password by entry title (+ optional username).
std::wstring FindVaultLocalPassword(const std::wstring& title,
                                    const std::wstring& user = {});

// field: "user" | "pass" | "url" | "note". Empty if index/field invalid.
std::wstring FindVaultLocalField(int index, const std::wstring& field);

// Create (index < 0) or update. Empty pass on update keeps the previous password.
// Writes DPAPI, refreshes 账号库.md, and upserts to cloud when logged in.
VaultLocalSaveResult UpsertVaultLocalEntry(int index, VaultLocalEntry entry,
                                           bool vault_enabled);

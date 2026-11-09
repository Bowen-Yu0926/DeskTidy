#pragma once

#include <string>
#include <vector>

// Lightweight local vault.dpapi reader for DeskNote (no WebView / cloud UI).
// Market: password managers show masked secrets; click/dblclick copies plaintext.

struct VaultLocalEntry {
  std::wstring title;
  std::wstring user;
  std::wstring pass;
  std::wstring url;
  std::wstring note;
  bool pinned = false;
};

std::wstring VaultAccountsNoteFileName();
bool IsVaultAccountsNotePath(const std::wstring& path);

std::vector<VaultLocalEntry> LoadVaultLocalEntries();

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

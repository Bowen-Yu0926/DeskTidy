#pragma once

#include <Windows.h>

#include <string>
#include <vector>

// WinHTTP client for DeskTidy vault API (enc_ver:0 plaintext path).
// Default base: https://bowen-yu0926.byethost10.com/desktidy-vault/
// Session: %LocalAppData%\DeskTidyNative\account_vault_session.json

struct VaultHttpCredential {
  int id = 0;
  std::wstring title;
  std::wstring username;
  std::wstring password;
  std::wstring url;
};

bool VaultHttpHealth();
bool VaultHttpLogin(const std::wstring& login, const std::wstring& password);
bool VaultHttpLogout();
std::vector<VaultHttpCredential> VaultHttpListCredentials();
bool VaultHttpUpsertCredential(const std::wstring& title,
                               const std::wstring& user,
                               const std::wstring& pass,
                               const std::wstring& url);
// Push local entries to cloud (create missing / update matched by title+user).
bool VaultHttpPushAll(const std::vector<VaultHttpCredential>& entries);
// Pull remote list and merge into *entries (remote wins on title+user clash).
bool VaultHttpPullMerge(std::vector<VaultHttpCredential>* entries);

// Zero-knowledge path: encrypt fields as enc_ver=1 before upload; decrypt on pull.
bool VaultHttpPushAllZk(const std::vector<VaultHttpCredential>& entries,
                        const std::vector<BYTE>& key);
bool VaultHttpPullMergeZk(std::vector<VaultHttpCredential>* entries,
                          const std::vector<BYTE>& key);
// Ensure server has master KDF params (salt hex + key_check). Creates if missing.
bool VaultHttpEnsureMaster(const std::vector<BYTE>& key,
                           std::string* salt_hex_inout);

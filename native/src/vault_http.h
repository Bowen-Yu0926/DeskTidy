#pragma once

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

#pragma once

#include <Windows.h>

#include <string>
#include <vector>

// DeskTidy vault ZK crypto — matches Python vault_crypto.py wire format:
// "v1:" + base64(nonce12 || tag16 || ciphertext), PBKDF2-HMAC-SHA256 600k.

constexpr int kVaultKeyLen = 32;
constexpr int kVaultNonceLen = 12;
constexpr int kVaultTagLen = 16;
constexpr int kVaultKdfIterations = 600000;

bool VaultDeriveKey(const std::wstring& master_password,
                    const std::vector<BYTE>& salt, std::vector<BYTE>* key_out);
std::wstring VaultEncryptField(const std::wstring& plain,
                               const std::vector<BYTE>& key);
std::wstring VaultDecryptField(const std::wstring& token,
                               const std::vector<BYTE>& key);
// key_check ciphertext for "desktidy-vault" plaintext.
std::wstring VaultMakeKeyCheck(const std::vector<BYTE>& key);
bool VaultVerifyKeyCheck(const std::wstring& key_check,
                         const std::vector<BYTE>& key);
std::vector<BYTE> VaultNewSalt(size_t n = 16);
std::string VaultBytesToHex(const std::vector<BYTE>& b);
std::vector<BYTE> VaultHexToBytes(const std::string& hex);

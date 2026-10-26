#pragma once

#include <Windows.h>

#include <string>
#include <vector>

// DeskTidy vault ZK crypto — matches Python vault_crypto.py wire format:
// "v1:" + base64(nonce12 || tag16 || ciphertext), PBKDF2 or Argon2id.

constexpr int kVaultKeyLen = 32;
constexpr int kVaultNonceLen = 12;
constexpr int kVaultTagLen = 16;
constexpr int kVaultKdfIterations = 600000;
// Python vault_crypto.DEFAULT_ARGON2_*
constexpr int kVaultArgon2MemoryKiB = 65536;
constexpr int kVaultArgon2Parallelism = 2;
constexpr int kVaultArgon2TimeCost = 3;

bool VaultDeriveKey(const std::wstring& master_password,
                    const std::vector<BYTE>& salt, std::vector<BYTE>* key_out);
// Derive with an explicit iteration count (server-adopted KDF params).
bool VaultDeriveKeyEx(const std::wstring& master_password,
                      const std::vector<BYTE>& salt, int iterations,
                      std::vector<BYTE>* key_out);
// algo: "pbkdf2" (default) or "argon2id". For argon2id, `iterations` is
// ignored — Python uses fixed time/memory/parallelism defaults.
bool VaultDeriveKeyForAlgo(const std::wstring& master_password,
                           const std::vector<BYTE>& salt, int iterations,
                           const std::wstring& algo,
                           std::vector<BYTE>* key_out);
// Preferred KDF when creating a new vault (always argon2id in native).
inline const wchar_t* VaultPreferredKdfAlgo() { return L"argon2id"; }
std::wstring VaultEncryptField(const std::wstring& plain,
                               const std::vector<BYTE>& key);
std::wstring VaultDecryptField(const std::wstring& token,
                               const std::vector<BYTE>& key);
// key_check ciphertext for "desktidy-vault" plaintext.
std::wstring VaultMakeKeyCheck(const std::vector<BYTE>& key);
bool VaultVerifyKeyCheck(const std::wstring& key_check,
                         const std::vector<BYTE>& key);
std::vector<BYTE> VaultNewSalt(size_t n = 16);
// Salt wire encoding: base64 (Python vault_crypto.py). Keep the legacy hex
// helpers so old local vault_zk_salt.txt data stays readable.
std::string VaultBytesToHex(const std::vector<BYTE>& b);
std::vector<BYTE> VaultHexToBytes(const std::string& hex);
std::string VaultBytesToB64(const std::vector<BYTE>& b);
std::vector<BYTE> VaultB64ToBytes(const std::string& b64);
// Decode a salt string that may be base64 (Python / server) or legacy hex.
std::vector<BYTE> VaultDecodeSalt(const std::string& s);

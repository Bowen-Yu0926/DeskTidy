#include "vault_crypto.h"

#include "paths_util.h"

#include "argon2.h"

#include <Windows.h>
#include <bcrypt.h>
#include <wincrypt.h>

#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")

namespace {

constexpr char kPrefix[] = "v1:";
constexpr char kKeyCheckPlain[] = "desktidy-vault";

std::string Narrow(const std::wstring& ws) { return NarrowUtf8(ws); }
std::wstring Widen(const std::string& s) { return WidenUtf8(s); }

std::string B64Encode(const BYTE* data, DWORD len) {
  DWORD out_len = 0;
  CryptBinaryToStringA(data, len, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                       nullptr, &out_len);
  std::string out(out_len, '\0');
  CryptBinaryToStringA(data, len, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                       out.data(), &out_len);
  while (!out.empty() && (out.back() == '\0' || out.back() == '\n' ||
                          out.back() == '\r')) {
    out.pop_back();
  }
  return out;
}

std::vector<BYTE> B64Decode(const std::string& s) {
  DWORD out_len = 0;
  CryptStringToBinaryA(s.c_str(), static_cast<DWORD>(s.size()),
                       CRYPT_STRING_BASE64, nullptr, &out_len, nullptr, nullptr);
  std::vector<BYTE> out(out_len);
  CryptStringToBinaryA(s.c_str(), static_cast<DWORD>(s.size()),
                       CRYPT_STRING_BASE64, out.data(), &out_len, nullptr,
                       nullptr);
  out.resize(out_len);
  return out;
}

struct AuthInfo {
  ULONG cbSize;
  ULONG dwInfoVersion;
  PUCHAR pbNonce;
  ULONG cbNonce;
  PUCHAR pbAuthData;
  ULONG cbAuthData;
  PUCHAR pbTag;
  ULONG cbTag;
  PUCHAR pbMacContext;
  ULONG cbMacContext;
  ULONG cbAAD;
  ULONGLONG cbData;
  ULONG dwFlags;
};

bool GcmCrypt(bool encrypt, const std::vector<BYTE>& key,
              const BYTE* nonce, ULONG nonce_len, const BYTE* in, ULONG in_len,
              BYTE* tag, ULONG tag_len, std::vector<BYTE>* out) {
  // Process-wide AES-GCM provider (key-independent, immutable). Opening the
  // provider + setting GCM chaining mode per field was the dominant cost of a
  // 4N-field vault sync (N records × 4 fields).
  static BCRYPT_ALG_HANDLE alg = []() -> BCRYPT_ALG_HANDLE {
    BCRYPT_ALG_HANDLE a = nullptr;
    if (BCryptOpenAlgorithmProvider(&a, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0) {
      return nullptr;
    }
    const wchar_t* mode = BCRYPT_CHAIN_MODE_GCM;
    if (BCryptSetProperty(
            a, BCRYPT_CHAINING_MODE,
            reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(mode)),
            static_cast<ULONG>((wcslen(mode) + 1) * sizeof(wchar_t)), 0) != 0) {
      BCryptCloseAlgorithmProvider(a, 0);
      return nullptr;
    }
    return a;
  }();
  if (!alg) {
    return false;
  }

  // Process-wide key-handle cache keyed by the last-used key bytes. The same
  // derived key encrypts/decrypts every field in a batch, so rebuilding the
  // handle only when the key actually changes keeps results identical while
  // removing per-field BCryptGenerateSymmetricKey.
  static BCRYPT_KEY_HANDLE hkey = nullptr;
  static std::vector<BYTE> cached_key;
  static std::mutex key_mutex;
  std::lock_guard<std::mutex> lock(key_mutex);
  if (hkey == nullptr || cached_key != key) {
    if (hkey) {
      BCryptDestroyKey(hkey);
    }
    hkey = nullptr;
    cached_key = key;
    if (BCryptGenerateSymmetricKey(
            alg, &hkey, nullptr, 0, const_cast<PUCHAR>(key.data()),
            static_cast<ULONG>(key.size()), 0) != 0) {
      hkey = nullptr;
      return false;
    }
  }
  AuthInfo info{};
  info.cbSize = sizeof(info);
  info.dwInfoVersion = 1;
  info.pbNonce = const_cast<PUCHAR>(nonce);
  info.cbNonce = nonce_len;
  info.pbTag = tag;
  info.cbTag = tag_len;
  info.cbData = in_len;
  out->assign(in_len ? in_len : 1, 0);
  ULONG out_len = 0;
  NTSTATUS st = 0;
  if (encrypt) {
    st = BCryptEncrypt(hkey, const_cast<PUCHAR>(in), in_len, &info, nullptr, 0,
                       out->data(), static_cast<ULONG>(out->size()), &out_len,
                       0);
  } else {
    st = BCryptDecrypt(hkey, const_cast<PUCHAR>(in), in_len, &info, nullptr, 0,
                       out->data(), static_cast<ULONG>(out->size()), &out_len,
                       0);
  }
  if (st != 0) {
    out->clear();
    return false;
  }
  out->resize(out_len);
  return true;
}

}  // namespace

std::vector<BYTE> VaultNewSalt(size_t n) {
  std::vector<BYTE> salt(n);
  BCryptGenRandom(nullptr, salt.data(), static_cast<ULONG>(n),
                  BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  return salt;
}

std::string VaultBytesToHex(const std::vector<BYTE>& b) {
  static const char* hex = "0123456789abcdef";
  std::string o;
  o.reserve(b.size() * 2);
  for (BYTE c : b) {
    o.push_back(hex[c >> 4]);
    o.push_back(hex[c & 0xf]);
  }
  return o;
}

std::vector<BYTE> VaultHexToBytes(const std::string& hex) {
  std::vector<BYTE> out;
  if (hex.size() % 2) {
    return out;
  }
  auto nib = [](char c) -> int {
    if (c >= '0' && c <= '9') {
      return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
      return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
      return c - 'A' + 10;
    }
    return -1;
  };
  for (size_t i = 0; i + 1 < hex.size(); i += 2) {
    const int hi = nib(hex[i]);
    const int lo = nib(hex[i + 1]);
    if (hi < 0 || lo < 0) {
      return {};
    }
    out.push_back(static_cast<BYTE>((hi << 4) | lo));
  }
  return out;
}

bool VaultDeriveKey(const std::wstring& master_password,
                    const std::vector<BYTE>& salt, std::vector<BYTE>* key_out) {
  return VaultDeriveKeyEx(master_password, salt, kVaultKdfIterations, key_out);
}

bool VaultDeriveKeyEx(const std::wstring& master_password,
                      const std::vector<BYTE>& salt, int iterations,
                      std::vector<BYTE>* key_out) {
  return VaultDeriveKeyForAlgo(master_password, salt, iterations, L"pbkdf2",
                               key_out);
}

bool VaultDeriveKeyForAlgo(const std::wstring& master_password,
                           const std::vector<BYTE>& salt, int iterations,
                           const std::wstring& algo,
                           std::vector<BYTE>* key_out) {
  if (!key_out || salt.empty()) {
    return false;
  }
  const std::string pw = Narrow(master_password);
  key_out->assign(kVaultKeyLen, 0);

  std::wstring a = algo;
  for (wchar_t& c : a) {
    if (c >= L'A' && c <= L'Z') {
      c = static_cast<wchar_t>(c - L'A' + L'a');
    }
  }
  if (a.empty() || a == L"pbkdf2") {
    if (iterations < 1) {
      iterations = kVaultKdfIterations;
    }
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr,
                                    BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0) {
      return false;
    }
    const NTSTATUS st = BCryptDeriveKeyPBKDF2(
        alg, reinterpret_cast<PUCHAR>(const_cast<char*>(pw.data())),
        static_cast<ULONG>(pw.size()), const_cast<PUCHAR>(salt.data()),
        static_cast<ULONG>(salt.size()), static_cast<ULONG>(iterations),
        key_out->data(), kVaultKeyLen, 0);
    BCryptCloseAlgorithmProvider(alg, 0);
    return st == 0;
  }
  if (a == L"argon2id") {
    // Match Python vault_crypto.derive_key(algo="argon2id") defaults.
    const int rc = argon2id_hash_raw(
        static_cast<uint32_t>(kVaultArgon2TimeCost),
        static_cast<uint32_t>(kVaultArgon2MemoryKiB),
        static_cast<uint32_t>(kVaultArgon2Parallelism), pw.data(), pw.size(),
        salt.data(), salt.size(), key_out->data(), kVaultKeyLen);
    return rc == ARGON2_OK;
  }
  return false;
}

std::wstring VaultEncryptField(const std::wstring& plain,
                               const std::vector<BYTE>& key) {
  if (key.size() != kVaultKeyLen) {
    return {};
  }
  const std::string utf8 = Narrow(plain);
  BYTE nonce[kVaultNonceLen] = {};
  BCryptGenRandom(nullptr, nonce, kVaultNonceLen,
                  BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  BYTE tag[kVaultTagLen] = {};
  std::vector<BYTE> cipher;
  if (!GcmCrypt(true, key, nonce, kVaultNonceLen,
                reinterpret_cast<const BYTE*>(utf8.data()),
                static_cast<ULONG>(utf8.size()), tag, kVaultTagLen, &cipher)) {
    return {};
  }
  std::vector<BYTE> packed;
  packed.insert(packed.end(), nonce, nonce + kVaultNonceLen);
  packed.insert(packed.end(), tag, tag + kVaultTagLen);
  packed.insert(packed.end(), cipher.begin(), cipher.end());
  return Widen(std::string(kPrefix) +
               B64Encode(packed.data(), static_cast<DWORD>(packed.size())));
}

std::wstring VaultDecryptField(const std::wstring& token,
                               const std::vector<BYTE>& key) {
  if (key.size() != kVaultKeyLen) {
    return {};
  }
  const std::string t = Narrow(token);
  if (t.rfind(kPrefix, 0) != 0) {
    return token;  // plaintext / enc_ver 0
  }
  const auto packed = B64Decode(t.substr(strlen(kPrefix)));
  if (packed.size() < kVaultNonceLen + kVaultTagLen) {
    return {};
  }
  const BYTE* nonce = packed.data();
  BYTE tag[kVaultTagLen];
  memcpy(tag, packed.data() + kVaultNonceLen, kVaultTagLen);
  const BYTE* cipher = packed.data() + kVaultNonceLen + kVaultTagLen;
  const ULONG cipher_len = static_cast<ULONG>(
      packed.size() - kVaultNonceLen - kVaultTagLen);
  std::vector<BYTE> plain;
  if (!GcmCrypt(false, key, nonce, kVaultNonceLen, cipher, cipher_len, tag,
                kVaultTagLen, &plain)) {
    return {};
  }
  return Widen(std::string(reinterpret_cast<char*>(plain.data()), plain.size()));
}

std::wstring VaultMakeKeyCheck(const std::vector<BYTE>& key) {
  return VaultEncryptField(Widen(kKeyCheckPlain), key);
}

bool VaultVerifyKeyCheck(const std::wstring& key_check,
                         const std::vector<BYTE>& key) {
  const std::wstring plain = VaultDecryptField(key_check, key);
  return Narrow(plain) == kKeyCheckPlain;
}

std::string VaultBytesToB64(const std::vector<BYTE>& b) {
  return B64Encode(b.data(), static_cast<DWORD>(b.size()));
}

std::vector<BYTE> VaultB64ToBytes(const std::string& b64) {
  if (b64.empty()) {
    return {};
  }
  return B64Decode(b64);
}

std::vector<BYTE> VaultDecodeSalt(const std::string& s) {
  if (s.empty()) {
    return {};
  }
  // Base64 salt always carries '=' padding (16 bytes → 24 chars, ends "==");
  // legacy hex salt is pure [0-9a-f] of even length with no padding.
  if (s.find('=') == std::string::npos && (s.size() % 2) == 0) {
    const std::vector<BYTE> hex = VaultHexToBytes(s);
    if (!hex.empty() && hex.size() * 2 == s.size()) {
      return hex;
    }
  }
  return VaultB64ToBytes(s);
}

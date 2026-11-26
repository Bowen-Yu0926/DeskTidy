#include "update_verify.h"

#include <cstring>

extern "C" {
#include "../third_party/tweetnacl/tweetnacl.h"
}

namespace {

// The public half of the release signing key.
//
// PLACEHOLDER — replace with the real key once generated:
//   python scripts/gen_update_signing_key.py
// prints the exact line to paste here. Left empty on purpose: an unset key
// makes checks fall back to digest-only (with a loud log) instead of silently
// trusting an attacker, and we never want a made-up key to look configured.
constexpr const char* kBuiltinSigningKeyB64 = "";

int B64Val(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+' || c == '-') return 62;
  if (c == '/' || c == '_') return 63;
  return -1;
}

}  // namespace

// Decode standard or URL-safe base64 (padding optional) into exactly `want`
// bytes. Shared by key (32) and signature (64) decoding.
static bool DecodeBase64Exact(const std::string& b64, unsigned char* out,
                              size_t want) {
  size_t n = 0;
  unsigned int acc = 0;
  int bits = 0;
  for (char c : b64) {
    if (c == '=' || c == ' ' || c == '\n' || c == '\r' || c == '\t') {
      continue;
    }
    const int v = B64Val(c);
    if (v < 0) {
      return false;
    }
    acc = (acc << 6) | static_cast<unsigned int>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (n >= want) {
        return false;  // longer than a `want`-byte value can encode
      }
      out[n++] = static_cast<unsigned char>((acc >> bits) & 0xFF);
    }
  }
  return n == want;
}

bool DecodeBase64To32(const std::string& b64, unsigned char out[32]) {
  return DecodeBase64Exact(b64, out, 32);
}

bool VerifyManifestSignature(const std::string& manifest,
                             const std::string& signature_b64,
                             const UpdateSigningKey& key) {
  if (!key.configured) {
    return false;
  }
  unsigned char sig[64];
  if (!DecodeBase64Exact(signature_b64, sig, sizeof(sig))) {
    return false;
  }

  // TweetNaCl's "open" takes signature||message and returns 0 on success. The
  // detached signature is the first 64 bytes, so hand it a joined buffer.
  std::string sm;
  sm.reserve(64 + manifest.size());
  sm.append(reinterpret_cast<const char*>(sig), 64);
  sm.append(manifest);

  std::string out(sm.size(), '\0');
  unsigned long long mlen = 0;
  const int rc = crypto_sign_open(
      reinterpret_cast<unsigned char*>(&out[0]), &mlen,
      reinterpret_cast<const unsigned char*>(sm.data()),
      static_cast<unsigned long long>(sm.size()), key.bytes);
  if (rc != 0) {
    return false;
  }
  // Signature could authenticate trailing bytes the caller didn't sign; require
  // the recovered message to be exactly the manifest.
  return mlen == manifest.size() &&
         std::memcmp(out.data(), manifest.data(), manifest.size()) == 0;
}

const UpdateSigningKey& BuiltinUpdateKey() {
  static const UpdateSigningKey key = [] {
    UpdateSigningKey k;
    if (kBuiltinSigningKeyB64[0] != '\0') {
      k.configured = DecodeBase64To32(kBuiltinSigningKeyB64, k.bytes);
    }
    return k;
  }();
  return key;
}

bool VerifyManifestWithBuiltinKey(const std::string& manifest,
                                  const std::string& signature_b64,
                                  bool* skipped) {
  if (skipped) {
    *skipped = false;
  }
  const UpdateSigningKey& key = BuiltinUpdateKey();
  if (!key.configured) {
    // Rollout window: no key embedded yet. Do not pretend we verified — report
    // skipped so the caller logs it and still enforces the SHA256 digest.
    if (skipped) {
      *skipped = true;
    }
    return true;
  }
  return VerifyManifestSignature(manifest, signature_b64, key);
}

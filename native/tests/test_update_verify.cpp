// Verifies Ed25519 manifest verification: the security boundary that stops a
// compromised GitHub release from shipping a malicious installer.
//
// Uses RFC 8032 TEST 1 (empty message) as a fixed, well-known vector so the
// test needs no fixture files and pinpoints a broken TweetNaCl import.
#include "update_verify.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

int g_bad = 0;

void Check(bool cond, const char* what) {
  if (!cond) {
    std::printf("FAIL %s\n", what);
    ++g_bad;
  }
}

// RFC 8032 §7.1 TEST 1: empty message, fixed keypair + signature.
constexpr const char* kRfcPk =
    "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a";
constexpr const char* kRfcSig =
    "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb88215"
    "90a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b";

void HexToBytes(const char* hex, unsigned char* out, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    unsigned v = 0;
    std::sscanf(hex + 2 * i, "%2x", &v);
    out[i] = static_cast<unsigned char>(v);
  }
}

std::string Base64(const unsigned char* data, size_t n) {
  static const char* kA =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  size_t i = 0;
  for (; i + 2 < n; i += 3) {
    const unsigned v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
    out += kA[(v >> 18) & 63];
    out += kA[(v >> 12) & 63];
    out += kA[(v >> 6) & 63];
    out += kA[v & 63];
  }
  if (i + 1 == n) {
    const unsigned v = data[i] << 16;
    out += kA[(v >> 18) & 63];
    out += kA[(v >> 12) & 63];
    out += "==";
  } else if (i + 2 == n) {
    const unsigned v = (data[i] << 16) | (data[i + 1] << 8);
    out += kA[(v >> 18) & 63];
    out += kA[(v >> 12) & 63];
    out += kA[(v >> 6) & 63];
    out += '=';
  }
  return out;
}

}  // namespace

int main() {
  unsigned char pk[32];
  unsigned char sig[64];
  HexToBytes(kRfcPk, pk, 32);
  HexToBytes(kRfcSig, sig, 64);

  UpdateSigningKey key;
  key.configured = true;
  std::memcpy(key.bytes, pk, 32);

  // Base64 round-trip of the key decodes back to the same bytes.
  const std::string pk64 = Base64(pk, 32);
  unsigned char rt[32] = {};
  Check(DecodeBase64To32(pk64, rt), "decode rfc pubkey");
  Check(std::memcmp(rt, pk, 32) == 0, "pubkey round-trip matches");

  // RFC 8032 TEST 1 signs the EMPTY message.
  const std::string empty;
  const std::string sig64 = Base64(sig, 64);
  Check(VerifyManifestSignature(empty, sig64, key),
        "valid rfc8032 signature accepted");

  // Every tamper path must be rejected.
  Check(!VerifyManifestSignature(empty + "x", sig64, key),
        "trailing byte rejected");
  Check(!VerifyManifestSignature("not empty", sig64, key),
        "different message rejected");

  unsigned char bad_sig[64];
  std::memcpy(bad_sig, sig, 64);
  bad_sig[0] ^= 0x01;
  Check(!VerifyManifestSignature(empty, Base64(bad_sig, 64), key),
        "tampered signature rejected");

  Check(!VerifyManifestSignature(empty, "", key), "empty signature rejected");
  Check(!VerifyManifestSignature(empty, "!!!notbase64!!!", key),
        "malformed base64 rejected");

  UpdateSigningKey wrong = key;
  wrong.bytes[0] ^= 0xFF;
  Check(!VerifyManifestSignature(empty, sig64, wrong),
        "wrong key rejected");

  UpdateSigningKey none;
  // An unconfigured key must never be treated as "verified".
  Check(!VerifyManifestSignature(empty, sig64, none),
        "unconfigured key never verifies");

  // The builtin key is either empty (rollout: signature checks are skipped and
  // the caller falls back to the sha256 digest) or a real public key (signature
  // enforced). This test must hold in BOTH states — asserting "always skipped"
  // broke the release the moment a key was embedded.
  bool skipped = false;
  const bool builtin_configured = BuiltinUpdateKey().configured;
  const bool wrapper_ok = VerifyManifestWithBuiltinKey(empty, sig64, &skipped);
  if (builtin_configured) {
    // Real key present: the BLAKE-derived RFC vector signature is not ours, so
    // verification must fail and the skip flag must stay clear.
    Check(!wrapper_ok, "configured key rejects foreign signature");
    Check(!skipped, "configured key does not report skipped");
  } else {
    Check(wrapper_ok, "unconfigured builtin passes through");
    Check(skipped, "unconfigured builtin sets skipped flag");
  }

  if (g_bad == 0) {
    std::printf("test_update_verify: all checks passed\n");
    return 0;
  }
  std::printf("test_update_verify: %d failure(s)\n", g_bad);
  return 1;
}

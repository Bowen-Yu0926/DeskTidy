#pragma once

#include <string>

// Ed25519 signature verification for the update manifest.
//
// Why this exists: SHA256 alone is not enough. The digest and the installer
// both come from the same GitHub release, so an attacker who takes over the
// account (or the release) can replace the exe AND its .sha256 together and
// every check still passes. The signature closes that hole because the
// verification key is compiled into the binary: only someone holding the
// offline private key can author a manifest this build will accept.
//
// Deliberately free of Win32 / network so it can be unit-tested directly
// (see tests/test_update_verify.cpp).

// Public key baked into the client (32 raw bytes). Base64 in the source so
// the same value is easy to paste into the signing script's output.
// A zero/empty key means "signing not configured" — see VerifyManifestSignature.
struct UpdateSigningKey {
  unsigned char bytes[32] = {};
  bool configured = false;
};

// The key compiled into this build. Never null; `configured` is false until a
// real key is embedded (then verification is skipped with a loud log so a
// half-finished rollout cannot brick updates).
const UpdateSigningKey& BuiltinUpdateKey();

// Decode standard base64 (with or without padding) into 32 bytes.
// Returns false on malformed input or wrong length.
bool DecodeBase64To32(const std::string& b64, unsigned char out[32]);

// Verify that `signature_b64` (standard base64) is a valid Ed25519 signature
// over `manifest` by the public key in `key`. Returns false on any problem
// (bad base64, bad length, bad signature, unconfigured key).
bool VerifyManifestSignature(const std::string& manifest,
                             const std::string& signature_b64,
                             const UpdateSigningKey& key);

// Convenience wrapper using BuiltinUpdateKey(). When the builtin key is not
// configured this returns true and sets *skipped (if non-null) so callers can
// fall back to digest-only checking during rollout, but log it loudly.
bool VerifyManifestWithBuiltinKey(const std::string& manifest,
                                  const std::string& signature_b64,
                                  bool* skipped);

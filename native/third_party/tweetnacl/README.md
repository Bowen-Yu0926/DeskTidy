# TweetNaCl (Ed25519 verify only)

Upstream: https://tweetnacl.cr.yp.to/20140427/ (tweetnacl.c / tweetnacl.h)
Author: Daniel J. Bernstein, Bernard van Gastel, Wesley Janssen,
        Tanja Lange, Peter Schwabe, Sjaak Smetsers.
License: public domain.

Used solely for **Ed25519 signature verification** of the update manifest
(`src/update_verify.cpp`). Signing happens offline/CI with the `cryptography`
Python package and the private key is never shipped.

`randombytes` is NOT provided by TweetNaCl: the build defines
`TWEETNACL_NO_RANDOMBYTES` and only the verify path is compiled, so the
keypair/signing entry points are never linked. If you ever need them, supply
a CSPRNG-backed `randombytes` (BCryptGenRandom).

Verified on import with RFC 8032 test vectors and a Python-signed
cross-check; see `native/tests/test_update_verify.cpp`.

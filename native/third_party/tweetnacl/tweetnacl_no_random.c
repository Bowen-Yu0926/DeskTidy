// randombytes shim for the vendored TweetNaCl.
//
// TweetNaCl's crypto_sign_keypair / crypto_box_keypair call randombytes().
// The client only ever VERIFIES signatures — it must never be able to generate
// a key or sign anything. Rather than link a real CSPRNG (which would make the
// signing path silently functional if ever called), we abort loudly. Nothing
// in the verify path reaches this, so a normal run never hits it.
//
// Kept in a separate file so tweetnacl.c stays byte-identical to upstream.

#include <stdlib.h>

void randombytes(unsigned char *x, unsigned long long n) {
  (void)x;
  (void)n;
  abort();
}

/* Arduino-core shim: the prebuilt ESP32 Arduino mbedTLS omits ChaCha20-Poly1305,
 * so MicroLink's Noise AEAD uses the bundled RFC 7539 reference implementation.
 * Tailscale's 96-bit nonce is 4 zero bytes + 64-bit BIG-endian counter, while the
 * reference code takes a 64-bit counter laid out little-endian after 4 zero bytes,
 * so the counter is byte-swapped here. */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "chacha20poly1305.h"

void ml_aead_encrypt(const uint8_t *key, uint64_t nonce, const uint8_t *ad, size_t ad_len,
                     const uint8_t *pt, size_t pt_len, uint8_t *ct) {
    chacha20poly1305_encrypt(ct, pt, pt_len, ad, ad_len, __builtin_bswap64(nonce), key);
}

int ml_aead_decrypt(const uint8_t *key, uint64_t nonce, const uint8_t *ad, size_t ad_len,
                    const uint8_t *ct, size_t ct_len, uint8_t *pt) {
    return chacha20poly1305_decrypt(pt, ct, ct_len, ad, ad_len, __builtin_bswap64(nonce), key) ? 0 : -1;
}

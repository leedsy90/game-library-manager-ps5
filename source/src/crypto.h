/* Small self-contained crypto for reading PS5 packages:
 * SHA-256, SHA3-256, HMAC-SHA256, AES-128 and AES-128-XTS. */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
  uint32_t h[8];
  uint64_t len;
  uint8_t buf[64];
  size_t n;
} sha256_ctx;

void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const void *data, size_t len);
void sha256_final(sha256_ctx *c, uint8_t out[32]);
void sha256(const void *data, size_t len, uint8_t out[32]);

void sha3_256(const void *data, size_t len, uint8_t out[32]);

void hmac_sha256(const uint8_t *key, size_t keylen, const void *data,
                 size_t len, uint8_t out[32]);

typedef struct {
  uint32_t ek[44]; /* encryption round keys */
  uint32_t dk[44]; /* decryption round keys (equivalent inverse cipher) */
} aes128_ctx;

void aes128_init(aes128_ctx *c, const uint8_t key[16]);
void aes128_encrypt(const aes128_ctx *c, const uint8_t in[16], uint8_t out[16]);
void aes128_decrypt(const aes128_ctx *c, const uint8_t in[16], uint8_t out[16]);

/* AES-128-XTS, as the PS5 uses it: the tweak is the 64-bit little-endian
 * sector number (upper 8 bytes zero); len must be a multiple of 16. */
typedef struct {
  aes128_ctx data, tweak;
} xts_ctx;

void xts_init(xts_ctx *x, const uint8_t data_key[16],
              const uint8_t tweak_key[16]);
void xts_decrypt(const xts_ctx *x, uint8_t *buf, size_t len, uint64_t sector);
void xts_encrypt(const xts_ctx *x, uint8_t *buf, size_t len, uint64_t sector);

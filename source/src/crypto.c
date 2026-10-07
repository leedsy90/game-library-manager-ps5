/* SHA-256, SHA3-256, HMAC-SHA256, AES-128 and AES-128-XTS.
 * Written for Game Library Manager; checked against Python's
 * hashlib/cryptography in tools/test_crypto.py. */
#include "crypto.h"

#include <pthread.h>
#include <string.h>

/* ------------------------------------------------------------ SHA-256 */

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(sha256_ctx *c, const uint8_t *p) {
  uint32_t w[64];
  for (int i = 0; i < 16; i++)
    w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 |
           (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = ROR32(w[i - 15], 7) ^ ROR32(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = ROR32(w[i - 2], 17) ^ ROR32(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4],
           f = c->h[5], g = c->h[6], h = c->h[7];
  for (int i = 0; i < 64; i++) {
    uint32_t S1 = ROR32(e, 6) ^ ROR32(e, 11) ^ ROR32(e, 25);
    uint32_t ch = (e & f) ^ (~e & g);
    uint32_t t1 = h + S1 + ch + K256[i] + w[i];
    uint32_t S0 = ROR32(a, 2) ^ ROR32(a, 13) ^ ROR32(a, 22);
    uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
    uint32_t t2 = S0 + mj;
    h = g, g = f, f = e, e = d + t1, d = cc, cc = b, b = a, a = t1 + t2;
  }
  c->h[0] += a, c->h[1] += b, c->h[2] += cc, c->h[3] += d;
  c->h[4] += e, c->h[5] += f, c->h[6] += g, c->h[7] += h;
}

void sha256_init(sha256_ctx *c) {
  static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372,
                                 0xa54ff53a, 0x510e527f, 0x9b05688c,
                                 0x1f83d9ab, 0x5be0cd19};
  memcpy(c->h, iv, sizeof(iv));
  c->len = 0;
  c->n = 0;
}

void sha256_update(sha256_ctx *c, const void *data, size_t len) {
  const uint8_t *p = data;
  c->len += len;
  if (c->n) {
    size_t k = 64 - c->n;
    if (k > len)
      k = len;
    memcpy(c->buf + c->n, p, k);
    c->n += k, p += k, len -= k;
    if (c->n == 64)
      sha256_block(c, c->buf), c->n = 0;
  }
  while (len >= 64)
    sha256_block(c, p), p += 64, len -= 64;
  if (len)
    memcpy(c->buf, p, len), c->n = len;
}

void sha256_final(sha256_ctx *c, uint8_t out[32]) {
  uint64_t bits = c->len * 8;
  uint8_t pad = 0x80;
  sha256_update(c, &pad, 1);
  pad = 0;
  while (c->n != 56)
    sha256_update(c, &pad, 1);
  uint8_t l[8];
  for (int i = 0; i < 8; i++)
    l[i] = (uint8_t)(bits >> (56 - 8 * i));
  sha256_update(c, l, 8);
  for (int i = 0; i < 8; i++) {
    out[4 * i] = (uint8_t)(c->h[i] >> 24);
    out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
    out[4 * i + 2] = (uint8_t)(c->h[i] >> 8);
    out[4 * i + 3] = (uint8_t)c->h[i];
  }
}

void sha256(const void *data, size_t len, uint8_t out[32]) {
  sha256_ctx c;
  sha256_init(&c);
  sha256_update(&c, data, len);
  sha256_final(&c, out);
}

void hmac_sha256(const uint8_t *key, size_t keylen, const void *data,
                 size_t len, uint8_t out[32]) {
  uint8_t k[64] = {0}, pad[64], inner[32];
  if (keylen > 64)
    sha256(key, keylen, k);
  else
    memcpy(k, key, keylen);
  sha256_ctx c;
  for (int i = 0; i < 64; i++)
    pad[i] = k[i] ^ 0x36;
  sha256_init(&c);
  sha256_update(&c, pad, 64);
  sha256_update(&c, data, len);
  sha256_final(&c, inner);
  for (int i = 0; i < 64; i++)
    pad[i] = k[i] ^ 0x5c;
  sha256_init(&c);
  sha256_update(&c, pad, 64);
  sha256_update(&c, inner, 32);
  sha256_final(&c, out);
}

/* ----------------------------------------------------------- SHA3-256 */

static const uint64_t KRC[24] = {
    0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808aULL,
    0x8000000080008000ULL, 0x000000000000808bULL, 0x0000000080000001ULL,
    0x8000000080008081ULL, 0x8000000000008009ULL, 0x000000000000008aULL,
    0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000aULL,
    0x000000008000808bULL, 0x800000000000008bULL, 0x8000000000008089ULL,
    0x8000000000008003ULL, 0x8000000000008002ULL, 0x8000000000000080ULL,
    0x000000000000800aULL, 0x800000008000000aULL, 0x8000000080008081ULL,
    0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL};
static const int KROT[24] = {1,  3,  6,  10, 15, 21, 28, 36, 45, 55, 2,  14,
                             27, 41, 56, 8,  25, 43, 62, 18, 39, 61, 20, 44};
static const int KPI[24] = {10, 7,  11, 17, 18, 3, 5,  16, 8,  21, 24, 4,
                            15, 23, 19, 13, 12, 2, 20, 14, 22, 9,  6,  1};

#define ROL64(x, n) (((x) << (n)) | ((x) >> (64 - (n))))

static void keccakf(uint64_t s[25]) {
  for (int r = 0; r < 24; r++) {
    uint64_t bc[5];
    for (int i = 0; i < 5; i++)
      bc[i] = s[i] ^ s[i + 5] ^ s[i + 10] ^ s[i + 15] ^ s[i + 20];
    for (int i = 0; i < 5; i++) {
      uint64_t t = bc[(i + 4) % 5] ^ ROL64(bc[(i + 1) % 5], 1);
      for (int j = 0; j < 25; j += 5)
        s[j + i] ^= t;
    }
    uint64_t t = s[1];
    for (int i = 0; i < 24; i++) {
      int j = KPI[i];
      uint64_t tmp = s[j];
      s[j] = ROL64(t, KROT[i]);
      t = tmp;
    }
    for (int j = 0; j < 25; j += 5) {
      for (int i = 0; i < 5; i++)
        bc[i] = s[j + i];
      for (int i = 0; i < 5; i++)
        s[j + i] ^= (~bc[(i + 1) % 5]) & bc[(i + 2) % 5];
    }
    s[0] ^= KRC[r];
  }
}

void sha3_256(const void *data, size_t len, uint8_t out[32]) {
  enum { RATE = 136 };
  uint64_t s[25] = {0};
  uint8_t blk[RATE];
  const uint8_t *p = data;
  for (;;) {
    size_t take = len < RATE ? len : RATE;
    memset(blk, 0, RATE);
    memcpy(blk, p, take);
    if (take < RATE) {
      blk[take] ^= 0x06;
      blk[RATE - 1] ^= 0x80;
    }
    for (int i = 0; i < RATE / 8; i++) {
      uint64_t w = 0;
      for (int b = 0; b < 8; b++)
        w |= (uint64_t)blk[8 * i + b] << (8 * b);
      s[i] ^= w;
    }
    keccakf(s);
    if (take < RATE)
      break;
    p += RATE, len -= RATE;
  }
  for (int i = 0; i < 32; i++)
    out[i] = (uint8_t)(s[i / 8] >> (8 * (i % 8)));
}

/* --------------------------------------------------------------- AES */

static const uint8_t SBOX[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b,
    0xfe, 0xd7, 0xab, 0x76, 0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0,
    0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26,
    0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2,
    0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0,
    0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84, 0x53, 0xd1, 0x00, 0xed,
    0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f,
    0x50, 0x3c, 0x9f, 0xa8, 0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5,
    0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd, 0x0c, 0x13, 0xec,
    0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14,
    0xde, 0x5e, 0x0b, 0xdb, 0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c,
    0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79, 0xe7, 0xc8, 0x37, 0x6d,
    0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f,
    0x4b, 0xbd, 0x8b, 0x8a, 0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e,
    0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e, 0xe1, 0xf8, 0x98, 0x11,
    0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f,
    0xb0, 0x54, 0xbb, 0x16};

static uint8_t SINV[256];
static uint32_t TE[4][256], TD[4][256];
static pthread_once_t g_aes_once = PTHREAD_ONCE_INIT;

static uint8_t gmul(uint8_t a, uint8_t b) {
  uint8_t r = 0;
  while (b) {
    if (b & 1)
      r ^= a;
    a = (uint8_t)((a << 1) ^ ((a & 0x80) ? 0x1b : 0));
    b >>= 1;
  }
  return r;
}

static uint32_t ror8(uint32_t x) { return (x >> 8) | (x << 24); }

static void aes_tables(void) {
  for (int i = 0; i < 256; i++)
    SINV[SBOX[i]] = (uint8_t)i;
  for (int i = 0; i < 256; i++) {
    uint8_t s = SBOX[i], si = SINV[i];
    uint32_t te = (uint32_t)gmul(s, 2) << 24 | (uint32_t)s << 16 |
                  (uint32_t)s << 8 | gmul(s, 3);
    uint32_t td = (uint32_t)gmul(si, 14) << 24 | (uint32_t)gmul(si, 9) << 16 |
                  (uint32_t)gmul(si, 13) << 8 | gmul(si, 11);
    for (int t = 0; t < 4; t++) {
      TE[t][i] = te;
      TD[t][i] = td;
      te = ror8(te);
      td = ror8(td);
    }
  }
}

static uint32_t be32(const uint8_t *p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 |
         p[3];
}
static void put_be32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8),
  p[3] = (uint8_t)v;
}

void aes128_init(aes128_ctx *c, const uint8_t key[16]) {
  pthread_once(&g_aes_once, aes_tables);
  static const uint8_t rcon[10] = {0x01, 0x02, 0x04, 0x08, 0x10,
                                   0x20, 0x40, 0x80, 0x1b, 0x36};
  uint32_t *rk = c->ek;
  for (int i = 0; i < 4; i++)
    rk[i] = be32(key + 4 * i);
  for (int i = 0; i < 10; i++, rk += 4) {
    uint32_t t = rk[3];
    rk[4] = rk[0] ^ ((uint32_t)SBOX[(t >> 16) & 0xff] << 24) ^
            ((uint32_t)SBOX[(t >> 8) & 0xff] << 16) ^
            ((uint32_t)SBOX[t & 0xff] << 8) ^ SBOX[t >> 24] ^
            ((uint32_t)rcon[i] << 24);
    rk[5] = rk[1] ^ rk[4];
    rk[6] = rk[2] ^ rk[5];
    rk[7] = rk[3] ^ rk[6];
  }
  /* decryption keys: reverse round order, InvMixColumns on the middle ones */
  for (int r = 0; r <= 10; r++)
    for (int j = 0; j < 4; j++)
      c->dk[4 * r + j] = c->ek[4 * (10 - r) + j];
  for (int i = 4; i < 40; i++) {
    uint32_t w = c->dk[i];
    c->dk[i] = TD[0][SBOX[w >> 24]] ^ TD[1][SBOX[(w >> 16) & 0xff]] ^
               TD[2][SBOX[(w >> 8) & 0xff]] ^ TD[3][SBOX[w & 0xff]];
  }
}

void aes128_encrypt(const aes128_ctx *c, const uint8_t in[16],
                    uint8_t out[16]) {
  const uint32_t *rk = c->ek;
  uint32_t s0 = be32(in) ^ rk[0], s1 = be32(in + 4) ^ rk[1],
           s2 = be32(in + 8) ^ rk[2], s3 = be32(in + 12) ^ rk[3];
  for (int r = 1; r < 10; r++) {
    rk += 4;
    uint32_t t0 = TE[0][s0 >> 24] ^ TE[1][(s1 >> 16) & 0xff] ^
                  TE[2][(s2 >> 8) & 0xff] ^ TE[3][s3 & 0xff] ^ rk[0];
    uint32_t t1 = TE[0][s1 >> 24] ^ TE[1][(s2 >> 16) & 0xff] ^
                  TE[2][(s3 >> 8) & 0xff] ^ TE[3][s0 & 0xff] ^ rk[1];
    uint32_t t2 = TE[0][s2 >> 24] ^ TE[1][(s3 >> 16) & 0xff] ^
                  TE[2][(s0 >> 8) & 0xff] ^ TE[3][s1 & 0xff] ^ rk[2];
    uint32_t t3 = TE[0][s3 >> 24] ^ TE[1][(s0 >> 16) & 0xff] ^
                  TE[2][(s1 >> 8) & 0xff] ^ TE[3][s2 & 0xff] ^ rk[3];
    s0 = t0, s1 = t1, s2 = t2, s3 = t3;
  }
  rk += 4;
#define SB(x, sh) ((uint32_t)SBOX[((x) >> (sh)) & 0xff] << (sh))
  put_be32(out, (SB(s0, 24) | SB(s1, 16) | SB(s2, 8) | SB(s3, 0)) ^ rk[0]);
  put_be32(out + 4, (SB(s1, 24) | SB(s2, 16) | SB(s3, 8) | SB(s0, 0)) ^ rk[1]);
  put_be32(out + 8, (SB(s2, 24) | SB(s3, 16) | SB(s0, 8) | SB(s1, 0)) ^ rk[2]);
  put_be32(out + 12, (SB(s3, 24) | SB(s0, 16) | SB(s1, 8) | SB(s2, 0)) ^ rk[3]);
#undef SB
}

void aes128_decrypt(const aes128_ctx *c, const uint8_t in[16],
                    uint8_t out[16]) {
  const uint32_t *rk = c->dk;
  uint32_t s0 = be32(in) ^ rk[0], s1 = be32(in + 4) ^ rk[1],
           s2 = be32(in + 8) ^ rk[2], s3 = be32(in + 12) ^ rk[3];
  for (int r = 1; r < 10; r++) {
    rk += 4;
    uint32_t t0 = TD[0][s0 >> 24] ^ TD[1][(s3 >> 16) & 0xff] ^
                  TD[2][(s2 >> 8) & 0xff] ^ TD[3][s1 & 0xff] ^ rk[0];
    uint32_t t1 = TD[0][s1 >> 24] ^ TD[1][(s0 >> 16) & 0xff] ^
                  TD[2][(s3 >> 8) & 0xff] ^ TD[3][s2 & 0xff] ^ rk[1];
    uint32_t t2 = TD[0][s2 >> 24] ^ TD[1][(s1 >> 16) & 0xff] ^
                  TD[2][(s0 >> 8) & 0xff] ^ TD[3][s3 & 0xff] ^ rk[2];
    uint32_t t3 = TD[0][s3 >> 24] ^ TD[1][(s2 >> 16) & 0xff] ^
                  TD[2][(s1 >> 8) & 0xff] ^ TD[3][s0 & 0xff] ^ rk[3];
    s0 = t0, s1 = t1, s2 = t2, s3 = t3;
  }
  rk += 4;
#define SI(x, sh) ((uint32_t)SINV[((x) >> (sh)) & 0xff] << (sh))
  put_be32(out, (SI(s0, 24) | SI(s3, 16) | SI(s2, 8) | SI(s1, 0)) ^ rk[0]);
  put_be32(out + 4, (SI(s1, 24) | SI(s0, 16) | SI(s3, 8) | SI(s2, 0)) ^ rk[1]);
  put_be32(out + 8, (SI(s2, 24) | SI(s1, 16) | SI(s0, 8) | SI(s3, 0)) ^ rk[2]);
  put_be32(out + 12, (SI(s3, 24) | SI(s2, 16) | SI(s1, 8) | SI(s0, 0)) ^ rk[3]);
#undef SI
}

/* --------------------------------------------------------------- XTS */

void xts_init(xts_ctx *x, const uint8_t data_key[16],
              const uint8_t tweak_key[16]) {
  aes128_init(&x->data, data_key);
  aes128_init(&x->tweak, tweak_key);
}

static void xts_crypt(const xts_ctx *x, uint8_t *buf, size_t len,
                      uint64_t sector, int enc) {
  uint8_t t[16] = {0}, b[16];
  for (int i = 0; i < 8; i++)
    t[i] = (uint8_t)(sector >> (8 * i));
  aes128_encrypt(&x->tweak, t, t);
  for (size_t off = 0; off + 16 <= len; off += 16) {
    for (int i = 0; i < 16; i++)
      b[i] = buf[off + i] ^ t[i];
    if (enc)
      aes128_encrypt(&x->data, b, b);
    else
      aes128_decrypt(&x->data, b, b);
    for (int i = 0; i < 16; i++)
      buf[off + i] = b[i] ^ t[i];
    /* multiply the tweak by x in GF(2^128), little-endian */
    int carry = 0;
    for (int i = 0; i < 16; i++) {
      int nc = t[i] >> 7;
      t[i] = (uint8_t)((t[i] << 1) | carry);
      carry = nc;
    }
    if (carry)
      t[0] ^= 0x87;
  }
}

void xts_decrypt(const xts_ctx *x, uint8_t *buf, size_t len, uint64_t sector) {
  xts_crypt(x, buf, len, sector, 0);
}
void xts_encrypt(const xts_ctx *x, uint8_t *buf, size_t len, uint64_t sector) {
  xts_crypt(x, buf, len, sector, 1);
}

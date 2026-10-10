/* Read-only peeking inside .exfat, .ffpkg (UFS2) and .ffpfs/.ffpfsc
 * (exFAT inside a PFS container) images without mounting them. We only
 * need the top one or two directory levels and sce_sys/param.json, so
 * this is a tiny reader, not a filesystem driver. The only write is the
 * save fix rename at the end of this file. */
#include "common.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "libdeflate.h"
#include "zlib.h"

const char *image_kind(const char *name) {
  if (ends_with_ci(name, ".ffpkg"))
    return "ufs";
  if (ends_with_ci(name, ".exfat"))
    return "exfat";
  if (ends_with_ci(name, ".ffpfsc"))
    return "pfsc";
  if (ends_with_ci(name, ".ffpfs"))
    return "pfs";
  return NULL;
}

static bool pread_full(int fd, void *buf, size_t n, uint64_t off) {
  uint8_t *p = buf;
  while (n) {
    ssize_t r = pread(fd, p, n, (off_t)off);
    if (r <= 0)
      return false;
    p += r;
    n -= (size_t)r;
    off += (uint64_t)r;
  }
  return true;
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
         (uint32_t)p[3] << 24;
}
static uint64_t le64(const uint8_t *p) {
  return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32;
}

/* ================================================================== */
/* Volume I/O: an exFAT volume is either the whole .exfat file, or the  */
/* single file inside a .ffpfs / .ffpfsc container (what PS5 FFPFSC PRO */
/* / MkPFS produce: an unsigned, unencrypted PFS holding one exFAT     */
/* image, stored plain (.ffpfs) or as PFSC zlib blocks (.ffpfsc)).     */
/*                                                                    */
/* PFSC: a header, a table of (n+1) u64 offsets, then n blocks of     */
/* 64 KB each, every block zlib-compressed on its own (or stored raw  */
/* when its slot is exactly 64 KB). A write re-encodes the block to   */
/* exactly its old length where it can, or else borrows a few bytes   */
/* from a nearby block that compresses smaller; nothing outside that  */
/* stretch of blocks and their offset table entries is ever touched.  */

#define PFS_MAGIC 20130315
#define PFSC_MAGIC 0x43534650u
#define PFS_DINODE_SIZE 0xA8

typedef struct {
  int fd;
  const char *path; /* for the backup file written before a block pair */
  uint64_t base;    /* byte offset of the volume (or PFSC payload) */
  uint64_t size;    /* logical size of the volume */
  bool pfsc;
  uint32_t lbs;     /* PFSC logical block size */
  uint64_t nblocks;
  uint64_t offtab;  /* absolute offset of the PFSC offset table */
  int64_t cache_idx;
  uint8_t *cache;
} vio_t;

static bool vio_open_plain(vio_t *io, int fd) {
  memset(io, 0, sizeof(*io));
  io->fd = fd;
  io->cache_idx = -1;
  off_t end = lseek(fd, 0, SEEK_END);
  io->size = end > 0 ? (uint64_t)end : 0;
  return true;
}

/* Find the one exFAT file inside an unsigned, unencrypted PFS container. */
static bool vio_open_pfs(vio_t *io, int fd) {
  memset(io, 0, sizeof(*io));
  io->fd = fd;
  io->cache_idx = -1;
  uint8_t h[0x48];
  if (!pread_full(fd, h, sizeof(h), 0) || le64(h + 8) != PFS_MAGIC)
    return false;
  uint16_t mode = le16(h + 0x1C);
  uint32_t bs = le32(h + 0x20);
  uint64_t ninodes = le64(h + 0x30);
  if ((mode & 0x5) || bs < 4096 || bs > 1024 * 1024 || ninodes > 1000000)
    return false; /* signed or encrypted: not something we can read */
  uint32_t per_block = bs / PFS_DINODE_SIZE;
  uint64_t best_size = 0, best_db = 0;
  bool best_comp = false;
  for (uint64_t i = 0; i < ninodes; i++) {
    uint8_t di[PFS_DINODE_SIZE];
    uint64_t off = (uint64_t)bs + (i / per_block) * bs +
                   (i % per_block) * PFS_DINODE_SIZE;
    if (!pread_full(fd, di, sizeof(di), off))
      return false;
    uint16_t imode = le16(di);
    uint32_t flags = le32(di + 4);
    uint64_t size = le64(di + 8);
    int32_t db0 = (int32_t)le32(di + 0x64);
    if (!(imode & 0x8000) || (flags & 0x20000) || db0 <= 0)
      continue; /* not a regular file, or the PFS's own internal files */
    if (size > best_size) {
      best_size = size;
      best_db = (uint64_t)db0;
      best_comp = (flags & 0x1) != 0;
    }
  }
  if (!best_size)
    return false;
  io->base = best_db * bs;
  io->size = best_size;
  if (!best_comp)
    return true;
  uint8_t p[0x30];
  if (!pread_full(fd, p, sizeof(p), io->base) || le32(p) != PFSC_MAGIC ||
      le32(p + 4) != 0 || le32(p + 8) != 6)
    return false;
  io->lbs = le32(p + 0x0C);
  uint64_t logical = le64(p + 0x28);
  if (io->lbs != 0x10000 || le64(p + 0x10) != io->lbs || !logical ||
      logical % io->lbs)
    return false;
  io->pfsc = true;
  io->size = logical;
  io->nblocks = logical / io->lbs;
  io->offtab = io->base + le64(p + 0x18);
  io->cache = malloc(io->lbs);
  return io->cache != NULL;
}

static void vio_close(vio_t *io) {
  free(io->cache);
  io->cache = NULL;
}

/* slot of PFSC block idx: absolute offset and stored length */
static bool pfsc_slot(vio_t *io, uint64_t idx, uint64_t *off, uint64_t *len) {
  uint8_t o[16];
  if (idx >= io->nblocks || !pread_full(io->fd, o, 16, io->offtab + idx * 8))
    return false;
  uint64_t a = le64(o), b = le64(o + 8);
  if (b <= a || b - a > io->lbs)
    return false;
  *off = io->base + a;
  *len = b - a;
  return true;
}

static bool pfsc_get(vio_t *io, uint64_t idx) {
  if (io->cache_idx == (int64_t)idx)
    return true;
  io->cache_idx = -1;
  uint64_t off, len;
  if (!pfsc_slot(io, idx, &off, &len))
    return false;
  uint8_t *stored = malloc(len);
  if (!stored)
    return false;
  bool ok = pread_full(io->fd, stored, len, off);
  if (ok && len == io->lbs) {
    memcpy(io->cache, stored, len);
  } else if (ok) {
    uLongf out = io->lbs;
    ok = uncompress(io->cache, &out, stored, (uLong)len) == Z_OK &&
         out == io->lbs;
  }
  free(stored);
  if (ok)
    io->cache_idx = (int64_t)idx;
  return ok;
}

static bool vio_read(vio_t *io, void *buf, size_t n, uint64_t off) {
  if (!io->pfsc)
    return pread_full(io->fd, buf, n, io->base + off);
  uint8_t *p = buf;
  while (n) {
    uint64_t idx = off / io->lbs, in = off % io->lbs;
    if (!pfsc_get(io, idx))
      return false;
    size_t k = io->lbs - in < n ? io->lbs - in : n;
    memcpy(p, io->cache + in, k);
    p += k;
    off += k;
    n -= k;
  }
  return true;
}

/* zlib-encode one block into exactly `want` bytes, as
 *   zlib header | padding | deflate(data) | adler32
 * The padding is empty, non-final deflate blocks, which every inflater
 * skips: g empty stored blocks (5 bytes each) for big steps, then m empty
 * fixed-Huffman blocks (10 bits each) for small ones. Those 10-bit blocks
 * leave the compressed data starting mid-byte, so it is copied bit by bit
 * using its exact bit length (from inflate).
 *
 * The block was originally made by zlib at level 9, so re-encoding it at
 * that level after a change often comes out a byte or two too big. That's
 * why libdeflate's near-optimal levels (12..10) are tried first: they
 * usually leave room to spare. zlib's settings are the fallback. The first
 * result that can be padded to exactly `want` is used. */
static void put_bits(uint8_t *out, uint64_t *bitpos, uint32_t v, int n) {
  for (int i = 0; i < n; i++, (*bitpos)++) {
    if ((*bitpos & 7) == 0)
      out[*bitpos >> 3] = 0;
    if (v >> i & 1)
      out[*bitpos >> 3] |= (uint8_t)(1u << (*bitpos & 7));
  }
}

/* exact length in bits of a raw deflate stream that inflates to n bytes */
static bool deflate_bits(const uint8_t *raw, uint64_t len, uint8_t *scratch,
                         uint32_t n, uint64_t *bits) {
  z_stream s;
  memset(&s, 0, sizeof(s));
  if (inflateInit2(&s, -MAX_WBITS) != Z_OK)
    return false;
  s.next_in = (Bytef *)raw;
  s.avail_in = (uInt)len;
  s.next_out = scratch;
  s.avail_out = n;
  bool ok = inflate(&s, Z_FINISH) == Z_STREAM_END && s.total_out == n;
  /* data_type holds the unused bits left in the last byte read */
  *bits = (uint64_t)s.total_in * 8 - (uint64_t)(s.data_type & 63);
  inflateEnd(&s);
  return ok && *bits <= len * 8 && *bits + 8 > len * 8;
}

/* raw-deflate data into at most `cap` bytes with setting number t.
 * Returns the compressed size, 0 if it doesn't fit, -1 when out of tries. */
static int64_t deflate_try(int t, const uint8_t *data, uint32_t n,
                           uint8_t *out, uint64_t cap) {
  static const int ld_levels[] = {12, 11, 10};
  static const struct {
    int level, mem, strategy;
  } zl[] = {{9, 8, Z_DEFAULT_STRATEGY}, {9, 9, Z_DEFAULT_STRATEGY},
            {8, 8, Z_DEFAULT_STRATEGY}, {6, 8, Z_DEFAULT_STRATEGY},
            {9, 8, Z_FILTERED},         {6, 8, Z_FILTERED}};
  int nld = (int)(sizeof(ld_levels) / sizeof(ld_levels[0]));
  if (t < nld) {
    struct libdeflate_compressor *c =
        libdeflate_alloc_compressor(ld_levels[t]);
    if (!c)
      return 0;
    size_t a = libdeflate_deflate_compress(c, data, n, out, (size_t)cap);
    libdeflate_free_compressor(c);
    return (int64_t)a;
  }
  t -= nld;
  if (t >= (int)(sizeof(zl) / sizeof(zl[0])))
    return -1;
  z_stream s;
  memset(&s, 0, sizeof(s));
  if (deflateInit2(&s, zl[t].level, Z_DEFLATED, -MAX_WBITS, zl[t].mem,
                   zl[t].strategy) != Z_OK)
    return 0;
  s.next_in = (Bytef *)data;
  s.avail_in = n;
  s.next_out = out;
  s.avail_out = (uInt)cap;
  int rc = deflate(&s, Z_FINISH);
  uint64_t a = s.total_out;
  deflateEnd(&s);
  return rc == Z_STREAM_END ? (int64_t)a : 0;
}

static bool zlib_encode_exact(const uint8_t *data, uint32_t n, uint8_t *out,
                              uint64_t want) {
  if (want < 8)
    return false;
  uint8_t *raw = malloc((size_t)want), *scratch = malloc(n);
  bool done = false;
  int64_t a;
  for (int t = 0; raw && scratch && !done &&
                  (a = deflate_try(t, data, n, raw, want - 6)) >= 0;
       t++) {
    uint64_t bits;
    if (a == 0 || !deflate_bits(raw, (uint64_t)a, scratch, n, &bits))
      continue; /* too big at this setting */
    /* find g stored + m fixed empty blocks giving exactly want bytes */
    uint64_t body = want - 6; /* bytes between zlib header and adler32 */
    int g = -1, m = -1;
    for (uint64_t gg = 0; gg * 5 <= body && g < 0; gg++)
      for (int mm = 0; mm < 16; mm++)
        if (gg * 5 + (10 * (uint64_t)mm + bits + 7) / 8 == body) {
          g = (int)gg;
          m = mm;
          break;
        }
    if (g < 0)
      continue;
    uint64_t bit = 16;
    out[0] = 0x78;
    out[1] = 0xDA;
    for (int i = 0; i < g; i++) {
      put_bits(out, &bit, 0, 3);   /* BFINAL=0, BTYPE=00 stored */
      put_bits(out, &bit, 0, 5);   /* align (we start byte-aligned) */
      put_bits(out, &bit, 0x0000, 16);
      put_bits(out, &bit, 0xFFFF, 16);
    }
    for (int i = 0; i < m; i++) {
      put_bits(out, &bit, 0, 1); /* BFINAL=0 */
      put_bits(out, &bit, 1, 2); /* BTYPE=01 fixed Huffman */
      put_bits(out, &bit, 0, 7); /* end-of-block, code 256 = 0000000 */
    }
    for (uint64_t i = 0; i < bits; i++)
      put_bits(out, &bit, raw[i >> 3] >> (i & 7) & 1, 1);
    while (bit & 7)
      put_bits(out, &bit, 0, 1);
    uint64_t o = bit >> 3;
    uLong ad = adler32(adler32(0L, Z_NULL, 0), data, n);
    out[o++] = (uint8_t)(ad >> 24);
    out[o++] = (uint8_t)(ad >> 16);
    out[o++] = (uint8_t)(ad >> 8);
    out[o++] = (uint8_t)ad;
    done = o == want;
  }
  free(raw);
  free(scratch);
  return done;
}

/* Encode `data` (one logical block) into exactly `len` bytes and check it
 * decodes back. A slot of exactly one block holds the data uncompressed. */
static bool pfsc_encode(vio_t *io, const uint8_t *data, uint8_t *enc,
                        uint64_t len) {
  if (len == io->lbs) {
    memcpy(enc, data, len);
    return true;
  }
  if (len > io->lbs)
    return false;
  uint8_t *chk = malloc(io->lbs);
  uLongf out = io->lbs;
  bool ok = chk && zlib_encode_exact(data, io->lbs, enc, len) &&
            uncompress(chk, &out, enc, (uLong)len) == Z_OK &&
            out == io->lbs && !memcmp(chk, data, io->lbs);
  free(chk);
  return ok;
}

#define PFSC_DONOR_RANGE 256 /* blocks searched on each side for spare bytes */

static void put_le64(uint8_t *p, uint64_t v) {
  for (int i = 0; i < 8; i++)
    p[i] = (uint8_t)(v >> (8 * i));
}

/* When block idx needs D more bytes than its slot, take them from the
 * nearest block j (either side) that can be re-encoded D bytes smaller.
 * The blocks between idx and j keep their compressed bytes exactly as they
 * are and just move by D; their offset table entries move with them. Only
 * the bytes from idx to j and those table entries change. Both are saved
 * to "<image>.glm-backup" first and restored if anything fails to read
 * back; the backup is removed once everything verifies. */
static bool pfsc_put_shift(vio_t *io, uint64_t idx, const uint8_t *data) {
  uint64_t off_i, len_i;
  if (!pfsc_slot(io, idx, &off_i, &len_i))
    return false;
  uint8_t *enc_me = malloc(io->lbs), *enc_j = malloc(io->lbs),
          *data_j = malloc(io->lbs), *raw = malloc(io->lbs);
  bool ok = enc_me && enc_j && data_j && raw;
  /* how many extra bytes the changed block needs */
  int64_t a = ok ? deflate_try(0, data, io->lbs, raw, io->lbs - 1) : 0;
  uint64_t w = 0;
  for (uint64_t t = len_i + 1; ok && a > 0 && t < io->lbs && t <= len_i + 96;
       t++)
    if (t >= (uint64_t)a + 6 && pfsc_encode(io, data, enc_me, t)) {
      w = t;
      break;
    }
  ok = ok && w;
  uint64_t d = w - len_i;
  /* nearest donor */
  int64_t j = -1;
  uint64_t off_j = 0, len_j = 0;
  for (uint64_t dist = 1; ok && j < 0 && dist <= PFSC_DONOR_RANGE; dist++) {
    for (int side = 0; side < 2 && j < 0; side++) {
      if (side == 0 && idx + dist >= io->nblocks)
        continue;
      if (side == 1 && dist > idx)
        continue;
      uint64_t c = side == 0 ? idx + dist : idx - dist, oc, lc;
      if (!pfsc_slot(io, c, &oc, &lc) || lc < d + 16 || !pfsc_get(io, c))
        continue;
      memcpy(data_j, io->cache, io->lbs);
      int64_t ac = deflate_try(0, data_j, io->lbs, raw, lc - d);
      if (ac <= 0 || (uint64_t)ac + 6 > lc - d)
        continue;
      if (pfsc_encode(io, data_j, enc_j, lc - d)) {
        j = (int64_t)c;
        off_j = oc;
        len_j = lc;
      }
    }
  }
  ok = ok && j >= 0;
  if (!ok)
    goto out;
  {
    bool right = (uint64_t)j > idx;
    uint64_t start = right ? off_i : off_j;
    uint64_t end = right ? off_j + len_j : off_i + len_i;
    uint64_t span = end - start;
    /* table entries that move: the starts of blocks after the lower one,
     * up to and including the higher one */
    uint64_t t0 = (right ? idx : (uint64_t)j) + 1;
    uint64_t t1 = right ? (uint64_t)j : idx;
    uint64_t nent = t1 - t0 + 1;
    uint8_t *old = malloc((size_t)span), *neu = malloc((size_t)span),
            *oldtab = malloc(8 * nent), *newtab = malloc(8 * nent);
    char bpath[1100];
    snprintf(bpath, sizeof(bpath), "%s.glm-backup", io->path ? io->path : "");
    ok = old && neu && oldtab && newtab && io->path &&
         pread_full(io->fd, old, (size_t)span, start) &&
         pread_full(io->fd, oldtab, 8 * nent, io->offtab + t0 * 8);
    if (ok) {
      /* new layout */
      if (right) {
        memcpy(neu, enc_me, w);
        memcpy(neu + w, old + len_i, (size_t)(off_j - off_i - len_i));
        memcpy(neu + (off_j - off_i) + d, enc_j, (size_t)(len_j - d));
      } else {
        memcpy(neu, enc_j, (size_t)(len_j - d));
        memcpy(neu + len_j - d, old + len_j, (size_t)(off_i - off_j - len_j));
        memcpy(neu + (off_i - off_j) - d, enc_me, w);
      }
      for (uint64_t k = 0; k < nent; k++)
        put_le64(newtab + 8 * k,
                 right ? le64(oldtab + 8 * k) + d : le64(oldtab + 8 * k) - d);
      /* backup: start, span, first entry, entry count, entries, bytes */
      uint8_t head[32];
      put_le64(head, start);
      put_le64(head + 8, span);
      put_le64(head + 16, t0);
      put_le64(head + 24, nent);
      int bfd = open(bpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
      ok = bfd >= 0 && write(bfd, head, 32) == 32 &&
           write(bfd, oldtab, 8 * nent) == (ssize_t)(8 * nent) &&
           write(bfd, old, (size_t)span) == (ssize_t)span && fsync(bfd) == 0;
      if (bfd >= 0)
        close(bfd);
    }
    if (ok) {
      bool wrote =
          pwrite(io->fd, neu, (size_t)span, (off_t)start) == (ssize_t)span &&
          pwrite(io->fd, newtab, 8 * nent, (off_t)(io->offtab + t0 * 8)) ==
              (ssize_t)(8 * nent) &&
          fsync(io->fd) == 0;
      io->cache_idx = -1;
      bool good = wrote && pfsc_get(io, (uint64_t)j) &&
                  !memcmp(io->cache, data_j, io->lbs) && pfsc_get(io, idx) &&
                  !memcmp(io->cache, data, io->lbs);
      for (uint64_t k = t0; good && k <= t1; k++)
        good = pfsc_get(io, k) || k == idx;
      if (good) {
        unlink(bpath);
      } else {
        if (pwrite(io->fd, old, (size_t)span, (off_t)start) ==
                (ssize_t)span &&
            pwrite(io->fd, oldtab, 8 * nent, (off_t)(io->offtab + t0 * 8)) ==
                (ssize_t)(8 * nent) &&
            fsync(io->fd) == 0)
          unlink(bpath);
        ok = false;
      }
      io->cache_idx = -1;
    }
    free(old);
    free(neu);
    free(oldtab);
    free(newtab);
  }
out:
  free(enc_me);
  free(enc_j);
  free(data_j);
  free(raw);
  return ok;
}

/* Rewrite PFSC block idx with new contents: in its own slot when it can be
 * re-encoded to exactly the same length, otherwise by borrowing a few
 * bytes from a nearby block (see pfsc_put_shift). Everything is decoded
 * again before it is written, and read back from disk afterwards. */
static bool pfsc_put(vio_t *io, uint64_t idx, const uint8_t *data) {
  uint64_t off, len;
  if (!pfsc_slot(io, idx, &off, &len))
    return false;
  uint8_t *enc = malloc(io->lbs);
  bool ok = enc && pfsc_encode(io, data, enc, len) &&
            pwrite(io->fd, enc, (size_t)len, (off_t)off) == (ssize_t)len &&
            fsync(io->fd) == 0;
  free(enc);
  io->cache_idx = -1;
  if (ok)
    return pfsc_get(io, idx) && !memcmp(io->cache, data, io->lbs);
  return pfsc_put_shift(io, idx, data);
}

static bool vio_write(vio_t *io, const void *buf, size_t n, uint64_t off) {
  if (!io->pfsc)
    return pwrite(io->fd, buf, n, (off_t)(io->base + off)) == (ssize_t)n;
  const uint8_t *p = buf;
  uint8_t *blk = malloc(io->lbs);
  bool ok = blk != NULL;
  while (ok && n) {
    uint64_t idx = off / io->lbs, in = off % io->lbs;
    size_t k = io->lbs - in < n ? io->lbs - in : n;
    ok = pfsc_get(io, idx);
    if (ok) {
      memcpy(blk, io->cache, io->lbs);
      memcpy(blk + in, p, k);
      ok = pfsc_put(io, idx, blk);
    }
    p += k;
    off += k;
    n -= k;
  }
  free(blk);
  return ok;
}

/* Generic directory entry */
typedef struct {
  char name[256];
  bool dir;
  uint64_t ref;   /* exfat: first cluster, ufs: inode */
  uint64_t size;
  bool contig;    /* exfat NoFatChain */
} dent_t;

#define MAX_DENTS 512

/* ================================================================== */
/* exFAT                                                               */
typedef struct {
  vio_t *io;
  uint64_t fat_off;
  uint64_t heap_off;
  uint32_t clus_size;
  uint32_t root_clus;
  uint32_t clus_count;
} exfat_t;

static bool exfat_open(exfat_t *x, vio_t *io) {
  uint8_t bs[512];
  if (!vio_read(io, bs, sizeof(bs), 0) || memcmp(bs + 3, "EXFAT   ", 8))
    return false;
  uint32_t bps = 1u << bs[108];
  uint32_t spc = 1u << bs[109];
  x->io = io;
  x->fat_off = (uint64_t)le32(bs + 80) * bps;
  x->heap_off = (uint64_t)le32(bs + 88) * bps;
  x->clus_count = le32(bs + 92);
  x->root_clus = le32(bs + 96);
  x->clus_size = bps * spc;
  return x->clus_size > 0 && x->clus_size <= 32 * 1024 * 1024;
}

static uint32_t exfat_next(exfat_t *x, uint32_t c) {
  uint8_t b[4];
  if (!vio_read(x->io, b, 4, x->fat_off + (uint64_t)c * 4))
    return 0xFFFFFFFF;
  return le32(b);
}

/* Read up to maxlen bytes of a cluster chain into a malloc'd buffer. */
static uint8_t *exfat_read_chain(exfat_t *x, uint32_t first, uint64_t len,
                                 bool contig, size_t maxlen, size_t *outlen) {
  if (len == 0 || len > maxlen)
    len = maxlen;
  uint8_t *buf = malloc(len);
  if (!buf)
    return NULL;
  size_t got = 0;
  uint32_t c = first;
  int guard = 0;
  while (got < len && c >= 2 && c < x->clus_count + 2 && guard++ < 100000) {
    size_t n = x->clus_size;
    if (n > len - got)
      n = len - got;
    if (!vio_read(x->io, buf + got, n,
                    x->heap_off + (uint64_t)(c - 2) * x->clus_size))
      break;
    got += n;
    c = contig ? c + 1 : exfat_next(x, c);
    if (!contig && c >= 0xFFFFFFF7)
      break;
  }
  *outlen = got;
  return buf;
}

static int exfat_list(exfat_t *x, uint32_t clus, uint64_t len, bool contig,
                      dent_t *out, int max) {
  size_t dlen = 0;
  uint8_t *d = exfat_read_chain(x, clus, len, contig, 1024 * 1024, &dlen);
  if (!d)
    return -1;
  int n = 0;
  for (size_t off = 0; off + 32 <= dlen && n < max; off += 32) {
    uint8_t *e = d + off;
    if (e[0] == 0x00)
      break;
    if (e[0] != 0x85)
      continue;
    int secondaries = e[1];
    uint16_t attrs = le16(e + 4);
    if (off + 32 * (size_t)(secondaries + 1) > dlen)
      break;
    uint8_t *se = e + 32;
    if (se[0] != 0xC0)
      continue;
    dent_t *de = &out[n];
    memset(de, 0, sizeof(*de));
    int namelen = se[3];
    de->contig = (se[1] & 0x02) != 0;
    de->ref = le32(se + 20);
    de->size = le64(se + 24);
    de->dir = (attrs & 0x10) != 0;
    int ni = 0;
    for (int s = 2; s <= secondaries && ni < namelen; s++) {
      uint8_t *ne = e + 32 * s;
      if (ne[0] != 0xC1)
        break;
      for (int k = 0; k < 15 && ni < namelen; k++, ni++) {
        uint16_t ch = le16(ne + 2 + k * 2);
        if (ni < 255)
          de->name[ni] = ch < 0x80 ? (char)ch : '?';
      }
    }
    de->name[ni < 255 ? ni : 255] = 0;
    n++;
    off += 32 * (size_t)secondaries;
  }
  free(d);
  return n;
}

/* ================================================================== */
/* UFS2                                                                */
typedef struct {
  int fd;
  int32_t iblkno, bsize, fsize, frag, inopb, ipg, fpg;
} ufs_t;

static bool ufs_open(ufs_t *u, int fd) {
  uint8_t sb[1376 + 8];
  /* same search order as FreeBSD's SBLOCKSEARCH */
  static const uint64_t locs[] = {65536, 8192, 0, 262144};
  bool found = false;
  for (size_t i = 0; i < sizeof(locs) / sizeof(locs[0]) && !found; i++)
    found = pread_full(fd, sb, sizeof(sb), locs[i]) &&
            le32(sb + 1372) == 0x19540119;
  if (!found)
    return false;
  u->fd = fd;
  u->iblkno = (int32_t)le32(sb + 16);
  u->bsize = (int32_t)le32(sb + 48);
  u->fsize = (int32_t)le32(sb + 52);
  u->frag = (int32_t)le32(sb + 56);
  u->inopb = (int32_t)le32(sb + 120);
  u->ipg = (int32_t)le32(sb + 184);
  u->fpg = (int32_t)le32(sb + 188);
  return u->bsize > 0 && u->fsize > 0 && u->inopb > 0 && u->ipg > 0 &&
         u->fpg > 0 && u->bsize <= 1024 * 1024;
}

static bool ufs_inode(ufs_t *u, uint64_t ino, uint8_t di[256]) {
  uint64_t cg = ino / (uint64_t)u->ipg;
  uint64_t cgimin = cg * (uint64_t)u->fpg + (uint64_t)u->iblkno;
  uint64_t fsba =
      cgimin + ((ino % (uint64_t)u->ipg) / (uint64_t)u->inopb) * u->frag;
  uint64_t off =
      fsba * (uint64_t)u->fsize + (ino % (uint64_t)u->inopb) * 256;
  return pread_full(u->fd, di, 256, off);
}

/* read file data via direct blocks only (enough for small files/dirs) */
static uint8_t *ufs_read(ufs_t *u, const uint8_t di[256], size_t maxlen,
                         size_t *outlen) {
  uint64_t size = le64(di + 16);
  if (size > maxlen)
    size = maxlen;
  uint64_t direct_max = 12ull * (uint64_t)u->bsize;
  if (size > direct_max)
    size = direct_max;
  uint8_t *buf = malloc(size ? size : 1);
  if (!buf)
    return NULL;
  size_t got = 0;
  for (int i = 0; i < 12 && got < size; i++) {
    uint64_t blk = le64(di + 112 + i * 8);
    size_t n = (size_t)u->bsize;
    if (n > size - got)
      n = size - got;
    if (blk == 0)
      memset(buf + got, 0, n);
    else if (!pread_full(u->fd, buf + got, n, blk * (uint64_t)u->fsize))
      break;
    got += n;
  }
  *outlen = got;
  return buf;
}

static int ufs_list(ufs_t *u, uint64_t ino, dent_t *out, int max) {
  uint8_t di[256];
  if (!ufs_inode(u, ino, di))
    return -1;
  if ((le16(di) & 0170000) != 0040000)
    return -1;
  size_t len = 0;
  uint8_t *d = ufs_read(u, di, 1024 * 1024, &len);
  if (!d)
    return -1;
  int n = 0;
  size_t off = 0;
  while (off + 8 <= len && n < max) {
    uint32_t dino = le32(d + off);
    uint16_t reclen = le16(d + off + 4);
    uint8_t type = d[off + 6];
    uint8_t namlen = d[off + 7];
    if (reclen < 8 || off + reclen > len)
      break;
    if (dino && namlen && off + 8 + namlen <= len) {
      dent_t *de = &out[n];
      memset(de, 0, sizeof(*de));
      memcpy(de->name, d + off + 8, namlen);
      de->name[namlen] = 0;
      de->ref = dino;
      de->dir = type == 4; /* DT_DIR */
      if (strcmp(de->name, ".") && strcmp(de->name, ".."))
        n++;
    }
    off += reclen;
  }
  free(d);
  return n;
}

/* ================================================================== */
/* Common logic                                                        */
typedef struct {
  bool is_exfat;
  int fd;
  vio_t io;
  exfat_t x;
  ufs_t u;
} img_t;

/* Open an image of any supported kind. Returns false if it can't be read
 * (*unsupported is set when it's a container we don't look inside). */
static bool img_open(img_t *im, const char *path, bool rw, bool *unsupported) {
  memset(im, 0, sizeof(*im));
  im->fd = -1;
  if (unsupported)
    *unsupported = false;
  const char *kind = image_kind(path);
  if (!kind) {
    if (unsupported)
      *unsupported = true;
    return false;
  }
  im->fd = open(path, rw ? O_RDWR : O_RDONLY);
  if (im->fd < 0)
    return false;
  bool ok;
  if (!strcmp(kind, "ufs")) {
    ok = ufs_open(&im->u, im->fd);
  } else {
    im->is_exfat = true;
    if (!strcmp(kind, "exfat")) {
      ok = vio_open_plain(&im->io, im->fd);
    } else {
      ok = vio_open_pfs(&im->io, im->fd);
      im->io.path = path;
      if (!ok && unsupported)
        *unsupported = true; /* not an exFAT-in-PFS container */
    }
    ok = ok && exfat_open(&im->x, &im->io);
  }
  if (!ok) {
    vio_close(&im->io);
    close(im->fd);
    im->fd = -1;
  }
  return ok;
}

static void img_close(img_t *im) {
  vio_close(&im->io);
  if (im->fd >= 0)
    close(im->fd);
  im->fd = -1;
}

static int img_list(img_t *im, const dent_t *dir, dent_t *out, int max) {
  if (im->is_exfat) {
    if (!dir)
      return exfat_list(&im->x, im->x.root_clus, 0, false, out, max);
    return exfat_list(&im->x, (uint32_t)dir->ref, dir->size, dir->contig, out,
                      max);
  }
  return ufs_list(&im->u, dir ? dir->ref : 2, out, max);
}

static char *img_read_file(img_t *im, const dent_t *f) {
  size_t len = 0;
  uint8_t *buf;
  if (im->is_exfat) {
    buf = exfat_read_chain(&im->x, (uint32_t)f->ref, f->size, f->contig,
                           256 * 1024, &len);
  } else {
    uint8_t di[256];
    if (!ufs_inode(&im->u, f->ref, di))
      return NULL;
    buf = ufs_read(&im->u, di, 256 * 1024, &len);
  }
  if (!buf)
    return NULL;
  char *s = malloc(len + 1);
  if (s) {
    memcpy(s, buf, len);
    s[len] = 0;
  }
  free(buf);
  return s;
}

static const dent_t *find(const dent_t *v, int n, const char *name,
                          bool want_dir) {
  for (int i = 0; i < n; i++)
    if (v[i].dir == want_dir && !strcasecmp(v[i].name, name))
      return &v[i];
  return NULL;
}

/* Does `dir` (NULL = root) contain sce_sys/param.json? Reads it if so. */
static bool check_game_dir(img_t *im, const dent_t *dir, char *tid,
                           size_t tidsz, char *title, size_t titlesz) {
  dent_t *ents = calloc(MAX_DENTS, sizeof(dent_t));
  dent_t *sys = calloc(MAX_DENTS, sizeof(dent_t));
  bool found = false;
  if (!ents || !sys)
    goto out;
  int n = img_list(im, dir, ents, MAX_DENTS);
  if (n <= 0)
    goto out;
  const dent_t *sce = find(ents, n, "sce_sys", true);
  if (!sce)
    goto out;
  int m = img_list(im, sce, sys, MAX_DENTS);
  const dent_t *pj = m > 0 ? find(sys, m, "param.json", false) : NULL;
  if (!pj)
    goto out;
  found = true;
  char *json = img_read_file(im, pj);
  if (json) {
    parse_param_json(json, tid, tidsz, title, titlesz);
    free(json);
  }
out:
  free(ents);
  free(sys);
  return found;
}

peek_result_t image_peek(const char *path, char *title_id, size_t tidsz,
                         char *title, size_t titlesz, char *wrapper,
                         size_t wrapsz) {
  title_id[0] = 0;
  if (title && titlesz)
    title[0] = 0;
  if (wrapper && wrapsz)
    wrapper[0] = 0;
  img_t im;
  bool unsupported;
  if (!img_open(&im, path, false, &unsupported))
    return unsupported ? PEEK_UNSUPPORTED : PEEK_ERROR;

  peek_result_t res = PEEK_NO_GAME;
  if (check_game_dir(&im, NULL, title_id, tidsz, title, titlesz)) {
    res = PEEK_OK;
  } else {
    dent_t *root = calloc(MAX_DENTS, sizeof(dent_t));
    int n = root ? img_list(&im, NULL, root, MAX_DENTS) : -1;
    if (n < 0)
      res = PEEK_ERROR;
    for (int i = 0; i < n; i++) {
      if (!root[i].dir || root[i].name[0] == '.' ||
          !strcasecmp(root[i].name, "System Volume Information"))
        continue;
      if (check_game_dir(&im, &root[i], title_id, tidsz, title, titlesz)) {
        res = PEEK_WRAPPED;
        if (wrapper && wrapsz)
          snprintf(wrapper, wrapsz, "%s", root[i].name);
        break;
      }
    }
    free(root);
  }
  img_close(&im);
  return res;
}

/* List a directory inside an .exfat/.ffpkg image (read-only), e.g. "fakelib"
 * or "sce_sys". Writes "name(size)" entries, comma separated, into out.
 * Returns number of entries, -1 on error. */
int image_list_dir(const char *path, const char *subdir, char *out,
                   size_t outsz) {
  out[0] = 0;
  img_t im;
  if (!img_open(&im, path, false, NULL))
    return -1;
  dent_t *a = calloc(MAX_DENTS, sizeof(dent_t));
  dent_t *b = calloc(MAX_DENTS, sizeof(dent_t));
  int n = -1;
  if (a && b) {
    n = img_list(&im, NULL, a, MAX_DENTS);
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", subdir ? subdir : "");
    char *save = NULL;
    for (char *part = strtok_r(buf, "/", &save); part && n >= 0;
         part = strtok_r(NULL, "/", &save)) {
      const dent_t *d = find(a, n, part, true);
      if (!d) {
        n = -1;
        break;
      }
      dent_t cur = *d;
      n = img_list(&im, &cur, b, MAX_DENTS);
      dent_t *t = a;
      a = b;
      b = t;
    }
    size_t used = 0;
    for (int i = 0; i < n; i++) {
      int w = snprintf(out + used, outsz - used, "%s%s%s(%llu)",
                       used ? ", " : "", a[i].name, a[i].dir ? "/" : "",
                       (unsigned long long)a[i].size);
      if (w < 0 || (size_t)w >= outsz - used)
        break;
      used += (size_t)w;
    }
  }
  free(a);
  free(b);
  img_close(&im);
  return n;
}

/* ================================================================== */
/* exFAT: rename one file inside an image, in place, same name length.  */
/*                                                                      */
/* Used for the save fix on .exfat images, which ShadowMount mounts      */
/* read-only. Only the directory entry set of that one file is touched:  */
/* the UTF-16 name characters, the stream entry's NameHash and the set's */
/* SetChecksum. Both checksums are verified against the existing entry   */
/* before anything is written, and the entry is re-read afterwards.      */

static uint16_t exfat_name_hash(const uint16_t *name, int len) {
  uint16_t h = 0;
  for (int i = 0; i < len; i++) {
    uint16_t c = name[i];
    if (c >= 'a' && c <= 'z')
      c = (uint16_t)(c - 32); /* up-case (ASCII names only) */
    h = (uint16_t)(((h & 1) ? 0x8000 : 0) + (h >> 1) + (c & 0xFF));
    h = (uint16_t)(((h & 1) ? 0x8000 : 0) + (h >> 1) + (c >> 8));
  }
  return h;
}

static uint16_t exfat_set_checksum(const uint8_t *set, int nentries) {
  uint16_t c = 0;
  for (int i = 0; i < nentries * 32; i++) {
    if (i == 2 || i == 3)
      continue;
    c = (uint16_t)(((c & 1) ? 0x8000 : 0) + (c >> 1) + set[i]);
  }
  return c;
}

/* byte offsets in the image of every byte-range of a directory */
static int exfat_dir_clusters(exfat_t *x, uint32_t first, uint64_t len,
                              bool contig, uint64_t *offs, int max) {
  int n = 0;
  uint32_t c = first;
  uint64_t got = 0;
  if (len == 0)
    len = 64ull * 1024 * 1024; /* root dir: follow the chain */
  while (n < max && got < len && c >= 2 && c < x->clus_count + 2) {
    offs[n++] = x->heap_off + (uint64_t)(c - 2) * x->clus_size;
    got += x->clus_size;
    c = contig ? c + 1 : exfat_next(x, c);
    if (!contig && c >= 0xFFFFFFF7)
      break;
  }
  return n;
}

static bool exfat_entry_rw(exfat_t *x, const uint64_t *offs, int nclus,
                           uint64_t index, uint8_t e[32], bool write) {
  uint64_t byte = index * 32;
  uint64_t ci = byte / x->clus_size;
  if (ci >= (uint64_t)nclus)
    return false;
  uint64_t off = offs[ci] + byte % x->clus_size;
  if (write)
    return vio_write(x->io, e, 32, off);
  return vio_read(x->io, e, 32, off);
}

/* Find directory `dirname` in the root, then rename `from` to `to` inside
 * it (same length, ASCII). Returns 0 on success, otherwise sets err. */
static int exfat_rename_in(exfat_t *x, const char *dirname, const char *from,
                           const char *to, char *err, size_t errsz) {
  int len = (int)strlen(from);
  if ((int)strlen(to) != len || len == 0 || len > 255) {
    snprintf(err, errsz, "names must have the same length");
    return -1;
  }
  /* locate the directory */
  dent_t *root = calloc(MAX_DENTS, sizeof(dent_t));
  if (!root)
    return -1;
  int n = exfat_list(x, x->root_clus, 0, false, root, MAX_DENTS);
  const dent_t *d = n > 0 ? find(root, n, dirname, true) : NULL;
  if (!d) {
    free(root);
    snprintf(err, errsz, "no %s folder in the image", dirname);
    return -1;
  }
  dent_t dir = *d;
  free(root);

  uint64_t offs[4096];
  int nclus = exfat_dir_clusters(x, (uint32_t)dir.ref, dir.size, dir.contig,
                                 offs, 4096);
  uint64_t total = (uint64_t)nclus * x->clus_size / 32;
  uint8_t set[19 * 32];
  for (uint64_t i = 0; i < total; i++) {
    uint8_t e[32];
    if (!exfat_entry_rw(x, offs, nclus, i, e, false))
      break;
    if (e[0] == 0x00)
      break;
    if (e[0] != 0x85)
      continue;
    int sec = e[1];
    if (sec < 2 || sec > 18)
      continue;
    memcpy(set, e, 32);
    bool ok = true;
    for (int k = 1; k <= sec && ok; k++)
      ok = exfat_entry_rw(x, offs, nclus, i + (uint64_t)k, set + 32 * k,
                          false);
    if (!ok || set[32] != 0xC0)
      continue;
    int namelen = set[32 + 3];
    if (namelen != len) {
      i += (uint64_t)sec;
      continue;
    }
    uint16_t name[255];
    int ni = 0;
    for (int k = 2; k <= sec && ni < namelen; k++) {
      uint8_t *ne = set + 32 * k;
      if (ne[0] != 0xC1)
        break;
      for (int c = 0; c < 15 && ni < namelen; c++, ni++)
        name[ni] = le16(ne + 2 + c * 2);
    }
    bool match = ni == len;
    for (int c = 0; c < len && match; c++)
      match = name[c] == (uint8_t)from[c];
    if (!match) {
      i += (uint64_t)sec;
      continue;
    }
    /* sanity: our hash/checksum must reproduce what's on disk */
    if (exfat_name_hash(name, len) != le16(set + 32 + 4) ||
        exfat_set_checksum(set, sec + 1) != le16(set + 2)) {
      snprintf(err, errsz, "entry checksums didn't verify - not touching it");
      return -1;
    }
    /* rewrite name, hash, checksum */
    for (int c = 0; c < len; c++)
      name[c] = (uint8_t)to[c];
    ni = 0;
    for (int k = 2; k <= sec && ni < len; k++) {
      uint8_t *ne = set + 32 * k;
      for (int c = 0; c < 15 && ni < len; c++, ni++) {
        ne[2 + c * 2] = (uint8_t)(name[ni] & 0xFF);
        ne[3 + c * 2] = (uint8_t)(name[ni] >> 8);
      }
    }
    uint16_t h = exfat_name_hash(name, len);
    set[32 + 4] = (uint8_t)(h & 0xFF);
    set[32 + 5] = (uint8_t)(h >> 8);
    uint16_t cs = exfat_set_checksum(set, sec + 1);
    set[2] = (uint8_t)(cs & 0xFF);
    set[3] = (uint8_t)(cs >> 8);
    /* one write for the whole set when it sits in one cluster, so a
     * compressed (.ffpfsc) block is only re-encoded once */
    uint64_t first = i * 32, last = (i + (uint64_t)sec) * 32;
    bool written;
    if (first / x->clus_size == last / x->clus_size &&
        first / x->clus_size < (uint64_t)nclus) {
      written = vio_write(x->io, set, 32 * (size_t)(sec + 1),
                          offs[first / x->clus_size] + first % x->clus_size);
    } else {
      written = true;
      for (int k = 0; k <= sec && written; k++)
        written = exfat_entry_rw(x, offs, nclus, i + (uint64_t)k,
                                 set + 32 * k, true);
    }
    if (!written) {
      if (x->io->pfsc)
        snprintf(err, errsz, "couldn't fit the changed block back into the "
                             ".ffpfsc - nothing was changed");
      else
        snprintf(err, errsz, "write failed: %s", strerror(errno));
      return -1;
    }
    if (!x->io->pfsc)
      fsync(x->io->fd);
    /* read back and verify */
    uint8_t chk[19 * 32];
    for (int k = 0; k <= sec; k++)
      if (!exfat_entry_rw(x, offs, nclus, i + (uint64_t)k, chk + 32 * k,
                          false) ||
          memcmp(chk + 32 * k, set + 32 * k, 32)) {
        snprintf(err, errsz, "read-back didn't match after writing");
        return -1;
      }
    return 0;
  }
  snprintf(err, errsz, "%s not found in %s", from, dirname);
  return -1;
}

int image_rename_in_dir(const char *path, const char *dirname,
                        const char *from, const char *to, char *err,
                        size_t errsz) {
  const char *kind = image_kind(path);
  if (!kind || !strcmp(kind, "ufs")) {
    snprintf(err, errsz,
             "only .exfat, .ffpfs and .ffpfsc images can be changed in place");
    return -1;
  }
  img_t im;
  bool unsupported;
  if (!img_open(&im, path, true, &unsupported)) {
    if (unsupported)
      snprintf(err, errsz, "this %s doesn't hold an exFAT image, so it can't "
                           "be changed in place", kind);
    else
      snprintf(err, errsz, "can't open the image for writing: %s",
               errno ? strerror(errno) : "not a readable exFAT image");
    return -1;
  }
  int rc = exfat_rename_in(&im.x, dirname, from, to, err, errsz);
  img_close(&im);
  return rc;
}

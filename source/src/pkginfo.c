/* Read-only PS5 package (.pkg) inspector.
 *
 * Reads just enough of a package to say what's inside and whether Game
 * Library Manager could turn it into a folder: the FIH header, the CNT
 * content ID, the outer PFS superblock, and (after trying the default
 * fake-package passcode) the outer file list and the head of the nested
 * pfs_image.dat. It only reports a summary; it never returns file data.
 *
 * Format notes (worked out against LibProsperoPKG, GPL-3, and checked
 * with packages it builds):
 *  FIH (little-endian): "\x7FFIH"; [5] 0 = debug/fake; [0x10] outer image
 *    offset; [0x18] its size; [0x20] absolute superblock offset for a
 *    "data-first" outer image; [0x58] CNT offset.
 *  CNT: "\x7FCNT"; content ID (36 chars) at +0x40.
 *  EKPFS = H(H(BE32 1) | H(content ID padded to 48) | passcode), H = SHA3-256
 *    (current tools) or SHA-256 (older ones); passcode = 32 x '0'.
 *  XTS keys: base = new_crypt ? HMAC(ekpfs, seed) : ekpfs;
 *    k = HMAC(base, LE32 1 | seed); tweak = k[0:16], data = k[16:32].
 *  Classic outer image: superblock first and plaintext, the rest is XTS in
 *    0x1000-byte sectors numbered from the image start.
 *  Data-first outer image: file data first, plaintext superblock near the
 *    end; each 0x10000 block is one XTS unit, sector = block index, with
 *    bit 47 set for metadata ("signed") blocks. */
#include "common.h"
#include "crypto.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define PFS_MAGIC 20130315ull
#define SIGNED_FLAG 0x800000000000ull
#define MAX_BS 0x10000u

static uint64_t le64(const uint8_t *p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; i--)
    v = v << 8 | p[i];
  return v;
}
static uint32_t le32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
         (uint32_t)p[3] << 24;
}

static bool pread_all(int fd, void *buf, size_t len, uint64_t off) {
  uint8_t *p = buf;
  while (len) {
    ssize_t r = pread(fd, p, len, (off_t)off);
    if (r <= 0)
      return false;
    p += r, len -= (size_t)r, off += (uint64_t)r;
  }
  return true;
}

typedef struct {
  int fd;
  uint64_t img_off, img_size;
  uint32_t bs;
  uint16_t mode;
  bool data_first;
  bool encrypted;
  uint64_t sb_block; /* data-first only */
  xts_ctx x;
  /* superblock fields */
  uint64_t dinode_count, dinode_blocks, n_indirect, ndblock;
  uint64_t dtab[8]; /* dinode table blocks, from the superblock's inode-block signature */
  uint8_t seed[16];
} outer_t;

/* Read and (if needed) decrypt one block of the outer image. */
static bool outer_block(outer_t *o, uint64_t blk, bool signed_blk,
                        uint8_t *buf) {
  if ((blk + 1) * o->bs > o->img_size)
    return false;
  if (!pread_all(o->fd, buf, o->bs, o->img_off + blk * o->bs))
    return false;
  if (!o->encrypted)
    return true;
  if (o->data_first) {
    if (blk != o->sb_block)
      xts_decrypt(&o->x, buf, o->bs, blk | (signed_blk ? SIGNED_FLAG : 0));
  } else if (blk > 0) {
    for (uint32_t s = 0; s < o->bs; s += 0x1000)
      xts_decrypt(&o->x, buf + s, 0x1000, (blk * o->bs + s) / 0x1000);
  }
  return true;
}

typedef struct {
  uint16_t mode;
  uint32_t flags;
  uint64_t size, size_c, blocks, start;
} dinode_t;

static size_t dinode_size(uint16_t mode) {
  if (mode & 1)
    return (mode & 2) ? 0x310 : 0x2C8;
  return 0xA8;
}

static void parse_dinode(const outer_t *o, const uint8_t *p, dinode_t *d) {
  d->mode = (uint16_t)(p[0] | p[1] << 8);
  d->flags = le32(p + 4);
  d->size = le64(p + 8);
  d->size_c = le64(p + 0x10);
  if ((o->mode & 3) == 3) {
    d->blocks = le64(p + 0x60);
    d->start = le64(p + 0x68 + 32);
  } else if (o->mode & 1) {
    d->blocks = le32(p + 0x60);
    d->start = le32(p + 0x64 + 32);
  } else {
    d->blocks = le32(p + 0x60);
    d->start = le32(p + 0x64);
  }
}

static bool get_dinode(outer_t *o, uint64_t ino, uint8_t *blk, dinode_t *d) {
  size_t ds = dinode_size(o->mode);
  uint64_t per = o->bs / ds;
  if (ino >= o->dinode_count || !per)
    return false;
  uint64_t first = (o->data_first ? o->sb_block : 0) + 1 + o->n_indirect;
  uint64_t bi = ino / per, blkno = first + bi;
  if (bi < 8 && o->dtab[bi])
    blkno = o->dtab[bi];
  if (!outer_block(o, blkno, true, blk))
    return false;
  parse_dinode(o, blk + (ino % per) * ds, d);
  return true;
}

typedef struct {
  char name[64];
  uint32_t ino;
  int type;
} dent_t;

/* list a directory's entries (first few blocks only - the outer image is tiny) */
static int list_dir(outer_t *o, const dinode_t *dir, uint8_t *blk, dent_t *out,
                    int max) {
  int n = 0;
  uint64_t total = o->img_size / o->bs;
  if (dir->start == 0 || dir->start >= total || dir->blocks == 0)
    return -1;
  for (uint64_t b = 0; b < dir->blocks && b < 8; b++) {
    if (!outer_block(o, dir->start + b, true, blk))
      return -1;
    uint32_t off = 0;
    while (off + 16 <= o->bs && n < max) {
      uint32_t ino = le32(blk + off), type = le32(blk + off + 4),
               nl = le32(blk + off + 8), es = le32(blk + off + 12);
      if (es == 0 || es > 512 || off + es > o->bs || nl > es - 16)
        break;
      if (type < 2 || type > 5)
        return -1; /* not a dirent: wrong key */
      dent_t *e = &out[n++];
      size_t k = nl < sizeof(e->name) - 1 ? nl : sizeof(e->name) - 1;
      memcpy(e->name, blk + off + 16, k);
      e->name[k] = 0;
      e->ino = ino;
      e->type = (int)type;
      off += es;
    }
  }
  return n;
}

static void derive_keys(const char *cid, bool use_sha3, bool new_crypt,
                        const uint8_t seed[16], xts_ctx *x) {
  uint8_t msg[96], idx[4] = {0, 0, 0, 1}, cidbuf[48] = {0}, ekpfs[32];
  memcpy(cidbuf, cid, strlen(cid) < 48 ? strlen(cid) : 48);
  if (use_sha3) {
    sha3_256(idx, 4, msg);
    sha3_256(cidbuf, 48, msg + 32);
  } else {
    sha256(idx, 4, msg);
    sha256(cidbuf, 48, msg + 32);
  }
  memset(msg + 64, '0', 32);
  if (use_sha3)
    sha3_256(msg, 96, ekpfs);
  else
    sha256(msg, 96, ekpfs);
  uint8_t base[32], in[20], k[32];
  if (new_crypt)
    hmac_sha256(ekpfs, 32, seed, 16, base);
  else
    memcpy(base, ekpfs, 32);
  in[0] = 1, in[1] = in[2] = in[3] = 0;
  memcpy(in + 4, seed, 16);
  hmac_sha256(base, 32, in, 20, k);
  xts_init(x, k + 16, k);
}

/* the key is right if dinode 0 is a directory whose entries include "uroot" */
static dinode_t g_last_root; /* diagnostics: what dinode 0 decrypted to */
static bool try_root(outer_t *o, uint8_t *blk, dinode_t *uroot) {
  dinode_t root;
  memset(&g_last_root, 0, sizeof(g_last_root));
  if (!get_dinode(o, 0, blk, &root))
    return false;
  g_last_root = root;
  if ((root.mode & 0xF000) != 0x4000)
    return false;
  dent_t ents[16];
  int n = list_dir(o, &root, blk, ents, 16);
  for (int i = 0; i < n; i++)
    if (ents[i].type == 3 && !strcmp(ents[i].name, "uroot"))
      return get_dinode(o, ents[i].ino, blk, uroot) &&
             (uroot->mode & 0xF000) == 0x4000;
  return false;
}

static bool read_superblock(outer_t *o, uint64_t abs, uint8_t *blk) {
  if (!pread_all(o->fd, blk, 0x400, abs))
    return false;
  if (le64(blk) != 2 && le64(blk) != 1)
    return false;
  if (le64(blk + 8) != PFS_MAGIC)
    return false;
  o->mode = (uint16_t)(blk[0x1c] | blk[0x1d] << 8);
  o->bs = le32(blk + 0x20);
  o->dinode_count = le64(blk + 0x30);
  o->dinode_blocks = le64(blk + 0x40);
  o->n_indirect = 0;
  for (int i = 0; i < 5; i++)
    if ((int64_t)le64(blk + 0x298 + i * 40 + 32) > 0)
      o->n_indirect++;
  o->ndblock = le64(blk + 0x38);
  for (int i = 0; i < 8; i++)
    o->dtab[i] = (uint64_t)i < o->dinode_blocks && i < 12
                     ? le64(blk + 0x50 + 0x68 + i * 40 + 32)
                     : 0;
  memcpy(o->seed, blk + 0x370, 16);
  o->encrypted = (o->mode & 4) != 0;
  return o->bs >= 0x1000 && o->bs <= MAX_BS && (o->bs & 0xFFF) == 0 &&
         o->dinode_count > 0 && o->dinode_count < 10000000;
}

static const char *fmt_size(uint64_t v, char *buf, size_t n) {
  if (v >= 1ull << 30)
    snprintf(buf, n, "%.1f GB", (double)v / (1 << 30));
  else
    snprintf(buf, n, "%.1f MB", (double)v / (1 << 20));
  return buf;
}

void pkg_inspect_json(const char *path, sbuf_t *b) {
  uint8_t *blk = malloc(MAX_BS);
  uint8_t hdr[0x100];
  outer_t o;
  memset(&o, 0, sizeof(o));
  char err[256] = "", cid[40] = "", sz[32];
  char summary[512] = "", verdict[32] = "unknown";
  struct stat st;
  int fd = -1;
  bool key_ok = false, use_sha3 = false, new_crypt = false;
  int nfiles = 0;
  dent_t files[16];
  dinode_t fdi[16];
  char inner_kind[48] = "", inner_comp[24] = "";
  uint64_t inner_logical = 0, inner_disk = 0;
  int naps_files = -1, naps_comp = -1, naps_ublocks = -1;
  bool debug = false;
  dinode_t tries[4];
  int ntries = 0;
  uint8_t fih[0x100];
  memset(fih, 0, sizeof(fih));

  if (!blk) {
    snprintf(err, sizeof(err), "out of memory");
    goto out;
  }
  fd = open(path, O_RDONLY);
  if (fd < 0 || fstat(fd, &st) != 0) {
    snprintf(err, sizeof(err), "can't open the package: %s", strerror(errno));
    goto out;
  }
  o.fd = fd;
  if (!pread_all(fd, hdr, sizeof(hdr), 0) || memcmp(hdr, "\x7F" "FIH", 4)) {
    snprintf(err, sizeof(err),
             "not a PS5 package (no FIH header) - PS4 packages aren't supported");
    goto out;
  }
  debug = hdr[5] == 0;
  memcpy(fih, hdr, sizeof(fih));
  o.img_off = le64(hdr + 0x10);
  o.img_size = le64(hdr + 0x18);
  uint64_t sb_abs = le64(hdr + 0x20), cnt = le64(hdr + 0x58);
  uint64_t fsize = (uint64_t)st.st_size;
  if (cnt + 0x64 <= fsize) {
    uint8_t c[0x64];
    if (pread_all(fd, c, sizeof(c), cnt) && !memcmp(c, "\x7F" "CNT", 4)) {
      memcpy(cid, c + 0x40, 36);
      cid[36] = 0;
      for (int i = 0; i < 36; i++)
        if (cid[i] < 0x20 || cid[i] > 0x7e)
          cid[i] = '?';
    }
  }
  if (!cid[0]) {
    snprintf(err, sizeof(err), "couldn't read the content ID (CNT block)");
    goto out;
  }
  if (!debug) {
    snprintf(err, sizeof(err),
             "retail-signed package: it can only be installed, not converted");
    snprintf(verdict, sizeof(verdict), "retail");
    goto out;
  }
  if (o.img_off == 0 || o.img_size == 0 || o.img_off + o.img_size > fsize) {
    snprintf(err, sizeof(err),
             "outer image is outside the file - is the package complete?");
    goto out;
  }
  /* data-first if the FIH points at a superblock inside the image, past its start */
  if (sb_abs > o.img_off && sb_abs < o.img_off + o.img_size &&
      read_superblock(&o, sb_abs, blk) && (sb_abs - o.img_off) % o.bs == 0) {
    o.data_first = true;
    o.sb_block = (sb_abs - o.img_off) / o.bs;
  } else if (!read_superblock(&o, o.img_off, blk)) {
    snprintf(err, sizeof(err), "no PFS superblock where the header says");
    goto out;
  }

  dinode_t uroot;
  if (!o.encrypted) {
    ntries = 1;
    key_ok = try_root(&o, blk, &uroot);
  } else {
    for (int h = 0; h < 2 && !key_ok; h++)
      for (int nc = 0; nc < 2 && !key_ok; nc++) {
        derive_keys(cid, h == 0, nc == 0, o.seed, &o.x);
        bool okk = try_root(&o, blk, &uroot);
        tries[ntries++] = g_last_root;
        if (okk) {
          key_ok = true;
          use_sha3 = h == 0;
          new_crypt = nc == 0;
        }
      }
  }
  if (!key_ok) {
    snprintf(err, sizeof(err),
             "the standard fake-package passcode doesn't unlock it. Packages "
             "built with Sony's publishing tools wrap their key differently, "
             "so this one can't be converted");
    snprintf(verdict, sizeof(verdict), "locked");
    goto out;
  }

  dent_t ents[16];
  int n = list_dir(&o, &uroot, blk, ents, 16);
  for (int i = 0; i < n && nfiles < 16; i++) {
    if (ents[i].type != 2)
      continue;
    if (!get_dinode(&o, ents[i].ino, blk, &fdi[nfiles]))
      continue;
    files[nfiles++] = ents[i];
  }

  int img = -1, naps = -1;
  for (int i = 0; i < nfiles; i++) {
    if (!strcasecmp(files[i].name, "pfs_image.dat"))
      img = i;
    if (!strcasecmp(files[i].name, "naps_pkg_layout.dat"))
      naps = i;
  }
  if (naps >= 0 && outer_block(&o, fdi[naps].start, true, blk)) {
    uint64_t w0 = le64(blk), w1 = le64(blk + 8);
    naps_files = (int)(w0 & 0xFFFFFF) + 1;
    naps_comp = (int)((w0 >> 24) & 3);
    naps_ublocks = (int)((w0 >> 32) & 0xFFFFFF);
    (void)w1;
  }
  if (img < 0) {
    snprintf(inner_kind, sizeof(inner_kind), "no pfs_image.dat");
  } else {
    inner_disk = fdi[img].size;
    inner_logical = fdi[img].size_c;
    if (!outer_block(&o, fdi[img].start, false, blk)) {
      snprintf(inner_kind, sizeof(inner_kind), "unreadable");
    } else if (!memcmp(blk, "PFSC", 4)) {
      int v = blk[4] | blk[5] << 8;
      snprintf(inner_kind, sizeof(inner_kind), "PFSC container v%d", v);
      snprintf(inner_comp, sizeof(inner_comp), "%s",
               v == 0 ? "zlib" : (v == 2 || v == 3) ? "Kraken" : "unknown");
    } else if ((le64(blk) == 2 || le64(blk) == 1) &&
               le64(blk + 8) == PFS_MAGIC) {
      snprintf(inner_kind, sizeof(inner_kind), "plain PFS image");
      snprintf(inner_comp, sizeof(inner_comp), "none");
    } else if (naps >= 0) {
      snprintf(inner_kind, sizeof(inner_kind), "data-first (naps layout)");
      snprintf(inner_comp, sizeof(inner_comp), "%s",
               naps_comp == 2 ? "Kraken" : naps_comp == 0 ? "none" : "other");
    } else {
      snprintf(inner_kind, sizeof(inner_kind), "unrecognised");
    }
  }

  /* what the converter can do with it */
  if (!strcmp(inner_comp, "none"))
    snprintf(verdict, sizeof(verdict), "convertible");
  else if (!strcmp(inner_comp, "zlib"))
    snprintf(verdict, sizeof(verdict), "convertible_zlib");
  else if (!strcmp(inner_comp, "Kraken"))
    snprintf(verdict, sizeof(verdict), "needs_kraken");
  else
    snprintf(verdict, sizeof(verdict), "unknown_inner");

  snprintf(summary, sizeof(summary),
           "Fake (debug) package, default passcode works. %s outer image, "
           "game data %s on disk%s%s, %s.",
           o.data_first ? "Data-first" : "Classic",
           fmt_size(inner_disk, sz, sizeof(sz)),
           inner_comp[0] ? ", compression: " : "", inner_comp, inner_kind);

out:
  if (fd >= 0)
    close(fd);
  free(blk);
  sb_printf(b, "{\"ok\":%s,\"path\":", err[0] ? "false" : "true");
  sb_json_str(b, path);
  sb_printf(b, ",\"debug\":%s,\"content_id\":", debug ? "true" : "false");
  sb_json_str(b, cid);
  sb_puts(b, ",\"verdict\":");
  sb_json_str(b, verdict);
  sb_puts(b, ",\"error\":");
  sb_json_str(b, err);
  sb_puts(b, ",\"summary\":");
  sb_json_str(b, summary);
  sb_printf(b,
            ",\"outer\":{\"layout\":\"%s\",\"block_size\":%u,\"mode\":%u,"
            "\"encrypted\":%s,\"key\":\"%s\",\"offset\":%llu,\"size\":%llu,"
            "\"files\":[",
            o.data_first ? "data-first" : "classic", o.bs, o.mode,
            o.encrypted ? "true" : "false",
            !key_ok ? "none"
                    : !o.encrypted ? "not needed"
                                   : use_sha3 ? (new_crypt ? "sha3+new" : "sha3")
                                              : (new_crypt ? "sha256+new"
                                                           : "sha256"),
            (unsigned long long)o.img_off, (unsigned long long)o.img_size);
  for (int i = 0; i < nfiles; i++) {
    sb_puts(b, i ? ",{\"name\":" : "{\"name\":");
    sb_json_str(b, files[i].name);
    sb_printf(b, ",\"size\":%llu,\"size_c\":%llu,\"flags\":%u}",
              (unsigned long long)fdi[i].size,
              (unsigned long long)fdi[i].size_c, fdi[i].flags);
  }
  sb_puts(b, "]},\"inner\":{\"kind\":");
  sb_json_str(b, inner_kind);
  sb_puts(b, ",\"compression\":");
  sb_json_str(b, inner_comp);
  sb_printf(b,
            ",\"on_disk\":%llu,\"logical\":%llu,\"naps_files\":%d,"
            "\"naps_comp\":%d,\"naps_ublocks\":%d}",
            (unsigned long long)inner_disk, (unsigned long long)inner_logical,
            naps_files, naps_comp, naps_ublocks);
  /* header geometry only (no file contents), to diagnose unknown layouts */
  sb_puts(b, ",\"diag\":{\"fih\":[");
  static const int fo[] = {0x08, 0x20, 0x28, 0x30, 0x60, 0x68, 0x90, 0x98, 0xa0, 0xa8, 0xf0, 0xf8};
  for (size_t i = 0; i < sizeof(fo) / sizeof(fo[0]); i++)
    sb_printf(b, "%s\"0x%x=%llx\"", i ? "," : "", fo[i],
              (unsigned long long)le64(fih + fo[i]));
  sb_printf(b,
            "],\"sb_block\":%llu,\"total_blocks\":%llu,\"ndblock\":%llu,"
            "\"dinodes\":%llu,\"dinode_blocks\":%llu,\"indirect\":%llu,"
            "\"dtab0\":%llu,\"tries\":[",
            (unsigned long long)o.sb_block,
            (unsigned long long)(o.bs ? o.img_size / o.bs : 0),
            (unsigned long long)o.ndblock, (unsigned long long)o.dinode_count,
            (unsigned long long)o.dinode_blocks,
            (unsigned long long)o.n_indirect, (unsigned long long)o.dtab[0]);
  for (int i = 0; i < ntries; i++)
    sb_printf(b, "%s\"mode=%x blocks=%llx start=%llx size=%llx\"", i ? "," : "",
              tries[i].mode, (unsigned long long)tries[i].blocks,
              (unsigned long long)tries[i].start,
              (unsigned long long)tries[i].size);
  sb_puts(b, "]}}");
}

/* Linux test tool for the image code in src/imgpeek.c:
 *   imgtest peek IMAGE
 *   imgtest ls IMAGE [DIR]
 *   imgtest rename IMAGE DIR FROM TO
 *   imgtest fit IMAGE.ffpfsc [SAMPLES]   (read-only: can blocks be
 *                                         re-encoded into their own slot?)
 * Build: make imgtest */
#include "../src/imgpeek.c"

int main(int argc, char **argv) {
  if (argc >= 3 && !strcmp(argv[1], "peek")) {
    char tid[16], title[256], wrap[256];
    peek_result_t r = image_peek(argv[2], tid, sizeof(tid), title,
                                 sizeof(title), wrap, sizeof(wrap));
    printf("peek=%d title_id=%s title=%s wrapper=%s\n", (int)r, tid, title,
           wrap);
    return r == PEEK_OK ? 0 : 1;
  }
  if (argc >= 3 && !strcmp(argv[1], "ls")) {
    char out[8192];
    int n = image_list_dir(argv[2], argc > 3 ? argv[3] : "", out, sizeof(out));
    printf("%d: %s\n", n, out);
    return n < 0;
  }
  if (argc >= 6 && !strcmp(argv[1], "rename")) {
    char err[512] = "";
    int rc = image_rename_in_dir(argv[2], argv[3], argv[4], argv[5], err,
                                 sizeof(err));
    printf("rename rc=%d %s\n", rc, err);
    return rc != 0;
  }
  if (argc >= 3 && !strcmp(argv[1], "fit")) {
    img_t im;
    if (!img_open(&im, argv[2], false, NULL) || !im.io.pfsc) {
      printf("not a readable .ffpfsc\n");
      return 1;
    }
    uint64_t samples = argc > 3 ? strtoull(argv[3], NULL, 10) : 200;
    uint64_t ok = 0, raw = 0, tried = 0;
    uint8_t *data = malloc(im.io.lbs), *enc = malloc(im.io.lbs);
    for (uint64_t s = 0; s < samples; s++) {
      uint64_t idx = s * im.io.nblocks / samples, off, len;
      if (!pfsc_slot(&im.io, idx, &off, &len) || !pfsc_get(&im.io, idx))
        continue;
      tried++;
      if (len == im.io.lbs) {
        raw++;
        ok++;
        continue;
      }
      memcpy(data, im.io.cache, im.io.lbs);
      data[100] ^= 1; /* a small change, like a rename */
      if (zlib_encode_exact(data, im.io.lbs, enc, len))
        ok++;
      else
        printf("  block %llu (slot %llu bytes) doesn't fit\n",
               (unsigned long long)idx, (unsigned long long)len);
    }
    printf("fit %llu/%llu blocks (%llu stored raw)\n", (unsigned long long)ok,
           (unsigned long long)tried, (unsigned long long)raw);
    img_close(&im);
    return ok != tried;
  }
  if (argc >= 4 && !strcmp(argv[1], "shift")) {
    /* change one byte of block N and force the borrow-from-a-neighbour
     * path, to test it in both directions */
    img_t im;
    if (!img_open(&im, argv[2], true, NULL) || !im.io.pfsc) {
      printf("not a writable .ffpfsc\n");
      return 1;
    }
    uint64_t idx = strtoull(argv[3], NULL, 10);
    if (idx >= im.io.nblocks)
      idx = im.io.nblocks - 1;
    uint8_t *data = malloc(im.io.lbs);
    bool ok = pfsc_get(&im.io, idx);
    if (ok) {
      memcpy(data, im.io.cache, im.io.lbs);
      data[im.io.lbs / 2] ^= 0x5A;
      ok = pfsc_put_shift(&im.io, idx, data);
    }
    printf("shift block %llu: %s\n", (unsigned long long)idx,
           ok ? "ok" : "failed");
    img_close(&im);
    return !ok;
  }
  fprintf(stderr, "usage: imgtest peek|ls|rename ...\n");
  return 2;
}

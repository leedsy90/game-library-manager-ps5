/* host test: ./pkginfo-test FILE.pkg -> prints the /api/pkginfo JSON */
#include "common.h"
#include <stdio.h>
int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++) {
    sbuf_t b; sb_init(&b); pkg_inspect_json(argv[i], &b);
    printf("%.*s\n", (int)b.len, b.data); sb_free(&b);
  }
  return 0;
}

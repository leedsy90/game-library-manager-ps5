zlib 1.3.2 (https://github.com/madler/zlib), zlib licence (see LICENSE).
Used to read and rewrite the zlib-compressed blocks inside .ffpfsc images.
Only the deflate/inflate sources are included (no gz* file functions; gzguts.h is only needed by zutil.c); unmodified.

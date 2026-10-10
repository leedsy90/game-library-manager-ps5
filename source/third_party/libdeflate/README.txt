libdeflate 1.26 (https://github.com/ebiggers/libdeflate), MIT licence (see COPYING).
Only the compressor is built (lib/deflate_compress.c, lib/utils.c). Its near-optimal
levels re-encode a changed .ffpfsc block smaller than zlib, so it fits back into its slot.
Unmodified; decompressor, gzip/zlib wrappers and checksums left out.

Files from the LZMA SDK / 7-Zip "C" directory (https://github.com/ip7z/7zip),
by Igor Pavlov. These files are in the public domain.
Used for reading .7z archives and streaming LZMA / LZMA2 (multi-threaded) decoding.
Local change: Threads.c sets a 512 KB thread stack (PS5 default stacks are too small).

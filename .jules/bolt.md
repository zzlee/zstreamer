## 2024-07-02 - Optimize Media Parsing (zst_find_start_code)
**Learning:** Naive byte-by-byte iteration for NALU start codes in video streams is a massive performance bottleneck. The `memchr` function is heavily optimized using SIMD in most standard libraries and speeds up the search by ~10-25x. Also, C++ compatibility requires explicit pointer casts when using `memchr` inside headers.
**Action:** Always prefer `memchr` (with proper bounds checking and explicit casting) over raw loops when searching for specific byte markers inside large data buffers.

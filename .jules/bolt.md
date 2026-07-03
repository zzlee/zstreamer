## 2024-07-02 - Optimize Media Parsing (zst_find_start_code)
**Learning:** Naive byte-by-byte iteration for NALU start codes in video streams is a massive performance bottleneck. The `memchr` function is heavily optimized using SIMD in most standard libraries and speeds up the search by ~10-25x. Also, C++ compatibility requires explicit pointer casts when using `memchr` inside headers.
**Action:** Always prefer `memchr` (with proper bounds checking and explicit casting) over raw loops when searching for specific byte markers inside large data buffers.

## 2024-07-04 - Optimize Video Encoder Frame Copying
**Learning:** Copying raw YUV planes using `memcpy` inside video encoders (`x264_encoder.c`, `x265_encoder.c`) causes significant performance overhead since these planes can be large. Both `libx264` and `libx265` APIs support zero-copy inputs by allowing the caller to map the input buffer pointers (`frame->plane[n]`) directly to the encoder's input picture structure (`pic_in->planes[n]`).
**Action:** When integrating with video encoders that accept raw pixel buffers (like `x264` and `x265`), directly map the incoming buffer's plane pointers and stride array to the encoder's input structures instead of issuing `memcpy` calls.

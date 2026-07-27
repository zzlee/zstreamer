## 2024-05-24 - Audio Mixer Vectorization Bottleneck
**Learning:** The `audio_mixer` element in zstreamer contained a heavy inner loop recalculating pan gains per sample with conditionals inside the frame iteration, severely limiting vectorization and branch prediction.
**Action:** When optimizing multi-channel media pipelines, hoist channel mapping and gain pre-calculation (including S16LE floating-point division factors) outside the per-frame loop. Implementing explicitly unrolled fast-paths for common channel layouts (like 2-to-2 or 1-to-2) allows the compiler to vectorize operations significantly better. Ensure fallback bounds map directly to output channel strides to prevent memory corruption.
## 2026-07-22 - Optimize trailing whitespace trim in SDP filtering
**Learning:** Re-evaluating string length calculations can uncover small but compoundable performance gains. Here, eliminating a redundant while loop for trimming `
` by reusing known pointer boundaries yielded a ~1.3x speedup on that function in microbenchmarks.
**Action:** Always check if previous iteration bounds or string limits have already done the work you are trying to do.
## 2026-11-09 - Avoid O(N^2) strncat inside loops in SDP parsing
**Learning:** In `zst_webrtc_twcc_inject_answer`, appending to a large string buffer inside a `while` loop using `strncat(..., max_len - strlen(temp) - 1)` resulted in `strlen` traversing the increasingly large output string on every iteration, leading to O(N^2) complexity.
**Action:** When iteratively building strings, track the `temp_len` explicitely instead of repeatedly calculating `strlen`, and use `memcpy(dest + temp_len, src, len)` rather than `strncat`. Combine this by replacing redundant dynamic allocations (like `strdup` and `strtok_r`) with a single-pass `strchr` iteration for compounding speedup.
## 2025-07-25 - Zero-allocation Substring Matching for SDP Filtering
**Learning:** In C, parsing algorithms that rely on repeated `malloc`, `memcpy`, and `free` within a loop can introduce significant heap fragmentation and performance overhead. For WebRTC endpoints that parse strings like SDP repeatedly, this results in considerable slow down, as demonstrated by the `zst_webrtc_filter_sdp` parsing overhead (saving ~45% run-time).
**Action:** When filtering strings line-by-line, avoid copying lines into temporary buffers just for matching. Instead, utilize `memcmp` against fixed tokens alongside inline `MEMSTR`-style matching directly over the original buffer, bounding searches via explicit string lengths, avoiding redundant allocations.

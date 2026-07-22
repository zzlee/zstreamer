## 2024-05-24 - Audio Mixer Vectorization Bottleneck
**Learning:** The `audio_mixer` element in zstreamer contained a heavy inner loop recalculating pan gains per sample with conditionals inside the frame iteration, severely limiting vectorization and branch prediction.
**Action:** When optimizing multi-channel media pipelines, hoist channel mapping and gain pre-calculation (including S16LE floating-point division factors) outside the per-frame loop. Implementing explicitly unrolled fast-paths for common channel layouts (like 2-to-2 or 1-to-2) allows the compiler to vectorize operations significantly better. Ensure fallback bounds map directly to output channel strides to prevent memory corruption.
## 2026-07-22 - Optimize trailing whitespace trim in SDP filtering
**Learning:** Re-evaluating string length calculations can uncover small but compoundable performance gains. Here, eliminating a redundant while loop for trimming `
` by reusing known pointer boundaries yielded a ~1.3x speedup on that function in microbenchmarks.
**Action:** Always check if previous iteration bounds or string limits have already done the work you are trying to do.

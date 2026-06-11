1. **Define Probe Types and API**
   - Update `include/zst_pad.h` with probe enumerations and structures:
     - `zst_pad_probe_type_t`: `ZST_PAD_PROBE_PRE_BUFFER`, `ZST_PAD_PROBE_POST_BUFFER`, `ZST_PAD_PROBE_PRE_EVENT`, `ZST_PAD_PROBE_POST_EVENT` (as bitflags).
     - `zst_pad_probe_return_t`: `ZST_PAD_PROBE_OK`, `ZST_PAD_PROBE_DROP`, `ZST_PAD_PROBE_BLOCK`, `ZST_PAD_PROBE_REBLOCK`.
     - `zst_pad_probe_info_t`: structure to pass into callback, containing the pad, type, and buffer pointer.
     - `typedef zst_pad_probe_return_t (*zst_pad_probe_cb)(zst_pad_t*, zst_pad_probe_info_t*, void*)`
   - Add new public pad functions:
     - `uint32_t zst_pad_add_probe(zst_pad_t* pad, uint32_t mask, zst_pad_probe_cb callback, void* user_data, void (*destroy_data)(void*))`
     - `void zst_pad_remove_probe(zst_pad_t* pad, uint32_t id)`
     - `void zst_pad_block(zst_pad_t* pad)`
     - `void zst_pad_unblock(zst_pad_t* pad)`

2. **Update Pad Struct**
   - In `include/zst_pad.h`, add fields to `struct zst_pad`:
     - Mutex and Condition variable (`pthread_mutex_t`, `pthread_cond_t`) to handle thread synchronization for blocking.
     - Linked list or array of probes.
     - Status flag (`is_blocked`).
     - Wait, zstreamer uses atomic builtins for state. However, thread blocking requires sleep/wakeup which implies mutex/condvar or semaphores. I will add `pthread_mutex_t` and `pthread_cond_t` for blocking.

3. **Implement Pad Probe Logic**
   - In `src/zst_pad.c`, initialize the mutex and condition variable in `zst_pad_create` and destroy them in `zst_pad_destroy`.
   - Implement `zst_pad_add_probe`, `zst_pad_remove_probe`, `zst_pad_block`, and `zst_pad_unblock`.
   - In `zst_pad_push` and `zst_pad_pull` (or rather `default_sink_pad_push` and `default_src_pad_pull`), intercept buffers.
   - Run `ZST_PAD_PROBE_PRE_BUFFER` probes before calling the element's process function.
   - Run `ZST_PAD_PROBE_POST_BUFFER` probes after the process function produces `out_buf` (but before pushing downstream).
   - If a probe returns `ZST_PAD_PROBE_DROP`, immediately stop processing and return `ZST_OK` (dropping the buffer).
   - If a pad is blocked (via `zst_pad_block` or `ZST_PAD_PROBE_BLOCK`), the calling thread should wait on the pad's condition variable until unblocked. Note: the "Block callback: fire on first blocked buffer, return PROBE_OK to unblock or PROBE_REBLOCK to keep blocking" implies that we also might need a block callback mechanism. Adding a probe with `BLOCK` type could act as a block callback.

4. **Verify Implementation**
   - Write tests in `tests/test_core.c` or a new test file to verify that pad probes can:
     - Intercept and modify buffers.
     - Drop buffers.
     - Block and unblock data flow successfully across threads.
   - Run tests (`./test_core`) with ThreadSanitizer enabled to ensure no races.

5. **Pre-commit Checks**
   - Run `pre_commit_instructions` tool and execute required steps (like formatting, tests, memory leaks, thread sanitizer).

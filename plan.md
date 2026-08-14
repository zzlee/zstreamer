1. **Fix `mininputbw` type**
   - Update `srt_common.h` and `srt_common.c` to use `int64_t* mininputbw` and `atoll(val)`.
   - In `srt_source.c` and `srt_sink.c`, change `int mininputbw;` to `int64_t mininputbw;` in state structs.
   - In `set_property` for both, use `atoll(value)`.
   - In `get_property` for both, use `%lld` and `(long long)s->mininputbw`.
   - In `apply_socket_opts` for both, use `int64_t val = s->mininputbw;`.
2. **Compile and Verify**
   - `mkdir -p build && cd build && cmake .. && make -j$(nproc)`
   - Run tests `cd tests && ctest -E "test_dynamic|test_install|test_rtp_integration|test_st2110"`
3. **Pre-commit and Submit**
   - Record memory.
   - Submit the changes.

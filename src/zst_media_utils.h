#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <string.h>

static inline int zst_find_start_code(const uint8_t* data, int size, int offset, int* code_size) {
    if (code_size) *code_size = 0;
    if (!data || size <= 0 || offset < 0 || offset + 2 >= size) return -1;

    const uint8_t* ptr = data + offset;
    const uint8_t* end = data + size - 2;

    while (ptr < end) {
        ptr = (const uint8_t*)memchr(ptr, 0, end - ptr);
        if (!ptr) break;

        if (ptr[1] == 0) {
            if (ptr[2] == 1) {
                if (code_size) *code_size = 3;
                return ptr - data;
            } else if (ptr[2] == 0 && ptr + 3 < data + size && ptr[3] == 1) {
                if (code_size) *code_size = 4;
                return ptr - data;
            }
        }
        ptr++;
    }
    return -1;
}

#ifdef __cplusplus
}
#endif

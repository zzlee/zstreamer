#include "zstreamer/elements/zst_st2110_utils.h"

bool zst_st2110_20_validate_packet(const zst_buffer_t* buf) {
    if (!buf || !buf->memory.data || buf->memory.size < 12) {
        return false;
    }

    const uint8_t* data = (const uint8_t*)buf->memory.data;
    // Basic RTP header checks
    uint8_t version = (data[0] >> 6) & 0x03;
    if (version != 2) {
        return false;
    }

    return true;
}

bool zst_st2110_30_validate_packet(const zst_buffer_t* buf) {
    if (!buf || !buf->memory.data || buf->memory.size < 12) {
        return false;
    }

    const uint8_t* data = (const uint8_t*)buf->memory.data;
    // Basic RTP header checks
    uint8_t version = (data[0] >> 6) & 0x03;
    if (version != 2) {
        return false;
    }

    return true;
}
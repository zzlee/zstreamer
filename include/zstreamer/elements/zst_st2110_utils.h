#ifndef ZST_ST2110_UTILS_H
#define ZST_ST2110_UTILS_H

#include "zst_buffer.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Validates a buffer as an RFC 4175 (ST2110-20) RTP packet.
 *
 * @param buf The buffer containing the RTP packet.
 * @return true if valid, false otherwise.
 */
bool zst_st2110_20_validate_packet(const zst_buffer_t* buf);

/**
 * Validates a buffer as an RFC 3190 (ST2110-30) RTP packet.
 *
 * @param buf The buffer containing the RTP packet.
 * @return true if valid, false otherwise.
 */
bool zst_st2110_30_validate_packet(const zst_buffer_t* buf);

#ifdef __cplusplus
}
#endif

#endif // ZST_ST2110_UTILS_H
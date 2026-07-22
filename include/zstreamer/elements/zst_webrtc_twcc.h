#ifndef ZST_WEBRTC_TWCC_H
#define ZST_WEBRTC_TWCC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "zst_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zst_webrtc_twcc zst_webrtc_twcc_t;

/**
 * @brief Create a Transport-Wide Congestion Control (TWCC) session
 *
 * @param pc_id The libdatachannel PeerConnection ID (used to attach the incoming RTCP interceptor)
 * @param bus The bus to post ZST_EVENT_BITRATE_CHANGED events to
 * @return zst_webrtc_twcc_t*
 */
zst_webrtc_twcc_t* zst_webrtc_twcc_create(int pc_id, zst_bus_t* bus);

/**
 * @brief Destroy a TWCC session
 */
void zst_webrtc_twcc_destroy(zst_webrtc_twcc_t* twcc);

/**
 * @brief Process an incoming RTCP packet (for TWCC feedback)
 *
 * @param twcc The TWCC session
 * @param message The packet data
 * @param size The packet size
 * @return void* The opaque message to return (or original message)
 */
void* zst_webrtc_twcc_process_incoming(zst_webrtc_twcc_t* twcc, const char* message, int size);

/**
 * @brief Process an outgoing RTP packet (add TWCC extension)
 *
 * @param twcc The TWCC session
 * @param message The packet data
 * @param size The packet size
 * @return void* The opaque message to return (or original message)
 */
void* zst_webrtc_twcc_process_outgoing(zst_webrtc_twcc_t* twcc, const char* message, int size);

/**
 * @brief Parse the remote offer SDP to find the TWCC extmap ID
 *
 * @param twcc The TWCC session
 * @param offer_sdp The SDP string from the remote offer
 * @return int The extmap ID found (e.g. 5), or -1 if not found
 */
int zst_webrtc_twcc_parse_offer(zst_webrtc_twcc_t* twcc, const char* offer_sdp);

/**
 * @brief Inject the TWCC extmap into the local answer SDP string
 *
 * @param twcc The TWCC session
 * @param answer_sdp The local answer SDP string (must be null-terminated)
 * @param max_len The total capacity of the answer_sdp buffer
 * @return int 0 on success, <0 on error (e.g. buffer too small)
 */
int zst_webrtc_twcc_inject_answer(zst_webrtc_twcc_t* twcc, char* answer_sdp, size_t max_len);

#ifdef __cplusplus
}
#endif

#endif /* ZST_WEBRTC_TWCC_H */

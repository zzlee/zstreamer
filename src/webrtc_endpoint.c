#include "zstreamer/elements/webrtc.h"
#include <stdlib.h>
#include <string.h>
#include "zst_element.h"

typedef struct {
    int dummy;
} webrtc_endpoint_t;

static zst_result_t _process(zst_element_t* elem, zst_buffer_t* in, zst_buffer_t** out) {
    (void)elem;
    (void)in;
    (void)out;
    return ZST_OK;
}

static zst_result_t _open(zst_element_t* el) {
    (void)el;
    return ZST_OK;
}

static zst_element_ops_t g_ops = {
    .name = "webrtcendpoint",
    .open = _open,
    .process = _process,
};

zst_element_t* zst_webrtc_endpoint_create(const char* name) {
    (void)name;
    webrtc_endpoint_t* priv = calloc(1, sizeof(webrtc_endpoint_t));
    zst_element_t* elem = zst_element_create(&g_ops, priv);

    return elem;
}

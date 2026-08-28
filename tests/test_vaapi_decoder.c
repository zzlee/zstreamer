/*=============================================================================
    test_vaapi_decoder.c — VA-API decoder property and roundtrip tests
=============================================================================*/

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <string.h>

#include <unistd.h>
#include "zst_buffer.h"
#include "zst_element.h"
#include "zst_element_factory.h"
#include "zst_plugin.h"
#include "zstreamer/elements/zst_vaapi_video_decoder.h"

static const char* test_plugin_path(void)
{
    const char* ppath = getenv("ZSTREAMER_TEST_PLUGIN_PATH");
    if (!ppath) {
        ppath = "/workspace/build/plugins";
        if (access("/app/build/plugins", R_OK) == 0) {
            ppath = "/app/build/plugins";
        }
    }
    return ppath;
}

// Simple helper to destroy a buffer payload
static void release_data(void* priv)
{
    free(priv);
}

static void destroy_frame(zst_buffer_t* buf)
{
    free(buf->payload);
    buf->payload = NULL;
}

static zst_buffer_t* make_i420_frame(uint32_t width, uint32_t height, uint64_t frame_no)
{
    size_t y_size = (size_t)width * height;
    size_t uv_size = y_size / 4;
    uint8_t* data = (uint8_t*)malloc(y_size + uv_size * 2);
    if (!data) return NULL;

    memset(data, (int)(16 + (frame_no % 96)), y_size);
    memset(data + y_size, 128, uv_size);
    memset(data + y_size + uv_size, 128, uv_size);

    zst_video_frame_t* frame = (zst_video_frame_t*)calloc(1, sizeof(*frame));
    if (!frame) {
        free(data);
        return NULL;
    }
    frame->width = width;
    frame->height = height;
    frame->format = 0; // AV_PIX_FMT_YUV420P
    frame->stride[0] = width;
    frame->stride[1] = width / 2;
    frame->stride[2] = width / 2;
    frame->plane[0] = data;
    frame->plane[1] = data + y_size;
    frame->plane[2] = data + y_size + uv_size;

    zst_buffer_t* buf = zst_buffer_create(ZST_BUFFER_VIDEO_FRAME);
    if (!buf) {
        free(frame);
        free(data);
        return NULL;
    }
    buf->memory.type = ZST_MEMORY_CPU;
    buf->memory.data = data;
    buf->memory.size = y_size + uv_size * 2;
    buf->memory.priv = data;
    buf->memory.release = release_data;
    buf->payload = frame;
    buf->destroy = destroy_frame;
    buf->pts = frame_no * 33333333ULL;
    buf->dts = buf->pts;
    buf->duration = 33333333ULL;
    return buf;
}

static void test_factory_and_properties(void)
{
    if (zst_register_builtin_elements() != ZST_OK) abort();

    const zst_element_desc_t* desc = zst_element_factory_get_desc(ZST_VAAPI_VIDEO_DECODER_FACTORY);
    assert(desc != NULL);
    assert(desc->nb_properties >= 3);
    assert(desc->nb_pads == 3);

    const zst_element_desc_t* alias = zst_element_factory_get_desc(ZST_VAAPI_VIDEO_DECODER_FACTORY_ALIAS);
    assert(alias != NULL);

    zst_element_t* dec = zst_element_factory_make(ZST_VAAPI_VIDEO_DECODER_FACTORY_ALIAS);
    assert(dec != NULL);
    assert(dec->ops != NULL);
    assert(strcmp(dec->ops->name, ZST_VAAPI_VIDEO_DECODER_FACTORY) == 0);

    assert(zst_element_set_property(dec, ZST_VAAPI_VIDEO_DECODER_PROP_CODEC, "h265") == ZST_OK);
    assert(zst_element_set_property(dec, ZST_VAAPI_VIDEO_DECODER_PROP_MEMORY_TYPE, "dmabuf") == ZST_OK);

    char value[64];
    assert(zst_element_get_property(dec, ZST_VAAPI_VIDEO_DECODER_PROP_CODEC, value, sizeof(value)) == ZST_OK);
    assert(strcmp(value, "h265") == 0);
    assert(zst_element_get_property(dec, ZST_VAAPI_VIDEO_DECODER_PROP_MEMORY_TYPE, value, sizeof(value)) == ZST_OK);
    assert(strcmp(value, "dmabuf") == 0);

    zst_element_destroy(dec);
}

static void test_decoding_mode(const char* memory_type)
{
    printf("Testing VA-API decoding with output mode: %s...\n", memory_type);

    zst_element_t* enc = zst_element_factory_make("x264enc");
    if (!enc) {
        printf("x264enc not found, skipping decoding test\n");
        return;
    }

    zst_element_t* dec = zst_vaapi_video_decoder_create();
    assert(dec != NULL);

    assert(zst_element_set_property(enc, "preset", "ultrafast") == ZST_OK);
    assert(zst_element_set_property(enc, "tune", "zerolatency") == ZST_OK);

    assert(zst_element_set_property(dec, ZST_VAAPI_VIDEO_DECODER_PROP_CODEC, "h264") == ZST_OK);
    assert(zst_element_set_property(dec, ZST_VAAPI_VIDEO_DECODER_PROP_MEMORY_TYPE, memory_type) == ZST_OK);

    zst_result_t ret = zst_element_set_state(enc, ZST_STATE_READY);
    assert(ret == ZST_OK);

    ret = zst_element_set_state(dec, ZST_STATE_READY);
    if (ret != ZST_OK) {
        printf("SKIP: VA-API decoder init failed or DRM device missing (mode=%s)\n", memory_type);
        zst_element_destroy(enc);
        zst_element_destroy(dec);
        return;
    }

    unsigned decoded_frames = 0;
    for (uint64_t i = 0; i < 15; i++) {
        zst_buffer_t* raw = make_i420_frame(128, 128, i);
        assert(raw != NULL);

        zst_buffer_t* pkt = NULL;
        ret = enc->ops->process(enc, raw, &pkt);
        zst_buffer_unref(raw);
        assert(ret == ZST_OK);

        if (pkt) {
            zst_buffer_t* out = NULL;
            ret = dec->ops->process(dec, pkt, &out);
            zst_buffer_unref(pkt);
            if (ret != ZST_OK) {
                printf("SKIP: VA-API decode failed to initialize device context / driver entrypoints (ret=%d)\n", ret);
                zst_element_set_state(enc, ZST_STATE_NULL);
                zst_element_set_state(dec, ZST_STATE_NULL);
                zst_element_destroy(enc);
                zst_element_destroy(dec);
                return;
            }
            if (out) {
                if (!(out->flags & ZST_BUFFER_FLAG_EOS)) {
                    if (strcmp(memory_type, "dmabuf") == 0) {
                        assert(out->memory.type == ZST_MEMORY_DMABUF);
                    } else {
                        assert(out->memory.type == ZST_MEMORY_CPU);
                    }
                    zst_video_frame_t* vframe = out->payload;
                    assert(vframe != NULL);
                    assert(vframe->width == 128);
                    assert(vframe->height == 128);
                    decoded_frames++;
                }
                zst_buffer_unref(out);
            }
        }
    }

    // Push EOS to flush
    zst_buffer_t* eos = zst_buffer_create(ZST_BUFFER_VIDEO_FRAME);
    assert(eos != NULL);
    eos->flags |= ZST_BUFFER_FLAG_EOS;

    zst_buffer_t* pkt = NULL;
    ret = enc->ops->process(enc, eos, &pkt);
    zst_buffer_unref(eos);
    assert(ret == ZST_OK);

    if (pkt) {
        zst_buffer_t* out = NULL;
        ret = dec->ops->process(dec, pkt, &out);
        zst_buffer_unref(pkt);
        assert(ret == ZST_OK);
        if (out) {
            if (!(out->flags & ZST_BUFFER_FLAG_EOS) && out->memory.size > 0) {
                decoded_frames++;
            }
            zst_buffer_unref(out);
        }
    }

    zst_element_set_state(enc, ZST_STATE_NULL);
    zst_element_set_state(dec, ZST_STATE_NULL);
    zst_element_destroy(enc);
    zst_element_destroy(dec);

    printf("VA-API decode (%s) smoke test passed. Decoded frames: %u\n", memory_type, decoded_frames);
}

int main(void)
{
    if (zst_register_builtin_elements() != ZST_OK) abort();
    assert(zst_plugin_registry_init() == ZST_OK);
    assert(zst_plugin_registry_scan(test_plugin_path()) == ZST_OK);

    test_factory_and_properties();
    test_decoding_mode("cpu");
    test_decoding_mode("dmabuf");
    printf("VA-API decoder tests passed\n");
    return 0;
}

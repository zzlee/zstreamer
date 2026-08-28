/*=============================================================================
    test_oneapi_encoder.c — Intel oneAPI/oneVPL encoder smoke tests
=============================================================================*/

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zst_buffer.h"
#include "zst_element.h"
#include "zst_element_factory.h"
#include "zstreamer/elements/zst_oneapi_video_encoder.h"

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

    memset(data, (int)(16 + (frame_no % 32)), y_size);
    memset(data + y_size, 128, uv_size);
    memset(data + y_size + uv_size, 128, uv_size);

    zst_video_frame_t* frame = (zst_video_frame_t*)calloc(1, sizeof(*frame));
    if (!frame) {
        free(data);
        return NULL;
    }
    frame->width = width;
    frame->height = height;
    frame->format = 0;
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

    const zst_element_desc_t* desc = zst_element_factory_get_desc(ZST_ONEAPI_VIDEO_ENCODER_FACTORY);
    assert(desc != NULL);
    assert(desc->nb_properties >= 6);
    assert(desc->nb_pads == 3);

    const zst_element_desc_t* alias = zst_element_factory_get_desc(ZST_ONEAPI_VIDEO_ENCODER_FACTORY_ALIAS);
    assert(alias != NULL);

    zst_element_t* enc = zst_element_factory_make(ZST_ONEAPI_VIDEO_ENCODER_FACTORY_ALIAS);
    assert(enc != NULL);
    assert(enc->ops != NULL);
    assert(strcmp(enc->ops->name, ZST_ONEAPI_VIDEO_ENCODER_FACTORY) == 0);

    assert(zst_element_set_property(enc, ZST_ONEAPI_VIDEO_ENCODER_PROP_CODEC, "h265") == ZST_OK);
    assert(zst_element_set_property(enc, ZST_ONEAPI_VIDEO_ENCODER_PROP_BITRATE, "2500000") == ZST_OK);
    assert(zst_element_set_property(enc, ZST_ONEAPI_VIDEO_ENCODER_PROP_GOP_SIZE, "60") == ZST_OK);
    assert(zst_element_set_property(enc, ZST_ONEAPI_VIDEO_ENCODER_PROP_PRESET, "speed") == ZST_OK);
    assert(zst_element_set_property(enc, ZST_ONEAPI_VIDEO_ENCODER_PROP_FPS, "30/1") == ZST_OK);

    char value[64];
    assert(zst_element_get_property(enc, ZST_ONEAPI_VIDEO_ENCODER_PROP_CODEC, value, sizeof(value)) == ZST_OK);
    assert(strcmp(value, "h265") == 0);
    assert(zst_element_get_property(enc, ZST_ONEAPI_VIDEO_ENCODER_PROP_BITRATE, value, sizeof(value)) == ZST_OK);
    assert(strcmp(value, "2500000") == 0);

    zst_element_destroy(enc);
}

static void test_runtime_or_skip(void)
{
    zst_element_t* enc = zst_oneapi_video_encoder_create();
    assert(enc != NULL);
    assert(zst_element_set_property(enc, ZST_ONEAPI_VIDEO_ENCODER_PROP_CODEC, "h264") == ZST_OK);
    assert(zst_element_set_property(enc, ZST_ONEAPI_VIDEO_ENCODER_PROP_BITRATE, "1000000") == ZST_OK);

    zst_result_t ret = zst_element_set_state(enc, ZST_STATE_READY);
    if (ret != ZST_OK) {
        printf("SKIP: Intel oneVPL hardware encoder runtime is not available in this environment\n");
        zst_element_destroy(enc);
        return;
    }

    unsigned packets = 0;
    for (uint64_t i = 0; i < 30; i++) {
        zst_buffer_t* in = make_i420_frame(128, 96, i);
        assert(in != NULL);
        zst_buffer_t* out = NULL;
        ret = enc->ops->process(enc, in, &out);
        zst_buffer_unref(in);
        assert(ret == ZST_OK);
        if (out) {
            if (!(out->flags & ZST_BUFFER_FLAG_EOS) && out->memory.size > 0) packets++;
            zst_buffer_unref(out);
        }
    }

    zst_buffer_t* eos = zst_buffer_create(ZST_BUFFER_VIDEO_FRAME);
    assert(eos != NULL);
    eos->flags |= ZST_BUFFER_FLAG_EOS;
    for (int i = 0; i < 16; i++) {
        zst_buffer_t* out = NULL;
        ret = enc->ops->process(enc, eos, &out);
        assert(ret == ZST_OK);
        if (!out) break;
        if (!(out->flags & ZST_BUFFER_FLAG_EOS) && out->memory.size > 0) packets++;
        int is_eos = (out->flags & ZST_BUFFER_FLAG_EOS) != 0;
        zst_buffer_unref(out);
        if (is_eos) break;
    }
    zst_buffer_unref(eos);

    assert(packets > 0);
    zst_element_set_state(enc, ZST_STATE_NULL);
    zst_element_destroy(enc);
}

int main(void)
{
    test_factory_and_properties();
    test_runtime_or_skip();
    printf("oneAPI encoder tests passed\n");
    return 0;
}

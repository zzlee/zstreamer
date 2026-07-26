/*=============================================================================
    test_oneapi_decoder.c — Intel oneAPI/oneVPL decoder smoke tests
=============================================================================*/

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zst_buffer.h"
#include "zst_log.h"
#include "zst_element.h"
#include "zst_element_factory.h"
#include "zstreamer/elements/zst_oneapi_video_encoder.h"
#include "zstreamer/elements/zst_oneapi_video_decoder.h"

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
    assert(zst_register_builtin_elements() == ZST_OK);

    const zst_element_desc_t* desc = zst_element_factory_get_desc(ZST_ONEAPI_VIDEO_DECODER_FACTORY);
    assert(desc != NULL);
    assert(desc->nb_properties >= 1);
    assert(desc->nb_pads == 3);

    const zst_element_desc_t* alias = zst_element_factory_get_desc(ZST_ONEAPI_VIDEO_DECODER_FACTORY_ALIAS);
    assert(alias != NULL);

    zst_element_t* dec = zst_element_factory_make(ZST_ONEAPI_VIDEO_DECODER_FACTORY_ALIAS);
    assert(dec != NULL);
    assert(dec->ops != NULL);
    assert(strcmp(dec->ops->name, ZST_ONEAPI_VIDEO_DECODER_FACTORY) == 0);

    assert(zst_element_set_property(dec, ZST_ONEAPI_VIDEO_DECODER_PROP_CODEC, "h265") == ZST_OK);

    char value[64];
    assert(zst_element_get_property(dec, ZST_ONEAPI_VIDEO_DECODER_PROP_CODEC, value, sizeof(value)) == ZST_OK);
    assert(strcmp(value, "h265") == 0);

    zst_element_destroy(dec);
}

static void test_runtime_or_skip(void)
{
    zst_element_t* enc = zst_oneapi_video_encoder_create();
    zst_element_t* dec = zst_oneapi_video_decoder_create();
    assert(enc != NULL && dec != NULL);

    assert(zst_element_set_property(enc, ZST_ONEAPI_VIDEO_ENCODER_PROP_CODEC, "h264") == ZST_OK);
    assert(zst_element_set_property(dec, ZST_ONEAPI_VIDEO_DECODER_PROP_CODEC, "h264") == ZST_OK);

    zst_log_set_level(ZST_LOG_LEVEL_TRACE);

    zst_result_t ret1 = zst_element_set_state(enc, ZST_STATE_READY);
    zst_result_t ret2 = zst_element_set_state(dec, ZST_STATE_READY);

    if (ret1 != ZST_OK || ret2 != ZST_OK) {
        printf("SKIP: Intel oneVPL hardware codec runtime is not available in this environment\n");
        zst_element_destroy(enc);
        zst_element_destroy(dec);
        return;
    }

    /* Encode 15 frames of 320x240 and pass the packets to decoder */
    unsigned decoded_frames = 0;
    for (uint64_t i = 0; i < 15; i++) {
        zst_buffer_t* in = make_i420_frame(320, 240, i);
        assert(in != NULL);
        zst_buffer_t* enc_out = NULL;
        zst_result_t ret = enc->ops->process(enc, in, &enc_out);
        zst_buffer_unref(in);
        assert(ret == ZST_OK);

        if (enc_out) {
            if (!(enc_out->flags & ZST_BUFFER_FLAG_EOS) && enc_out->memory.size > 0) {
                zst_buffer_t* dec_out = NULL;
                zst_result_t dec_ret = dec->ops->process(dec, enc_out, &dec_out);
                assert(dec_ret == ZST_OK);
                if (dec_out) {
                    if (!(dec_out->flags & ZST_BUFFER_FLAG_EOS) && dec_out->memory.size > 0) {
                        zst_video_frame_t* vf = (zst_video_frame_t*)dec_out->payload;
                        assert(vf != NULL);
                        assert(vf->width == 320);
                        assert(vf->height == 240);
                        assert(vf->format == 1); /* NV12 */
                        decoded_frames++;
                    }
                    zst_buffer_unref(dec_out);
                }
            }
            zst_buffer_unref(enc_out);
        }
    }

    /* Drain encoder */
    zst_buffer_t* eos_enc = zst_buffer_create(ZST_BUFFER_VIDEO_FRAME);
    assert(eos_enc != NULL);
    eos_enc->flags |= ZST_BUFFER_FLAG_EOS;

    while (1) {
        zst_buffer_t* enc_out = NULL;
        zst_result_t ret = enc->ops->process(enc, eos_enc, &enc_out);
        assert(ret == ZST_OK);
        if (!enc_out) break;

        int is_eos = (enc_out->flags & ZST_BUFFER_FLAG_EOS) != 0;
        if (!is_eos && enc_out->memory.size > 0) {
            zst_buffer_t* dec_out = NULL;
            zst_result_t dec_ret = dec->ops->process(dec, enc_out, &dec_out);
            assert(dec_ret == ZST_OK);
            if (dec_out) {
                if (!(dec_out->flags & ZST_BUFFER_FLAG_EOS) && dec_out->memory.size > 0) {
                    zst_video_frame_t* vf = (zst_video_frame_t*)dec_out->payload;
                    assert(vf != NULL);
                    assert(vf->width == 320);
                    assert(vf->height == 240);
                    decoded_frames++;
                }
                zst_buffer_unref(dec_out);
            }
        }
        zst_buffer_unref(enc_out);
        if (is_eos) break;
    }
    zst_buffer_unref(eos_enc);

    /* Drain decoder */
    zst_buffer_t* eos_dec = zst_buffer_create(ZST_BUFFER_VIDEO_PACKET);
    assert(eos_dec != NULL);
    eos_dec->flags |= ZST_BUFFER_FLAG_EOS;

    while (1) {
        zst_buffer_t* dec_out = NULL;
        zst_result_t ret = dec->ops->process(dec, eos_dec, &dec_out);
        assert(ret == ZST_OK);
        if (!dec_out) break;

        int is_eos = (dec_out->flags & ZST_BUFFER_FLAG_EOS) != 0;
        if (!is_eos && dec_out->memory.size > 0) {
            zst_video_frame_t* vf = (zst_video_frame_t*)dec_out->payload;
            assert(vf != NULL);
            assert(vf->width == 320);
            assert(vf->height == 240);
            decoded_frames++;
        }
        zst_buffer_unref(dec_out);
        if (is_eos) break;
    }
    zst_buffer_unref(eos_dec);

    assert(decoded_frames > 0);
    printf("oneAPI decoder successfully decoded %u frames\n", decoded_frames);

    zst_element_set_state(enc, ZST_STATE_NULL);
    zst_element_set_state(dec, ZST_STATE_NULL);
    zst_element_destroy(enc);
    zst_element_destroy(dec);
}

int main(void)
{
    test_factory_and_properties();
    test_runtime_or_skip();
    printf("oneAPI decoder tests passed\n");
    return 0;
}

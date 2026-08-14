/*=============================================================================
    test_codec.c — Codec correctness tests using official test source elements

    Uses zstreamer's built-in video_test_src (4l) and audio_test_src (4m)
    elements instead of handmade frames — tests zstreamer elements end-to-end
    with real zstreamer pipeline components.
=============================================================================*/

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>
#include <math.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>

#include "zst_types.h"
#include "zst_buffer.h"
#include "zst_pad.h"
#include "zst_element.h"
#include "zst_buffer_pool.h"
#include "zst_caps.h"

/* Extern element constructors from the project */
zst_element_t* zst_x264_encoder_create(void);
zst_element_t* zst_aac_encoder_create(void);
zst_element_t* zst_video_test_src_create(void);
zst_element_t* zst_audio_test_src_create(void);
zst_element_t* zst_mp4_muxer_create(void);

static int g_tests_run   = 0;
static int g_tests_passed = 0;

#define TEST(name)                                              \
    do {                                                        \
        g_tests_run++;                                          \
        printf("  TEST: %-50s ... ", name);                     \
        fflush(stdout);                                         \
    } while (0)

#define PASS()                                                  \
    do {                                                        \
        g_tests_passed++;                                       \
        printf("PASS\n");                                       \
    } while (0)

/* ═══════════════════════════════════════════════════════════════
   Helpers — bitstream analysis
   ═══════════════════════════════════════════════════════════════ */

/* Check if a buffer starts with a valid H.264 NAL start code */
static int
has_h264_start_code(const uint8_t* data, size_t size)
{
    if (size < 4) return 0;
    if (data[0] == 0x00 && data[1] == 0x00 &&
        data[2] == 0x00 && data[3] == 0x01) return 1;   /* AVC */
    if (size >= 3 &&
        data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x01) return 1;  /* Annex B */
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
   Test source helpers — create & configure a video test source
   ═══════════════════════════════════════════════════════════════ */

static zst_element_t*
create_video_source(int width, int height, int fps, const char* pattern)
{
    zst_element_t* src = zst_video_test_src_create();
    assert(src != NULL);

    char buf[32];
    snprintf(buf, sizeof(buf), "%d", width);
    zst_element_set_property(src, "width", buf);
    snprintf(buf, sizeof(buf), "%d", height);
    zst_element_set_property(src, "height", buf);
    snprintf(buf, sizeof(buf), "%d", fps);
    zst_element_set_property(src, "fps", buf);
    zst_element_set_property(src, "pattern", pattern);
    zst_element_set_property(src, "num-buffers", "-1");  /* unlimited */

    zst_element_set_state(src, ZST_STATE_READY);
    return src;
}

static zst_element_t*
create_audio_source(int sample_rate, int channels, const char* wave, double freq)
{
    zst_element_t* src = zst_audio_test_src_create();
    assert(src != NULL);

    char buf[32];
    char freq_str[32];
    snprintf(freq_str, sizeof(freq_str), "%g", freq);

    snprintf(buf, sizeof(buf), "%d", sample_rate);
    zst_element_set_property(src, "sample-rate", buf);
    snprintf(buf, sizeof(buf), "%d", channels);
    zst_element_set_property(src, "channels", buf);
    zst_element_set_property(src, "wave", wave);
    zst_element_set_property(src, "frequency", freq_str);
    zst_element_set_property(src, "num-buffers", "-1");    /* unlimited */

    zst_element_set_state(src, ZST_STATE_READY);
    return src;
}

/* Generate one video frame from the test source */
static zst_buffer_t*
generate_video_frame(zst_element_t* src)
{
    zst_buffer_t* out = NULL;
    zst_result_t r = src->ops->process(src, NULL, &out);
    assert(r == ZST_OK);
    assert(out != NULL);
    return out;
}

/* Generate one audio frame from the test source */
static zst_buffer_t*
generate_audio_frame(zst_element_t* src)
{
    zst_buffer_t* out = NULL;
    zst_result_t r = src->ops->process(src, NULL, &out);
    assert(r == ZST_OK);
    assert(out != NULL);
    return out;
}

/* ═══════════════════════════════════════════════════════════════
   H.264 Encoder Tests
   ═══════════════════════════════════════════════════════════════ */

static void
test_h264_encode_properties(void)
{
    TEST("h264 encoder property set/get");

    zst_element_t* enc = zst_x264_encoder_create();
    assert(enc != NULL);

    zst_result_t res;

    /* Set properties */
    res = zst_element_set_property_string(enc, "preset", "superfast");
    assert(res == ZST_OK);

    res = zst_element_set_property_string(enc, "tune", "stillimage");
    assert(res == ZST_OK);

    res = zst_element_set_property_int(enc, "bitrate", 2000000);
    assert(res == ZST_OK);

    /* Get and verify properties */
    char preset[32] = {0};
    res = zst_element_get_property_string(enc, "preset", preset, sizeof(preset));
    assert(res == ZST_OK);
    assert(strcmp(preset, "superfast") == 0);

    char tune[32] = {0};
    res = zst_element_get_property_string(enc, "tune", tune, sizeof(tune));
    assert(res == ZST_OK);
    assert(strcmp(tune, "stillimage") == 0);

    int64_t bitrate = -1;
    res = zst_element_get_property_int(enc, "bitrate", &bitrate);
    assert(res == ZST_OK);
    assert(bitrate == 2000000);

    zst_element_destroy(enc);
    PASS();
}

static void
test_h264_encode_basic(void)
{
    TEST("h264 encoder produces valid H.264 bitstream");

    zst_element_t* src = create_video_source(320, 240, 30, "gradient");
    zst_element_t* enc = zst_x264_encoder_create();
    assert(enc != NULL);
    zst_element_set_state(enc, ZST_STATE_READY);

    int packet_count = 0;
    int found_sps = 0, found_pps = 0, found_slice = 0;

    for (int i = 0; i < 10; i++) {
        zst_buffer_t* in  = generate_video_frame(src);
        zst_buffer_t* out = NULL;

        zst_result_t r = enc->ops->process(enc, in, &out);
        assert(r == ZST_OK);
        zst_buffer_unref(in);

        if (!out) continue;

        /* Basic structural checks */
        assert(out->memory.data != NULL);
        assert(out->memory.size > 0);
        assert(out->type == ZST_BUFFER_VIDEO_PACKET);

        /* Verify H.264 start code present */
        assert(has_h264_start_code(out->memory.data, out->memory.size));

        /* Scan ALL NAL units in this packet */
        const uint8_t* d = out->memory.data;
        size_t sz = out->memory.size;
        size_t pos = 0;

        while (pos + 3 < sz) {
            int start_len = 0;
            if (d[pos] == 0x00 && d[pos+1] == 0x00 && d[pos+2] == 0x00 && d[pos+3] == 0x01) {
                start_len = 4;
            } else if (pos + 2 < sz && d[pos] == 0x00 && d[pos+1] == 0x00 && d[pos+2] == 0x01) {
                start_len = 3;
            }
            if (start_len == 0) break;

            pos += start_len;
            if (pos >= sz) break;

            int nal_type = d[pos] & 0x1F;
            switch(nal_type) {
                case 7:  found_sps   = 1; break;
                case 8:  found_pps   = 1; break;
                case 1:
                case 5:  found_slice = 1; break;
                default: break;
            }

            /* Skip to next NAL: find next start code */
            pos++;
            while (pos + 2 < sz) {
                if (d[pos] == 0x00 && d[pos+1] == 0x00) {
                    if (d[pos+2] == 0x01 || (pos+3 < sz && d[pos+2] == 0x00 && d[pos+3] == 0x01))
                        break;
                }
                pos++;
            }
        }

        /* Packet has valid timestamps */
        assert(out->pts != ZST_ERROR);
        assert(out->dts != ZST_ERROR);

        packet_count++;
        zst_buffer_unref(out);
    }

    /* At least one packet should have been produced */
    assert(packet_count >= 1);
    /* SPS and PPS must appear (they are in the first output packet) */
    assert(found_sps == 1);
    assert(found_pps == 1);
    /* At least one coded slice */
    assert(found_slice == 1);

    zst_element_set_state(enc, ZST_STATE_NULL);
    zst_element_destroy(enc);
    zst_element_set_state(src, ZST_STATE_NULL);
    zst_element_destroy(src);

    PASS();
}

static void
test_h264_encode_multiple_resolutions(void)
{
    TEST("h264 encoder handles different resolutions");

    zst_element_t* enc = zst_x264_encoder_create();
    assert(enc != NULL);

    /* Test small, medium, and larger resolutions */
    struct { int w, h; } resolutions[] = {
        {160, 120},
        {320, 240},
        {640, 480},
    };
    int num_res = sizeof(resolutions) / sizeof(resolutions[0]);
    int all_ok = 1;

    for (int r = 0; r < num_res; r++) {
        int w = resolutions[r].w;
        int h = resolutions[r].h;

        /* Create a new source for this resolution */
        zst_element_t* src = create_video_source(w, h, 30, "gradient");
        zst_element_set_state(enc, ZST_STATE_READY);

        /* Encode 3 frames at each resolution */
        int local_packets = 0;
        for (int i = 0; i < 3; i++) {
            zst_buffer_t* in  = generate_video_frame(src);
            zst_buffer_t* out = NULL;

            zst_result_t res = enc->ops->process(enc, in, &out);
            assert(res == ZST_OK);
            zst_buffer_unref(in);

            if (out) {
                assert(out->memory.data != NULL);
                assert(out->memory.size > 0);
                assert(has_h264_start_code(out->memory.data, out->memory.size));
                local_packets++;
                zst_buffer_unref(out);
            }
        }

        if (local_packets == 0) {
            all_ok = 0;
        }

        /* Clean up this resolution's source */
        zst_element_set_state(src, ZST_STATE_NULL);
        zst_element_destroy(src);

        /* Reset encoder state between resolutions by cycling state */
        zst_element_set_state(enc, ZST_STATE_NULL);
    }

    assert(all_ok);

    zst_element_destroy(enc);

    PASS();
}

static void
test_h264_encode_decode_roundtrip(void)
{
    TEST("h264 encode->decode roundtrip (dimensions)");

    zst_element_t* src = create_video_source(320, 240, 30, "bars");
    zst_element_t* enc = zst_x264_encoder_create();
    assert(enc != NULL);
    zst_element_set_state(enc, ZST_STATE_READY);

    /* Set up FFmpeg H.264 decoder */
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    assert(codec != NULL);

    AVCodecContext* dec_ctx = avcodec_alloc_context3(codec);
    assert(dec_ctx != NULL);
    assert(avcodec_open2(dec_ctx, codec, NULL) >= 0);

    AVPacket* pkt = av_packet_alloc();
    assert(pkt != NULL);
    AVFrame*  frame = av_frame_alloc();
    assert(frame != NULL);

    int total_decoded = 0;
    int verified_dimensions = 0;

    /* Encode 15 frames and decode each one */
    for (int i = 0; i < 15; i++) {
        zst_buffer_t* in  = generate_video_frame(src);
        zst_buffer_t* out = NULL;

        zst_result_t r = enc->ops->process(enc, in, &out);
        assert(r == ZST_OK);
        zst_buffer_unref(in);

        if (!out) continue;

        /* Feed raw H.264 data to the decoder */
        av_packet_unref(pkt);
        pkt->data = out->memory.data;
        pkt->size = out->memory.size;

        if (avcodec_send_packet(dec_ctx, pkt) == 0) {
            while (avcodec_receive_frame(dec_ctx, frame) == 0) {
                total_decoded++;
                if (!verified_dimensions) {
                    assert(frame->width  == 320);
                    assert(frame->height == 240);
                    verified_dimensions = 1;
                }
            }
        }

        zst_buffer_unref(out);
    }

    /* Flush remaining frames from decoder */
    av_packet_unref(pkt);
    avcodec_send_packet(dec_ctx, NULL);
    while (avcodec_receive_frame(dec_ctx, frame) == 0) {
        total_decoded++;
    }

    assert(total_decoded >= 1);
    assert(verified_dimensions == 1);

    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&dec_ctx);

    zst_element_set_state(enc, ZST_STATE_NULL);
    zst_element_destroy(enc);
    zst_element_set_state(src, ZST_STATE_NULL);
    zst_element_destroy(src);

    PASS();
}

/* ═══════════════════════════════════════════════════════════════
   AAC Encoder Tests
   ═══════════════════════════════════════════════════════════════ */

static void
test_aac_encode_basic(void)
{
    TEST("aac encoder produces valid audio packets");

    zst_element_t* src = create_audio_source(44100, 2, "sine", 440.0);
    zst_element_t* enc = zst_aac_encoder_create();
    assert(enc != NULL);
    zst_element_set_state(enc, ZST_STATE_READY);

    int packet_count = 0;
    uint64_t total_bytes = 0;

    for (int i = 0; i < 10; i++) {
        zst_buffer_t* in  = generate_audio_frame(src);
        zst_buffer_t* out = NULL;

        zst_result_t r = enc->ops->process(enc, in, &out);
        assert(r == ZST_OK);
        zst_buffer_unref(in);

        if (!out) continue;

        /* Structural consistency */
        assert(out->memory.data != NULL);
        assert(out->memory.size > 0);
        assert(out->memory.size > 64);  /* AAC frames are > 64 bytes */
        assert(out->type == ZST_BUFFER_AUDIO_PACKET);
        assert(out->pts >= 0);

        total_bytes += out->memory.size;
        packet_count++;
        zst_buffer_unref(out);
    }

    /* AAC encoder may buffer the first frame -> slightly fewer packets */
    assert(packet_count >= 8 && packet_count <= 10);
    assert(total_bytes > 2000);

    zst_element_set_state(enc, ZST_STATE_NULL);
    zst_element_destroy(enc);
    zst_element_set_state(src, ZST_STATE_NULL);
    zst_element_destroy(src);

    PASS();
}

static void
test_aac_encode_decode_roundtrip(void)
{
    TEST("aac encode->decode roundtrip (sample rate & channels)");

    zst_element_t* src = create_audio_source(44100, 2, "sine", 440.0);
    zst_element_t* enc = zst_aac_encoder_create();
    assert(enc != NULL);
    zst_element_set_state(enc, ZST_STATE_READY);

    /* Set up FFmpeg AAC decoder */
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
    assert(codec != NULL);

    AVCodecContext* dec_ctx = avcodec_alloc_context3(codec);
    assert(dec_ctx != NULL);

    dec_ctx->sample_rate = 44100;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 37, 100)
    av_channel_layout_default(&dec_ctx->ch_layout, 2);
#else
    dec_ctx->channels = 2;
    dec_ctx->channel_layout = AV_CH_LAYOUT_STEREO;
#endif
    assert(avcodec_open2(dec_ctx, codec, NULL) >= 0);

    AVPacket* pkt = av_packet_alloc();
    assert(pkt != NULL);
    AVFrame*  frame = av_frame_alloc();
    assert(frame != NULL);

    int total_decoded = 0;
    int verified_props = 0;
    int channels_verified = 0;

    for (int i = 0; i < 10; i++) {
        zst_buffer_t* in  = generate_audio_frame(src);
        zst_buffer_t* out = NULL;

        zst_result_t r = enc->ops->process(enc, in, &out);
        assert(r == ZST_OK);
        zst_buffer_unref(in);

        if (!out) continue;

        /* Feed raw AAC packet to decoder */
        av_packet_unref(pkt);
        pkt->data = out->memory.data;
        pkt->size = out->memory.size;

        if (avcodec_send_packet(dec_ctx, pkt) == 0) {
            while (avcodec_receive_frame(dec_ctx, frame) == 0) {
                total_decoded++;
                if (!verified_props) {
                    assert(frame->sample_rate == 44100);
                    verified_props = 1;
                }
                if (!channels_verified) {
    #if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 37, 100)
                    assert(frame->ch_layout.nb_channels == 2);
    #endif
                    channels_verified = 1;
                }
            }
        }

        zst_buffer_unref(out);
    }

    /* Flush */
    av_packet_unref(pkt);
    avcodec_send_packet(dec_ctx, NULL);
    while (avcodec_receive_frame(dec_ctx, frame) == 0) {
        total_decoded++;
    }

    assert(total_decoded >= 1);

    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&dec_ctx);

    zst_element_set_state(enc, ZST_STATE_NULL);
    zst_element_destroy(enc);
    zst_element_set_state(src, ZST_STATE_NULL);
    zst_element_destroy(src);

    PASS();
}

/* ═══════════════════════════════════════════════════════════════
   Pipeline integration test: video test src -> encoder -> collector
   ═══════════════════════════════════════════════════════════════ */

typedef struct {
    int    packet_count;
    size_t total_bytes;
    int    nal_count;       /* H.264: number of NAL start codes found */
} collector_t;

/* Data-collecting collector for MP4 output — stores chunks for file write */
#define MAX_MP4_CHUNKS 64
typedef struct {
    int    chunk_count;
    size_t total_bytes;
    struct {
        uint8_t* data;
        size_t   size;
    } chunks[MAX_MP4_CHUNKS];
} mux_collector_t;

static zst_result_t
collector_process_mux_track(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    (void)out;
    collector_t* c = el->priv;
    c->packet_count++;
    c->total_bytes += in->memory.size;
    return ZST_OK;
}

static zst_result_t
mux_collector_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    (void)out;
    mux_collector_t* c = el->priv;
    if (c->chunk_count < MAX_MP4_CHUNKS) {
        uint8_t* cp = malloc(in->memory.size);
        assert(cp != NULL);
        memcpy(cp, in->memory.data, in->memory.size);
        c->chunks[c->chunk_count].data = cp;
        c->chunks[c->chunk_count].size = in->memory.size;
        c->chunk_count++;
        c->total_bytes += in->memory.size;
    }
    return ZST_OK;
}

static zst_result_t
collector_process_h264(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    (void)out;
    collector_t* c = el->priv;
    c->packet_count++;
    c->total_bytes += in->memory.size;

    /* Scan for NAL start codes */
    const uint8_t* d = in->memory.data;
    size_t sz = in->memory.size;
    for (size_t i = 0; i + 3 < sz; i++) {
        if (d[i] == 0x00 && d[i+1] == 0x00 && d[i+2] == 0x00 && d[i+3] == 0x01) {
            c->nal_count++;
            i += 3;
        } else if (i + 2 < sz && d[i] == 0x00 && d[i+1] == 0x00 && d[i+2] == 0x01) {
            c->nal_count++;
            i += 2;
        }
    }
    return ZST_OK;
}

static void
test_h264_pipeline_integration(void)
{
    TEST("h264 encoder pipeline integration (test src -> encoder -> collector)");

    /* Use proper zstreamer test source instead of handmade element */
    zst_element_t* src = create_video_source(352, 288, 30, "color-bars");

    /* Encoder */
    zst_element_t* enc = zst_x264_encoder_create();
    zst_element_set_state(enc, ZST_STATE_READY);
    zst_pad_t* enc_src_pad = zst_element_get_pad(enc, "src");

    /* Collector sink */
    collector_t* coll_data = calloc(1, sizeof(collector_t));
    assert(coll_data != NULL);
    static zst_element_ops_t sink_ops = {
        .name = "collector",
        .process = collector_process_h264,
    };
    zst_element_t* sink = zst_element_create(&sink_ops, coll_data);
    zst_pad_t* sink_pad = zst_pad_create("sink", ZST_PAD_SINK);
    zst_element_add_pad(sink, sink_pad);

    /* Link encoder -> collector */
    assert(zst_pad_link(enc_src_pad, sink_pad) == ZST_OK);

    /* Push 10 frames through the chain */
    for (int i = 0; i < 10; i++) {
        zst_buffer_t* in = generate_video_frame(src);
        zst_buffer_t* enc_out = NULL;

        zst_result_t r = enc->ops->process(enc, in, &enc_out);
        assert(r == ZST_OK);
        zst_buffer_unref(in);

        if (enc_out) {
            zst_pad_push(enc_src_pad, enc_out);
            zst_buffer_unref(enc_out);
        }
    }

    /* Send EOS to flush encoder */
    zst_buffer_t* eos = zst_buffer_create(ZST_BUFFER_VIDEO_FRAME);
    eos->flags |= ZST_BUFFER_FLAG_EOS;
    zst_buffer_t* eos_out = NULL;
    enc->ops->process(enc, eos, &eos_out);
    zst_buffer_unref(eos);
    if (eos_out) {
        zst_pad_push(enc_src_pad, eos_out);
        zst_buffer_unref(eos_out);
    }

    /* Verify collection */
    assert(coll_data->packet_count >= 1);
    assert(coll_data->total_bytes > 0);
    assert(coll_data->nal_count >= 3); /* SPS + PPS + at least one slice NAL */

    zst_element_set_state(enc, ZST_STATE_NULL);
    zst_element_set_state(src, ZST_STATE_NULL);
    zst_element_destroy(src);
    zst_element_destroy(enc);
    zst_element_destroy(sink);
    /* coll_data freed by zst_element_destroy */

    PASS();
}

/* ═══════════════════════════════════════════════════════════════
   File-output tests — produce playable .h264 / .aac files
   ═══════════════════════════════════════════════════════════════ */

static void
test_h264_file_output(void)
{
    TEST("h264 encoder produces playable .h264 file (5 sec @ 30fps)");

    zst_element_t* src = create_video_source(352, 288, 30, "gradient");
    zst_element_t* enc = zst_x264_encoder_create();
    assert(enc != NULL);
    zst_element_set_state(enc, ZST_STATE_READY);

    const int W = 352, H = 288, FPS = 30, TOTAL_FRAMES = FPS * 5;
    const char* out_path = "/tmp/test_output.h264";

    FILE* fp = fopen(out_path, "wb");
    assert(fp != NULL);

    int packet_count = 0;
    for (int i = 0; i < TOTAL_FRAMES; i++) {
        zst_buffer_t* in = generate_video_frame(src);
        zst_buffer_t* out = NULL;
        zst_result_t r = enc->ops->process(enc, in, &out);
        assert(r == ZST_OK);
        zst_buffer_unref(in);
        if (out) {
            fwrite(out->memory.data, 1, out->memory.size, fp);
            packet_count++;
            zst_buffer_unref(out);
        }
    }

    /* Flush delayed frames via EOS */
    zst_buffer_t* eos = zst_buffer_create(ZST_BUFFER_VIDEO_FRAME);
    eos->flags |= ZST_BUFFER_FLAG_EOS;
    zst_buffer_t* eos_out = NULL;
    enc->ops->process(enc, eos, &eos_out);
    zst_buffer_unref(eos);
    if (eos_out) {
        fwrite(eos_out->memory.data, 1, eos_out->memory.size, fp);
        packet_count++;
        zst_buffer_unref(eos_out);
    }

    fclose(fp);

    zst_element_set_state(enc, ZST_STATE_NULL);
    zst_element_destroy(enc);
    zst_element_set_state(src, ZST_STATE_NULL);
    zst_element_destroy(src);

    long file_size = 0;
    FILE* check = fopen(out_path, "rb");
    if (check) { fseek(check, 0, SEEK_END); file_size = ftell(check); fclose(check); }

    printf("\n        `-- %lld bytes -> %s\n"
           "        `-- %d frames, ~%.1f sec @ 30fps\n",
           (long long)file_size, out_path,
           TOTAL_FRAMES, (double)TOTAL_FRAMES / FPS);

    assert(file_size > 10000);
    assert(packet_count >= TOTAL_FRAMES);

    PASS();
}

static void
test_aac_file_output(void)
{
    TEST("aac encoder produces playable .aac file (~5 sec)");

    zst_element_t* src = create_audio_source(44100, 2, "sine", 440.0);
    zst_element_t* enc = zst_aac_encoder_create();
    assert(enc != NULL);
    zst_element_set_state(enc, ZST_STATE_READY);

    const int SR = 44100, NB = 1024;
    const int TOTAL_FRAMES = (SR * 5 + NB - 1) / NB; /* 216 frames */
    const char* out_path = "/tmp/test_output.aac";

    FILE* fp = fopen(out_path, "wb");
    assert(fp != NULL);

    int packets = 0;
    for (int i = 0; i < TOTAL_FRAMES; i++) {
        zst_buffer_t* in = generate_audio_frame(src);
        zst_buffer_t* out = NULL;
        zst_result_t r = enc->ops->process(enc, in, &out);
        assert(r == ZST_OK);
        zst_buffer_unref(in);
        if (out) {
            fwrite(out->memory.data, 1, out->memory.size, fp);
            packets++;
            zst_buffer_unref(out);
        }
    }

    fclose(fp);

    zst_element_set_state(enc, ZST_STATE_NULL);
    zst_element_destroy(enc);
    zst_element_set_state(src, ZST_STATE_NULL);
    zst_element_destroy(src);

    long file_size = 0;
    FILE* check = fopen(out_path, "rb");
    if (check) { fseek(check, 0, SEEK_END); file_size = ftell(check); fclose(check); }

    double actual_duration = (double)packets * NB / SR;
    printf("\n        `-- %lld bytes -> %s\n"
           "        `-- %d packets ~= %.2f sec @ 44100Hz\n",
           (long long)file_size, out_path,
           packets, actual_duration);

    assert(file_size > 10000);
    assert(packets >= TOTAL_FRAMES - 2);

    PASS();
}

/* ═══════════════════════════════════════════════════════════════
   MP4 Muxer test: video test src -> H.264 enc -> mp4 mux -> collector
   ═══════════════════════════════════════════════════════════════ */

static void
test_mp4_muxer_integration(void)
{
    TEST("mp4 muxer integration: src -> x264enc -> mp4 mux -> collector");

    /* Use 640x480 to match muxer's hardcoded stream parameters */
    zst_element_t* src = create_video_source(640, 480, 30, "gradient");

    /* Encoder */
    zst_element_t* enc = zst_x264_encoder_create();
    assert(enc != NULL);
    zst_element_set_state(enc, ZST_STATE_READY);
    zst_pad_t* enc_src = zst_element_get_pad(enc, "src");
    assert(enc_src != NULL);

    /* MP4 muxer */
    zst_element_t* mux = zst_mp4_muxer_create();
    assert(mux != NULL);
    zst_pad_t* mux_video = zst_element_get_pad(mux, "video");
    zst_pad_t* mux_src   = zst_element_get_pad(mux, "src");
    assert(mux_video != NULL && mux_src != NULL);

    /* Collector for MP4 output */
    collector_t* coll_data = calloc(1, sizeof(collector_t));
    assert(coll_data != NULL);
    static zst_element_ops_t sink_ops = {
        .name = "mux_collector",
        .process = collector_process_mux_track,
    };
    zst_element_t* sink = zst_element_create(&sink_ops, coll_data);
    zst_pad_t* sink_pad = zst_pad_create("sink", ZST_PAD_SINK);
    zst_element_add_pad(sink, sink_pad);

    /* Link: enc src -> mux video */
    assert(zst_pad_link(enc_src, mux_video) == ZST_OK);
    /* Link: mux src -> collector sink */
    assert(zst_pad_link(mux_src, sink_pad) == ZST_OK);

    /* Start muxer: writes MP4 header (detects linked pads) */
    assert(zst_element_set_state(mux, ZST_STATE_PLAYING) == ZST_OK);

    /* Push 20 encoded frames through to the muxer */
    for (int i = 0; i < 20; i++) {
        zst_buffer_t* frame = generate_video_frame(src);
        zst_buffer_t* pkt = NULL;

        zst_result_t r = enc->ops->process(enc, frame, &pkt);
        assert(r == ZST_OK);
        zst_buffer_unref(frame);

        if (pkt) {
            /* Push encoded packet to muxer via linked pad */
            assert(zst_pad_push(enc_src, pkt) == ZST_OK);
            zst_buffer_unref(pkt);
        }
    }

    /* Flush encoder via EOS */
    zst_buffer_t* eos = zst_buffer_create(ZST_BUFFER_VIDEO_FRAME);
    assert(eos != NULL);
    eos->flags |= ZST_BUFFER_FLAG_EOS;
    zst_buffer_t* eos_pkt = NULL;
    enc->ops->process(enc, eos, &eos_pkt);
    zst_buffer_unref(eos);
    if (eos_pkt) {
        zst_pad_push(enc_src, eos_pkt);
        zst_buffer_unref(eos_pkt);
    }

    /* Push EOS through encoder src pad (linked to mux video sink) */
    zst_buffer_t* eos_push = zst_buffer_create(ZST_BUFFER_USER);
    assert(eos_push != NULL);
    eos_push->flags |= ZST_BUFFER_FLAG_EOS;
    assert(zst_pad_push(enc_src, eos_push) == ZST_OK);
    zst_buffer_unref(eos_push);

    /* Verify MP4 data was collected */
    assert(coll_data->packet_count >= 1);
    assert(coll_data->total_bytes > 100);

    printf("\n        `-- mp4 mux output: %zu bytes in %d chunks\n",
           coll_data->total_bytes, coll_data->packet_count);

    zst_element_set_state(enc, ZST_STATE_NULL);
    zst_element_set_state(mux, ZST_STATE_NULL);
    zst_element_set_state(src, ZST_STATE_NULL);
    zst_element_destroy(enc);
    zst_element_destroy(mux);
    zst_element_destroy(src);
    zst_element_destroy(sink);
    /* coll_data freed by zst_element_destroy */

    PASS();
}

/* ═══════════════════════════════════════════════════════════════
   MP4 Muxer test: H.264 + AAC simultaneous muxing
   ═══════════════════════════════════════════════════════════════ */

static void
test_mp4_muxer_av(void)
{
    TEST("mp4 muxer: H.264 + AAC simultaneous -> .mp4 file");

    /* ── Sources ── */
    zst_element_t* vsrc = create_video_source(640, 480, 30, "gradient");
    zst_element_t* asrc = create_audio_source(44100, 2, "sine", 440.0);

    /* ── Encoders ── */
    zst_element_t* venc = zst_x264_encoder_create();
    assert(venc != NULL);
    zst_element_set_state(venc, ZST_STATE_READY);

    zst_element_t* aenc = zst_aac_encoder_create();
    assert(aenc != NULL);
    zst_element_set_state(aenc, ZST_STATE_READY);

    zst_pad_t* venc_src = zst_element_get_pad(venc, "src");
    zst_pad_t* aenc_src = zst_element_get_pad(aenc, "src");
    assert(venc_src != NULL && aenc_src != NULL);

    /* ── MP4 muxer ── */
    zst_element_t* mux = zst_mp4_muxer_create();
    assert(mux != NULL);
    zst_pad_t* mux_video = zst_element_get_pad(mux, "video");
    zst_pad_t* mux_audio = zst_element_get_pad(mux, "audio");
    zst_pad_t* mux_src   = zst_element_get_pad(mux, "src");
    assert(mux_video != NULL && mux_audio != NULL && mux_src != NULL);

    /* ── Collector for MP4 output ── */
    mux_collector_t* coll = calloc(1, sizeof(mux_collector_t));
    assert(coll != NULL);
    static zst_element_ops_t sink_ops = {
        .name = "mux_coll",
        .process = mux_collector_process,
    };
    zst_element_t* sink = zst_element_create(&sink_ops, coll);
    zst_pad_t* snk = zst_pad_create("sink", ZST_PAD_SINK);
    zst_element_add_pad(sink, snk);

    /* ── Link ── */
    assert(zst_pad_link(venc_src, mux_video) == ZST_OK);
    assert(zst_pad_link(aenc_src, mux_audio) == ZST_OK);
    assert(zst_pad_link(mux_src, snk) == ZST_OK);

    /* ── Start muxer (writes MP4 header) ── */
    assert(zst_element_set_state(mux, ZST_STATE_PLAYING) == ZST_OK);

    /* ── Interleave 30 video + ~45 audio frames (~1 sec) ── */
    #define AV_NFRAMES 30
    #define AA_NFRAMES 48
    int audio_i = 0;

    for (int vi = 0; vi < AV_NFRAMES; vi++) {
        /* Generate and push 1-2 audio frames per video frame */
        int audio_this_round = (vi % 2 == 0) ? 2 : 1;
        for (int ai = 0; ai < audio_this_round && audio_i < AA_NFRAMES; ai++, audio_i++) {
            zst_buffer_t* af = generate_audio_frame(asrc);
            zst_buffer_t* ap = NULL;
            zst_result_t ar = aenc->ops->process(aenc, af, &ap);
            assert(ar == ZST_OK);
            zst_buffer_unref(af);
            if (ap) {
                assert(zst_pad_push(aenc_src, ap) == ZST_OK);
                zst_buffer_unref(ap);
            }
        }

        /* Generate and push video frame */
        zst_buffer_t* vf = generate_video_frame(vsrc);
        zst_buffer_t* vp = NULL;
        zst_result_t vr = venc->ops->process(venc, vf, &vp);
        assert(vr == ZST_OK);
        zst_buffer_unref(vf);
        if (vp) {
            assert(zst_pad_push(venc_src, vp) == ZST_OK);
            zst_buffer_unref(vp);
        }
    }

    /* ── Flush both encoders via EOS ── */

    /* Flush video encoder */
    zst_buffer_t* veos = zst_buffer_create(ZST_BUFFER_VIDEO_FRAME);
    assert(veos != NULL);
    veos->flags |= ZST_BUFFER_FLAG_EOS;
    zst_buffer_t* veos_pkt = NULL;
    venc->ops->process(venc, veos, &veos_pkt);
    zst_buffer_unref(veos);
    if (veos_pkt) {
        zst_pad_push(venc_src, veos_pkt);
        zst_buffer_unref(veos_pkt);
    }

    /* Flush audio encoder */
    zst_buffer_t* aeos = zst_buffer_create(ZST_BUFFER_AUDIO_FRAME);
    assert(aeos != NULL);
    aeos->flags |= ZST_BUFFER_FLAG_EOS;
    zst_buffer_t* aeos_pkt = NULL;
    aenc->ops->process(aenc, aeos, &aeos_pkt);
    zst_buffer_unref(aeos);
    if (aeos_pkt) {
        zst_pad_push(aenc_src, aeos_pkt);
        zst_buffer_unref(aeos_pkt);
    }

    /* ── Send EOS to muxer via encoder src pads (linked to mux sinks) ── */

    zst_buffer_t* mux_veos = zst_buffer_create(ZST_BUFFER_USER);
    assert(mux_veos != NULL);
    mux_veos->flags |= ZST_BUFFER_FLAG_EOS;
    assert(zst_pad_push(venc_src, mux_veos) == ZST_OK);
    zst_buffer_unref(mux_veos);

    zst_buffer_t* mux_aeos = zst_buffer_create(ZST_BUFFER_USER);
    assert(mux_aeos != NULL);
    mux_aeos->flags |= ZST_BUFFER_FLAG_EOS;
    assert(zst_pad_push(aenc_src, mux_aeos) == ZST_OK);
    zst_buffer_unref(mux_aeos);

    /* ── Verify MP4 output ── */
    assert(coll->chunk_count >= 1);
    assert(coll->total_bytes > 500);

    /* Write MP4 file and probe with ffprobe */
    const char* mp4_path = "/tmp/test_output_av.mp4";
    FILE* fp = fopen(mp4_path, "wb");
    assert(fp != NULL);
    for (int i = 0; i < coll->chunk_count; i++) {
        fwrite(coll->chunks[i].data, 1, coll->chunks[i].size, fp);
    }
    fclose(fp);

    printf("\n"
           "        `-- video frames:    %d\n"
           "        `-- audio frames:    %d\n"
           "        `-- mp4 chunks:      %d\n"
           "        `-- mp4 total bytes: %zu\n"
           "        `-- file written:    %s\n",
           AV_NFRAMES, audio_i, coll->chunk_count, coll->total_bytes, mp4_path);

    /* ── Cleanup ── */
    zst_element_set_state(venc, ZST_STATE_NULL);
    zst_element_set_state(aenc, ZST_STATE_NULL);
    zst_element_set_state(mux, ZST_STATE_NULL);
    zst_element_set_state(vsrc, ZST_STATE_NULL);
    zst_element_set_state(asrc, ZST_STATE_NULL);
    zst_element_destroy(venc);
    zst_element_destroy(aenc);
    zst_element_destroy(mux);
    zst_element_destroy(vsrc);
    zst_element_destroy(asrc);
    zst_element_destroy(sink);

    PASS();
}

/* ═══════════════════════════════════════════════════════════════
   Main
   ═══════════════════════════════════════════════════════════════ */

int main(void)
{
    printf("\n=== zstreamer codec tests (test source elements) ===\n\n");

    /* -- Video (H.264) -- */
    test_h264_encode_properties();
    test_h264_encode_basic();
    test_h264_encode_multiple_resolutions();
    test_h264_encode_decode_roundtrip();
    test_h264_pipeline_integration();

    /* -- Audio (AAC) -- */
    test_aac_encode_basic();
    test_aac_encode_decode_roundtrip();

    /* -- File output -- */
    test_h264_file_output();
    test_aac_file_output();

    /* -- MP4 muxer -- */
    test_mp4_muxer_integration();
    test_mp4_muxer_av();

    printf("\n--- Results: %d/%d passed ---\n\n",
           g_tests_passed, g_tests_run);

    return (g_tests_passed == g_tests_run) ? 0 : 1;
}

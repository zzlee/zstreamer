/* Offline regression for FFmpeg AAC SIMD correctness. CPU flags are global,
 * so run serially in this dedicated process, never in a live application. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavcodec/avcodec.h>
#include <libavutil/cpu.h>
#include <libavutil/samplefmt.h>
#include "zst_buffer.h"
#include "zst_element.h"
#include "zst_pad.h"
#include "zstreamer/elements/zst_aac_decoder.h"
#include "zstreamer/elements/zst_aac_encoder.h"
#include "zstreamer/elements/zst_fake_sink.h"

#define INPUT_FRAMES 32
#define FRAME_SAMPLES 1024
#define SIMD_RMSE_LIMIT 0.00001
#define SIMD_PEAK_LIMIT 0.0001
#define FIXED_RMSE_LIMIT 0.0001
#define FIXED_PEAK_LIMIT 0.001
#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
    exit(2); } } while (0)

typedef struct { uint8_t *data; size_t size; } packet_t;
typedef struct {
    float *samples;
    size_t count;
    unsigned frames, rate, channels;
} pcm_t;

static zst_pad_probe_return_t
capture_pcm(zst_pad_t *pad, zst_buffer_t *buf, zst_pad_probe_type_t type, void *opaque)
{
    (void)pad; (void)type;
    pcm_t *pcm = opaque;
    const zst_audio_frame_t *frame = buf->payload;
    CHECK(frame && frame->data && frame->nb_samples);
    CHECK(frame->sample_rate == pcm->rate && frame->channels == pcm->channels);
    /* aacdec exposes AVSampleFormat, not ZST_AUDIO_FMT values. */
    CHECK(frame->format == AV_SAMPLE_FMT_FLTP || frame->format == AV_SAMPLE_FMT_S32P);
    size_t count = (size_t)frame->nb_samples * frame->channels;
    float *samples = realloc(pcm->samples, (pcm->count + count) * sizeof(float));
    CHECK(samples);
    pcm->samples = samples;
    for (unsigned i = 0; i < frame->nb_samples; ++i) {
        for (unsigned c = 0; c < frame->channels; ++c) {
            float sample = frame->format == AV_SAMPLE_FMT_FLTP
                ? ((float *const *)frame->data)[c][i]
                : (float)(((int32_t *const *)frame->data)[c][i] / 2147483648.0);
            CHECK(isfinite(sample));
            pcm->samples[pcm->count++] = sample;
        }
    }
    pcm->frames++;
    return ZST_PAD_PROBE_OK;
}

static unsigned
make_packets(packet_t *packets, unsigned rate, unsigned channels)
{
    /* Scalar encoding produces one shared bitstream for all three decoders.
     * Different channel tones reveal planar/interleaved/channel mistakes. */
    av_force_cpu_flags(0);
    zst_element_t *enc = zst_aac_encoder_create();
    CHECK(enc && zst_element_set_state(enc, ZST_STATE_READY) == ZST_OK);
    unsigned count = 0;
    for (unsigned n = 0; n < INPUT_FRAMES; ++n) {
        int16_t data[FRAME_SAMPLES * 2];
        for (unsigned i = 0; i < FRAME_SAMPLES; ++i) {
            double t = ((double)n * FRAME_SAMPLES + i) / rate;
            for (unsigned c = 0; c < channels; ++c) {
                double hz = c ? 1409.0 : 997.0;
                double signal = 0.35 * sin(6.283185307179586 * hz * t)
                              + 0.1 * sin(6.283185307179586 * (hz * 2.7) * t);
                data[i * channels + c] = (int16_t)(signal * 32767.0);
            }
        }
        zst_audio_frame_t frame = {0};
        frame.sample_rate = rate; frame.channels = channels;
        frame.format = ZST_AUDIO_FMT_S16LE;
        frame.nb_samples = FRAME_SAMPLES; frame.data = data;
        zst_buffer_t *input = zst_buffer_create(ZST_BUFFER_AUDIO_FRAME);
        CHECK(input);
        input->payload = &frame;
        input->memory.data = data;
        input->memory.size = FRAME_SAMPLES * channels * sizeof(int16_t);
        input->pts = (uint64_t)n * FRAME_SAMPLES * 1000000000ULL / rate;
        zst_buffer_t *output = NULL;
        CHECK(enc->ops->process(enc, input, &output) == ZST_OK);
        input->payload = NULL;
        zst_buffer_unref(input);
        if (!output) continue; /* Encoder priming delay. */
        CHECK(count < INPUT_FRAMES && output->memory.size > 0);
        packet_t *p = &packets[count++];
        p->size = output->memory.size + 7;
        CHECK(p->size < 8192);
        p->data = malloc(p->size);
        CHECK(p->data);
        unsigned frequency_index = rate == 48000 ? 3 : 4;
        p->data[0] = 0xff; p->data[1] = 0xf1;
        p->data[2] = (1 << 6) | (frequency_index << 2);
        p->data[3] = (channels << 6) | (p->size >> 11);
        p->data[4] = p->size >> 3;
        p->data[5] = ((p->size & 7) << 5) | 0x1f;
        p->data[6] = 0xfc;
        memcpy(p->data + 7, output->memory.data, output->memory.size);
        zst_buffer_unref(output);
    }
    zst_element_destroy(enc);
    CHECK(count >= INPUT_FRAMES - 3);
    return count;
}

static pcm_t
decode(const packet_t *packets, unsigned count, unsigned rate, unsigned channels,
       const char *backend, int cpu_flags)
{
    av_force_cpu_flags(cpu_flags);
    pcm_t pcm = {.rate = rate, .channels = channels};
    zst_element_t *dec = zst_aac_decoder_create();
    zst_element_t *sink = zst_fake_sink_create();
    CHECK(dec && sink);
    CHECK(zst_element_set_property_string(dec, "decoder", backend) == ZST_OK);
    zst_pad_t *src = zst_element_get_pad(dec, "src");
    zst_pad_t *input = zst_element_get_pad(dec, "sink");
    CHECK(zst_pad_link(src, zst_element_get_pad(sink, "sink")) == ZST_OK);
    CHECK(zst_pad_add_probe(src, ZST_PAD_PROBE_PRE_BUFFER, capture_pcm, &pcm));
    CHECK(zst_element_set_state(dec, ZST_STATE_PLAYING) == ZST_OK);
    for (unsigned i = 0; i < count; ++i) {
        zst_buffer_t *buf = zst_buffer_create(ZST_BUFFER_AUDIO_PACKET);
        CHECK(buf);
        buf->memory.data = packets[i].data; buf->memory.size = packets[i].size;
        CHECK(input->push(input, buf) == ZST_OK);
        zst_buffer_unref(buf);
    }
    zst_element_destroy(dec);
    zst_element_destroy(sink);
    CHECK(pcm.frames == count && pcm.count == (size_t)count * FRAME_SAMPLES * channels);
    return pcm;
}

static int
compare(const char *name, const pcm_t *reference, const pcm_t *actual,
        double rmse_limit, double peak_limit)
{
    CHECK(reference->count == actual->count && reference->count > 0);
    double error = 0, peak = 0, energy = 0;
    for (size_t i = 0; i < reference->count; ++i) {
        double delta = actual->samples[i] - reference->samples[i];
        error += delta * delta;
        if (fabs(delta) > peak) peak = fabs(delta);
        energy += (double)reference->samples[i] * reference->samples[i];
    }
    CHECK(sqrt(energy / reference->count) > 0.05); /* Matching silence is not a pass. */
    double rmse = sqrt(error / reference->count);
    int pass = rmse <= rmse_limit && peak <= peak_limit;
    printf("%s: frames=%u RMSE=%.9g peak_error=%.9g %s\n",
           name, actual->frames, rmse, peak, pass ? "PASS" : "FAIL");
    return pass;
}

int main(void)
{
    int saved_flags = av_get_cpu_flags();
    av_force_cpu_flags(-1);
    int auto_flags = av_get_cpu_flags();
    unsigned codec_version = avcodec_version(), util_version = avutil_version();
    printf("FFmpeg avcodec=%u.%u.%u avutil=%u.%u.%u auto CPU flags=0x%x\n",
           codec_version >> 16, (codec_version >> 8) & 255, codec_version & 255,
           util_version >> 16, (util_version >> 8) & 255, util_version & 255, auto_flags);
    int pass = 1;
    const unsigned rates[] = {48000, 44100};
    for (size_t r = 0; r < sizeof(rates) / sizeof(rates[0]); ++r) {
        packet_t packets[INPUT_FRAMES] = {{0}};
        unsigned count = make_packets(packets, rates[r], 2);
        printf("AAC LC %u Hz stereo, %u identical packets\n", rates[r], count);
        pcm_t scalar = decode(packets, count, rates[r], 2, "aac", 0);
        pcm_t simd = decode(packets, count, rates[r], 2, "aac", auto_flags);
        pcm_t fixed = decode(packets, count, rates[r], 2, "aac_fixed", auto_flags);
        pass &= compare("aac auto vs scalar", &scalar, &simd, SIMD_RMSE_LIMIT, SIMD_PEAK_LIMIT);
        pass &= compare("aac_fixed vs scalar", &scalar, &fixed, FIXED_RMSE_LIMIT, FIXED_PEAK_LIMIT);
#if defined(__aarch64__) || defined(__arm__)
        if (auto_flags & AV_CPU_FLAG_NEON) {
            pcm_t no_neon = decode(packets, count, rates[r], 2, "aac", auto_flags & ~AV_CPU_FLAG_NEON);
            pass &= compare("aac no-NEON vs scalar", &scalar, &no_neon, SIMD_RMSE_LIMIT, SIMD_PEAK_LIMIT);
            free(no_neon.samples);
        }
#endif
        free(scalar.samples); free(simd.samples); free(fixed.samples);
        for (unsigned i = 0; i < count; ++i) free(packets[i].data);
    }
    av_force_cpu_flags(saved_flags);
    if (!pass) fprintf(stderr, "AAC regression detected. Passing aac_fixed does not hide SIMD failure.\n");
    return pass ? 0 : 1;
}

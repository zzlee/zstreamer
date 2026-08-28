#define _POSIX_C_SOURCE 200809L

#include "dante_dep_abi.h"
#include "zstreamer/elements/zst_dante_dep_audio.h"

#include "zst_buffer.h"
#include "zst_element.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    char base[96];
    char property_name[96];
    char semaphore_name[96];
    char tx_name[100];
    char rx_name[100];
    int control_fd;
    int tx_fd;
    int rx_fd;
    uint8_t* control;
    uint8_t* tx;
    uint8_t* rx;
    size_t control_size;
    size_t tx_size;
    size_t rx_size;
    dep_shared_header_t* header;
    sem_t* semaphore;
    uint32_t capacity;
    uint32_t period_samples;
    uint32_t channels;
} fixture_t;

static uint64_t monotonic_ns(void)
{
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

static void sleep_ms(unsigned milliseconds)
{
    struct timespec delay = {
        .tv_sec = milliseconds / 1000,
        .tv_nsec = (long)(milliseconds % 1000) * 1000000L
    };
    nanosleep(&delay, NULL);
}

static uint8_t* create_mapping(const char* name, size_t length, int* fd_out)
{
    int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    assert(fd >= 0);
    assert(ftruncate(fd, (off_t)length) == 0);
    uint8_t* address = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    assert(address != MAP_FAILED);
    memset(address, 0, length);
    *fd_out = fd;
    return address;
}

static void fixture_create(fixture_t* fixture, bool separate,
                           uint32_t capacity, uint32_t period_samples,
                           uint32_t channels)
{
    static unsigned serial;
    size_t metadata = sizeof(dep_shared_header_t) + sizeof(dep_timing_descriptor_t);
    size_t channel_bytes = (size_t)capacity * sizeof(int32_t);
    memset(fixture, 0, sizeof(*fixture));
    fixture->control_fd = fixture->tx_fd = fixture->rx_fd = -1;
    fixture->capacity = capacity;
    fixture->period_samples = period_samples;
    fixture->channels = channels;
    snprintf(fixture->base, sizeof(fixture->base), "/zst_dep_%ld_%u",
             (long)getpid(), serial++);
    snprintf(fixture->property_name, sizeof(fixture->property_name), "%s",
             fixture->base + 1);
    snprintf(fixture->semaphore_name, sizeof(fixture->semaphore_name),
             "/zst_dep_tick_%ld_%u", (long)getpid(), serial++);
    snprintf(fixture->tx_name, sizeof(fixture->tx_name), "%sTx", fixture->base);
    snprintf(fixture->rx_name, sizeof(fixture->rx_name), "%sRx", fixture->base);

    uint32_t tx_offset = separate ? 16u : (uint32_t)metadata;
    uint32_t rx_offset = separate ? 24u
        : tx_offset + (uint32_t)(channels * channel_bytes);
    fixture->control_size = separate ? metadata
        : rx_offset + channels * channel_bytes;
    fixture->control = create_mapping(fixture->base, fixture->control_size,
                                      &fixture->control_fd);
    if (separate) {
        fixture->tx_size = tx_offset + channels * channel_bytes;
        fixture->rx_size = rx_offset + channels * channel_bytes;
        fixture->tx = create_mapping(fixture->tx_name, fixture->tx_size,
                                     &fixture->tx_fd);
        fixture->rx = create_mapping(fixture->rx_name, fixture->rx_size,
                                     &fixture->rx_fd);
    } else {
        fixture->tx = fixture->control;
        fixture->rx = fixture->control;
        fixture->tx_size = fixture->rx_size = fixture->control_size;
    }

    fixture->header = (dep_shared_header_t*)fixture->control;
    fixture->header->object_bytes = (uint32_t)fixture->control_size;
    fixture->header->metadata_bytes = (uint32_t)metadata;
    fixture->header->flags = separate ? DEP_LAYOUT_SEPARATE : 0;
    fixture->header->tx_offset = tx_offset;
    fixture->header->rx_offset = rx_offset;
    fixture->header->timing_offset = sizeof(dep_shared_header_t);
    fixture->header->sample_rate = 48000;
    fixture->header->encoding = DEP_ENCODING_PCM32;
    fixture->header->samples_per_channel = capacity;
    fixture->header->bytes_per_channel = capacity * sizeof(int32_t);
    fixture->header->tx_channels = channels;
    fixture->header->rx_channels = channels;
    fixture->header->samples_per_period = period_samples;
    dep_timing_descriptor_t* timing = (dep_timing_descriptor_t*)(
        fixture->control + fixture->header->timing_offset);
    timing->descriptor_bytes = sizeof(*timing);
    timing->kind = DEP_TIMING_SIGNAL_EVENT;
    snprintf(timing->name, sizeof(timing->name), "%s", fixture->semaphore_name);
    fixture->semaphore = sem_open(fixture->semaphore_name,
                                  O_CREAT | O_EXCL, 0600, 0);
    assert(fixture->semaphore != SEM_FAILED);
    __atomic_store_n(&fixture->header->magic, DEP_HEADER_MAGIC, __ATOMIC_RELEASE);
}

static void fixture_destroy(fixture_t* fixture)
{
    if (fixture->semaphore != SEM_FAILED) sem_close(fixture->semaphore);
    sem_unlink(fixture->semaphore_name);
    if (fixture->tx != fixture->control && fixture->tx)
        munmap(fixture->tx, fixture->tx_size);
    if (fixture->rx != fixture->control && fixture->rx)
        munmap(fixture->rx, fixture->rx_size);
    if (fixture->control) munmap(fixture->control, fixture->control_size);
    if (fixture->tx_fd >= 0) close(fixture->tx_fd);
    if (fixture->rx_fd >= 0) close(fixture->rx_fd);
    if (fixture->control_fd >= 0) close(fixture->control_fd);
    shm_unlink(fixture->tx_name);
    shm_unlink(fixture->rx_name);
    shm_unlink(fixture->base);
}

static int32_t* fixture_plane(fixture_t* fixture, bool transmit, uint32_t channel)
{
    uint8_t* object = transmit ? fixture->tx : fixture->rx;
    uint32_t offset = transmit ? fixture->header->tx_offset : fixture->header->rx_offset;
    return (int32_t*)(object + offset +
        (size_t)channel * fixture->header->bytes_per_channel);
}

static int32_t sample_value(uint64_t period, uint32_t channel, uint32_t sample)
{
    return (int32_t)(period * 1000 + channel * 100 + sample);
}

static void fixture_fill_rx(fixture_t* fixture, uint64_t period)
{
    uint32_t start = (uint32_t)((period * fixture->period_samples) % fixture->capacity);
    for (uint32_t channel = 0; channel < fixture->channels; ++channel) {
        int32_t* plane = fixture_plane(fixture, false, channel);
        for (uint32_t i = 0; i < fixture->period_samples; ++i)
            plane[(start + i) % fixture->capacity] = sample_value(period, channel, i);
    }
}

static void fixture_publish(fixture_t* fixture, uint64_t period)
{
    fixture_fill_rx(fixture, period);
    __atomic_store_n(&fixture->header->monotonic_ns,
                     UINT64_C(5000000000) +
                         (period + 1) * fixture->period_samples * UINT64_C(1000000000) /
                             fixture->header->sample_rate,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&fixture->header->period_count, period + 1, __ATOMIC_RELEASE);
    assert(sem_post(fixture->semaphore) == 0);
}

static void fixture_remap_control(fixture_t* fixture)
{
    size_t metadata = sizeof(dep_shared_header_t) + sizeof(dep_timing_descriptor_t);
    assert(fixture->tx == fixture->control && fixture->rx == fixture->control);
    munmap(fixture->control, fixture->control_size);
    close(fixture->control_fd);
    assert(shm_unlink(fixture->base) == 0);
    fixture->control = create_mapping(fixture->base, fixture->control_size,
                                      &fixture->control_fd);
    fixture->tx = fixture->rx = fixture->control;
    fixture->header = (dep_shared_header_t*)fixture->control;
    fixture->header->object_bytes = (uint32_t)fixture->control_size;
    fixture->header->metadata_bytes = (uint32_t)metadata;
    fixture->header->tx_offset = (uint32_t)metadata;
    fixture->header->rx_offset = fixture->header->tx_offset +
        fixture->channels * fixture->capacity * sizeof(int32_t);
    fixture->header->timing_offset = sizeof(dep_shared_header_t);
    fixture->header->sample_rate = 48000;
    fixture->header->encoding = DEP_ENCODING_PCM32;
    fixture->header->samples_per_channel = fixture->capacity;
    fixture->header->bytes_per_channel = fixture->capacity * sizeof(int32_t);
    fixture->header->tx_channels = fixture->header->rx_channels = fixture->channels;
    fixture->header->samples_per_period = fixture->period_samples;
    dep_timing_descriptor_t* timing = (dep_timing_descriptor_t*)(fixture->control +
        fixture->header->timing_offset);
    timing->descriptor_bytes = sizeof(*timing);
    timing->kind = DEP_TIMING_SIGNAL_EVENT;
    snprintf(timing->name, sizeof(timing->name), "%s", fixture->semaphore_name);
    __atomic_store_n(&fixture->header->magic, DEP_HEADER_MAGIC, __ATOMIC_RELEASE);
}

static uint64_t property_uint(zst_element_t* element, const char* name)
{
    char text[64];
    char* end;
    assert(zst_element_get_property(element, name, text, sizeof(text)) == ZST_OK);
    errno = 0;
    uint64_t value = strtoull(text, &end, 10);
    assert(errno == 0 && *end == '\0');
    return value;
}

static bool property_bool(zst_element_t* element, const char* name)
{
    char text[16];
    assert(zst_element_get_property(element, name, text, sizeof(text)) == ZST_OK);
    return strcmp(text, "true") == 0;
}

static zst_buffer_t* receive_audio(zst_element_t* source)
{
    for (unsigned i = 0; i < 20; ++i) {
        zst_buffer_t* output = NULL;
        zst_result_t result = source->ops->process(source, NULL, &output);
        if (result == ZST_OK) return output;
        assert(result == ZST_TIMEOUT);
    }
    return NULL;
}

static void expect_period(zst_buffer_t* buffer, uint64_t period,
                          uint32_t first_channel, uint32_t second_channel,
                          uint32_t samples)
{
    assert(buffer);
    zst_audio_frame_t* audio = buffer->payload;
    assert(audio && audio->format == 1 && audio->channels == 2);
    assert(audio->sample_rate == 48000 && audio->nb_samples == samples);
    assert(buffer->duration == (uint64_t)samples * UINT64_C(1000000000) / 48000);
    int32_t* pcm = audio->data;
    for (uint32_t i = 0; i < samples; ++i) {
        assert(pcm[i * 2] == sample_value(period, first_channel, i));
        assert(pcm[i * 2 + 1] == sample_value(period, second_channel, i));
    }
}

static void configure_pair(fixture_t* fixture, zst_element_t* source,
                           zst_element_t* sink)
{
    assert(zst_element_set_property(source, "shm-name", fixture->property_name) == ZST_OK);
    assert(zst_element_set_property(source, "channels", "2,0") == ZST_OK);
    assert(zst_element_set_property(source, "queue-periods", "4") == ZST_OK);
    assert(zst_element_set_property(source, "block-samples", "4") == ZST_OK);
    assert(zst_element_set_property(source, "reconnect-interval-ms", "5") == ZST_OK);
    assert(zst_element_set_property(sink, "shm-name", fixture->base) == ZST_OK);
    assert(zst_element_set_property(sink, "channels", "1,0") == ZST_OK);
    assert(zst_element_set_property(sink, "queue-periods", "4") == ZST_OK);
    assert(zst_element_set_property(sink, "tx-lead-us", "0") == ZST_OK);
    assert(zst_element_set_property(sink, "reconnect-interval-ms", "5") == ZST_OK);
}

static void test_contiguous_transport_and_reset(void)
{
    fixture_t fixture;
    fixture_create(&fixture, false, 10, 4, 3);
    zst_element_t* source = zst_dante_dep_audio_source_create();
    zst_element_t* sink = zst_dante_dep_audio_sink_create();
    assert(source && sink);
    assert(strcmp(source->ops->name, "dantedepaudiosrc") == 0);
    assert(strcmp(sink->ops->name, "dantedepaudiosink") == 0);
    configure_pair(&fixture, source, sink);
    assert(zst_element_set_property(source, "channels", "0,0") == ZST_ERROR);
    assert(zst_element_set_state(source, ZST_STATE_PLAYING) == ZST_OK);
    assert(zst_element_set_state(sink, ZST_STATE_PLAYING) == ZST_OK);

    for (unsigned i = 0; i < 100 &&
         (!property_bool(source, "active") || !property_bool(sink, "active")); ++i)
        sleep_ms(2);
    assert(property_bool(source, "active") && property_bool(sink, "active"));

    fixture_publish(&fixture, 0);
    zst_buffer_t* received = receive_audio(source);
    expect_period(received, 0, 2, 0, 4);
    assert(received->pts == 0);
    zst_buffer_unref(received);
    for (uint32_t channel = 0; channel < 2; ++channel) {
        int32_t* plane = fixture_plane(&fixture, true, channel);
        for (uint32_t i = 0; i < 4; ++i) assert(plane[i] == 0);
    }
    assert(property_uint(sink, "underflows") == 1);

    fixture_publish(&fixture, 1);
    received = receive_audio(source);
    expect_period(received, 1, 2, 0, 4);
    zst_buffer_unref(received);
    fixture_publish(&fixture, 2);
    received = receive_audio(source);
    expect_period(received, 2, 2, 0, 4);
    zst_buffer_unref(received);

    fixture_fill_rx(&fixture, 4);
    fixture_fill_rx(&fixture, 5);
    __atomic_store_n(&fixture.header->period_count, 6, __ATOMIC_RELEASE);
    assert(sem_post(fixture.semaphore) == 0);
    received = receive_audio(source);
    expect_period(received, 4, 2, 0, 4);
    zst_buffer_unref(received);
    received = receive_audio(source);
    expect_period(received, 5, 2, 0, 4);
    zst_buffer_unref(received);
    assert(property_uint(source, "overruns") >= 1);

    __atomic_store_n(&fixture.header->reset_serial, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&fixture.header->magic, 0, __ATOMIC_RELEASE);
    assert(sem_post(fixture.semaphore) == 0);
    for (unsigned i = 0; i < 100 && property_uint(source, "resets") == 0; ++i)
        sleep_ms(2);
    assert(property_uint(source, "resets") >= 1);
    fixture_remap_control(&fixture);
    for (unsigned i = 0; i < 100 && !property_bool(source, "active"); ++i)
        sleep_ms(2);
    fixture_publish(&fixture, 0);
    received = receive_audio(source);
    expect_period(received, 0, 2, 0, 4);
    zst_buffer_unref(received);

    assert(property_uint(source, "sample-rate") == 48000);
    assert(zst_element_set_state(source, ZST_STATE_NULL) == ZST_OK);
    fixture_publish(&fixture, 101);
    for (unsigned i = 0; i < 50 && property_uint(sink, "periods") < 5; ++i)
        sleep_ms(2);
    uint64_t before = monotonic_ns();
    assert(zst_element_set_state(sink, ZST_STATE_NULL) == ZST_OK);
    assert(monotonic_ns() - before < UINT64_C(500000000));
    zst_element_destroy(source);
    zst_element_destroy(sink);
    fixture_destroy(&fixture);
}

static void test_tx_transport(void)
{
    fixture_t fixture;
    fixture_create(&fixture, false, 8, 4, 3);
    zst_element_t* sink = zst_dante_dep_audio_sink_create();
    assert(sink);
    assert(zst_element_set_property(sink, "shm-name", fixture.base) == ZST_OK);
    assert(zst_element_set_property(sink, "channels", "1,0") == ZST_OK);
    assert(zst_element_set_property(sink, "tx-lead-us", "0") == ZST_OK);
    assert(zst_element_set_state(sink, ZST_STATE_PLAYING) == ZST_OK);
    for (unsigned i = 0; i < 100 && !property_bool(sink, "active"); ++i) sleep_ms(2);
    assert(property_bool(sink, "active"));
    int32_t outgoing[] = { 11, 21, 12, 22, 13, 23, 14, 24 };
    zst_buffer_t* input = zst_buffer_create(ZST_BUFFER_AUDIO_FRAME);
    zst_audio_frame_t audio = {
        .sample_rate = 48000, .channels = 2, .format = 1,
        .nb_samples = 4, .data = outgoing
    };
    input->payload = &audio;
    input->memory.data = outgoing;
    input->memory.size = sizeof(outgoing);
    assert(sink->ops->process(sink, input, NULL) == ZST_OK);
    fixture_publish(&fixture, 0);
    for (unsigned i = 0; i < 100 && property_uint(sink, "periods") == 0; ++i) sleep_ms(2);
    int32_t* tx1 = fixture_plane(&fixture, true, 1);
    int32_t* tx0 = fixture_plane(&fixture, true, 0);
    for (uint32_t i = 0; i < 4; ++i) {
        assert(tx1[i] == outgoing[i * 2]);
        assert(tx0[i] == outgoing[i * 2 + 1]);
    }
    zst_buffer_unref(input);
    assert(zst_element_set_state(sink, ZST_STATE_NULL) == ZST_OK);
    zst_element_destroy(sink);
    fixture_destroy(&fixture);
}

static void test_separate_objects(void)
{
    fixture_t fixture;
    fixture_create(&fixture, true, 8, 4, 3);
    zst_element_t* source = zst_dante_dep_audio_source_create();
    zst_element_t* sink = zst_dante_dep_audio_sink_create();
    assert(source && sink);
    configure_pair(&fixture, source, sink);
    assert(zst_element_set_property(source, "block-samples", "8") == ZST_OK);
    assert(zst_element_set_state(source, ZST_STATE_PLAYING) == ZST_OK);
    assert(zst_element_set_state(sink, ZST_STATE_PLAYING) == ZST_OK);
    for (unsigned i = 0; i < 100 &&
         (!property_bool(source, "active") || !property_bool(sink, "active")); ++i)
        sleep_ms(2);
    assert(property_bool(source, "active") && property_bool(sink, "active"));
    fixture_publish(&fixture, 0);
    fixture_publish(&fixture, 1);
    zst_buffer_t* received = receive_audio(source);
    assert(received);
    zst_audio_frame_t* audio = received->payload;
    assert(audio && audio->nb_samples == 8 && audio->channels == 2);
    int32_t* pcm = audio->data;
    for (uint32_t i = 0; i < 8; ++i) {
        uint64_t period = i / 4;
        uint32_t sample = i % 4;
        assert(pcm[i * 2] == sample_value(period, 2, sample));
        assert(pcm[i * 2 + 1] == sample_value(period, 0, sample));
    }
    assert(received->duration == UINT64_C(8000000000) / 48000);
    zst_buffer_unref(received);
    assert(property_bool(source, "active"));
    assert(property_bool(sink, "active"));
    assert(zst_element_set_state(source, ZST_STATE_NULL) == ZST_OK);
    assert(zst_element_set_state(sink, ZST_STATE_NULL) == ZST_OK);
    zst_element_destroy(source);
    zst_element_destroy(sink);
    fixture_destroy(&fixture);
}

int main(void)
{
    test_contiguous_transport_and_reset();
    test_tx_transport();
    test_separate_objects();
    printf("Dante DEP audio tests passed\n");
    return 0;
}

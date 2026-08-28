/* Cross-plane Dante A/V transmitter test using a fake DVR, RTP loopback, and
 * a minimal DEP shared-memory endpoint. */
#define _POSIX_C_SOURCE 200809L

#include "dante_dep_abi.h"
#include "zst_buffer.h"
#include "zst_bus.h"
#include "zst_element.h"
#include "zst_pad.h"
#include "zst_pipeline.h"
#include "zstreamer/elements/zst_audio_test_src.h"
#include "zstreamer/elements/zst_dante_dep_audio.h"
#include "zstreamer/elements/zst_dante_session.h"
#include "zstreamer/elements/zst_dante_video_coordinator.h"
#include "zstreamer/elements/zst_video_test_src.h"
#include "zstreamer/elements/zst_x264_encoder.h"

#include <arpa/inet.h>
#include <assert.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    char name[96];
    char sem_name[96];
    int fd;
    uint8_t* data;
    size_t size;
    dep_shared_header_t* header;
    sem_t* semaphore;
} dep_mock_t;

typedef struct {
    int listener;
    uint16_t port;
    int failed;
    int saw_stop;
} dvr_mock_t;

static int wait_fd(int fd, short events, int timeout_ms)
{
    struct pollfd descriptor = { .fd = fd, .events = events };
    return poll(&descriptor, 1, timeout_ms) == 1 && (descriptor.revents & events);
}

static uint16_t unused_port(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in address = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t size = sizeof(address);
    assert(fd >= 0 && bind(fd, (struct sockaddr*)&address, sizeof(address)) == 0);
    assert(getsockname(fd, (struct sockaddr*)&address, &size) == 0);
    close(fd);
    return ntohs(address.sin_port);
}

static void dep_mock_create(dep_mock_t* mock)
{
    static unsigned serial;
    const uint32_t samples = 32, channels = 2, period = 4;
    size_t metadata = sizeof(dep_shared_header_t) + sizeof(dep_timing_descriptor_t);
    size_t planes = (size_t)samples * channels * sizeof(int32_t);
    memset(mock, 0, sizeof(*mock));
    mock->fd = -1;
    snprintf(mock->name, sizeof(mock->name), "/zst_av_mock_%ld_%u", (long)getpid(), serial++);
    snprintf(mock->sem_name, sizeof(mock->sem_name), "/zst_av_tick_%ld_%u", (long)getpid(), serial++);
    mock->size = metadata + planes * 2;
    mock->fd = shm_open(mock->name, O_CREAT | O_EXCL | O_RDWR, 0600);
    assert(mock->fd >= 0 && ftruncate(mock->fd, (off_t)mock->size) == 0);
    mock->data = mmap(NULL, mock->size, PROT_READ | PROT_WRITE, MAP_SHARED, mock->fd, 0);
    assert(mock->data != MAP_FAILED);
    memset(mock->data, 0, mock->size);
    mock->header = (dep_shared_header_t*)mock->data;
    mock->header->object_bytes = (uint32_t)mock->size;
    mock->header->metadata_bytes = (uint32_t)metadata;
    mock->header->tx_offset = (uint32_t)metadata;
    mock->header->rx_offset = mock->header->tx_offset + (uint32_t)planes;
    mock->header->timing_offset = sizeof(dep_shared_header_t);
    mock->header->sample_rate = 48000;
    mock->header->encoding = DEP_ENCODING_PCM32;
    mock->header->samples_per_channel = samples;
    mock->header->bytes_per_channel = samples * sizeof(int32_t);
    mock->header->tx_channels = mock->header->rx_channels = channels;
    mock->header->samples_per_period = period;
    dep_timing_descriptor_t* timing = (dep_timing_descriptor_t*)(mock->data + mock->header->timing_offset);
    timing->descriptor_bytes = sizeof(*timing);
    timing->kind = DEP_TIMING_SIGNAL_EVENT;
    snprintf(timing->name, sizeof(timing->name), "%s", mock->sem_name);
    mock->semaphore = sem_open(mock->sem_name, O_CREAT | O_EXCL, 0600, 0);
    assert(mock->semaphore != SEM_FAILED);
    __atomic_store_n(&mock->header->magic, DEP_HEADER_MAGIC, __ATOMIC_RELEASE);
}

static void dep_mock_destroy(dep_mock_t* mock)
{
    sem_close(mock->semaphore);
    sem_unlink(mock->sem_name);
    munmap(mock->data, mock->size);
    close(mock->fd);
    shm_unlink(mock->name);
}

static void* dvr_mock_thread(void* argument)
{
    dvr_mock_t* mock = argument;
    char record[1024];
    if (!wait_fd(mock->listener, POLLIN, 3000)) { mock->failed = 1; return NULL; }
    int client = accept(mock->listener, NULL, NULL);
    ssize_t length = client >= 0 ? recv(client, record, sizeof(record), 0) : -1;
    if (length <= 0 || !memmem(record, (size_t)length, "\"action\":\"start\"", 16)) {
        mock->failed = 2;
        goto done;
    }
    char flow[512];
    int written = snprintf(flow, sizeof(flow),
        "{\"action\":\"createVideoUnicastTxFlow\",\"parameters\":{\"flowIndex\":7,"
        "\"channelIndex\":0,\"port\":%u,\"receiverAddress\":\"127.0.0.1\","
        "\"transmitterAddress\":\"127.0.0.1\"}}", mock->port);
    if (send(client, flow, (size_t)written, MSG_NOSIGNAL) != written) { mock->failed = 3; goto done; }
    /* The session lifecycle test validates stop/reconnect. This test only
     * needs the DVR mock to keep the route alive while media is produced. */
    (void)wait_fd(client, POLLIN, 500);
done:
    if (client >= 0) close(client);
    return NULL;
}

static int apply_flow_event(zst_bus_t* bus, zst_element_t* coordinator)
{
    for (unsigned i = 0; i < 40; ++i) {
        zst_event_t* event = NULL;
        if (zst_bus_pop(bus, &event, 100) != ZST_OK) continue;
        int applied = 0;
        if (event->type == ZST_EVENT_DANTE_FLOW_CREATED &&
            event->as.dante_flow.flow.direction == ZST_DANTE_FLOW_TX) {
            assert(zst_dante_video_coordinator_apply_flow(coordinator,
                                                           &event->as.dante_flow.flow) == ZST_OK);
            applied = 1;
        }
        zst_event_destroy(event);
        if (applied) return 1;
    }
    return 0;
}

int main(void)
{
    char directory[] = "/tmp/zst-dante-av-XXXXXX";
    char socket_path[108];
    assert(mkdtemp(directory));
    snprintf(socket_path, sizeof(socket_path), "%s/dvr", directory);
    dvr_mock_t dvr = { .listener = socket(AF_UNIX, SOCK_SEQPACKET, 0), .port = unused_port() };
    assert(dvr.listener >= 0);
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", socket_path);
    assert(bind(dvr.listener, (struct sockaddr*)&address, sizeof(address)) == 0);
    assert(listen(dvr.listener, 1) == 0);
    int rtp_receiver = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in rtp_address = { .sin_family = AF_INET, .sin_port = htons(dvr.port),
                                       .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    assert(rtp_receiver >= 0 && bind(rtp_receiver, (struct sockaddr*)&rtp_address, sizeof(rtp_address)) == 0);

    dep_mock_t dep;
    dep_mock_create(&dep);
    pthread_t dvr_thread;
    assert(pthread_create(&dvr_thread, NULL, dvr_mock_thread, &dvr) == 0);
    zst_pipeline_t* pipeline = zst_pipeline_create();
    zst_element_t* session = zst_dante_session_create(socket_path);
    zst_element_t* coordinator = zst_dante_video_coordinator_create();
    zst_element_t* video = zst_video_test_src_create();
    zst_element_t* encoder = zst_x264_encoder_create();
    zst_element_t* audio = zst_audio_test_src_create();
    zst_element_t* dep_sink = zst_dante_dep_audio_sink_create();
    assert(pipeline && session && coordinator && video && encoder && audio && dep_sink);
    assert(zst_element_set_property(session, "tx-video-channels", "1") == ZST_OK);
    assert(zst_element_set_property(session, "rx-video-channels", "0") == ZST_OK);
    assert(zst_element_set_property(video, "width", "64") == ZST_OK);
    assert(zst_element_set_property(video, "height", "64") == ZST_OK);
    assert(zst_element_set_property(encoder, "fps", "30/1") == ZST_OK);
    assert(zst_element_set_property(audio, "sample-rate", "48000") == ZST_OK);
    assert(zst_element_set_property(audio, "channels", "2") == ZST_OK);
    assert(zst_element_set_property(audio, "sample-format", "S32LE") == ZST_OK);
    assert(zst_element_set_property(audio, "samples-per-buffer", "4") == ZST_OK);
    assert(zst_element_set_property(dep_sink, "shm-name", dep.name) == ZST_OK);
    assert(zst_element_set_property(dep_sink, "channels", "0,1") == ZST_OK);
    assert(zst_element_set_property(dep_sink, "tx-lead-us", "0") == ZST_OK);
    zst_pad_t* tx_pad = zst_dante_video_coordinator_request_tx_input_pad(coordinator, 0);
    assert(tx_pad);
    assert(zst_pipeline_add(pipeline, coordinator) == ZST_OK);
    assert(zst_pipeline_add(pipeline, video) == ZST_OK);
    assert(zst_pipeline_add(pipeline, encoder) == ZST_OK);
    assert(zst_pipeline_add(pipeline, audio) == ZST_OK);
    assert(zst_pipeline_add(pipeline, dep_sink) == ZST_OK);
    assert(zst_dante_video_coordinator_attach_session(coordinator, session) == ZST_OK);
    assert(zst_pad_link(zst_element_get_pad(encoder, "src"), tx_pad) == ZST_OK);
    assert(zst_pipeline_set_state(pipeline, ZST_STATE_PLAYING) == ZST_OK);
    session->bus = pipeline->bus;
    assert(zst_element_set_state(session, ZST_STATE_PLAYING) == ZST_OK);
    assert(apply_flow_event(pipeline->bus, coordinator));

    zst_buffer_t *raw = NULL, *encoded = NULL, *pcm = NULL;
    assert(video->ops->process(video, NULL, &raw) == ZST_OK && raw);
    assert(encoder->ops->process(encoder, raw, &encoded) == ZST_OK && encoded);
    assert(zst_pad_push(zst_element_get_pad(encoder, "src"), encoded) == ZST_OK);
    zst_buffer_unref(encoded);
    zst_buffer_unref(raw);
    uint8_t packet[1500];
    assert(wait_fd(rtp_receiver, POLLIN, 1000));
    assert(recv(rtp_receiver, packet, sizeof(packet), 0) > 12 && (packet[1] & 0x7f) == 96);
    assert(audio->ops->process(audio, NULL, &pcm) == ZST_OK && pcm);
    assert(dep_sink->ops->process(dep_sink, pcm, NULL) == ZST_OK);
    zst_buffer_unref(pcm);
    __atomic_store_n(&dep.header->period_count, 1, __ATOMIC_RELEASE);
    assert(sem_post(dep.semaphore) == 0);
    int32_t* tx = (int32_t*)(dep.data + dep.header->tx_offset);
    int wrote_audio = 0;
    for (unsigned attempt = 0; attempt < 100 && !wrote_audio; ++attempt) {
        for (uint32_t channel = 0; channel < 2; ++channel) {
            for (uint32_t sample = 0; sample < 4; ++sample)
                wrote_audio |= tx[channel * dep.header->samples_per_channel + sample] != 0;
        }
        if (!wrote_audio) {
            struct timespec delay = { .tv_nsec = 2000000L };
            nanosleep(&delay, NULL);
        }
    }
    assert(wrote_audio);

    /* Wake the shared DEP worker if it returned to its timing wait. */
    assert(sem_post(dep.semaphore) == 0);
    assert(zst_element_set_state(session, ZST_STATE_NULL) == ZST_OK);
    session->bus = NULL;
    assert(zst_element_set_state(video, ZST_STATE_NULL) == ZST_OK);
    assert(zst_element_set_state(encoder, ZST_STATE_NULL) == ZST_OK);
    assert(zst_element_set_state(audio, ZST_STATE_NULL) == ZST_OK);
    assert(zst_element_set_state(dep_sink, ZST_STATE_NULL) == ZST_OK);
    assert(zst_element_set_state(coordinator, ZST_STATE_NULL) == ZST_OK);
    assert(zst_pipeline_set_state(pipeline, ZST_STATE_NULL) == ZST_OK);
    zst_dante_video_coordinator_attach_session(coordinator, NULL);
    zst_pipeline_destroy(pipeline);
    zst_element_destroy(session);
    pthread_join(dvr_thread, NULL);
    assert(!dvr.failed);
    dep_mock_destroy(&dep);
    close(rtp_receiver);
    close(dvr.listener);
    unlink(socket_path);
    rmdir(directory);
    puts("Dante A/V TX mock test passed");
    return 0;
}

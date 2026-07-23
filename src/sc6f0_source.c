/*=============================================================================
    sc6f0_source.c — SC6F0 Video/Audio Hardware Source (Composite Bin)
=============================================================================*/

#define _POSIX_C_SOURCE 200809L  /* strdup */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zst_log.h"

#if defined(HAS_V4L2) && defined(HAS_ALSA)

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include <linux/v4l2-subdev.h>

#include "zst_bin.h"
#include "zst_bus.h"
#include "zst_pad.h"
#include "zst_caps.h"
#include "zst_stream.h"
#include "zst_pad_event.h"
#include "zst_pipeline.h"
#include "zstreamer/elements/zst_sc6f0_source.h"
#include "zstreamer/elements/zst_v4l2_source.h"
#include "zstreamer/elements/zst_alsa_source.h"

/* ── Custom Private Structure ────────────────────────────────────────── */

/* Thread safety:
 *
 * The SC6F0 source uses a monitor thread (started in sc6f0_start / joined in
 * sc6f0_stop) that polls for signal presence changes — either from mock-mode
 * triggers or from real hardware (V4L2 subdev + HDMI sysfs).
 *
 * Lock hierarchy:
 *   s->lock  →  (no sub-locks, never re-entered)
 *
 * - The monitor thread holds s->lock only while reading/writing signal state
 *   fields (signal_locked, width, height, fps, audio_*).  It releases the lock
 *   before calling sc6f0_reconfigure_capture_branch() / sc6f0_teardown_capture_branch().
 * - sc6f0_reconfigure_capture_branch() acquires s->lock to read signal state
 *   and manipulate ghost pads / child elements.  It does NOT call back into
 *   any function that re-acquires s->lock.
 * - Property setters/getters (sc6f0_set_property / sc6f0_get_property) hold
 *   s->lock for the duration.
 * - The volatile "running" flag is used as a single-writer/many-reader stop
 *   signal; it is set before the thread starts and cleared before join().
 */

typedef struct {
    /* --- SC6F0 configuration (set via properties, protected by s->lock) --- */
    char media_device[128];
    char platform_id[16];
    bool mock_mode;
    char mock_trigger[32]; // "1080p", "720p", "none"

    /* Configurable device paths */
    char subdev_path[128];
    char vpss_csc_path[128];
    char audio_sysfs_path[256];

    /* --- V4L2 monitor variables --- */
    int media_fd;
    int subdev_fd;
    int vpss_csc_fd;
    pthread_t monitor_thread;
    volatile bool running;
    pthread_mutex_t lock;

    /* --- Current simulated/real signal state (protected by s->lock) --- */
    bool signal_locked;
    uint32_t width;
    uint32_t height;
    double fps;
    bool audio_detected;
    int audio_channels;
    int audio_rate;

    /* --- Child capture elements --- */
    zst_element_t* v4l2_src;
    zst_element_t* alsa_src;

    /* --- Exposed dynamic source ghost pads --- */
    zst_pad_t* video_pad;
    zst_pad_t* audio_pad;
} sc6f0_source_priv_t;

/* ── Forward Declarations ──────────────────────────────────────────── */

static void* sc6f0_monitor_thread_func(void* arg);
static void  sc6f0_update_input_format(zst_element_t* el);
static void  sc6f0_reconfigure_capture_branch(zst_element_t* el);
static void  sc6f0_teardown_capture_branch(zst_element_t* el);
static void  sc6f0_teardown_capture_branch_internal(zst_element_t* el, bool dynamic_reconfig);

/* ── Caps Comparison Helper ────────────────────────────────────────── */

static int
sc6f0_caps_equal(const zst_caps_t* a, const zst_caps_t* b)
{
    if (a == b) return 1;
    if (!a || !b || !a->structs || !b->structs) return 0;
    const zst_caps_struct_t* as = a->structs;
    const zst_caps_struct_t* bs = b->structs;
    if (as->next || bs->next) return 0;
    if (strcmp(as->media_type, bs->media_type) != 0) return 0;
    if (as->type != bs->type) return 0;
    if (as->video.width != bs->video.width ||
        as->video.height != bs->video.height ||
        as->video.framerate != bs->video.framerate ||
        strcmp(as->video.pixel_format, bs->video.pixel_format) != 0) return 0;
    if (as->audio.channels != bs->audio.channels ||
        as->audio.sample_rate != bs->audio.sample_rate ||
        strcmp(as->audio.format, bs->audio.format) != 0) return 0;
    if (as->nb_fields != bs->nb_fields) return 0;
    for (uint32_t i = 0; i < as->nb_fields; i++) {
        const zst_caps_field_t* af = &as->fields[i];
        const zst_caps_field_t* bf = &bs->fields[i];
        if (strcmp(af->key, bf->key) != 0 || af->type != bf->type) return 0;
        switch (af->type) {
        case ZST_CAPS_FIELD_INT:
            if (af->value.i_val != bf->value.i_val) return 0;
            break;
        case ZST_CAPS_FIELD_UINT:
            if (af->value.u_val != bf->value.u_val) return 0;
            break;
        case ZST_CAPS_FIELD_DOUBLE:
            if (af->value.d_val != bf->value.d_val) return 0;
            break;
        case ZST_CAPS_FIELD_STRING:
            if (strcmp(af->value.s_val ? af->value.s_val : "",
                       bf->value.s_val ? bf->value.s_val : "") != 0) return 0;
            break;
        case ZST_CAPS_FIELD_FRACTION:
            if (af->value.f_val.num != bf->value.f_val.num ||
                af->value.f_val.denom != bf->value.f_val.denom) return 0;
            break;
        case ZST_CAPS_FIELD_BUFFER:
            if (af->value.b_val.size != bf->value.b_val.size) return 0;
            if (af->value.b_val.size > 0 &&
                memcmp(af->value.b_val.data, bf->value.b_val.data, af->value.b_val.size) != 0) return 0;
            break;
        }
    }
    return 1;
}

/* =====================================================================
    SC6F0 Element Callbacks
   ===================================================================== */

static zst_result_t
sc6f0_open(zst_element_t* el)
{
    sc6f0_source_priv_t* s = el->priv;
    s->media_fd = -1;
    s->subdev_fd = -1;
    s->vpss_csc_fd = -1;
    s->signal_locked = false;
    s->width = 0;
    s->height = 0;
    s->fps = 0.0;
    s->audio_detected = false;
    s->audio_channels = 0;
    s->audio_rate = 0;
    s->video_pad = NULL;
    s->audio_pad = NULL;

    // Check if mock mode is explicitly requested or if no physical devices are specified
    if (s->mock_mode || s->media_device[0] == '\0') {
        s->mock_mode = true;
        ZST_LOG_INFO("sc6f0src", "Opening SC6F0 source in MOCK mode.");
        return ZST_OK;
    }

    // Try opening media controller
    s->media_fd = open(s->media_device, O_RDWR);
    if (s->media_fd < 0) {
        ZST_LOG_WARN("sc6f0src", "Failed to open media device %s: %s. Falling back to MOCK mode.", s->media_device, strerror(errno));
        s->mock_mode = true;
        return ZST_OK;
    }

    // For portability, if topology elements are missing, fall back to mock mode.
    s->subdev_fd = open(s->subdev_path, O_RDWR | O_NONBLOCK);
    if (s->subdev_fd < 0) {
        ZST_LOG_WARN("sc6f0src", "Failed to open subdevice %s. Falling back to MOCK mode.", s->subdev_path);
        close(s->media_fd);
        s->media_fd = -1;
        s->mock_mode = true;
        return ZST_OK;
    }

    s->vpss_csc_fd = open(s->vpss_csc_path, O_RDWR | O_NONBLOCK);
    if (s->vpss_csc_fd < 0) {
        ZST_LOG_WARN("sc6f0src", "Failed to open CSC VPSS subdevice %s. Fall back to MOCK mode.", s->vpss_csc_path);
        close(s->subdev_fd);
        s->subdev_fd = -1;
        close(s->media_fd);
        s->media_fd = -1;
        s->mock_mode = true;
        return ZST_OK;
    }

    ZST_LOG_INFO("sc6f0src", "Successfully initialized SC6F0 hardware subdevices.");
    return ZST_OK;
}

static zst_result_t
sc6f0_close(zst_element_t* el)
{
    sc6f0_source_priv_t* s = el->priv;
    if (s->vpss_csc_fd >= 0) {
        close(s->vpss_csc_fd);
        s->vpss_csc_fd = -1;
    }
    if (s->subdev_fd >= 0) {
        close(s->subdev_fd);
        s->subdev_fd = -1;
    }
    if (s->media_fd >= 0) {
        close(s->media_fd);
        s->media_fd = -1;
    }
    pthread_mutex_destroy(&s->lock);
    return ZST_OK;
}

static zst_result_t
sc6f0_start(zst_element_t* el)
{
    sc6f0_source_priv_t* s = el->priv;
    s->running = true;
    if (pthread_create(&s->monitor_thread, NULL, sc6f0_monitor_thread_func, el) != 0) {
        ZST_LOG_ERROR("sc6f0src", "Failed to start monitoring thread.");
        s->running = false;
        return ZST_ERROR;
    }
    return ZST_OK;
}

static zst_result_t
sc6f0_stop(zst_element_t* el)
{
    sc6f0_source_priv_t* s = el->priv;
    s->running = false;
    pthread_join(s->monitor_thread, NULL);

    // Teardown any remaining capture branches (do not reconfigure dynamically during stop)
    sc6f0_teardown_capture_branch_internal(el, false);

    return ZST_OK;
}

static zst_result_t
sc6f0_set_property(zst_element_t* el, const char* name, const char* value)
{
    sc6f0_source_priv_t* s = el->priv;
    if (!name || !value) return ZST_ERROR;

    pthread_mutex_lock(&s->lock);
    if (strcmp(name, ZST_SC6F0_SOURCE_PROP_MEDIA_DEVICE) == 0) {
        snprintf(s->media_device, sizeof(s->media_device), "%s", value);
    } else if (strcmp(name, ZST_SC6F0_SOURCE_PROP_PLATFORM_ID) == 0) {
        snprintf(s->platform_id, sizeof(s->platform_id), "%s", value);
    } else if (strcmp(name, ZST_SC6F0_SOURCE_PROP_MOCK_MODE) == 0) {
        s->mock_mode = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
    } else if (strcmp(name, ZST_SC6F0_SOURCE_PROP_TRIGGER_SIGNAL) == 0) {
        snprintf(s->mock_trigger, sizeof(s->mock_trigger), "%s", value);
    } else if (strcmp(name, ZST_SC6F0_SOURCE_PROP_SUBDEV_PATH) == 0) {
        snprintf(s->subdev_path, sizeof(s->subdev_path), "%s", value);
    } else if (strcmp(name, ZST_SC6F0_SOURCE_PROP_VPSS_CSC_PATH) == 0) {
        snprintf(s->vpss_csc_path, sizeof(s->vpss_csc_path), "%s", value);
    } else if (strcmp(name, ZST_SC6F0_SOURCE_PROP_AUDIO_SYSFS_PATH) == 0) {
        snprintf(s->audio_sysfs_path, sizeof(s->audio_sysfs_path), "%s", value);
    } else {
        pthread_mutex_unlock(&s->lock);
        return ZST_ERROR;
    }
    pthread_mutex_unlock(&s->lock);
    return ZST_OK;
}

static zst_result_t
sc6f0_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len)
{
    sc6f0_source_priv_t* s = el->priv;
    if (!name || !value_out || max_len == 0) return ZST_ERROR;

    pthread_mutex_lock(&s->lock);
    if (strcmp(name, ZST_SC6F0_SOURCE_PROP_MEDIA_DEVICE) == 0) {
        snprintf(value_out, max_len, "%s", s->media_device);
    } else if (strcmp(name, ZST_SC6F0_SOURCE_PROP_PLATFORM_ID) == 0) {
        snprintf(value_out, max_len, "%s", s->platform_id);
    } else if (strcmp(name, ZST_SC6F0_SOURCE_PROP_MOCK_MODE) == 0) {
        snprintf(value_out, max_len, "%s", s->mock_mode ? "true" : "false");
    } else if (strcmp(name, ZST_SC6F0_SOURCE_PROP_TRIGGER_SIGNAL) == 0) {
        snprintf(value_out, max_len, "%s", s->mock_trigger);
    } else if (strcmp(name, ZST_SC6F0_SOURCE_PROP_SUBDEV_PATH) == 0) {
        snprintf(value_out, max_len, "%s", s->subdev_path);
    } else if (strcmp(name, ZST_SC6F0_SOURCE_PROP_VPSS_CSC_PATH) == 0) {
        snprintf(value_out, max_len, "%s", s->vpss_csc_path);
    } else if (strcmp(name, ZST_SC6F0_SOURCE_PROP_AUDIO_SYSFS_PATH) == 0) {
        snprintf(value_out, max_len, "%s", s->audio_sysfs_path);
    } else {
        pthread_mutex_unlock(&s->lock);
        return ZST_ERROR;
    }
    pthread_mutex_unlock(&s->lock);
    return ZST_OK;
}

/* =====================================================================
    Event and Stream Table Queries (Adaptive Demuxing Contract)
   ===================================================================== */

static uint32_t
sc6f0_get_stream_count(zst_element_t* el)
{
    sc6f0_source_priv_t* s = el->priv;
    pthread_mutex_lock(&s->lock);
    uint32_t count = 0;
    if (s->video_pad) count++;
    if (s->audio_pad) count++;
    pthread_mutex_unlock(&s->lock);
    return count;
}

static zst_result_t
sc6f0_get_stream_info(zst_element_t* el, uint32_t index, zst_stream_info_t* info_out)
{
    sc6f0_source_priv_t* s = el->priv;
    pthread_mutex_lock(&s->lock);

    zst_pad_t* target_pad = NULL;
    zst_media_kind_t kind = ZST_MEDIA_UNKNOWN;
    zst_stream_id_t stream_id = 0;

    uint32_t curr = 0;
    if (s->video_pad) {
        if (curr == index) {
            target_pad = s->video_pad;
            kind = ZST_MEDIA_VIDEO;
            stream_id = 1;
        }
        curr++;
    }
    if (s->audio_pad) {
        if (curr == index) {
            target_pad = s->audio_pad;
            kind = ZST_MEDIA_AUDIO;
            stream_id = 2;
        }
    }

    if (!target_pad) {
        pthread_mutex_unlock(&s->lock);
        return ZST_ERROR;
    }

    memset(info_out, 0, sizeof(*info_out));
    info_out->struct_size = sizeof(*info_out);
    info_out->id = stream_id;
    info_out->kind = kind;
    info_out->status = ZST_STREAM_STATUS_PRESENT;
    info_out->name = target_pad->name ? strdup(target_pad->name) : NULL;
    info_out->caps = target_pad->caps ? zst_caps_copy(target_pad->caps) : NULL;

    pthread_mutex_unlock(&s->lock);
    return ZST_OK;
}

static zst_pad_t*
sc6f0_get_stream_pad(zst_element_t* el, zst_stream_id_t stream_id)
{
    sc6f0_source_priv_t* s = el->priv;
    pthread_mutex_lock(&s->lock);
    zst_pad_t* pad = NULL;
    if (stream_id == 1) pad = s->video_pad;
    else if (stream_id == 2) pad = s->audio_pad;
    pthread_mutex_unlock(&s->lock);
    return pad;
}

/* =====================================================================
    Thread Event Monitoring & Stream Instantiation
   ===================================================================== */

static void
sc6f0_check_mock_state_changes(zst_element_t* el)
{
    sc6f0_source_priv_t* s = el->priv;
    pthread_mutex_lock(&s->lock);

    // Default trigger
    if (s->mock_trigger[0] == '\0') {
        strcpy(s->mock_trigger, "1080p");
    }

    bool do_reconfigure = false;
    bool do_teardown = false;

    if (strcmp(s->mock_trigger, "none") == 0) {
        if (s->signal_locked) {
            s->signal_locked = false;
            ZST_LOG_INFO("sc6f0src", "Mock: Signal Lost.");
            if (el->bus) {
                zst_bus_post(el->bus, zst_event_new_signal_lost(el));
            }
            do_teardown = true;
        }
    } else if (strcmp(s->mock_trigger, "1080p") == 0) {
        uint32_t w = 1920, h = 1080;
        double f = 60.0;
        bool audio = true;
        int ar = 48000, ac = 2;

        if (!s->signal_locked || s->width != w || s->height != h || s->fps != f ||
            s->audio_detected != audio || s->audio_rate != ar || s->audio_channels != ac) {
            s->signal_locked = true;
            s->width = w;
            s->height = h;
            s->fps = f;
            s->audio_detected = audio;
            s->audio_rate = ar;
            s->audio_channels = ac;

            ZST_LOG_INFO("sc6f0src", "Mock: Signal Present (1080p60).");
            if (el->bus) {
                zst_bus_post(el->bus, zst_event_new_signal_present(el));
            }
            do_reconfigure = true;
        }
    } else if (strcmp(s->mock_trigger, "720p") == 0) {
        uint32_t w = 1280, h = 720;
        double f = 30.0;
        bool audio = true;
        int ar = 44100, ac = 2;

        if (!s->signal_locked || s->width != w || s->height != h || s->fps != f ||
            s->audio_detected != audio || s->audio_rate != ar || s->audio_channels != ac) {
            s->signal_locked = true;
            s->width = w;
            s->height = h;
            s->fps = f;
            s->audio_detected = audio;
            s->audio_rate = ar;
            s->audio_channels = ac;

            ZST_LOG_INFO("sc6f0src", "Mock: Signal Present (720p30).");
            if (el->bus) {
                zst_bus_post(el->bus, zst_event_new_signal_present(el));
            }
            do_reconfigure = true;
        }
    }

    pthread_mutex_unlock(&s->lock);

    if (do_reconfigure) {
        sc6f0_reconfigure_capture_branch(el);
    } else if (do_teardown) {
        sc6f0_teardown_capture_branch(el);
    }
}

static void
sc6f0_check_audio_sysfs(zst_element_t* el)
{
    sc6f0_source_priv_t* s = el->priv;
    if (s->mock_mode) return;

    // HDMI sysfs parsing
    FILE* fp = fopen(s->audio_sysfs_path, "r");
    if (!fp) return;

    char line[128];
    if (fgets(line, sizeof(line), fp)) {
        int detected = 0, format = -1, channels = 0, rate = 0;
        if (sscanf(line, "%d %d %d %d", &detected, &format, &channels, &rate) == 4) {
            pthread_mutex_lock(&s->lock);
            bool audio = (detected != 0);
            bool do_reconfigure = false;
            if (s->signal_locked && (s->audio_detected != audio || s->audio_channels != channels || s->audio_rate != rate)) {
                s->audio_detected = audio;
                s->audio_channels = channels;
                s->audio_rate = rate;
                ZST_LOG_INFO("sc6f0src", "Audio status changed: detected=%d, channels=%d, rate=%d", detected, channels, rate);
                do_reconfigure = true;
            }
            pthread_mutex_unlock(&s->lock);

            if (do_reconfigure) {
                sc6f0_reconfigure_capture_branch(el);
            }
        }
    }
    fclose(fp);
}

static void
sc6f0_update_input_format(zst_element_t* el)
{
    sc6f0_source_priv_t* s = el->priv;
    if (s->mock_mode) return;

    pthread_mutex_lock(&s->lock);
    struct v4l2_dv_timings timings;
    memset(&timings, 0, sizeof(timings));

    if (ioctl(s->subdev_fd, VIDIOC_SUBDEV_QUERY_DV_TIMINGS, &timings) < 0) {
        bool do_teardown = false;
        if (s->signal_locked) {
            s->signal_locked = false;
            ZST_LOG_INFO("sc6f0src", "Signal Lost.");
            if (el->bus) {
                zst_bus_post(el->bus, zst_event_new_signal_lost(el));
            }
            do_teardown = true;
        }
        pthread_mutex_unlock(&s->lock);
        if (do_teardown) {
            sc6f0_teardown_capture_branch(el);
        }
        return;
    }

    uint32_t w = timings.bt.width;
    uint32_t h = timings.bt.height;
    double pixel_clock = timings.bt.pixelclock;
    uint32_t htotal = timings.bt.width + timings.bt.hfrontporch + timings.bt.hsync + timings.bt.hbackporch;
    uint32_t vtotal = timings.bt.height + timings.bt.vfrontporch + timings.bt.vsync + timings.bt.vbackporch;
    double f = (htotal * vtotal) > 0 ? (pixel_clock / (double)(htotal * vtotal)) : 30.0;

    // Apply VPSS crop & scale adjustments on hardware
    struct v4l2_subdev_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.pad = 0;
    fmt.which = V4L2_SUBDEV_FORMAT_ACTIVE;
    if (ioctl(s->subdev_fd, VIDIOC_SUBDEV_G_FMT, &fmt) == 0) {
        if (s->vpss_csc_fd >= 0) {
            struct v4l2_subdev_format csc_sink;
            memcpy(&csc_sink, &fmt, sizeof(fmt));
            csc_sink.pad = 0;
            ioctl(s->vpss_csc_fd, VIDIOC_SUBDEV_S_FMT, &csc_sink);

            struct v4l2_subdev_format csc_src;
            memcpy(&csc_src, &fmt, sizeof(fmt));
            csc_src.pad = 1;
            if (strcmp(s->platform_id, "DNTX") == 0) {
                csc_src.format.code = 0x200f; // MEDIA_BUS_FMT_VUY8_1X24
            } else {
                csc_src.format.code = 0x2015; // MEDIA_BUS_FMT_VUY10_1X30
            }
            ioctl(s->vpss_csc_fd, VIDIOC_SUBDEV_S_FMT, &csc_src);
        }
    }

    // Default audio properties if SDI
    bool audio = true;
    int ar = 48000;
    int ac = 2;

    if (strcmp(s->platform_id, "DNTX") == 0) {
        audio = s->audio_detected;
        ar = s->audio_rate;
        ac = s->audio_channels;
    }

    bool do_reconfigure = false;
    if (!s->signal_locked || s->width != w || s->height != h || s->fps != f ||
        s->audio_detected != audio || s->audio_rate != ar || s->audio_channels != ac) {
        s->signal_locked = true;
        s->width = w;
        s->height = h;
        s->fps = f;
        s->audio_detected = audio;
        s->audio_rate = ar;
        s->audio_channels = ac;

        ZST_LOG_INFO("sc6f0src", "Signal Present: %dx%d @ %.2f fps", w, h, f);
        if (el->bus) {
            zst_bus_post(el->bus, zst_event_new_signal_present(el));
        }
        do_reconfigure = true;
    }
    pthread_mutex_unlock(&s->lock);

    if (do_reconfigure) {
        sc6f0_reconfigure_capture_branch(el);
    }
}

static void*
sc6f0_monitor_thread_func(void* arg)
{
    zst_element_t* el = arg;
    sc6f0_source_priv_t* s = el->priv;

    ZST_LOG_INFO("sc6f0src", "Monitor thread started.");

    while (s->running) {
        if (s->mock_mode) {
            sc6f0_check_mock_state_changes(el);
            usleep(100000); // 100ms polling
        } else {
            sc6f0_update_input_format(el);
            for (int i = 0; i < 10 && s->running; i++) {
                usleep(50000);
            }
        }
    }
    ZST_LOG_INFO("sc6f0src", "Monitor thread stopped.");
    return NULL;
}

static void
sc6f0_reconfigure_capture_branch(zst_element_t* el)
{
    sc6f0_source_priv_t* s = el->priv;

    if (!el->pipeline) return;

    // Begin dynamic pipeline transaction
    zst_pipeline_reconfigure_begin(el->pipeline);

    pthread_mutex_lock(&s->lock);

    if (!s->signal_locked) {
        pthread_mutex_unlock(&s->lock);
        zst_pipeline_reconfigure_end(el->pipeline);
        return;
    }

    // 1. Create or verify children
    if (!s->v4l2_src) {
        s->v4l2_src = zst_v4l2_source_create();
        s->v4l2_src->bus = el->bus;
        s->v4l2_src->pipeline = el->pipeline;
        zst_element_set_clock(s->v4l2_src, el->clock);
    }

    zst_element_set_property_uint(s->v4l2_src, "width", s->width);
    zst_element_set_property_uint(s->v4l2_src, "height", s->height);
    zst_element_set_property_string(s->v4l2_src, "pixel-format", "YUY2");

    if (s->audio_detected) {
        if (!s->alsa_src) {
            s->alsa_src = zst_alsa_source_create();
            s->alsa_src->bus = el->bus;
            s->alsa_src->pipeline = el->pipeline;
            zst_element_set_clock(s->alsa_src, el->clock);
        }
        zst_element_set_property_uint(s->alsa_src, "channels", s->audio_channels);
        zst_element_set_property_uint(s->alsa_src, "sample-rate", s->audio_rate);
    } else {
        if (s->alsa_src) {
            zst_element_set_state(s->alsa_src, ZST_STATE_NULL);
            s->alsa_src->bus = NULL;
            s->alsa_src->pipeline = NULL;
            zst_element_set_clock(s->alsa_src, NULL);
            zst_element_destroy(s->alsa_src);
            s->alsa_src = NULL;
        }
    }

    // 2. Setup dynamic ghost pads
    zst_caps_t* old_vcaps = s->video_pad ? (s->video_pad->caps ? zst_caps_copy(s->video_pad->caps) : NULL) : NULL;
    zst_caps_t* vcaps = zst_caps_new_simple("video/x-raw");
    zst_caps_set_int(vcaps, "width", s->width);
    zst_caps_set_int(vcaps, "height", s->height);
    zst_caps_set_string(vcaps, "format", "YUY2");
    zst_caps_set_double(vcaps, "framerate", s->fps);

    if (!s->video_pad) {
        zst_pad_t* target = zst_element_get_pad(s->v4l2_src, "src");
        s->video_pad = zst_ghost_pad_create("video_0", target);
        zst_pad_set_caps(s->video_pad, vcaps);
        zst_pad_set_unlinked_policy(s->video_pad, ZST_PAD_UNLINKED_DROP, 0);

        zst_stream_info_t info = {
            .struct_size = sizeof(info),
            .id = 1,
            .kind = ZST_MEDIA_VIDEO,
            .name = "video_0",
            .caps = vcaps,
            .status = ZST_STREAM_STATUS_PRESENT
        };

        zst_element_add_dynamic_pad(el, s->video_pad, &info);

        zst_pad_event_t* ss = zst_pad_event_new_stream_start(1);
        zst_pad_push_event(s->video_pad, ss);
        zst_pad_event_unref(ss);

        zst_caps_t* copy_vcaps = zst_caps_copy(vcaps);
        zst_pad_event_t* ce = zst_pad_event_new_caps(copy_vcaps);
        zst_pad_push_event(s->video_pad, ce);
        zst_pad_event_unref(ce);
        zst_caps_destroy(copy_vcaps);

        zst_segment_t seg = zst_segment_default();
        zst_pad_event_t* se = zst_pad_event_new_segment(&seg);
        zst_pad_push_event(s->video_pad, se);
        zst_pad_event_unref(se);
    } else {
        if (!old_vcaps || !sc6f0_caps_equal(old_vcaps, vcaps)) {
            zst_pad_set_caps(s->video_pad, vcaps);
            if (el->bus) {
                zst_bus_post(el->bus, zst_event_new_caps_changed(el, s->video_pad, old_vcaps, vcaps));
                zst_stream_info_t info = {
                    .struct_size = sizeof(info),
                    .id = 1,
                    .kind = ZST_MEDIA_VIDEO,
                    .name = "video_0",
                    .caps = vcaps,
                    .status = ZST_STREAM_STATUS_CHANGED
                };
                zst_bus_post(el->bus, zst_event_new_stream_changed(el, &info));
            }
            zst_caps_t* copy_vcaps = zst_caps_copy(vcaps);
            zst_pad_event_t* ce = zst_pad_event_new_caps(copy_vcaps);
            zst_pad_push_event(s->video_pad, ce);
            zst_pad_event_unref(ce);
            zst_caps_destroy(copy_vcaps);
        }
    }
    zst_caps_destroy(vcaps);
    if (old_vcaps) zst_caps_destroy(old_vcaps);

    if (s->audio_detected) {
        zst_caps_t* old_acaps = s->audio_pad ? (s->audio_pad->caps ? zst_caps_copy(s->audio_pad->caps) : NULL) : NULL;
        zst_caps_t* acaps = zst_caps_new_simple("audio/x-raw");
        zst_caps_set_int(acaps, "channels", s->audio_channels);
        zst_caps_set_int(acaps, "sample-rate", s->audio_rate);
        zst_caps_set_string(acaps, "format", "S16LE");

        if (!s->audio_pad) {
            zst_pad_t* target = zst_element_get_pad(s->alsa_src, "src");
            s->audio_pad = zst_ghost_pad_create("audio_0", target);
            zst_pad_set_caps(s->audio_pad, acaps);
            zst_pad_set_unlinked_policy(s->audio_pad, ZST_PAD_UNLINKED_DROP, 0);

            zst_stream_info_t info = {
                .struct_size = sizeof(info),
                .id = 2,
                .kind = ZST_MEDIA_AUDIO,
                .name = "audio_0",
                .caps = acaps,
                .status = ZST_STREAM_STATUS_PRESENT
            };

            zst_element_add_dynamic_pad(el, s->audio_pad, &info);

            zst_pad_event_t* ss = zst_pad_event_new_stream_start(2);
            zst_pad_push_event(s->audio_pad, ss);
            zst_pad_event_unref(ss);

            zst_caps_t* copy_acaps = zst_caps_copy(acaps);
            zst_pad_event_t* ce = zst_pad_event_new_caps(copy_acaps);
            zst_pad_push_event(s->audio_pad, ce);
            zst_pad_event_unref(ce);
            zst_caps_destroy(copy_acaps);

            zst_segment_t seg = zst_segment_default();
            zst_pad_event_t* se = zst_pad_event_new_segment(&seg);
            zst_pad_push_event(s->audio_pad, se);
            zst_pad_event_unref(se);
        } else {
            if (!old_acaps || !sc6f0_caps_equal(old_acaps, acaps)) {
                zst_pad_set_caps(s->audio_pad, acaps);
                if (el->bus) {
                    zst_bus_post(el->bus, zst_event_new_caps_changed(el, s->audio_pad, old_acaps, acaps));
                    zst_stream_info_t info = {
                        .struct_size = sizeof(info),
                        .id = 2,
                        .kind = ZST_MEDIA_AUDIO,
                        .name = "audio_0",
                        .caps = acaps,
                        .status = ZST_STREAM_STATUS_CHANGED
                    };
                    zst_bus_post(el->bus, zst_event_new_stream_changed(el, &info));
                }
                zst_caps_t* copy_acaps = zst_caps_copy(acaps);
                zst_pad_event_t* ce = zst_pad_event_new_caps(copy_acaps);
                zst_pad_push_event(s->audio_pad, ce);
                zst_pad_event_unref(ce);
                zst_caps_destroy(copy_acaps);
            }
        }
        zst_caps_destroy(acaps);
        if (old_acaps) zst_caps_destroy(old_acaps);
    } else {
        if (s->audio_pad) {
            zst_element_remove_dynamic_pad(el, s->audio_pad);
            zst_pad_destroy(s->audio_pad);
            s->audio_pad = NULL;
        }
    }

    // 3. Move children states to match bin's state (PLAYING)
    zst_state_t el_state = __atomic_load_n(&el->state, __ATOMIC_ACQUIRE);
    if (s->v4l2_src) zst_element_set_state(s->v4l2_src, el_state);
    if (s->alsa_src) zst_element_set_state(s->alsa_src, el_state);

    pthread_mutex_unlock(&s->lock);

    zst_pipeline_reconfigure_end(el->pipeline);
}

static void
sc6f0_teardown_capture_branch_internal(zst_element_t* el, bool dynamic_reconfig)
{
    sc6f0_source_priv_t* s = el->priv;

    if (dynamic_reconfig && el->pipeline) {
        zst_pipeline_reconfigure_begin(el->pipeline);
    }

    pthread_mutex_lock(&s->lock);

    if (s->video_pad) {
        zst_element_remove_dynamic_pad(el, s->video_pad);
        zst_pad_destroy(s->video_pad);
        s->video_pad = NULL;
    }

    if (s->audio_pad) {
        zst_element_remove_dynamic_pad(el, s->audio_pad);
        zst_pad_destroy(s->audio_pad);
        s->audio_pad = NULL;
    }

    if (s->v4l2_src) {
        zst_element_set_state(s->v4l2_src, ZST_STATE_NULL);
        s->v4l2_src->bus = NULL;
        s->v4l2_src->pipeline = NULL;
        zst_element_set_clock(s->v4l2_src, NULL);
        zst_element_destroy(s->v4l2_src);
        s->v4l2_src = NULL;
    }

    if (s->alsa_src) {
        zst_element_set_state(s->alsa_src, ZST_STATE_NULL);
        s->alsa_src->bus = NULL;
        s->alsa_src->pipeline = NULL;
        zst_element_set_clock(s->alsa_src, NULL);
        zst_element_destroy(s->alsa_src);
        s->alsa_src = NULL;
    }

    pthread_mutex_unlock(&s->lock);

    if (dynamic_reconfig && el->pipeline) {
        zst_pipeline_reconfigure_end(el->pipeline);
    }
}

static void
sc6f0_teardown_capture_branch(zst_element_t* el)
{
    sc6f0_teardown_capture_branch_internal(el, true);
}

/* =====================================================================
    Element Creation & Factory Registration
   ===================================================================== */

static zst_result_t
sc6f0_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    (void)in; (void)out;
    return ZST_OK;
}

static zst_caps_t*
sc6f0_get_caps(zst_element_t* el, zst_pad_t* pad, const zst_caps_t* filter)
{
    (void)el; (void)filter;
    return pad->caps ? zst_caps_copy(pad->caps) : NULL;
}

static zst_clock_t*
sc6f0_provide_clock(zst_element_t* el)
{
    sc6f0_source_priv_t* s = el->priv;
    pthread_mutex_lock(&s->lock);
    zst_clock_t* clk = NULL;
    if (s->alsa_src && s->alsa_src->ops && s->alsa_src->ops->provide_clock) {
        clk = s->alsa_src->ops->provide_clock(s->alsa_src);
    } else if (s->v4l2_src && s->v4l2_src->ops && s->v4l2_src->ops->provide_clock) {
        clk = s->v4l2_src->ops->provide_clock(s->v4l2_src);
    }
    pthread_mutex_unlock(&s->lock);
    return clk;
}

static zst_buffer_pool_t*
sc6f0_get_pool(zst_element_t* el)
{
    sc6f0_source_priv_t* s = el->priv;
    pthread_mutex_lock(&s->lock);
    zst_buffer_pool_t* pool = NULL;
    if (s->v4l2_src && s->v4l2_src->ops && s->v4l2_src->ops->get_pool) {
        pool = s->v4l2_src->ops->get_pool(s->v4l2_src);
    }
    pthread_mutex_unlock(&s->lock);
    return pool;
}

static zst_element_ops_t g_sc6f0_ops = {
    .name             = ZST_SC6F0_SOURCE_FACTORY,
    .open             = sc6f0_open,
    .close            = sc6f0_close,
    .start            = sc6f0_start,
    .stop             = sc6f0_stop,
    .process          = sc6f0_process,
    .get_caps         = sc6f0_get_caps,
    .provide_clock    = sc6f0_provide_clock,
    .get_pool         = sc6f0_get_pool,
    .set_property     = sc6f0_set_property,
    .get_property     = sc6f0_get_property,
    .get_stream_count = sc6f0_get_stream_count,
    .get_stream_info  = sc6f0_get_stream_info,
    .get_stream_pad   = sc6f0_get_stream_pad,
};

zst_element_t*
zst_sc6f0_source_create(void)
{
    sc6f0_source_priv_t* s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    /* Default configurable paths */
    strncpy(s->subdev_path, "/dev/v4l-subdev0", sizeof(s->subdev_path) - 1);
    strncpy(s->vpss_csc_path, "/dev/v4l-subdev1", sizeof(s->vpss_csc_path) - 1);
    strncpy(s->audio_sysfs_path,
            "/sys/devices/platform/amba_pl@0/b0070000.v_hdmi_rx_ss/audio_format",
            sizeof(s->audio_sysfs_path) - 1);
    s->mock_mode = true;
    pthread_mutex_init(&s->lock, NULL);

    zst_element_t* el = zst_element_create(&g_sc6f0_ops, s);
    if (!el) {
        pthread_mutex_destroy(&s->lock);
        free(s);
        return NULL;
    }
    return el;
}

#else

#include "zstreamer/elements/zst_sc6f0_source.h"

zst_element_t*
zst_sc6f0_source_create(void)
{
    ZST_LOG_WARN("sc6f0src", "SC6F0 source requested, but V4L2 or ALSA support is disabled in this build.");
    return NULL;
}

#endif

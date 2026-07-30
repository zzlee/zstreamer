/*=============================================================================
    zst_builtins.c — Built-in element registration for the Element Public API

    Provides the strong implementation of zst_register_builtin_elements() that
    lives in libzstreamer-elements.  Each element constructor is referenced
    via a direct (strong) extern declaration — no weak symbols — so the
    linker naturally pulls in the element .o files from the static archive,
    and --as-needed keeps libzstreamer-elements.so in the NEEDED list.

    Because this function is ONLY defined here (not in the core library),
    any app that calls zst_register_builtin_elements() creates a strong
    reference chain:

        app → zst_register_builtin_elements (this file)
            → zst_video_test_src_create (direct call, strong ref)

    This ensures both static and shared linking work without
    --whole-archive or --no-as-needed flags.
=============================================================================*/

#define _POSIX_C_SOURCE 200809L

#include "zst_plugin.h"
#ifdef HAS_FFMPEG
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#endif
#include "zstreamer/elements/zst_st2110_20.h"
#include "zstreamer/elements/zst_st2110_21.h"
#include "zstreamer/elements/zst_st2110_30.h"
#include "zst_element_factory.h"
#include <string.h>
#include <stdlib.h>

/*──────────────────────────────────────────────────────────────────────────
  Strong (non-weak) forward declarations for every element constructor in
  libzstreamer-elements.  These force the linker to include the element
  object files when zst_register_builtin_elements() is reachable.
──────────────────────────────────────────────────────────────────────────*/
zst_element_t* zst_queue_element_create(const char* name);

zst_element_t* zst_file_source_create(const char* path);
#ifdef HAS_FFMPEG
zst_element_t* zst_http_source_create(const char* url);
#endif
zst_element_t* zst_file_sink_create(const char* path);
zst_element_t* zst_fake_sink_create(void);
#ifdef HAS_V4L2
zst_element_t* zst_v4l2_source_create(void);
zst_element_t* zst_v4l2_sink_create(void);
#endif
#ifdef HAS_ALSA
zst_element_t* zst_alsa_source_create(void);
zst_element_t* zst_alsa_sink_create(void);
#endif
#ifdef HAS_X264
zst_element_t* zst_x264_encoder_create(void);
#endif
#ifdef HAS_X265
zst_element_t* zst_x265_encoder_create(void);
#endif
#ifdef HAS_FFMPEG
zst_element_t* zst_h264_decoder_create(void);
zst_element_t* zst_h265_encoder_create(void);
zst_element_t* zst_h265_decoder_create(void);
zst_element_t* zst_vp8_decoder_create(void);
zst_element_t* zst_vp9_decoder_create(void);
zst_element_t* zst_vp8_encoder_create(void);
zst_element_t* zst_vp9_encoder_create(void);
#endif
#ifdef HAS_JETSON
zst_element_t* zst_nv_video_encoder_create(void);
zst_element_t* zst_nv_video_decoder_create(void);
#endif
#ifdef HAS_ONEAPI_ENCODER
zst_element_t* zst_oneapi_video_encoder_create(void);
#endif
#ifdef HAS_ONEAPI_DECODER
zst_element_t* zst_oneapi_video_decoder_create(void);
#endif
#ifdef HAS_VAAPI_ENCODER
zst_element_t* zst_vaapi_video_encoder_create(void);
#endif
#ifdef HAS_VAAPI_DECODER
zst_element_t* zst_vaapi_video_decoder_create(void);
#endif
#ifdef HAS_FFMPEG
zst_element_t* zst_aac_encoder_create(void);
zst_element_t* zst_aac_decoder_create(void);
zst_element_t* zst_opus_encoder_create(void);
zst_element_t* zst_opus_decoder_create(void);
zst_element_t* zst_mp4_muxer_create(void);
zst_element_t* zst_hls_sink_create(void);
zst_element_t* zst_video_scaler_create(int target_width, int target_height, const char* target_pixel_format);
zst_element_t* zst_audio_resampler_create(int target_sample_rate, int target_channels, const char* target_format);
#endif
zst_element_t* zst_video_test_src_create(void);
zst_element_t* zst_audio_test_src_create(void);
#ifdef HAS_FREETYPE
zst_element_t* zst_text_overlay_create(const char* text);
zst_element_t* zst_text_source_create(void);
#endif
zst_element_t* zst_srt_parser_create(const char* path);
zst_element_t* zst_net_source_create(void);
zst_element_t* zst_net_sink_create(void);
zst_element_t* zst_sc6f0_source_create(void);
zst_element_t* zst_audio_mixer_create(void);
#ifdef HAS_FFMPEG
zst_element_t* zst_rtsp_source_create(const char* url);
zst_element_t* zst_rtsp_sink_create(void);
#endif
zst_element_t* zst_rtsp_server_create(void);
zst_element_t* zst_sdp_muxer_create(void);
zst_element_t* zst_rtp_payloader_create(void);
zst_element_t* zst_rtp_depayloader_create(void);
#ifdef HAS_FFMPEG
zst_element_t* zst_rtmp_source_create(const char* url);
zst_element_t* zst_rtmp_sink_create(void);
#endif
#ifdef HAS_SRT
zst_element_t* zst_srt_source_create(void);
zst_element_t* zst_srt_sink_create(void);
#endif
#ifdef HAS_FFMPEG
zst_element_t* zst_mpegts_muxer_create(void);
zst_element_t* zst_mpegts_demuxer_create(void);
zst_element_t* zst_mp4_demuxer_create(void);
#endif
zst_element_t* zst_nv_video_scaler_create(void);
#ifdef HAS_GLSINK
zst_element_t* zst_gl_sink_create(void);
#endif
#ifdef HAS_GLCOMPSINK
zst_element_t* zst_gl_comp_sink_create(void);
#endif
#ifdef HAS_IPP_COMP_SINK
zst_element_t* zst_ipp_comp_sink_create(void);
#endif
#ifdef HAS_X11SINK
zst_element_t* zst_x11_sink_create(const char* display);
#endif
#ifdef HAS_WEBRTC
zst_element_t* zst_webrtc_endpoint_create(void);
#endif
zst_element_t* zst_ws_server_element_create(void);
zst_element_t* zst_http_server_element_create(void);
zst_element_t* zst_st2110_20_payloader_create(void);
zst_element_t* zst_st2110_20_depayloader_create(void);
zst_element_t* zst_st2110_30_payloader_create(void);
zst_element_t* zst_st2110_30_depayloader_create(void);
zst_element_t* zst_st2110_40_payloader_create(void);
zst_element_t* zst_st2110_40_depayloader_create(void);
zst_element_t* zst_ptp_clock_create(void);
zst_element_t* zst_st2110_redundancy_mux_create(void);
zst_element_t* zst_st2110_redundancy_demux_create(void);
#ifdef HAS_SVT_JPEGXS
zst_element_t* zst_st2110_22_payloader_create(void);
zst_element_t* zst_st2110_22_depayloader_create(void);
#endif
zst_element_t* zst_st2110_fec_encoder_create(void);
zst_element_t* zst_st2110_fec_decoder_create(void);


/*──────────────────────────────────────────────────────────────────────────
  Pad template tables (used by descriptor tables below).
──────────────────────────────────────────────────────────────────────────*/
static const zst_pad_template_t g_pad_src[]          = { { "src", ZST_PAD_SRC, ZST_PAD_ALWAYS, "ANY" } };
static const zst_pad_template_t g_pad_sink[]         = { { "sink", ZST_PAD_SINK, ZST_PAD_ALWAYS, "ANY" } };
static const zst_pad_template_t g_pad_filter[]       = {
    { "sink", ZST_PAD_SINK, ZST_PAD_ALWAYS, "ANY" }, { "src", ZST_PAD_SRC, ZST_PAD_ALWAYS, "ANY" }
};
static const zst_pad_template_t g_pad_video_src[]    = { { "src", ZST_PAD_SRC, ZST_PAD_ALWAYS, "video/x-raw" } };
static const zst_pad_template_t g_pad_video_sink[]   = { { "sink_%u", ZST_PAD_SINK, ZST_PAD_ALWAYS, "video/x-raw" } };
static const zst_pad_template_t g_pad_audio_src[]    = { { "src", ZST_PAD_SRC, ZST_PAD_ALWAYS, "audio/x-raw" } };
static const zst_pad_template_t g_pad_audio_mixer[] = {
    { "src", ZST_PAD_SRC, ZST_PAD_ALWAYS, "audio/x-raw" },
    { "sink_%u", ZST_PAD_SINK, ZST_PAD_ALWAYS, "audio/x-raw" }
};
static const zst_pad_template_t g_pad_srt_parser[]   = { { "src", ZST_PAD_SRC, ZST_PAD_ALWAYS, "text/x-raw" } };

#ifdef HAS_WEBRTC
static const zst_pad_template_t g_pad_webrtc_endpoint[] = {
    { "sink", ZST_PAD_SINK, ZST_PAD_ALWAYS,
      "video/x-h264;video/x-h265;audio/x-aac;audio/opus;application/octet-stream" },
    { "src",  ZST_PAD_SRC,  ZST_PAD_ALWAYS,
      "video/x-h264;video/x-h265;audio/x-aac;audio/opus;application/octet-stream" }
};
#endif

#ifdef HAS_X264
static const zst_pad_template_t g_pad_x264enc[] = {
    { "sink", ZST_PAD_SINK, ZST_PAD_ALWAYS, "video/x-raw" }, { "src", ZST_PAD_SRC, ZST_PAD_ALWAYS, "video/x-h264" }
};
#endif
#ifdef HAS_X265
static const zst_pad_template_t g_pad_x265enc[] = {
    { "sink", ZST_PAD_SINK, ZST_PAD_ALWAYS, "video/x-raw" }, { "src", ZST_PAD_SRC, ZST_PAD_ALWAYS, "video/x-h265" }
};
#endif
#ifdef HAS_ONEAPI_ENCODER
static const zst_pad_template_t g_pad_oneapienc[] = {
    { "sink", ZST_PAD_SINK, ZST_PAD_ALWAYS, "video/x-raw" },
    { "src", ZST_PAD_SRC, ZST_PAD_ALWAYS, "video/x-h264" },
    { "src", ZST_PAD_SRC, ZST_PAD_ALWAYS, "video/x-h265" }
};
#endif
#ifdef HAS_ONEAPI_DECODER
static const zst_pad_template_t g_pad_oneapidec[] = {
    { "sink", ZST_PAD_SINK, ZST_PAD_ALWAYS, "video/x-h264" },
    { "sink", ZST_PAD_SINK, ZST_PAD_ALWAYS, "video/x-h265" },
    { "src", ZST_PAD_SRC, ZST_PAD_ALWAYS, "video/x-raw" }
};
#endif
#ifdef HAS_VAAPI_ENCODER
static const zst_pad_template_t g_pad_vaapienc[] = {
    { "sink", ZST_PAD_SINK, ZST_PAD_ALWAYS, "video/x-raw" },
    { "src", ZST_PAD_SRC, ZST_PAD_ALWAYS, "video/x-h264" },
    { "src", ZST_PAD_SRC, ZST_PAD_ALWAYS, "video/x-h265" }
};
#endif
#ifdef HAS_VAAPI_DECODER
static const zst_pad_template_t g_pad_vaapidec[] = {
    { "sink", ZST_PAD_SINK, ZST_PAD_ALWAYS, "video/x-h264" },
    { "sink", ZST_PAD_SINK, ZST_PAD_ALWAYS, "video/x-h265" },
    { "src", ZST_PAD_SRC, ZST_PAD_ALWAYS, "video/x-raw" }
};
#endif
#ifdef HAS_FFMPEG
static const zst_pad_template_t g_pad_h264dec[] = {
    { "sink", ZST_PAD_SINK, ZST_PAD_ALWAYS, "video/x-h264" }, { "src", ZST_PAD_SRC, ZST_PAD_ALWAYS, "video/x-raw" }
};
static const zst_pad_template_t g_pad_h265enc[] = {
    { "sink", ZST_PAD_SINK, ZST_PAD_ALWAYS, "video/x-raw" }, { "src", ZST_PAD_SRC, ZST_PAD_ALWAYS, "video/x-h265" }
};
static const zst_pad_template_t g_pad_h265dec[] = {
    { "sink", ZST_PAD_SINK, ZST_PAD_ALWAYS, "video/x-h265" }, { "src", ZST_PAD_SRC, ZST_PAD_ALWAYS, "video/x-raw" }
};
static const zst_pad_template_t g_pad_vp8enc[] = {
    { "sink", ZST_PAD_SINK, ZST_PAD_ALWAYS, "video/x-raw;video/x-vp8" }, { "src", ZST_PAD_SRC, ZST_PAD_ALWAYS, "video/x-vp8" }
};
static const zst_pad_template_t g_pad_vp8dec[] = {
    { "sink", ZST_PAD_SINK, ZST_PAD_ALWAYS, "video/x-vp8" }, { "src", ZST_PAD_SRC, ZST_PAD_ALWAYS, "video/x-raw" }
};
static const zst_pad_template_t g_pad_vp9enc[] = {
    { "sink", ZST_PAD_SINK, ZST_PAD_ALWAYS, "video/x-raw;video/x-vp9" }, { "src", ZST_PAD_SRC, ZST_PAD_ALWAYS, "video/x-vp9" }
};
static const zst_pad_template_t g_pad_vp9dec[] = {
    { "sink", ZST_PAD_SINK, ZST_PAD_ALWAYS, "video/x-vp9" }, { "src", ZST_PAD_SRC, ZST_PAD_ALWAYS, "video/x-raw" }
};
static const zst_pad_template_t g_pad_aacenc[] = {
    { "sink", ZST_PAD_SINK, ZST_PAD_ALWAYS, "audio/x-raw" }, { "src", ZST_PAD_SRC, ZST_PAD_ALWAYS, "audio/x-aac" }
};
static const zst_pad_template_t g_pad_aacdec[] = {
    { "sink", ZST_PAD_SINK, ZST_PAD_ALWAYS, "audio/x-aac" }, { "src", ZST_PAD_SRC, ZST_PAD_ALWAYS, "audio/x-raw" }
};
static const zst_pad_template_t g_pad_opusenc[] = {
    { "sink", ZST_PAD_SINK, ZST_PAD_ALWAYS, "audio/x-raw" }, { "src", ZST_PAD_SRC, ZST_PAD_ALWAYS, "audio/x-opus" }
};
static const zst_pad_template_t g_pad_opusdec[] = {
    { "sink", ZST_PAD_SINK, ZST_PAD_ALWAYS, "audio/x-opus" }, { "src", ZST_PAD_SRC, ZST_PAD_ALWAYS, "audio/x-raw" }
};
#endif

static const zst_pad_template_t g_pad_video_filter[] = {
    { "sink", ZST_PAD_SINK, ZST_PAD_ALWAYS, "video/x-raw" }, { "src", ZST_PAD_SRC, ZST_PAD_ALWAYS, "video/x-raw" }
};
static const zst_pad_template_t g_pad_audio_filter[] = {
    { "sink", ZST_PAD_SINK, ZST_PAD_ALWAYS, "audio/x-raw" }, { "src", ZST_PAD_SRC, ZST_PAD_ALWAYS, "audio/x-raw" }
};

#ifdef HAS_FFMPEG
static const zst_pad_template_t g_pad_mp4mux[]       = {
    { "video", ZST_PAD_SINK, ZST_PAD_ALWAYS, "video/x-h264" }, { "audio", ZST_PAD_SINK, ZST_PAD_ALWAYS, "audio/x-aac" }, { "src", ZST_PAD_SRC, ZST_PAD_ALWAYS, "video/quicktime" }
};
#endif
static const zst_pad_template_t g_pad_net_src[] = {
    { "src", ZST_PAD_SRC, ZST_PAD_ALWAYS, "application/octet-stream" }
};
static const zst_pad_template_t g_pad_net_sink[] = {
    { "sink", ZST_PAD_SINK, ZST_PAD_ALWAYS, "application/octet-stream" }
};
static const zst_pad_template_t g_pad_sc6f0src[] = {
    { "video_%u", ZST_PAD_SRC, ZST_PAD_SOMETIMES, "video/x-raw;ANY" },
    { "audio_%u", ZST_PAD_SRC, ZST_PAD_SOMETIMES, "audio/x-raw;ANY" }
};
#ifdef HAS_FREETYPE
static const zst_pad_template_t g_pad_textoverlay[]  = {
    { "sink", ZST_PAD_SINK, ZST_PAD_ALWAYS, "video/x-raw" }, { "text", ZST_PAD_SINK, ZST_PAD_ALWAYS, "text/x-raw" }, { "src", ZST_PAD_SRC, ZST_PAD_ALWAYS, "video/x-raw" }
};
#endif
#ifdef HAS_FFMPEG
static const zst_pad_template_t g_pad_rtsp_src[]     = {
    { "video", ZST_PAD_SRC, ZST_PAD_ALWAYS, "video/x-h264" }, { "audio", ZST_PAD_SRC, ZST_PAD_ALWAYS, "audio/x-aac" }
};
static const zst_pad_template_t g_pad_rtsp_sink[]    = {
    { "video", ZST_PAD_SINK, ZST_PAD_ALWAYS, "video/x-h264" }, { "audio", ZST_PAD_SINK, ZST_PAD_ALWAYS, "audio/x-aac" }
};
#endif
static const zst_pad_template_t g_pad_rtsp_server[]  = {
    { "video_%u", ZST_PAD_SINK, ZST_PAD_ALWAYS, "video/x-h264" }, { "audio_%u", ZST_PAD_SINK, ZST_PAD_ALWAYS, "audio/x-aac" }
};
static const zst_pad_template_t g_pad_sdpmuxer[] = {
    { "video", ZST_PAD_SINK, ZST_PAD_ALWAYS, "video/x-h264;video/x-h265;video/x-vp8;video/x-vp9;video/x-av1" },
    { "audio", ZST_PAD_SINK, ZST_PAD_ALWAYS, "audio/aac;audio/opus;audio/PCMU;audio/PCMA;audio/L16" },
    { "src",   ZST_PAD_SRC,  ZST_PAD_ALWAYS, "application/sdp" }
};
static const zst_pad_template_t g_pad_rtppay[] = {
    { "sink", ZST_PAD_SINK, ZST_PAD_ALWAYS, "video/x-h264;video/x-h265;audio/x-aac;audio/aac;audio/x-raw" },
    { "src",  ZST_PAD_SRC,  ZST_PAD_ALWAYS, "application/x-rtp" }
};
static const zst_pad_template_t g_pad_rtpdepay[] = {
    { "sink", ZST_PAD_SINK, ZST_PAD_ALWAYS, "application/x-rtp" },
    { "src",  ZST_PAD_SRC,  ZST_PAD_ALWAYS, "video/x-h264;video/x-h265;audio/x-aac;audio/aac;audio/x-raw" }
};

#ifdef HAS_FFMPEG
static const zst_pad_template_t g_pad_rtmp_src[]     = {
    { "video", ZST_PAD_SRC, ZST_PAD_ALWAYS, "video/x-h264" },
    { "audio", ZST_PAD_SRC, ZST_PAD_ALWAYS, "audio/x-aac" }
};

static const zst_pad_template_t g_pad_rtmp_sink[]    = {
    { "video", ZST_PAD_SINK, ZST_PAD_ALWAYS, "video/x-h264" },
    { "audio", ZST_PAD_SINK, ZST_PAD_ALWAYS, "audio/x-aac" }
};

static const zst_pad_template_t g_pad_tsmux[]       = {
    { "video", ZST_PAD_SINK, ZST_PAD_ALWAYS, "video/x-h264" }, { "audio", ZST_PAD_SINK, ZST_PAD_ALWAYS, "audio/x-aac" }, { "src", ZST_PAD_SRC, ZST_PAD_ALWAYS, "video/mpegts" }
};

static const zst_pad_template_t g_pad_tsdemux[]     = {
    { "sink", ZST_PAD_SINK, ZST_PAD_ALWAYS, "video/mpegts" },
    { "video_%u", ZST_PAD_SRC, ZST_PAD_SOMETIMES, "video/x-h264;video/x-h265;video/mpeg2;ANY" },
    { "audio_%u", ZST_PAD_SRC, ZST_PAD_SOMETIMES, "audio/aac;audio/mpeg;audio/ac3;ANY" },
    { "data_%u", ZST_PAD_SRC, ZST_PAD_SOMETIMES, "ANY" }
};

static const zst_pad_template_t g_pad_mp4demux[]     = {
    { "sink", ZST_PAD_SINK, ZST_PAD_ALWAYS, "video/quicktime;video/mp4;ANY" },
    { "video_%u", ZST_PAD_SRC, ZST_PAD_SOMETIMES, "video/x-h264;video/x-h265;video/mpeg4;video/x-vp9;video/x-av1;ANY" },
    { "audio_%u", ZST_PAD_SRC, ZST_PAD_SOMETIMES, "audio/aac;audio/mpeg;audio/opus;audio/vorbis;audio/flac;ANY" },
    { "data_%u", ZST_PAD_SRC, ZST_PAD_SOMETIMES, "ANY" }
};
#endif

/*──────────────────────────────────────────────────────────────────────────
  Property spec tables (for elements that expose typed properties).
──────────────────────────────────────────────────────────────────────────*/
static const zst_property_spec_t g_builtin_filesrc_props[] = {
    { "path", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "Input file path" },
    { "chunk-size", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "4096", "Maximum bytes to read per buffer" },
    { "loop", ZST_PROPERTY_BOOL, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "false", "Loop back to the start at EOF" },
    { "offset", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "0", "Initial byte offset" },
    { "length", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "-1", "Maximum number of bytes to read; -1 means unlimited" }
};

#ifdef HAS_FFMPEG
static const zst_property_spec_t g_builtin_httpsrc_props[] = {
    { "url", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "URL of the HTTP/HTTPS resource" },
    { "uri", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "Alias for url" },
    { "user-agent", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "zstreamer/0.1.0", "User-Agent header value" },
    { "headers", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "Custom HTTP request headers" },
    { "timeout", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "5000", "Connection & read timeout in milliseconds" },
    { "chunk-size", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "4096", "Maximum bytes to read per buffer" },
    { "reconnect", ZST_PROPERTY_BOOL, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "false", "Reconnect on stream loss" },
    { "reconnect-delay-ms", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "500", "Delay between reconnect attempts in milliseconds" },
    { "max-reconnect-attempts", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "-1", "Maximum reconnect attempts; -1 means unlimited" }
};
#endif

static const zst_property_spec_t g_builtin_filesink_props[] = {
    { "path", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "Output file path" },
    { "location", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "Alias for path" }
};

static const zst_property_spec_t g_builtin_fakesink_props[] = {
    { "drop-probability", ZST_PROPERTY_DOUBLE, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "0.0", "Probability in [0.0, 1.0] of dropping a buffer without counting it" },
    { "total-buffers", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE, "0", "Number of buffers received since open" },
    { "total-bytes", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE, "0", "Number of bytes received since open" },
    { "bits-per-second", ZST_PROPERTY_BOOL, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE | ZST_PROPERTY_RUNTIME, "false", "Log received bitrate statistics every log-period seconds" },
    { "log-period", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE | ZST_PROPERTY_RUNTIME, "1", "Statistics log period in seconds" },
    { "push-per-second", ZST_PROPERTY_BOOL, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE | ZST_PROPERTY_RUNTIME, "false", "Log received buffer push-rate statistics every log-period seconds" }
};



static const zst_property_spec_t g_builtin_sc6f0src_props[] = {
    { "media-device", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "Media device node path" },
    { "platform-id", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "DNTX", "SC6F0 platform identifier (DNTX or SDI1)" },
    { "mock-mode", ZST_PROPERTY_BOOL, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "true", "Operate in mock mode with synthetic signal emulation" },
    { "trigger-signal", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE | ZST_PROPERTY_RUNTIME, "1080p", "Trigger a mock signal state change: 1080p, 720p, or none" },
    { "subdev-path", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "/dev/v4l-subdev0", "V4L2 sub-device path for HDMI/DVI Rx" },
    { "vpss-csc-path", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "/dev/v4l-subdev1", "VPSS CSC sub-device path" },
    { "audio-sysfs-path", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "/sys/devices/platform/amba_pl@0/b0070000.v_hdmi_rx_ss/audio_format", "HDMI audio sysfs status path" }
};

#ifdef HAS_V4L2
static const zst_property_spec_t g_builtin_v4l2src_props[] = {
    { "device", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "/dev/video0", "V4L2 capture device path" },
    { "width", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "640", "Capture width" },
    { "height", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "480", "Capture height" },
    { "pixel-format", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "YUYV", "Capture pixel format (YUYV, YUV420P/I420)" },
    { "memory-type", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "mmap", "V4L2 memory mode: mmap, userptr, dmabuf, mmap-export" }
};
#endif

#ifdef HAS_FFMPEG
static const zst_property_spec_t g_builtin_rtmpsrc_props[] = {
    { "url", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "RTMP Endpoint URL (supports rtmp://user:pass@host/app/stream)" },
    { "rtmp_url", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "Alias for url" },
    { "live", ZST_PROPERTY_BOOL, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "true", "Use live RTMP mode instead of recorded/VOD" },
    { "buffer-time", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "3000", "RTMP client buffer time in milliseconds" },
    { "swf-url", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "Optional SWF URL for legacy RTMP authentication" },
    { "reconnect", ZST_PROPERTY_BOOL, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "false", "Reconnect on stream loss" },
    { "reconnect-delay-ms", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "500", "Delay between reconnect attempts" },
    { "max-reconnect-attempts", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "-1", "Maximum reconnect attempts; -1 means unlimited" }
};

static const zst_property_spec_t g_builtin_rtmpsink_props[] = {
    { "url", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "RTMP Destination URL" },
    { "rtmp_url", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "Alias for url" },
    { "live", ZST_PROPERTY_BOOL, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "true", "Use live RTMP mode" },
    { "reconnect", ZST_PROPERTY_BOOL, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "false", "Reconnect on publish failure" },
    { "reconnect-delay-ms", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "500", "Delay between reconnect attempts" },
    { "max-reconnect-attempts", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "-1", "Maximum reconnect attempts; -1 means unlimited" }
};
#endif

#ifdef HAS_SRT
static const zst_property_spec_t g_builtin_srtsrc_props[] = {
    { "uri", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "SRT Connection URI" },
    { "host", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "127.0.0.1", "SRT peer host (caller/rendezvous modes)" },
    { "port", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "9000", "SRT port" },
    { "mode", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "caller", "SRT connection mode (caller, listener, rendezvous)" },
    { "latency", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "120", "SRT latency in milliseconds" },
    { "passphrase", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "SRT AES encryption passphrase" },
    { "pbkeylen", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "16", "SRT AES key length (16, 24, 32)" },
    { "streamid", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "SRT stream ID" },
    { "payload-size", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "1316", "SRT packet payload size" }
};

static const zst_property_spec_t g_builtin_srtsink_props[] = {
    { "uri", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "SRT Destination URI" },
    { "host", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "127.0.0.1", "SRT peer host (caller/rendezvous modes)" },
    { "port", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "9000", "SRT port" },
    { "mode", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "caller", "SRT connection mode (caller, listener, rendezvous)" },
    { "latency", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "120", "SRT latency in milliseconds" },
    { "passphrase", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "SRT AES encryption passphrase" },
    { "pbkeylen", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "16", "SRT AES key length (16, 24, 32)" },
    { "streamid", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "SRT stream ID" },
    { "payload-size", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "1316", "SRT packet payload size" }
};
#endif

#ifdef HAS_ONEAPI_ENCODER
static const zst_property_spec_t g_builtin_oneapienc_props[] = {
    { "codec", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "h264", "h264 or h265" },
    { "preset", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "balanced", "speed, balanced, quality" },
    { "bitrate", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "4000000", "Target bitrate in bits/sec" },
    { "gop-size", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "30", "GOP/keyframe interval" },
    { "profile", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "main", "Codec profile" },
    { "level", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "Codec level (reserved for backend support)" },
    { "fps", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "30/1", "Frame rate as integer or num/den" }
};
#endif
#ifdef HAS_ONEAPI_DECODER
static const zst_property_spec_t g_builtin_oneapidec_props[] = {
    { "codec", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "h264", "h264 or h265" }
};
#endif

#ifdef HAS_VAAPI_ENCODER
static const zst_property_spec_t g_builtin_vaapienc_props[] = {
    { "device", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "/dev/dri/renderD128", "DRM render node path" },
    { "codec", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "h264", "h264 or h265" },
    { "preset", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "balanced", "speed, balanced, quality" },
    { "bitrate", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "4000000", "Target bitrate in bits/sec" },
    { "gop-size", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "30", "GOP/keyframe interval" },
    { "profile", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "main", "Codec profile" },
    { "level", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "Codec level" },
    { "fps", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "30/1", "Frame rate as integer or num/den" },
    { "rate-control", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "VAAPI rate control mode (empty = backend default)" }
};
#endif
#ifdef HAS_VAAPI_DECODER
static const zst_property_spec_t g_builtin_vaapidec_props[] = {
    { "device", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "/dev/dri/renderD128", "DRM render node path" },
    { "codec", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "h264", "h264 or h265" },
    { "memory-type", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "cpu", "Output memory type: cpu, dmabuf" }
};
#endif

#ifdef HAS_FFMPEG
static const zst_property_spec_t g_builtin_rtspsrc_props[] = {
    { "url", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "RTSP URL" },
    { "rtsp_url", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "Alias for url" },
    { "username", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "RTSP username" },
    { "password", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "RTSP password" },
    { "transport", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "tcp", "RTSP transport: tcp, udp, or multicast" },
    { "buffer-size", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "16384", "Receive buffer size" },
    { "reconnect", ZST_PROPERTY_BOOL, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "false", "Reconnect on transport loss" },
    { "reconnect-delay-ms", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "500", "Delay between reconnect attempts" },
    { "max-reconnect-attempts", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "-1", "Maximum reconnect attempts; -1 means unlimited" },
    { "keepalive-interval-sec", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "30", "RTSP OPTIONS keepalive interval in seconds" }
};
#ifdef HAS_HTTP_SERVER

static const zst_property_spec_t g_builtin_httpserver_props[] = {
    { "port", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "8080", "HTTP listen port" },
    { "document-root", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, ".", "HTTP document root directory" }
};
#endif

static const zst_property_spec_t g_builtin_rtspsink_props[] = {
    { "url", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "rtsp://0.0.0.0:8554/live", "RTSP listen URL" },
    { "listen-port", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "8554", "RTSP listen port" },
    { "mount-point", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "live", "RTSP mount point" },
    { "transport", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "tcp", "Preferred RTSP transport" },
    { "max-clients", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "1", "Maximum concurrent clients requested by the application" },
    { "rtcp-interval-ms", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "5000", "RTCP sender report interval in milliseconds" },
    { "udp-timestamp-pacing", ZST_PROPERTY_BOOL, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "true", "Pace UDP RTSP output according to buffer timestamps" },
    { "udp-pacing-tolerance-ms", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "5", "UDP timestamp pacing tolerance in milliseconds" },
    { "udp-pacing-reset-threshold-ms", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "2000", "UDP timestamp discontinuity threshold before resetting pacing" },
    { "udp-max-lateness-ms", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "0", "Drop UDP RTSP buffers later than this many milliseconds; 0 disables dropping" }
};

static const zst_property_spec_t g_builtin_h265enc_props[] = {
    { "preset", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "ultrafast", "FFmpeg/libx265 preset" },
    { "tune", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "zerolatency", "FFmpeg/libx265 tune" },
    { "crf", ZST_PROPERTY_DOUBLE, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "23.0", "Constant Rate Factor (0-51); used when bitrate is 0" },
    { "bitrate", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "0", "Target bitrate in bits/sec; 0 enables CRF mode" },
    { "gop-size", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "30", "GOP/keyframe interval" },
    { "keyint-min", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "1", "Minimum keyframe interval" },
    { "profile", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "main", "HEVC profile" },
    { "level", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "HEVC level (empty = encoder default)" }
};

static const zst_property_spec_t g_builtin_tsmux_props[] = {
    { "width", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "640", "Video width" },
    { "height", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "480", "Video height" },
    { "fps", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "30", "Video frame rate" },
    { "sample-rate", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "44100", "Audio sample rate" },
    { "channels", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "2", "Audio channels" },
    { "location", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "Output file path (optional)" }
};

static const zst_property_spec_t g_builtin_tsdemux_props[] = {
    { "location", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "Input file path (optional)" }
};

static const zst_property_spec_t g_builtin_mp4demux_props[] = {
    { "location", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "Input MP4 file path (optional; enables direct-file mode)" },
    { "real-time-pacing", ZST_PROPERTY_BOOL, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "false", "Make stream output at pts timing" }
};
#endif

static const zst_property_spec_t g_builtin_videotestsrc_props[] = {
    { "width", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "640", "Video width" },
    { "height", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "480", "Video height" },
    { "fps", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "30", "Video frame rate" },
    { "pattern", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "bars", "Synthetic pattern type" },
    { "pixel-format", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "YUV420P", "Pixel format" },
    { "num-buffers", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "-1", "Number of buffers to output before EOF" },
    { "loop", ZST_PROPERTY_BOOL, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "false", "Loop property" },
    { "use-clock", ZST_PROPERTY_BOOL, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "false", "Use pipeline clock for generated PTS" },
    { "real-time-pacing", ZST_PROPERTY_BOOL, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "false", "Pace generated frames in real time" }
};

static const zst_property_spec_t g_builtin_audiotestsrc_props[] = {
    { "sample-rate", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "44100", "Audio sample rate" },
    { "channels", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "2", "Audio channels count" },
    { "sample-format", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "S16LE", "Audio sample format" },
    { "wave", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "sine", "Audio wave type" },
    { "frequency", ZST_PROPERTY_DOUBLE, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "440.0", "Frequency of tone" },
    { "volume", ZST_PROPERTY_DOUBLE, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "0.8", "Audio volume level" },
    { "samples-per-buffer", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "1024", "Samples per output buffer" },
    { "num-samples", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "-1", "Number of samples to output before EOF" },
    { "num-buffers", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "-1", "Number of buffers to output before EOF" },
    { "loop", ZST_PROPERTY_BOOL, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "false", "Loop property" },
    { "use-clock", ZST_PROPERTY_BOOL, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "false", "Use pipeline clock for generated PTS" },
    { "real-time-pacing", ZST_PROPERTY_BOOL, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "false", "Pace generated audio buffers in real time" }
};

static const zst_property_spec_t g_builtin_audioresampler_props[] = {
    { "sample-rate", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "0", "Target sample rate; 0 = passthrough input rate" },
    { "channels", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "0", "Target channel count; 0 = passthrough input channels" },
    { "format", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "Target sample format (S16LE, F32LE, etc.); empty = passthrough" },
    { "asrc-mode", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "none", "ASRC mode: 'none' (fixed SRC) or 'pts' (PTS-based drift compensation)" },
    { "max-drift-ppm", ZST_PROPERTY_DOUBLE, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "1000", "Maximum expected drift in parts per million (default: 1000 = 0.1%)" },
    { "drift-check-interval", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "4", "Drift check interval in buffers" },
    { "rate-numer", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "0", "Rational target rate numerator (0 = use sample-rate)" },
    { "rate-denom", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "0", "Rational target rate denominator (0 = 1)" },
    { "total-input-samples", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE, "0", "Total input samples processed (read-only)" },
    { "total-output-samples", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE, "0", "Total output samples produced (read-only)" },
    { "drift-adjust-count", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE, "0", "Number of drift adjustments performed (read-only)" }
};

#ifdef HAS_X11SINK
#ifdef HAS_WEBRTC
static const zst_property_spec_t g_builtin_webrtc_endpoint_props[] = {
    { "stun-servers",       ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "Comma-separated list of STUN server URLs" },
    { "turn-servers",       ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "Comma-separated list of TURN server URLs" },
    { "ice-urls",           ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "Combined comma-separated ICE URLs (auto-detects STUN vs TURN)" },
    { "remote-sdp",         ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "Remote SDP offer or answer" },
    { "ice-state",          ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE, "new", "ICE connection state" },
    { "dtls-state",         ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE, "new", "DTLS handshake state" },
    { "sctp-state",         ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE, "new", "SCTP association state" },
    { "signalling-state",   ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE, "stable", "SDP signalling state" },
    { "negotiated",         ZST_PROPERTY_BOOL,   ZST_PROPERTY_READABLE, "false", "Whether SDP negotiation has completed" },
    { "local-sdp",          ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE, "", "Local SDP offer or answer (read-only)" }
};
#endif

static const zst_property_spec_t g_builtin_x11sink_props[] = {
    { "display",      ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "X11 display name; empty uses DISPLAY" },
    { "window-title", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "zstreamer X11 Sink", "Window title" },
    { "frame-count",  ZST_PROPERTY_UINT,   ZST_PROPERTY_READABLE, "0", "Number of rendered or discarded frames" }
};
#endif

#ifdef HAS_FREETYPE
static const zst_property_spec_t g_builtin_textoverlay_props[] = {
    { "text", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "Text to overlay" },
    { "timecode", ZST_PROPERTY_BOOL, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "false", "Show timecode" },
    { "font-size", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "24", "Font size" },
    { "font-path", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "Path to TTF font file" },
    { "x", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "10", "X coordinate" },
    { "y", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "10", "Y coordinate" }
};
#endif

static const zst_property_spec_t g_builtin_netsrc_props[] = {
    { "host", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "127.0.0.1", "Network host to bind or connect to" },
    { "port", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "5000", "Network port" },
    { "protocol", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "tcp", "Network protocol (tcp, udp, unix, tcp-server, unix-server)" },
    { "path", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "Unix domain socket path" },
    { "chunk-size", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "4096", "Chunk size in bytes to read at a time" },
    { "read-timeout", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "1000", "Read timeout in milliseconds" }
};

static const zst_property_spec_t g_builtin_netsink_props[] = {
    { "host", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "127.0.0.1", "Network host to connect to or listen on" },
    { "port", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "5000", "Network port" },
    { "protocol", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "tcp", "Network protocol (tcp, udp, udp-client, udp-server, unix, tcp-server, unix-server)" },
    { "path", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "Unix domain socket path" },
    { "write-timeout", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "1000", "Write timeout in milliseconds" },
    { "ttl", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "16", "Multicast TTL for UDP" },
    { "loop", ZST_PROPERTY_BOOL, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "true", "Enable UDP multicast loopback" },
    { "timestamp-pacing", ZST_PROPERTY_BOOL, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "false", "Pace UDP sends according to buffer timestamps" },
    { "pacing-tolerance-ms", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "5", "Timestamp pacing tolerance in milliseconds" },
    { "pacing-reset-threshold-ms", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "2000", "Timestamp discontinuity threshold before resetting pacing" },
    { "max-lateness-ms", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "0", "Drop UDP buffers later than this many milliseconds; 0 disables dropping" }
};

static const zst_property_spec_t g_builtin_rtppay_props[] = {
    { "codec", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "h264", "RTP payload codec: h264, h265, aac, pcm" },
    { "payload-type", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "96", "RTP payload type" },
    { "ssrc", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "1398030928", "RTP SSRC" },
    { "clock-rate", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "90000", "RTP clock rate" },
    { "sample-rate", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "90000", "Alias for clock-rate" },
    { "mtu", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "1200", "Maximum RTP payload bytes" },
    { "channels", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "2", "PCM channel count" },
    { "sample-size", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "2", "PCM bytes per sample" },
    { "seq", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "28672", "Next RTP sequence number" },
    { "packets", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE, "0", "RTP packets produced" },
    { "bytes", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE, "0", "RTP bytes produced" }
};

static const zst_property_spec_t g_builtin_rtpdepay_props[] = {
    { "codec", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "h264", "RTP payload codec: h264, h265, aac, pcm" },
    { "payload-type", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "96", "RTP payload type to accept" },
    { "ssrc", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "0", "Expected RTP SSRC; 0 accepts any SSRC" },
    { "clock-rate", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "90000", "RTP clock rate" },
    { "sample-rate", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "90000", "Alias for clock-rate" },
    { "channels", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "2", "PCM channel count" },
    { "sample-size", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "2", "PCM bytes per sample" },
    { "packets", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE, "0", "RTP packets consumed" },
    { "bytes", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE, "0", "RTP bytes consumed" },
    { "out-buffers", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE, "0", "Depayloaded buffers produced" },
    { "out-bytes", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE, "0", "Depayloaded bytes produced" },
    { "dropped-packets", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE, "0", "Malformed, mismatched, or discontinuous RTP packets" }
};

static const zst_property_spec_t g_builtin_sdpmuxer_props[] = {
    { "sdp", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE, "", "Generated SDP text" },
    { "address", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "127.0.0.1", "Connection address for c=/o= lines" },
    { "session-name", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "zstreamer", "SDP session name" },
    { "sdp-file", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "Optional path to write generated SDP" },
    { "video-codec", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "h264", "Video codec: h264, h265, vp8, vp9, or av1" },
    { "audio-codec", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "aac", "Audio codec: aac, opus, pcmu, pcma, or l16" },
    { "enable-video", ZST_PROPERTY_BOOL, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "true", "Include video media section" },
    { "enable-audio", ZST_PROPERTY_BOOL, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "false", "Include audio media section" },
    { "video-port", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "5004", "Video RTP port" },
    { "audio-port", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "5006", "Audio RTP port" },
    { "video-payload-type", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "96", "Video RTP payload type" },
    { "audio-payload-type", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "97", "Audio RTP payload type" },
    { "sample-rate", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "48000", "AAC sample rate" },
    { "channels", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "2", "AAC channel count" },
    { "emit-once", ZST_PROPERTY_BOOL, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "true", "Emit SDP only once" }
};

static const zst_property_spec_t g_builtin_rtspserver_props[] = {
    { "listen-port", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "8554", "RTSP server listen port" },
    { "listen_port", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "8554", "Alias for listen-port" },
    { "force-tcp", ZST_PROPERTY_BOOL, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "false", "Force RTP over RTSP/TCP interleaved transport" },
    { "force_tcp", ZST_PROPERTY_BOOL, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "false", "Alias for force-tcp" },
    { "multicast-address", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "239.255.42.42", "Default multicast destination group" },
    { "multicast-port-base", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "56000", "Default multicast RTP port for video; audio uses +2" },
    { "multicast-ttl", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "16", "Default multicast IP TTL" },
    { "session_count", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE, "0", "Number of active RTSP streaming sessions" },
    { "client_count", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE, "0", "Number of connected RTSP clients" },
    { "udp-timestamp-pacing", ZST_PROPERTY_BOOL, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "true", "Pace UDP RTSP output according to buffer timestamps" },
    { "udp_timestamp_pacing", ZST_PROPERTY_BOOL, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "true", "Alias for udp-timestamp-pacing" },
    { "udp-pacing-tolerance-ms", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "5", "Pacing tolerance in milliseconds" },
    { "udp_pacing_tolerance_ms", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "5", "Alias for udp-pacing-tolerance-ms" },
    { "udp-pacing-reset-threshold-ms", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "2000", "Discontinuity reset threshold in milliseconds" },
    { "udp_pacing_reset_threshold_ms", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "2000", "Alias for udp-pacing-reset-threshold-ms" },
    { "udp-max-lateness-ms", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "0", "Max lateness in milliseconds before packet drop (0=disabled)" },
    { "udp_max_lateness_ms", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "0", "Alias for udp-max-lateness-ms" }
};

static const zst_property_spec_t g_builtin_wsserver_props[] = {
    { "port", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "8000", "WebSocket server listen port" }
};


#ifdef HAS_FREETYPE
static const zst_property_spec_t g_builtin_textsource_props[] = {
    { "width", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "640", "Video width" },
    { "height", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "480", "Video height" },
    { "fps", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "30", "Video frame rate" },
    { "text", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "Hello", "Text content to display" },
    { "text-content", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "Hello", "Alias for text" },
    { "font-size", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "24", "Font size in pixels" },
    { "font_size", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "24", "Alias for font-size" },
    { "font-path", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "Path to TrueType font file" },
    { "font_path", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "Alias for font-path" },
    { "bg-color", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "black", "Background color name or #RRGGBB hex" },
    { "background-color", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "black", "Alias for bg-color" },
    { "text-color", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "white", "Text color name or #RRGGBB hex" },
    { "color", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "white", "Alias for text-color" },
    { "text_color", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "white", "Alias for text-color" },
    { "pixel-format", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "YUV420P", "Pixel format" },
    { "pixel_format", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "YUV420P", "Alias for pixel-format" },
    { "num-buffers", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "-1", "Number of buffers to output before EOF" },
    { "num_buffers", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "-1", "Alias for num-buffers" },
    { "loop", ZST_PROPERTY_BOOL, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "false", "Loop the source input" },
    { "use-clock", ZST_PROPERTY_BOOL, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "false", "Use pipeline clock" },
    { "do-timestamp", ZST_PROPERTY_BOOL, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "false", "Alias for use-clock" },
    { "x", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "10", "X coordinate offset" },
    { "y", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "10", "Y coordinate offset" }
};
#endif

#ifdef HAS_FFMPEG
static const zst_property_spec_t g_builtin_mp4mux_props[] = {
    { "width", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "640", "Video width" },
    { "height", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "480", "Video height" },
    { "fps", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "30", "Video frame rate" },
    { "framerate", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "30", "Alias for fps" },
    { "sample-rate", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "44100", "Audio sample rate" },
    { "rate", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "44100", "Alias for sample-rate" },
    { "channels", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "2", "Audio channels count" },
    { "location", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "Output file path" },
    { "path", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "Alias for location" }
};
#endif

/*──────────────────────────────────────────────────────────────────────────
  create_element callback — constructs an element by name using direct
  constructor calls (no weak symbols).
  ──────────────────────────────────────────────────────────────────────────*/
static zst_element_t*
create_builtin_element(const char* name)
{
    if (!name) return NULL;
    if (strcmp(name, "queue") == 0)        return zst_queue_element_create(NULL);
    if (strcmp(name, "filesrc") == 0)      return zst_file_source_create("");
#ifdef HAS_FFMPEG
    if (strcmp(name, "httpsrc") == 0)      return zst_http_source_create("");
#endif
    if (strcmp(name, "filesink") == 0)     return zst_file_sink_create("");
    if (strcmp(name, "fakesink") == 0)     return zst_fake_sink_create();
#ifdef HAS_V4L2
    if (strcmp(name, "v4l2src") == 0)      return zst_v4l2_source_create();
    if (strcmp(name, "v4l2sink") == 0)     return zst_v4l2_sink_create();
#endif
#ifdef HAS_ALSA
    if (strcmp(name, "alsasrc") == 0)      return zst_alsa_source_create();
    if (strcmp(name, "alsasink") == 0)     return zst_alsa_sink_create();
#endif
#ifdef HAS_X264
    if (strcmp(name, "x264enc") == 0)      return zst_x264_encoder_create();
#endif
#ifdef HAS_X265
    if (strcmp(name, "x265enc") == 0)      return zst_x265_encoder_create();
#endif
#ifdef HAS_FFMPEG
    if (strcmp(name, "h264dec") == 0)      return zst_h264_decoder_create();
    if (strcmp(name, "h265enc") == 0)      return zst_h265_encoder_create();
    if (strcmp(name, "h265dec") == 0)      return zst_h265_decoder_create();
    if (strcmp(name, "vp8dec") == 0)      return zst_vp8_decoder_create();
    if (strcmp(name, "vp9dec") == 0)      return zst_vp9_decoder_create();
    if (strcmp(name, "vp8enc") == 0)      return zst_vp8_encoder_create();
    if (strcmp(name, "vp9enc") == 0)      return zst_vp9_encoder_create();
#endif
#ifdef HAS_JETSON
    if (strcmp(name, "nvenc") == 0)        return zst_nv_video_encoder_create();
    if (strcmp(name, "nvdec") == 0)        return zst_nv_video_decoder_create();
#endif
#ifdef HAS_ONEAPI_ENCODER
    if (strcmp(name, "oneapienc") == 0 || strcmp(name, "oneapi_video_encoder") == 0) return zst_oneapi_video_encoder_create();
#endif
#ifdef HAS_ONEAPI_DECODER
    if (strcmp(name, "oneapidec") == 0 || strcmp(name, "oneapi_video_decoder") == 0) return zst_oneapi_video_decoder_create();
#endif
#ifdef HAS_VAAPI_ENCODER
    if (strcmp(name, "vaapienc") == 0 || strcmp(name, "vaapi_video_encoder") == 0) return zst_vaapi_video_encoder_create();
#endif
#ifdef HAS_VAAPI_DECODER
    if (strcmp(name, "vaapidec") == 0 || strcmp(name, "vaapi_video_decoder") == 0) return zst_vaapi_video_decoder_create();
#endif
#ifdef HAS_FFMPEG
    if (strcmp(name, "aacenc") == 0)       return zst_aac_encoder_create();
    if (strcmp(name, "aacdec") == 0)       return zst_aac_decoder_create();
    if (strcmp(name, "opusenc") == 0)       return zst_opus_encoder_create();
    if (strcmp(name, "opusdec") == 0)       return zst_opus_decoder_create();
    if (strcmp(name, "mp4mux") == 0)       return zst_mp4_muxer_create();
    if (strcmp(name, "hls_sink") == 0)     return zst_hls_sink_create();
    if (strcmp(name, "videoscaler") == 0)  return zst_video_scaler_create(0, 0, NULL);
    if (strcmp(name, "audioresampler") == 0) return zst_audio_resampler_create(0, 0, NULL);
#endif
    if (strcmp(name, "videotestsrc") == 0) return zst_video_test_src_create();
    if (strcmp(name, "audiotestsrc") == 0) return zst_audio_test_src_create();
    if (strcmp(name, "audiomixer") == 0)   return zst_audio_mixer_create();
#ifdef HAS_FREETYPE
    if (strcmp(name, "textoverlay") == 0)  return zst_text_overlay_create(NULL);
    if (strcmp(name, "textsource") == 0)   return zst_text_source_create();
#endif
    if (strcmp(name, "srt_parser") == 0)   return zst_srt_parser_create(NULL);
    if (strcmp(name, "netsrc") == 0)       return zst_net_source_create();
    if (strcmp(name, "netsink") == 0)      return zst_net_sink_create();
#ifdef HAS_FFMPEG
    if (strcmp(name, "rtspsrc") == 0)      return zst_rtsp_source_create(NULL);
    if (strcmp(name, "rtspsink") == 0)     return zst_rtsp_sink_create();
#endif
    if (strcmp(name, "rtsp_server") == 0)  return zst_rtsp_server_create();
    if (strcmp(name, "sdpmuxer") == 0 || strcmp(name, "sdpmux") == 0) return zst_sdp_muxer_create();
    if (strcmp(name, "rtppay") == 0 || strcmp(name, "rtp_payloader") == 0) return zst_rtp_payloader_create();
    if (strcmp(name, "rtpdepay") == 0 || strcmp(name, "rtp_depayloader") == 0 || strcmp(name, "rtpdepayload") == 0) return zst_rtp_depayloader_create();
#ifdef HAS_FFMPEG
    if (strcmp(name, "rtmpsrc") == 0)      return zst_rtmp_source_create(NULL);
    if (strcmp(name, "rtmpsink") == 0)     return zst_rtmp_sink_create();
#endif
#ifdef HAS_SRT
    if (strcmp(name, "srtsrc") == 0)       return zst_srt_source_create();
    if (strcmp(name, "srtsink") == 0)      return zst_srt_sink_create();
#endif
#ifdef HAS_FFMPEG
    if (strcmp(name, "tsmux") == 0)        return zst_mpegts_muxer_create();
    if (strcmp(name, "tsdemux") == 0)      return zst_mpegts_demuxer_create();
    if (strcmp(name, "mp4demux") == 0)     return zst_mp4_demuxer_create();
#endif
    if (strcmp(name, "nvvideoscaler") == 0) return zst_nv_video_scaler_create();
    if (strcmp(name, "ws_server") == 0)    return zst_ws_server_element_create();
    if (strcmp(name, "http_server") == 0)  return zst_http_server_element_create();
    if (strcmp(name, "sc6f0src") == 0)      return zst_sc6f0_source_create();
#ifdef HAS_GLSINK
    if (strcmp(name, "glsink") == 0)       return zst_gl_sink_create();
#endif
#ifdef HAS_GLCOMPSINK
    if (strcmp(name, "glcompsink") == 0)   return zst_gl_comp_sink_create();
#endif
#ifdef HAS_IPP_COMP_SINK
    if (strcmp(name, "ippcompsink") == 0)  return zst_ipp_comp_sink_create();
#endif
#ifdef HAS_X11SINK
    if (strcmp(name, "x11sink") == 0)      return zst_x11_sink_create(NULL);
#endif
#ifdef HAS_WEBRTC
    if (strcmp(name, "webrtc_endpoint") == 0 || strcmp(name, "webrtc") == 0) return zst_webrtc_endpoint_create();
#endif
    if (strcmp(name, "st2110_20_payloader") == 0) return zst_st2110_20_payloader_create();
    if (strcmp(name, "st2110_20_depayloader") == 0) return zst_st2110_20_depayloader_create();
    if (strcmp(name, "st2110_21_payloader") == 0) return zst_st2110_21_payloader_create();
    if (strcmp(name, "st2110_21_depayloader") == 0) return zst_st2110_21_depayloader_create();
    if (strcmp(name, "st2110_30_payloader") == 0) return zst_st2110_30_payloader_create();
    if (strcmp(name, "st2110_30_depayloader") == 0) return zst_st2110_30_depayloader_create();
    if (strcmp(name, "st2110_40_payloader") == 0) return zst_st2110_40_payloader_create();
    if (strcmp(name, "st2110_40_depayloader") == 0) return zst_st2110_40_depayloader_create();
    if (strcmp(name, "ptp_clock") == 0) return zst_ptp_clock_create();
    if (strcmp(name, "st2110_redundancy_mux") == 0) return zst_st2110_redundancy_mux_create();
    if (strcmp(name, "st2110_redundancy_demux") == 0) return zst_st2110_redundancy_demux_create();
#ifdef HAS_SVT_JPEGXS
    if (strcmp(name, "st2110_22_payloader") == 0) return zst_st2110_22_payloader_create();
    if (strcmp(name, "st2110_22_depayloader") == 0) return zst_st2110_22_depayloader_create();
#endif
    if (strcmp(name, "st2110_fec_encoder") == 0) return zst_st2110_fec_encoder_create();
    if (strcmp(name, "st2110_fec_decoder") == 0) return zst_st2110_fec_decoder_create();
    return NULL;
}

/*──────────────────────────────────────────────────────────────────────────
  Descriptor tables (one per element).
──────────────────────────────────────────────────────────────────────────*/
#define DESC(name, longname, category, desc, props, nprops, pads) \
    { name, longname, category, desc, "zstreamer", props, nprops, pads, sizeof(pads) / sizeof((pads)[0]), NULL }

static const zst_element_desc_t g_builtin_descs[] = {
    DESC("queue",   "Queue",            "Generic",      "Thread-safe buffering element",                                                                                        NULL,                           0, g_pad_filter),
    DESC("filesrc", "File Source",      "Source/File",  "Reads buffers from a local file",                                                                                      g_builtin_filesrc_props,        sizeof(g_builtin_filesrc_props) / sizeof(g_builtin_filesrc_props[0]), g_pad_src),
#ifdef HAS_FFMPEG
    DESC("httpsrc", "HTTP Source",      "Source/Network", "Reads buffers from HTTP/HTTPS server",                                                                                 g_builtin_httpsrc_props,        sizeof(g_builtin_httpsrc_props) / sizeof(g_builtin_httpsrc_props[0]), g_pad_src),
#endif
    DESC("filesink", "File Sink",       "Sink/File",    "Writes incoming buffers to a local file",                                                                              g_builtin_filesink_props,       sizeof(g_builtin_filesink_props) / sizeof(g_builtin_filesink_props[0]), g_pad_sink),
    DESC("fakesink", "Fake Sink",       "Sink/Test",    "Consumes buffers and records simple statistics",                                                                       g_builtin_fakesink_props,       sizeof(g_builtin_fakesink_props) / sizeof(g_builtin_fakesink_props[0]), g_pad_sink),
#ifdef HAS_V4L2
    DESC("v4l2src", "V4L2 Source",      "Source/Video", "Captures video from a V4L2 device",                                                                                    g_builtin_v4l2src_props,        sizeof(g_builtin_v4l2src_props) / sizeof(g_builtin_v4l2src_props[0]), g_pad_video_src),
    DESC("v4l2sink", "V4L2 Sink",       "Sink/Video",   "Outputs video to a V4L2 device",                                                                                       NULL,                           0, g_pad_sink),
#endif
#ifdef HAS_ALSA
    DESC("alsasrc", "ALSA Source",      "Source/Audio", "Captures audio from ALSA",                                                                                             NULL,                           0, g_pad_audio_src),
    DESC("alsasink", "ALSA Sink",       "Sink/Audio",   "Outputs audio to ALSA",                                                                                                NULL,                           0, g_pad_sink),
#endif
#ifdef HAS_X264
    DESC("x264enc", "H.264 Encoder",    "Codec/Encoder","Encodes raw video to H.264",                                                                                           NULL,                           0, g_pad_x264enc),
#endif
#ifdef HAS_X265
    DESC("x265enc", "H.265 Encoder",    "Codec/Encoder","Encodes raw video to H.265",                                                                                           NULL,                           0, g_pad_x265enc),
#endif
#ifdef HAS_FFMPEG
    DESC("h264dec", "H.264 Decoder",    "Codec/Decoder","Decodes H.264 video frames",                                                                                           NULL,                           0, g_pad_h264dec),
    DESC("h265enc", "H.265 Encoder",    "Codec/Encoder","Encodes raw video to H.265",                                                                                           g_builtin_h265enc_props,        sizeof(g_builtin_h265enc_props) / sizeof(g_builtin_h265enc_props[0]), g_pad_h265enc),
    DESC("h265dec", "H.265 Decoder",    "Codec/Decoder","Decodes H.265 video frames",                                                                                           NULL,                           0, g_pad_h265dec),
    DESC("vp8enc",  "VP8 Encoder",      "Codec/Encoder","Encodes raw video to VP8",                                                                                             NULL,                           0, g_pad_vp8enc),
    DESC("vp8dec",  "VP8 Decoder",      "Codec/Decoder","Decodes VP8 video frames",                                                                                             NULL,                           0, g_pad_vp8dec),
    DESC("vp9enc",  "VP9 Encoder",      "Codec/Encoder","Encodes raw video to VP9",                                                                                             NULL,                           0, g_pad_vp9enc),
    DESC("vp9dec",  "VP9 Decoder",      "Codec/Decoder","Decodes VP9 video frames",                                                                                             NULL,                           0, g_pad_vp9dec),
    DESC("aacenc",  "AAC Encoder",      "Codec/Encoder","Encodes raw audio to AAC",                                                                                             NULL,                           0, g_pad_aacenc),
    DESC("aacdec",  "AAC Decoder",      "Codec/Decoder","Decodes AAC audio frames",                                                                                             NULL,                           0, g_pad_aacdec),
    DESC("opusenc",  "Opus Encoder",     "Codec/Encoder","Encodes raw audio to Opus",                                                                                            NULL,                           0, g_pad_opusenc),
    DESC("opusdec",  "Opus Decoder",     "Codec/Decoder","Decodes Opus audio frames",                                                                                            NULL,                           0, g_pad_opusdec),
    DESC("mp4mux",  "MP4 Muxer",        "Muxer/File",   "Muxes encoded audio/video into MP4",                                                                                  g_builtin_mp4mux_props,         sizeof(g_builtin_mp4mux_props) / sizeof(g_builtin_mp4mux_props[0]), g_pad_mp4mux),
    DESC("hls_sink", "HLS Sink",        "Sink/File",    "Muxes encoded audio/video into HLS playlist",                                                                         NULL,                           0, g_pad_mp4mux),
    DESC("videoscaler", "Video Scaler", "Filter/Video", "Converts video resolution or pixel format",                                                                            NULL,                           0, g_pad_video_filter),
    DESC("audioresampler", "Audio Resampler", "Filter/Audio", "Converts audio sample rate, channels, or format; supports optional ASRC drift compensation",                 g_builtin_audioresampler_props, sizeof(g_builtin_audioresampler_props) / sizeof(g_builtin_audioresampler_props[0]), g_pad_audio_filter),
#endif
    DESC("videotestsrc", "Video Test Source", "Source/Test", "Generates synthetic video test patterns",                                                                         g_builtin_videotestsrc_props,   sizeof(g_builtin_videotestsrc_props) / sizeof(g_builtin_videotestsrc_props[0]), g_pad_video_src),
    DESC("audiotestsrc", "Audio Test Source", "Source/Test", "Generates synthetic audio test signals",                                                                          g_builtin_audiotestsrc_props,   sizeof(g_builtin_audiotestsrc_props) / sizeof(g_builtin_audiotestsrc_props[0]), g_pad_audio_src),
    DESC("audiomixer", "Audio Mixer", "Filter/Audio", "Mixes multiple audio inputs into a single output stream",                                                                  NULL,                           0, g_pad_audio_mixer),
#ifdef HAS_FREETYPE
    DESC("textoverlay", "Text Overlay", "Filter/Video", "Overlays text on video frames",                                                                                        g_builtin_textoverlay_props,    sizeof(g_builtin_textoverlay_props) / sizeof(g_builtin_textoverlay_props[0]), g_pad_textoverlay),
    DESC("textsource", "Text Source",   "Source/Video", "Generates video frames containing text",                                                                                g_builtin_textsource_props,     sizeof(g_builtin_textsource_props) / sizeof(g_builtin_textsource_props[0]), g_pad_video_src),
#endif
    DESC("srt_parser", "SRT Parser",    "Parser/Text",  "Parses SubRip subtitle data",                                                                                          NULL,                           0, g_pad_srt_parser),
    DESC("netsrc",  "Network Source",   "Source/Network","Receives buffers from TCP/UDP or Unix sockets",                                                                       g_builtin_netsrc_props,         sizeof(g_builtin_netsrc_props) / sizeof(g_builtin_netsrc_props[0]), g_pad_net_src),
    DESC("netsink", "Network Sink",     "Sink/Network", "Sends buffers to TCP/UDP or Unix sockets",                                                                             g_builtin_netsink_props,        sizeof(g_builtin_netsink_props) / sizeof(g_builtin_netsink_props[0]), g_pad_net_sink),
#ifdef HAS_FFMPEG
    DESC("rtspsrc", "RTSP Source",      "Source/Network","Receives audio/video from an RTSP endpoint",                                                                          g_builtin_rtspsrc_props,        sizeof(g_builtin_rtspsrc_props) / sizeof(g_builtin_rtspsrc_props[0]), g_pad_rtsp_src),
    DESC("rtspsink", "RTSP Sink",       "Sink/Network", "Publishes audio/video to an RTSP endpoint",                                                                             g_builtin_rtspsink_props,       sizeof(g_builtin_rtspsink_props) / sizeof(g_builtin_rtspsink_props[0]), g_pad_rtsp_sink),
#endif
    DESC("rtsp_server", "RTSP Server",  "Sink/Network", "Serves RTP streams over RTSP",                                                                                         g_builtin_rtspserver_props,     sizeof(g_builtin_rtspserver_props) / sizeof(g_builtin_rtspserver_props[0]), g_pad_rtsp_server),
#ifdef HAS_HTTP_SERVER
    DESC("http_server", "HTTP Server",  "Sink/Network", "Serves HTTP requests",                                                                                         g_builtin_httpserver_props,     sizeof(g_builtin_httpserver_props) / sizeof(g_builtin_httpserver_props[0]), NULL),
#endif
    DESC("sdpmuxer", "SDP Muxer",        "Muxer/RTP",    "Generates SDP descriptions for H.264/H.265/AAC RTP sessions",                                                          g_builtin_sdpmuxer_props,       sizeof(g_builtin_sdpmuxer_props) / sizeof(g_builtin_sdpmuxer_props[0]), g_pad_sdpmuxer),
    DESC("rtppay",   "RTP Payloader",    "RTP",          "Packetizes H.264/H.265/AAC/PCM buffers into RTP packet buffers",                                                        g_builtin_rtppay_props,         sizeof(g_builtin_rtppay_props) / sizeof(g_builtin_rtppay_props[0]), g_pad_rtppay),
    DESC("rtpdepay", "RTP Depayloader",  "RTP",          "Depayloads RTP packet buffers into H.264/H.265/AAC/PCM access units",                                                    g_builtin_rtpdepay_props,       sizeof(g_builtin_rtpdepay_props) / sizeof(g_builtin_rtpdepay_props[0]), g_pad_rtpdepay),
    DESC("st2110_20_payloader", "ST2110-20 Payloader", "RTP", "Packetizes raw video into ST2110-20 RTP packets", NULL, 0, g_pad_rtppay),
    DESC("st2110_20_depayloader", "ST2110-20 Depayloader", "RTP", "Depayloads ST2110-20 RTP packets into raw video", NULL, 0, g_pad_rtpdepay),
    DESC("st2110_21_payloader", "ST2110-21 Payloader", "RTP", "Packetizes compressed video (H.264/H.265) into ST2110-21 paced RTP packets", NULL, 0, g_pad_rtppay),
    DESC("st2110_21_depayloader", "ST2110-21 Depayloader", "RTP", "Depayloads ST2110-21 RTP packets into compressed video", NULL, 0, g_pad_rtpdepay),
    DESC("st2110_30_payloader", "ST2110-30 Payloader", "RTP", "Packetizes raw audio into ST2110-30 RTP packets", NULL, 0, g_pad_rtppay),
    DESC("st2110_30_depayloader", "ST2110-30 Depayloader", "RTP", "Depayloads ST2110-30 RTP packets into raw audio", NULL, 0, g_pad_rtpdepay),
    DESC("st2110_40_payloader", "ST2110-40 Payloader", "RTP", "Packetizes ancillary data into ST2110-40 RTP packets", NULL, 0, g_pad_rtppay),
    DESC("st2110_40_depayloader", "ST2110-40 Depayloader", "RTP", "Depayloads ST2110-40 RTP packets into ancillary data", NULL, 0, g_pad_rtpdepay),
#ifdef HAS_SVT_JPEGXS
    DESC("st2110_22_payloader", "ST2110-22 Payloader", "RTP", "Packetizes raw video into ST2110-22 RTP packets via SVT-JPEG-XS", NULL, 0, g_pad_rtppay),
    DESC("st2110_22_depayloader", "ST2110-22 Depayloader", "RTP", "Depayloads ST2110-22 RTP packets into raw video via SVT-JPEG-XS", NULL, 0, g_pad_rtpdepay),
#endif
    DESC("ptp_clock", "PTP Clock", "Timing", "IEEE 1588-2019 PTP client for media timing", NULL, 0, NULL),
    DESC("st2110_redundancy_mux", "ST2110 Redundancy Muxer", "Muxer", "Muxes single stream to redundant dual streams", NULL, 0, NULL),
    DESC("st2110_redundancy_demux", "ST2110 Redundancy Demuxer", "Demuxer", "Demuxes redundant dual streams to single stream", NULL, 0, NULL),

#ifdef HAS_FFMPEG
    DESC("rtmpsrc",  "RTMP Source",      "Source/Network","Receives audio/video from an RTMP endpoint",                                                                          g_builtin_rtmpsrc_props,        sizeof(g_builtin_rtmpsrc_props) / sizeof(g_builtin_rtmpsrc_props[0]), g_pad_rtmp_src),
    DESC("rtmpsink", "RTMP Sink",        "Sink/Network", "Publishes audio/video to an RTMP endpoint",                                                                             g_builtin_rtmpsink_props,       sizeof(g_builtin_rtmpsink_props) / sizeof(g_builtin_rtmpsink_props[0]), g_pad_rtmp_sink),
#endif
#ifdef HAS_SRT
    DESC("srtsrc",   "SRT Source",       "Source/Network","Receives buffers over Secure Reliable Transport (SRT)",                                                                 g_builtin_srtsrc_props,         sizeof(g_builtin_srtsrc_props) / sizeof(g_builtin_srtsrc_props[0]), g_pad_src),
    DESC("srtsink",  "SRT Sink",         "Sink/Network", "Sends buffers over Secure Reliable Transport (SRT)",                                                                   g_builtin_srtsink_props,        sizeof(g_builtin_srtsink_props) / sizeof(g_builtin_srtsink_props[0]), g_pad_sink),
#endif
#ifdef HAS_FFMPEG
    DESC("tsmux",    "MPEG-TS Muxer",    "Muxer/File",   "Muxes encoded audio/video into MPEG-TS (.ts)",                                                                         g_builtin_tsmux_props,          sizeof(g_builtin_tsmux_props) / sizeof(g_builtin_tsmux_props[0]), g_pad_tsmux),
    DESC("tsdemux",  "MPEG-TS Demuxer",  "Demuxer",      "Demuxes MPEG-TS (.ts) into encoded audio/video",                                                                       g_builtin_tsdemux_props,        sizeof(g_builtin_tsdemux_props) / sizeof(g_builtin_tsdemux_props[0]), g_pad_tsdemux),
    DESC("mp4demux", "MP4 Demuxer",      "Demuxer/File", "Demuxes MP4 (.mp4/.mov/.m4a/.m4v) into encoded audio/video",                                                             g_builtin_mp4demux_props,       sizeof(g_builtin_mp4demux_props) / sizeof(g_builtin_mp4demux_props[0]), g_pad_mp4demux),
#endif
#ifdef HAS_JETSON
    DESC("nvenc",    "NVIDIA V4L2 Video Encoder", "Codec/Encoder","Encodes raw video to H.264/H.265 using NV V4L2 extensions",                                                              NULL,                           0, g_pad_x264enc),
    DESC("nvdec",    "NVIDIA V4L2 Video Decoder", "Codec/Decoder","Decodes H.264/H.265 video frames using NV V4L2 extensions",                                                              NULL,                           0, g_pad_h264dec),
#endif
#ifdef HAS_ONEAPI_ENCODER
    DESC("oneapienc", "Intel oneAPI Video Encoder", "Codec/Encoder", "Encodes raw video to H.264/H.265 using Intel oneVPL", g_builtin_oneapienc_props, sizeof(g_builtin_oneapienc_props) / sizeof(g_builtin_oneapienc_props[0]), g_pad_oneapienc),
    DESC("oneapi_video_encoder", "Intel oneAPI Video Encoder", "Codec/Encoder", "Alias for oneapienc", g_builtin_oneapienc_props, sizeof(g_builtin_oneapienc_props) / sizeof(g_builtin_oneapienc_props[0]), g_pad_oneapienc),
#endif
#ifdef HAS_ONEAPI_DECODER
    DESC("oneapidec", "Intel oneAPI Video Decoder", "Codec/Decoder", "Decodes H.264/H.265 video using Intel oneVPL", g_builtin_oneapidec_props, sizeof(g_builtin_oneapidec_props) / sizeof(g_builtin_oneapidec_props[0]), g_pad_oneapidec),
    DESC("oneapi_video_decoder", "Intel oneAPI Video Decoder", "Codec/Decoder", "Alias for oneapidec", g_builtin_oneapidec_props, sizeof(g_builtin_oneapidec_props) / sizeof(g_builtin_oneapidec_props[0]), g_pad_oneapidec),
#endif
#ifdef HAS_VAAPI_ENCODER
    DESC("vaapienc", "VA-API Video Encoder", "Codec/Encoder", "Encodes raw video to H.264/H.265 using Linux VA-API", g_builtin_vaapienc_props, sizeof(g_builtin_vaapienc_props) / sizeof(g_builtin_vaapienc_props[0]), g_pad_vaapienc),
    DESC("vaapi_video_encoder", "VA-API Video Encoder", "Codec/Encoder", "Alias for vaapienc", g_builtin_vaapienc_props, sizeof(g_builtin_vaapienc_props) / sizeof(g_builtin_vaapienc_props[0]), g_pad_vaapienc),
#endif
#ifdef HAS_VAAPI_DECODER
    DESC("vaapidec", "VA-API Video Decoder", "Codec/Decoder", "Decodes H.264/H.265 video using Linux VA-API", g_builtin_vaapidec_props, sizeof(g_builtin_vaapidec_props) / sizeof(g_builtin_vaapidec_props[0]), g_pad_vaapidec),
    DESC("vaapi_video_decoder", "VA-API Video Decoder", "Codec/Decoder", "Alias for vaapidec", g_builtin_vaapidec_props, sizeof(g_builtin_vaapidec_props) / sizeof(g_builtin_vaapidec_props[0]), g_pad_vaapidec),
#endif
    DESC("nvvideoscaler", "NVIDIA V4L2 Video Scaler", "Filter/Video", "Hardware video scaler and format converter using NV V4L2 extensions",                                    NULL,                           0, g_pad_video_filter),
    DESC("sc6f0src", "SC6F0 Source", "Source/Video", "Captures video/audio from SC6F0 platforms with dynamic signal detection", g_builtin_sc6f0src_props, sizeof(g_builtin_sc6f0src_props) / sizeof(g_builtin_sc6f0src_props[0]), g_pad_sc6f0src),
#ifdef HAS_X11SINK
    DESC("x11sink", "X11 Video Sink", "Sink/Video", "Displays raw video frames in an X11 window", g_builtin_x11sink_props, sizeof(g_builtin_x11sink_props) / sizeof(g_builtin_x11sink_props[0]), g_pad_sink),
#endif
#ifdef HAS_WEBRTC
    DESC("webrtc_endpoint", "WebRTC Endpoint", "Network/WebRTC", "Unified WebRTC peer connection (sender + receiver)", g_builtin_webrtc_endpoint_props, sizeof(g_builtin_webrtc_endpoint_props) / sizeof(g_builtin_webrtc_endpoint_props[0]), g_pad_webrtc_endpoint),
#endif
#ifdef HAS_GLSINK
    DESC("glsink", "OpenGL Sink", "Sink/Video", "Displays video frames in an OpenGL window with GPU YUV\u2192RGB conversion",                                              NULL,                           0, g_pad_sink),
#endif
#ifdef HAS_GLCOMPSINK
    DESC("glcompsink", "OpenGL Compositor Sink", "Sink/Video", "Composites multiple raw video streams into one OpenGL window",                                           NULL,                           0, g_pad_video_sink),
#endif
#ifdef HAS_IPP_COMP_SINK
    DESC("ippcompsink", "Intel IPP Compositor Sink", "Sink/Video", "Composites multiple raw video streams into one buffer using Intel IPP", NULL, 0, g_pad_video_sink),
#endif
    DESC("ws_server", "WebSocket Server", "Network/Signaling", "Lightweight WebSocket server for WebRTC signaling", g_builtin_wsserver_props, sizeof(g_builtin_wsserver_props) / sizeof(g_builtin_wsserver_props[0]), NULL)
};


/*──────────────────────────────────────────────────────────────────────────
  zst_register_builtin_elements — register every built-in element with the
  factory.  Only exists in libzstreamer-elements (not in core), so any
  caller creates a strong link dependency on the elements library.
──────────────────────────────────────────────────────────────────────────*/
zst_result_t
zst_register_builtin_elements(void)
{
    zst_result_t ret;

#ifdef HAS_FFMPEG
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58, 9, 100)
    static int ffmpeg_initialized = 0;
    if (!ffmpeg_initialized) {
        avcodec_register_all();
        av_register_all();
        ffmpeg_initialized = 1;
    }
#endif
#endif

    ret = zst_plugin_registry_init();
    if (ret != ZST_OK) return ret;

    ret = zst_plugin_registry_add_entry(
        g_builtin_descs,
        sizeof(g_builtin_descs) / sizeof(g_builtin_descs[0]),
        create_builtin_element);

    return ret;
}

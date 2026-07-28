# ── Core library sources ─────────────────────────────────────────────────
set(CORE_SOURCES
    zst_caps.c
    zst_buffer.c
    zst_buffer_pool.c
    zst_pad.c
    zst_element.c
    zst_bin.c
    zst_pipeline.c
    zst_queue.c
    zst_queue_element.c
    zst_scheduler.c
    zst_bus.c
    zst_log.c
    zst_allocator.c
    zst_allocator_dmabuf.c
    zst_allocator_cuda.c
    zst_clock.c
    zst_timestamp_pacer.c
    zst_pad_event.c
    zst_stream.c
)

if(ENABLE_PLUGINS)
    list(APPEND CORE_SOURCES zst_plugin.c)
endif()

if(HAS_ONEAPI)
    list(APPEND CORE_SOURCES zst_allocator_oneapi.cpp)
endif()

if(ENABLE_JETSON)
    list(APPEND CORE_SOURCES zst_allocator_jetson.cpp ${STUB_SOURCES})
endif()

# ── Build the core library ───────────────────────────────────────────────
if(BUILD_SHARED)
    add_library(zstreamer SHARED ${CORE_SOURCES})
else()
    add_library(zstreamer STATIC ${CORE_SOURCES})
endif()

if(HAS_ONEAPI)
    if(HAS_SYCL_SUPPORT)
        set_source_files_properties(src/zst_allocator_oneapi.cpp PROPERTIES COMPILE_OPTIONS "-fsycl")
        target_link_options(zstreamer PUBLIC "-fsycl")
        set_target_properties(zstreamer PROPERTIES LINKER_LANGUAGE CXX)
    endif()
endif()

include(GNUInstallDirs)

target_include_directories(zstreamer
    PUBLIC
        $<BUILD_INTERFACE:${ZSTREAMER_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:include/zstreamer>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
)

# Required for shared plugin libraries that link against the static lib
set_target_properties(zstreamer PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_link_libraries(zstreamer
    PUBLIC  Threads::Threads
    PRIVATE ${DL_LIB}
)
if(ENABLE_VULKAN)
    target_link_libraries(zstreamer PRIVATE Vulkan::Vulkan)
endif()

if(ENABLE_JETSON)
    target_link_libraries(zstreamer PRIVATE ${JETSON_LIBRARIES})
endif()

set(ZSTREAMER_PKG_LIBS "")
if(CUDAToolkit_FOUND)
    target_link_libraries(zstreamer PUBLIC CUDA::cudart)
    set(ZSTREAMER_PKG_LIBS "-lcudart")
endif()

if(ENABLE_VULKAN)
    if(ZSTREAMER_PKG_LIBS STREQUAL "")
        set(ZSTREAMER_PKG_LIBS "-lvulkan")
    else()
        set(ZSTREAMER_PKG_LIBS "${ZSTREAMER_PKG_LIBS} -lvulkan")
    endif()
endif()

target_compile_definitions(zstreamer PRIVATE
    ZSTREAMER_DEFAULT_PLUGIN_DIR="${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/zstreamer/plugins"
)

# ── Build the elements library ───────────────────────────────────────────
set(ELEMENT_LIBRARY_SOURCES
    zst_builtins.c
    file_sink.c
    file_source.c
    fake_sink.c
    video_test_src.c
    audio_test_src.c
    audio_mixer.c
    srt_parser.c
    sdp_demuxer.c
    sdp_muxer.c
    rtp_payloader.c
    rtp_depayloader.c
    net_source.c
    net_sink.c
    rtsp_server.c
    http_server.c
    nv_video_scaler.c
    sc6f0_source.c
    zst_ws_server.c
)

if(HAS_V4L2)
    list(APPEND ELEMENT_LIBRARY_SOURCES v4l2_source.c v4l2_sink.c)
endif()

if(HAS_ALSA)
    list(APPEND ELEMENT_LIBRARY_SOURCES alsa_source.c alsa_sink.c)
endif()

if(HAS_X264)
    list(APPEND ELEMENT_LIBRARY_SOURCES x264_encoder.c)
endif()

if(HAS_X265)
    list(APPEND ELEMENT_LIBRARY_SOURCES x265_encoder.c)
endif()

if(HAS_FFMPEG)
    list(APPEND ELEMENT_LIBRARY_SOURCES
        h264_decoder.c
        h265_encoder.c
        h265_decoder.c
        vp8_decoder.c
        vp8_encoder.c
        vp9_decoder.c
        vp9_encoder.c
        aac_encoder.c
        opus_encoder.c
        opus_decoder.c
        aac_decoder.c
        mp4_muxer.c
        hls_sink.c
        video_scaler.c
        audio_resampler.c
        rtsp_source.c
        rtsp_sink.c
        rtmp_source.c
        rtmp_sink.c
        mpegts_muxer.c
        mpegts_demuxer.c
        mp4_demuxer.c
        http_source.c
    )
endif()

if(HAS_FREETYPE)
    list(APPEND ELEMENT_LIBRARY_SOURCES text_overlay.c text_source.c)
endif()

if(HAS_SRT)
    list(APPEND ELEMENT_LIBRARY_SOURCES srt_source.c srt_sink.c srt_common.c)
endif()

if(ENABLE_JETSON)
    list(APPEND ELEMENT_LIBRARY_SOURCES nv_video_encoder.c nv_video_decoder.c)
endif()

if(ENABLE_ONEAPI_ENCODER)
    list(APPEND ELEMENT_LIBRARY_SOURCES oneapi_video_encoder.c)
endif()

if(ENABLE_ONEAPI_DECODER)
    list(APPEND ELEMENT_LIBRARY_SOURCES oneapi_video_decoder.c)
endif()

if(ENABLE_VAAPI_ENCODER)
    list(APPEND ELEMENT_LIBRARY_SOURCES vaapi_video_encoder.c)
endif()

if(ENABLE_VAAPI_DECODER)
    list(APPEND ELEMENT_LIBRARY_SOURCES vaapi_video_decoder.c)
endif()

if(ENABLE_GLSINK)
    list(APPEND ELEMENT_LIBRARY_SOURCES gl_sink.c)
endif()

if(ENABLE_GLCOMPSINK)
    list(APPEND ELEMENT_LIBRARY_SOURCES gl_comp_sink.c)
endif()

if(ENABLE_IPP_COMP_SINK)
    list(APPEND ELEMENT_LIBRARY_SOURCES ipp_comp_sink.c)
endif()

if(ENABLE_X11SINK)
    list(APPEND ELEMENT_LIBRARY_SOURCES x11_sink.c)
endif()

if(HAS_WEBRTC)
    list(APPEND ELEMENT_LIBRARY_SOURCES webrtc_endpoint.c zst_webrtc_twcc.c)
endif()

if(ENABLE_ST2110)
    list(APPEND ELEMENT_LIBRARY_SOURCES
        st2110_20_payloader.c
        st2110_20_depayloader.c
        st2110_21_payloader.c
        st2110_21_depayloader.c
        st2110_30_payloader.c
        st2110_30_depayloader.c
        st2110_40_payloader.c
        st2110_40_depayloader.c
        ptp_clock.c
        st2110_redundancy_mux.c
        st2110_redundancy_demux.c
        st2110_fec.c
        zst_st2110_utils.c
    )
    if(HAS_SVT_JPEGXS)
        list(APPEND ELEMENT_LIBRARY_SOURCES
            st2110_22_payloader.c
            st2110_22_depayloader.c
        )
    endif()
endif()

if(BUILD_SHARED)
    add_library(zstreamer-elements SHARED ${ELEMENT_LIBRARY_SOURCES})
else()
    add_library(zstreamer-elements STATIC ${ELEMENT_LIBRARY_SOURCES})
endif()

target_include_directories(zstreamer-elements
    PUBLIC
        $<BUILD_INTERFACE:${ZSTREAMER_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:include/zstreamer>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${ELEMENT_INCLUDE_DIRS}
)

set_target_properties(zstreamer-elements PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_link_libraries(zstreamer-elements
    PUBLIC  zstreamer
    PRIVATE ${ELEMENT_LIBRARIES} m
)

# ── Element plugins (built-in) ──────────────────────────────────────────
# These live inside the same library for now.  In the future they can be
# compiled as separate .so files and loaded via zst_plugin.
set(ELEMENT_SOURCES
    v4l2_source.c
    v4l2_sink.c
    x264_encoder.c
    x265_encoder.c
    mp4_muxer.c
    hls_sink.c
    file_sink.c
    fake_sink.c
    alsa_source.c
    aac_encoder.c
    opus_encoder.c
    opus_decoder.c
    video_scaler.c
    nv_video_scaler.c
    audio_resampler.c
    text_overlay.c
    video_test_src.c
    audio_test_src.c
    file_source.c
    text_source.c
    h264_decoder.c
    h265_encoder.c
    h265_decoder.c
    aac_decoder.c
    net_source.c
    net_sink.c
    rtmp_source.c
    rtmp_sink.c
    http_source.c
    sdp_demuxer.c
    sdp_muxer.c
    rtp_payloader.c
    rtp_depayloader.c
)

# ── Dynamic Plugins ──────────────────────────────────────────────────────
add_library(zst_filesink SHARED file_sink.c)
target_compile_definitions(zst_filesink PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_filesink PRIVATE zstreamer)

add_library(zst_fakesink SHARED fake_sink.c)
target_compile_definitions(zst_fakesink PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_fakesink PRIVATE zstreamer)

if(HAS_V4L2)
    add_library(zst_v4l2source SHARED v4l2_source.c)
    target_compile_definitions(zst_v4l2source PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_v4l2source PRIVATE zstreamer ${V4L2_LIBRARIES})
    
    add_library(zst_v4l2sink SHARED v4l2_sink.c)
    target_compile_definitions(zst_v4l2sink PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_v4l2sink PRIVATE zstreamer ${V4L2_LIBRARIES})
endif()

if(HAS_FFMPEG)
    add_library(zst_rtspsource SHARED rtsp_source.c)
    target_compile_definitions(zst_rtspsource PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_rtspsource PRIVATE zstreamer ${AVUTIL_LIBRARIES})
    
    add_library(zst_rtspsink SHARED rtsp_sink.c)
    target_compile_definitions(zst_rtspsink PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_rtspsink PRIVATE zstreamer ${AVFORMAT_LIBRARIES} ${AVUTIL_LIBRARIES})
endif()

add_library(zst_rtsp_server SHARED rtsp_server.c)
target_compile_definitions(zst_rtsp_server PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_rtsp_server PRIVATE zstreamer)

add_library(zst_ws_server SHARED zst_ws_server.c)
target_compile_definitions(zst_ws_server PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_ws_server PRIVATE zstreamer)


if(HAS_ALSA)
    add_library(zst_alsasource SHARED alsa_source.c)
    target_compile_definitions(zst_alsasource PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_alsasource PRIVATE zstreamer ${ALSA_LIBRARIES})
    
    add_library(zst_alsasink SHARED alsa_sink.c)
    target_compile_definitions(zst_alsasink PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_alsasink PRIVATE zstreamer ${ALSA_LIBRARIES})
endif()

if(HAS_X264)
    add_library(zst_x264encoder SHARED x264_encoder.c)
    target_compile_definitions(zst_x264encoder PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_x264encoder PRIVATE zstreamer ${X264_LIBRARIES})
endif()

if(HAS_SVT_JPEGXS)
    add_library(zst_st2110_22_payloader SHARED st2110_22_payloader.c)
    target_compile_definitions(zst_st2110_22_payloader PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_st2110_22_payloader PRIVATE zstreamer ${SVT_JPEGXS_LIBRARIES})

    add_library(zst_st2110_22_depayloader SHARED st2110_22_depayloader.c)
    target_compile_definitions(zst_st2110_22_depayloader PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_st2110_22_depayloader PRIVATE zstreamer ${SVT_JPEGXS_LIBRARIES})
endif()

if(HAS_X265)
    add_library(zst_x265encoder SHARED x265_encoder.c)
    target_compile_definitions(zst_x265encoder PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_x265encoder PRIVATE zstreamer ${X265_LIBRARIES})
endif()

if(HAS_FFMPEG)
    add_library(zst_aacencoder SHARED aac_encoder.c)
    add_library(zst_opusencoder SHARED opus_encoder.c)
    target_compile_definitions(zst_opusencoder PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_opusencoder PRIVATE zstreamer ${AVCODEC_LIBRARIES})
    target_compile_definitions(zst_aacencoder PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_aacencoder PRIVATE zstreamer ${AVCODEC_LIBRARIES})
    
    add_library(zst_mp4muxer SHARED mp4_muxer.c)
    target_compile_definitions(zst_mp4muxer PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_mp4muxer PRIVATE zstreamer ${AVFORMAT_LIBRARIES} ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES})

    add_library(zst_hlssink SHARED hls_sink.c)
    target_compile_definitions(zst_hlssink PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_hlssink PRIVATE zstreamer ${AVFORMAT_LIBRARIES} ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES})
    
    add_library(zst_videoscaler SHARED video_scaler.c)
    target_compile_definitions(zst_videoscaler PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_videoscaler PRIVATE zstreamer ${SWSCALE_LIBRARIES} ${AVUTIL_LIBRARIES})
    
    add_library(zst_audioresampler SHARED audio_resampler.c)
    target_compile_definitions(zst_audioresampler PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_audioresampler PRIVATE zstreamer ${SWRESAMPLE_LIBRARIES} ${AVUTIL_LIBRARIES})
endif()

if(HAS_FREETYPE)
    add_library(zst_textoverlay SHARED text_overlay.c)
    target_compile_definitions(zst_textoverlay PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_textoverlay PRIVATE zstreamer ${FREETYPE_LIBRARIES})
endif()

add_library(zst_srtparser SHARED srt_parser.c)
target_compile_definitions(zst_srtparser PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_srtparser PRIVATE zstreamer)

add_library(zst_videotestsrc SHARED video_test_src.c)
target_compile_definitions(zst_videotestsrc PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_videotestsrc PRIVATE zstreamer)

add_library(zst_audiotestsrc SHARED audio_test_src.c)
target_compile_definitions(zst_audiotestsrc PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_audiotestsrc PRIVATE zstreamer)

add_library(zst_audiomixer SHARED audio_mixer.c)
target_compile_definitions(zst_audiomixer PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_audiomixer PRIVATE zstreamer)

add_library(zst_filesource SHARED file_source.c)
target_compile_definitions(zst_filesource PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_filesource PRIVATE zstreamer)

if(HAS_FREETYPE)
    add_library(zst_textsource SHARED text_source.c)
    target_compile_definitions(zst_textsource PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_textsource PRIVATE zstreamer ${FREETYPE_LIBRARIES})
endif()

add_library(zst_netsrc SHARED net_source.c)
target_compile_definitions(zst_netsrc PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_netsrc PRIVATE zstreamer)

add_library(zst_netsink SHARED net_sink.c)
target_compile_definitions(zst_netsink PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_netsink PRIVATE zstreamer)

if(ENABLE_GLSINK)
    add_library(zst_glsink SHARED gl_sink.c)
    target_compile_definitions(zst_glsink PRIVATE BUILDING_PLUGIN)
    target_include_directories(zst_glsink PRIVATE ${X11_INCLUDE_DIR} ${OPENGL_INCLUDE_DIR})
    target_link_libraries(zst_glsink PRIVATE zstreamer ${X11_LIBRARIES} ${OPENGL_LIBRARIES} ${X11_Xext_LIB})
endif()

if(ENABLE_GLCOMPSINK)
    add_library(zst_glcompsink SHARED gl_comp_sink.c)
    target_compile_definitions(zst_glcompsink PRIVATE BUILDING_PLUGIN)
    target_include_directories(zst_glcompsink PRIVATE ${X11_INCLUDE_DIR} ${OPENGL_INCLUDE_DIR})
    target_link_libraries(zst_glcompsink PRIVATE zstreamer ${X11_LIBRARIES} ${OPENGL_LIBRARIES} ${X11_Xext_LIB})
endif()


if(ENABLE_IPP_COMP_SINK)
    add_compile_definitions(HAS_IPP_COMP_SINK=1)
    add_library(zst_ippcompsink SHARED ipp_comp_sink.c)
    target_compile_definitions(zst_ippcompsink PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_ippcompsink PRIVATE zstreamer ippcore ippi ippcc ipps)
endif()

if(ENABLE_X11SINK)
    add_library(zst_x11sink SHARED x11_sink.c)
    target_compile_definitions(zst_x11sink PRIVATE BUILDING_PLUGIN)
    target_include_directories(zst_x11sink PRIVATE ${X11_INCLUDE_DIR})
    target_link_libraries(zst_x11sink PRIVATE zstreamer ${X11_LIBRARIES})
endif()

if(HAS_FFMPEG)
    add_library(zst_h264decoder SHARED h264_decoder.c)
    target_compile_definitions(zst_h264decoder PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_h264decoder PRIVATE zstreamer ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES})
    
    add_library(zst_h265encoder SHARED h265_encoder.c)
    target_compile_definitions(zst_h265encoder PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_h265encoder PRIVATE zstreamer ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES})
    
    add_library(zst_h265decoder SHARED h265_decoder.c)
    target_compile_definitions(zst_h265decoder PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_h265decoder PRIVATE zstreamer ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES})
    
    add_library(zst_vp8decoder SHARED vp8_decoder.c)
    target_compile_definitions(zst_vp8decoder PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_vp8decoder PRIVATE zstreamer ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES})
    
    add_library(zst_vp9decoder SHARED vp9_decoder.c)
    target_compile_definitions(zst_vp9decoder PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_vp9decoder PRIVATE zstreamer ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES})
    
    add_library(zst_vp8encoder SHARED vp8_encoder.c)
    target_compile_definitions(zst_vp8encoder PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_vp8encoder PRIVATE zstreamer ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES})
    
    add_library(zst_vp9encoder SHARED vp9_encoder.c)
    target_compile_definitions(zst_vp9encoder PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_vp9encoder PRIVATE zstreamer ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES})
    
    add_library(zst_aacdecoder SHARED aac_decoder.c)
    target_compile_definitions(zst_aacdecoder PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_aacdecoder PRIVATE zstreamer ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES})
    add_library(zst_opusdecoder SHARED opus_decoder.c)
    target_compile_definitions(zst_opusdecoder PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_opusdecoder PRIVATE zstreamer ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES})
    
    add_library(zst_rtmpsource SHARED rtmp_source.c)
    target_compile_definitions(zst_rtmpsource PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_rtmpsource PRIVATE zstreamer ${AVFORMAT_LIBRARIES} ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES})
    
    add_library(zst_rtmpsink SHARED rtmp_sink.c)
    target_compile_definitions(zst_rtmpsink PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_rtmpsink PRIVATE zstreamer ${AVFORMAT_LIBRARIES} ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES})
endif()

if(HAS_SRT)
    add_library(zst_srt SHARED srt_source.c srt_sink.c srt_common.c srt_plugin.c)
    target_compile_definitions(zst_srt PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_srt PRIVATE zstreamer ${SRT_LIBRARIES})
endif()

if(HAS_FFMPEG)
    add_library(zst_mpegtsmux SHARED mpegts_muxer.c)
    target_compile_definitions(zst_mpegtsmux PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_mpegtsmux PRIVATE zstreamer ${AVFORMAT_LIBRARIES} ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES})
    
    add_library(zst_mpegtsdemux SHARED mpegts_demuxer.c)
    target_compile_definitions(zst_mpegtsdemux PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_mpegtsdemux PRIVATE zstreamer ${AVFORMAT_LIBRARIES} ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES})
    
    add_library(zst_mp4demux SHARED mp4_demuxer.c)
    target_compile_definitions(zst_mp4demux PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_mp4demux PRIVATE zstreamer ${AVFORMAT_LIBRARIES} ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES})
    
    add_library(zst_httpsource SHARED http_source.c)
    target_compile_definitions(zst_httpsource PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_httpsource PRIVATE zstreamer ${AVFORMAT_LIBRARIES} ${AVUTIL_LIBRARIES})
endif()

add_library(zst_sdpdemux SHARED sdp_demuxer.c)
target_compile_definitions(zst_sdpdemux PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_sdpdemux PRIVATE zstreamer)

add_library(zst_sdpmuxer SHARED sdp_muxer.c)
target_compile_definitions(zst_sdpmuxer PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_sdpmuxer PRIVATE zstreamer)

add_library(zst_rtppay SHARED rtp_payloader.c)
target_compile_definitions(zst_rtppay PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_rtppay PRIVATE zstreamer)

add_library(zst_rtpdepay SHARED rtp_depayloader.c)
target_compile_definitions(zst_rtpdepay PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_rtpdepay PRIVATE zstreamer)


if(ENABLE_JETSON)
    add_library(zst_nvenc SHARED nv_video_encoder.c)
    target_compile_definitions(zst_nvenc PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_nvenc PRIVATE zstreamer ${JETSON_LIBRARIES})

    add_library(zst_nvdec SHARED nv_video_decoder.c)
    target_compile_definitions(zst_nvdec PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_nvdec PRIVATE zstreamer ${JETSON_LIBRARIES})

    set(JETSON_PLUGINS zst_nvenc zst_nvdec)
else()
    set(JETSON_PLUGINS "")
endif()

if(ENABLE_ONEAPI_ENCODER)
    add_library(zst_oneapienc SHARED oneapi_video_encoder.c)
    target_compile_definitions(zst_oneapienc PRIVATE BUILDING_PLUGIN)
    target_include_directories(zst_oneapienc PRIVATE ${ONEVPL_INCLUDE_DIRS})
    target_link_libraries(zst_oneapienc PRIVATE zstreamer ${ONEVPL_LIBRARIES})
    set(ONEAPI_ENCODER_PLUGINS zst_oneapienc)
else()
    set(ONEAPI_ENCODER_PLUGINS "")
endif()

if(ENABLE_ONEAPI_DECODER)
    add_library(zst_oneapidec SHARED oneapi_video_decoder.c)
    target_compile_definitions(zst_oneapidec PRIVATE BUILDING_PLUGIN)
    target_include_directories(zst_oneapidec PRIVATE ${ONEVPL_INCLUDE_DIRS} ${LIBVA_INCLUDE_DIRS})
    target_link_libraries(zst_oneapidec PRIVATE zstreamer ${ONEVPL_LIBRARIES} ${LIBVA_LIBRARIES})
    set(ONEAPI_DECODER_PLUGINS zst_oneapidec)
else()
    set(ONEAPI_DECODER_PLUGINS "")
endif()

if(ENABLE_VAAPI_ENCODER)
    add_library(zst_vaapienc SHARED vaapi_video_encoder.c)
    target_compile_definitions(zst_vaapienc PRIVATE BUILDING_PLUGIN)
    target_include_directories(zst_vaapienc PRIVATE ${LIBVA_INCLUDE_DIRS})
    target_link_libraries(zst_vaapienc PRIVATE zstreamer ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES} ${LIBVA_LIBRARIES})
    set(VAAPI_ENCODER_PLUGINS zst_vaapienc)
else()
    set(VAAPI_ENCODER_PLUGINS "")
endif()

if(ENABLE_VAAPI_DECODER)
    add_library(zst_vaapidec SHARED vaapi_video_decoder.c)
    target_compile_definitions(zst_vaapidec PRIVATE BUILDING_PLUGIN)
    target_include_directories(zst_vaapidec PRIVATE ${LIBVA_INCLUDE_DIRS})
    target_link_libraries(zst_vaapidec PRIVATE zstreamer ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES} ${LIBVA_LIBRARIES})
    set(VAAPI_DECODER_PLUGINS zst_vaapidec)
else()
    set(VAAPI_DECODER_PLUGINS "")
endif()

if(HAS_WEBRTC)
    add_library(zst_webrtc SHARED webrtc_endpoint.c zst_webrtc_twcc.c)
    target_compile_definitions(zst_webrtc PRIVATE BUILDING_PLUGIN HAS_WEBRTC=1)
    target_link_libraries(zst_webrtc PRIVATE zstreamer)
    if(LibDataChannel_FOUND)
        target_link_libraries(zst_webrtc PRIVATE LibDataChannel::LibDataChannel)
    elseif(DATACHANNEL_FOUND)
        target_include_directories(zst_webrtc PRIVATE ${DATACHANNEL_INCLUDE_DIRS})
        target_link_libraries(zst_webrtc PRIVATE ${DATACHANNEL_LIBRARIES})
    endif()
    set(WEBRTC_PLUGINS zst_webrtc)
    target_link_libraries(zst_webrtc PRIVATE m)
else()
    set(WEBRTC_PLUGINS "")
endif()

set(PLUGIN_TARGETS zst_filesink zst_fakesink zst_rtsp_server zst_ws_server zst_srtparser zst_videotestsrc zst_audiotestsrc zst_audiomixer zst_filesource zst_netsrc zst_netsink zst_sdpdemux zst_sdpmuxer zst_rtppay zst_rtpdepay ${JETSON_PLUGINS} ${ONEAPI_ENCODER_PLUGINS} ${ONEAPI_DECODER_PLUGINS} ${VAAPI_ENCODER_PLUGINS} ${VAAPI_DECODER_PLUGINS} ${WEBRTC_PLUGINS})

if(HAS_V4L2)
    list(APPEND PLUGIN_TARGETS zst_v4l2source zst_v4l2sink)
endif()

if(HAS_ALSA)
    list(APPEND PLUGIN_TARGETS zst_alsasource zst_alsasink)
endif()

if(HAS_X264)
    list(APPEND PLUGIN_TARGETS zst_x264encoder)
endif()

if(HAS_SVT_JPEGXS)
    list(APPEND PLUGIN_TARGETS zst_st2110_22_payloader zst_st2110_22_depayloader)
endif()

if(HAS_X265)
    list(APPEND PLUGIN_TARGETS zst_x265encoder)
endif()

if(ENABLE_ONEAPI_ENCODER)
    list(APPEND PLUGIN_TARGETS zst_oneapienc)
endif()

if(ENABLE_ONEAPI_DECODER)
    list(APPEND PLUGIN_TARGETS zst_oneapidec)
endif()

if(ENABLE_GLSINK)
    list(APPEND PLUGIN_TARGETS zst_glsink)
endif()
if(ENABLE_GLCOMPSINK)
    list(APPEND PLUGIN_TARGETS zst_glcompsink)
endif()
if(ENABLE_IPP_COMP_SINK)
    list(APPEND PLUGIN_TARGETS zst_ippcompsink)
endif()
if(ENABLE_X11SINK)
    list(APPEND PLUGIN_TARGETS zst_x11sink)
endif()

if(HAS_WEBRTC)
    list(APPEND PLUGIN_TARGETS zst_webrtc)
endif()

if(HAS_FFMPEG)
    list(APPEND PLUGIN_TARGETS
        zst_rtspsource
        zst_rtspsink
        zst_aacencoder
        zst_mp4muxer
        zst_hlssink
        zst_opusencoder
        zst_videoscaler
        zst_opusdecoder
        zst_audioresampler
        zst_h264decoder
        zst_h265encoder
        zst_h265decoder
        zst_aacdecoder
        zst_rtmpsource
        zst_rtmpsink
        zst_mpegtsmux
        zst_mpegtsdemux
        zst_mp4demux
        zst_httpsource
    )
endif()

if(HAS_FREETYPE)
    list(APPEND PLUGIN_TARGETS zst_textoverlay zst_textsource)
endif()

if(HAS_SRT)
    list(APPEND PLUGIN_TARGETS zst_srt)
endif()

foreach(plugin ${PLUGIN_TARGETS})
    target_include_directories(${plugin} PRIVATE ${ZSTREAMER_INCLUDE_DIR} PRIVATE ${ELEMENT_INCLUDE_DIRS})
    set_target_properties(${plugin} PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins")
endforeach()



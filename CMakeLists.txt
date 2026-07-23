cmake_minimum_required(VERSION 3.16)
project(zstreamer VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_definitions(_GNU_SOURCE)


# ── Options ──────────────────────────────────────────────────────────────
option(BUILD_TESTS     "Build test programs" ON)
option(BUILD_SHARED    "Build core as shared library" OFF)
option(ENABLE_PLUGINS  "Enable dlopen-based plugin loading" ON)
option(ENABLE_ONEAPI_ENCODER "Build Intel oneAPI video encoder element" OFF)
option(ENABLE_VAAPI_ENCODER "Build Linux VA-API video encoder element" OFF)
option(ENABLE_VAAPI_DECODER "Build Linux VA-API video decoder element" OFF)
option(ENABLE_GLSINK "Build OpenGL display sink element" ON)
option(ENABLE_GLCOMPSINK "Build OpenGL compositor sink element" ON)
option(ENABLE_IPP_COMP_SINK "Build Intel IPP compositor sink element" OFF)
option(ENABLE_X11SINK "Build X11 display sink element" ON)
option(ENABLE_WEBRTC "Build WebRTC endpoint element (requires libdatachannel)" OFF)
option(ENABLE_ST2110 "Enable SMPTE ST2110 elements" ON)
# ── Dependencies ─────────────────────────────────────────────────────────
find_package(Threads REQUIRED)

if(ENABLE_X11SINK OR ENABLE_GLSINK OR ENABLE_GLCOMPSINK)
    find_package(X11 QUIET)
endif()

if(ENABLE_PLUGINS)
    # dlopen / libdl
    if(UNIX)
        set(DL_LIB ${CMAKE_DL_LIBS})
    endif()
endif()

find_package(PkgConfig REQUIRED)
option(ENABLE_VULKAN "Enable Vulkan allocator support" ON)
if(ENABLE_VULKAN)
    find_package(Vulkan)
    if(Vulkan_FOUND)
        add_compile_definitions(HAS_VULKAN=1)
        message(STATUS "Vulkan allocator support enabled")
    else()
        message(WARNING "Vulkan not found, Vulkan allocator support disabled")
        set(ENABLE_VULKAN OFF)
    endif()
endif()
pkg_check_modules(AVFORMAT libavformat)
pkg_check_modules(AVCODEC libavcodec)
pkg_check_modules(AVUTIL libavutil)
pkg_check_modules(SWSCALE libswscale)
pkg_check_modules(SWRESAMPLE libswresample)

if(AVFORMAT_FOUND AND AVCODEC_FOUND AND AVUTIL_FOUND AND SWSCALE_FOUND AND SWRESAMPLE_FOUND)
    set(HAS_FFMPEG ON)
    add_compile_definitions(HAS_FFMPEG=1)
    message(STATUS "FFmpeg plugins enabled")
else()
    set(HAS_FFMPEG OFF)
    message(STATUS "FFmpeg plugins disabled (missing one or more of libavformat, libavcodec, libavutil, libswscale, libswresample)")
endif()

pkg_check_modules(X264 x264)
if(X264_FOUND)
    set(HAS_X264 ON)
    add_compile_definitions(HAS_X264=1)
    message(STATUS "x264 encoder enabled")
else()
    set(HAS_X264 OFF)
    message(STATUS "x264 encoder disabled")
endif()

pkg_check_modules(X265 x265)
if(X265_FOUND)
    set(HAS_X265 ON)
    add_compile_definitions(HAS_X265=1)
    message(STATUS "x265 encoder enabled")
else()
    set(HAS_X265 OFF)
    message(STATUS "x265 encoder disabled")
endif()

pkg_check_modules(ALSA alsa)
if(ALSA_FOUND)
    set(HAS_ALSA ON)
    add_compile_definitions(HAS_ALSA=1)
    message(STATUS "ALSA plugins enabled")
else()
    set(HAS_ALSA OFF)
    message(STATUS "ALSA plugins disabled")
endif()

pkg_check_modules(V4L2 libv4l2)
if(V4L2_FOUND)
    set(HAS_V4L2 ON)
    add_compile_definitions(HAS_V4L2=1)
    message(STATUS "V4L2 plugins enabled")
else()
    set(HAS_V4L2 OFF)
    message(STATUS "V4L2 plugins disabled")
endif()

pkg_check_modules(FREETYPE freetype2)
if(FREETYPE_FOUND)
    set(HAS_FREETYPE ON)
    add_compile_definitions(HAS_FREETYPE=1)
    message(STATUS "Freetype text rendering enabled")
else()
    set(HAS_FREETYPE OFF)
    message(STATUS "Freetype text rendering disabled")
endif()

pkg_check_modules(SRT_PC srt)
if(NOT SRT_PC_FOUND)
    pkg_check_modules(SRT_PC srt-gnutls)
endif()
if(SRT_PC_FOUND)
    set(HAS_SRT ON)
    add_compile_definitions(HAS_SRT=1)
    message(STATUS "SRT plugins enabled")
else()
    set(HAS_SRT OFF)
    message(STATUS "SRT plugins disabled")
endif()

# ── WebRTC (libdatachannel) ───────────────────────────────────────────────
if(ENABLE_WEBRTC)
    find_package(LibDataChannel QUIET)
    if(NOT LibDataChannel_FOUND)
        # Fallback: try pkg-config
        pkg_check_modules(DATACHANNEL QUIET libdatachannel)
    endif()
    if(LibDataChannel_FOUND OR DATACHANNEL_FOUND)
        set(HAS_WEBRTC ON)
        add_compile_definitions(HAS_WEBRTC=1)
        if(LibDataChannel_FOUND)
            message(STATUS "WebRTC endpoint enabled (libdatachannel found via find_package)")
        else()
            message(STATUS "WebRTC endpoint enabled (libdatachannel found via pkg-config)")
        endif()
    else()
        set(HAS_WEBRTC OFF)
        message(WARNING "WebRTC endpoint disabled (libdatachannel not found)")
        set(ENABLE_WEBRTC OFF)
    endif()
endif()

if(HAS_SRT)
    set(SRT_INCLUDE_DIRS ${SRT_PC_INCLUDE_DIRS})
    set(SRT_LIBRARY_DIRS ${SRT_PC_LIBRARY_DIRS})

    find_library(SRT_GNUTLS_LIBRARY NAMES srt-gnutls HINTS ${SRT_PC_LIBRARY_DIRS})
    find_library(SRT_GENERIC_LIBRARY NAMES srt HINTS ${SRT_PC_LIBRARY_DIRS})
    
    set(ZST_FFMPEG_SRT_BACKEND "none")
    set(ZST_AVFORMAT_DYNAMIC_SECTION "")
    if(HAS_FFMPEG)
        find_library(AVFORMAT_LIBRARY NAMES avformat HINTS ${AVFORMAT_LIBRARY_DIRS})
        if(UNIX AND NOT CMAKE_CROSSCOMPILING AND AVFORMAT_LIBRARY)
            find_program(READELF_EXECUTABLE NAMES readelf)
            if(READELF_EXECUTABLE)
                execute_process(
                    COMMAND ${READELF_EXECUTABLE} -d ${AVFORMAT_LIBRARY}
                    RESULT_VARIABLE ZST_READELF_RESULT
                    OUTPUT_VARIABLE ZST_AVFORMAT_DYNAMIC_SECTION
                    ERROR_QUIET
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                )
            elseif(CMAKE_OBJDUMP)
                execute_process(
                    COMMAND ${CMAKE_OBJDUMP} -p ${AVFORMAT_LIBRARY}
                    RESULT_VARIABLE ZST_READELF_RESULT
                    OUTPUT_VARIABLE ZST_AVFORMAT_DYNAMIC_SECTION
                    ERROR_QUIET
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                )
            endif()

            if(ZST_AVFORMAT_DYNAMIC_SECTION MATCHES "libsrt-gnutls\\.so")
                set(ZST_FFMPEG_SRT_BACKEND "gnutls")
            elseif(ZST_AVFORMAT_DYNAMIC_SECTION MATCHES "libsrt[^/\n]*\\.so")
                set(ZST_FFMPEG_SRT_BACKEND "generic")
            endif()
        endif()
    endif()

    if(ZST_FFMPEG_SRT_BACKEND STREQUAL "gnutls")
        if(NOT SRT_GNUTLS_LIBRARY)
            message(FATAL_ERROR
                "libavformat is linked against libsrt-gnutls, but libsrt-gnutls "
                "was not found for zstreamer. Install the matching SRT development "
                "package or disable one of the conflicting components."
            )
        endif()
        set(SRT_LIBRARIES ${SRT_GNUTLS_LIBRARY})
        set(ZST_SRT_LINK_BACKEND "libsrt-gnutls (matched libavformat)")
    elseif(ZST_FFMPEG_SRT_BACKEND STREQUAL "generic")
        if(NOT SRT_GENERIC_LIBRARY)
            message(FATAL_ERROR
                "libavformat is linked against libsrt.so, but libsrt.so was not "
                "found for zstreamer. Install the matching SRT development package "
                "or disable one of the conflicting components."
            )
        endif()
        set(SRT_LIBRARIES ${SRT_GENERIC_LIBRARY})
        set(ZST_SRT_LINK_BACKEND "libsrt (matched libavformat)")
    else()
        if(SRT_GNUTLS_LIBRARY)
            set(SRT_LIBRARIES ${SRT_GNUTLS_LIBRARY})
            set(ZST_SRT_LINK_BACKEND "libsrt-gnutls (preferred; libavformat SRT not detected)")
        elseif(SRT_GENERIC_LIBRARY)
            set(SRT_LIBRARIES ${SRT_GENERIC_LIBRARY})
            set(ZST_SRT_LINK_BACKEND "libsrt (fallback; libavformat SRT not detected)")
        else()
            set(SRT_LIBRARIES ${SRT_PC_LIBRARIES})
            set(ZST_SRT_LINK_BACKEND "${SRT_PC_LIBRARIES} (pkg-config fallback)")
        endif()
    endif()

    message(STATUS "libavformat SRT backend: ${ZST_FFMPEG_SRT_BACKEND}")
    message(STATUS "zstreamer SRT link: ${ZST_SRT_LINK_BACKEND}")
endif()

find_package(CUDAToolkit QUIET)
if(CUDAToolkit_FOUND)
    add_compile_definitions(HAS_CUDA=1)
endif()

# ── NVIDIA Jetson Allocator Support ──────────────────────────────────
option(ENABLE_JETSON "Enable Jetson NvBuffer allocator support" OFF)
# ── OpenGL Sinks (glsink / glcompsink) ───────────────────────────────────────
if(ENABLE_GLSINK OR ENABLE_GLCOMPSINK)
    find_package(OpenGL QUIET)
    if(X11_FOUND AND OPENGL_FOUND)
        set(HAS_GL ON)
        add_compile_definitions(HAS_GL=1)
        if(ENABLE_GLSINK)
            add_compile_definitions(HAS_GLSINK=1)
        endif()
        if(ENABLE_GLCOMPSINK)
            add_compile_definitions(HAS_GLCOMPSINK=1)
        endif()
        message(STATUS "OpenGL sinks enabled: glsink=${ENABLE_GLSINK}, glcompsink=${ENABLE_GLCOMPSINK} (X11=${X11_FOUND}, GL=${OPENGL_FOUND})")
    else()
        message(STATUS "OpenGL sinks disabled: X11_FOUND=${X11_FOUND}, OPENGL_FOUND=${OPENGL_FOUND}")
        set(ENABLE_GLSINK OFF)
        set(ENABLE_GLCOMPSINK OFF)
    endif()
endif()

# ── X11 Sink (x11sink) ───────────────────────────────────────────────────────
if(ENABLE_X11SINK)
    if(X11_FOUND)
        set(HAS_X11SINK ON)
        add_compile_definitions(HAS_X11SINK=1)
        message(STATUS "X11 sink enabled")
    else()
        set(ENABLE_X11SINK OFF)
        set(HAS_X11SINK OFF)
        message(STATUS "X11 sink disabled: X11 not found")
    endif()
endif()

if(ENABLE_JETSON)
    add_compile_definitions(HAS_JETSON=1)
    find_library(NVBUF_UTILS_LIBRARY NAMES nvbuf_utils)
    if(NVBUF_UTILS_LIBRARY)
        set(JETSON_LIBRARIES ${NVBUF_UTILS_LIBRARY})
        message(STATUS "Jetson NvBuffer utils found")
    else()
        message(WARNING "Jetson NvBuffer utils not found, falling back to stub for tests")
        set(JETSON_LIBRARIES "")
        # Add stub path
        include_directories(tests)
        set(STUB_SOURCES tests/stub_nvbuf_utils.cpp)
    endif()
endif()

# ── Intel oneAPI SYCL Allocator Support ──────────────────────────────
option(ENABLE_ONEAPI "Enable Intel oneAPI SYCL allocator support" ON)
if(ENABLE_ONEAPI)
    enable_language(CXX OPTIONAL)
    if(CMAKE_CXX_COMPILER_LOADED)
        include(CheckCXXSourceCompiles)
        set(CMAKE_REQUIRED_FLAGS "-fsycl")
        check_cxx_source_compiles("
            #include <sycl/sycl.hpp>
            int main() {
                sycl::queue q;
                return 0;
            }
        " HAS_SYCL_SUPPORT)
        unset(CMAKE_REQUIRED_FLAGS)

        if(HAS_SYCL_SUPPORT)
            set(HAS_ONEAPI 1)
            add_compile_definitions(HAS_ONEAPI=1)
            message(STATUS "Intel oneAPI SYCL allocator support enabled (-fsycl)")
        else()
            find_package(sycl QUIET)
            if(sycl_FOUND)
                set(HAS_ONEAPI 1)
                add_compile_definitions(HAS_ONEAPI=1)
                message(STATUS "SYCL allocator support enabled via find_package(sycl)")
            else()
                message(STATUS "Intel oneAPI SYCL allocator support not found or compiler lacks -fsycl")
            endif()
        endif()
    else()
        message(STATUS "C++ compiler not loaded, skipping Intel oneAPI SYCL check")
    endif()
endif()

if(ENABLE_ONEAPI_ENCODER)
    if(NOT HAS_ONEAPI)
        message(FATAL_ERROR "ENABLE_ONEAPI_ENCODER requires ENABLE_ONEAPI with working SYCL support")
    endif()

    pkg_check_modules(ONEVPL vpl)
    if(ONEVPL_FOUND)
        add_compile_definitions(HAS_ONEAPI_ENCODER=1)
        message(STATUS "Intel oneAPI video encoder dependencies found")
    else()
        message(FATAL_ERROR "ENABLE_ONEAPI_ENCODER requires oneVPL development files (pkg-config module: vpl)")
    endif()
endif()

if(ENABLE_VAAPI_ENCODER)
    if(NOT HAS_FFMPEG)
        message(FATAL_ERROR "ENABLE_VAAPI_ENCODER requires FFmpeg libavcodec/libavutil development files")
    endif()
    pkg_check_modules(LIBVA libva libva-drm)
    if(LIBVA_FOUND)
        add_compile_definitions(HAS_VAAPI_ENCODER=1)
        message(STATUS "VA-API video encoder dependencies found")
    else()
        message(FATAL_ERROR "ENABLE_VAAPI_ENCODER requires libva development files (pkg-config modules: libva libva-drm)")
    endif()
endif()

if(ENABLE_VAAPI_DECODER)
    if(NOT HAS_FFMPEG)
        message(FATAL_ERROR "ENABLE_VAAPI_DECODER requires FFmpeg libavcodec/libavutil development files")
    endif()
    pkg_check_modules(LIBVA libva libva-drm)
    if(LIBVA_FOUND)
        add_compile_definitions(HAS_VAAPI_DECODER=1)
        message(STATUS "VA-API video decoder dependencies found")
    else()
        message(FATAL_ERROR "ENABLE_VAAPI_DECODER requires libva development files (pkg-config modules: libva libva-drm)")
    endif()
endif()

set(ELEMENT_INCLUDE_DIRS)
set(ELEMENT_LIBRARIES)

if(HAS_FFMPEG)
    list(APPEND ELEMENT_INCLUDE_DIRS
        ${AVFORMAT_INCLUDE_DIRS}
        ${AVCODEC_INCLUDE_DIRS}
        ${AVUTIL_INCLUDE_DIRS}
        ${SWSCALE_INCLUDE_DIRS}
        ${SWRESAMPLE_INCLUDE_DIRS}
    )
    list(APPEND ELEMENT_LIBRARIES
        ${AVFORMAT_LIBRARIES}
        ${AVCODEC_LIBRARIES}
        ${AVUTIL_LIBRARIES}
        ${SWSCALE_LIBRARIES}
        ${SWRESAMPLE_LIBRARIES}
    )
endif()

if(HAS_X264)
    list(APPEND ELEMENT_INCLUDE_DIRS ${X264_INCLUDE_DIRS})
    list(APPEND ELEMENT_LIBRARIES ${X264_LIBRARIES})
endif()

if(HAS_X265)
    list(APPEND ELEMENT_INCLUDE_DIRS ${X265_INCLUDE_DIRS})
    list(APPEND ELEMENT_LIBRARIES ${X265_LIBRARIES})
endif()

if(HAS_ALSA)
    list(APPEND ELEMENT_INCLUDE_DIRS ${ALSA_INCLUDE_DIRS})
    list(APPEND ELEMENT_LIBRARIES ${ALSA_LIBRARIES})
endif()

if(HAS_V4L2)
    list(APPEND ELEMENT_INCLUDE_DIRS ${V4L2_INCLUDE_DIRS})
    list(APPEND ELEMENT_LIBRARIES ${V4L2_LIBRARIES})
endif()

if(HAS_FREETYPE)
    list(APPEND ELEMENT_INCLUDE_DIRS ${FREETYPE_INCLUDE_DIRS})
    list(APPEND ELEMENT_LIBRARIES ${FREETYPE_LIBRARIES})
endif()

if(HAS_SRT)
    list(APPEND ELEMENT_INCLUDE_DIRS ${SRT_INCLUDE_DIRS})
    list(APPEND ELEMENT_LIBRARIES ${SRT_LIBRARIES})
endif()

if(ENABLE_GLSINK OR ENABLE_GLCOMPSINK)
    list(APPEND ELEMENT_INCLUDE_DIRS ${X11_INCLUDE_DIR} ${OPENGL_INCLUDE_DIR})
    list(APPEND ELEMENT_LIBRARIES ${X11_LIBRARIES} ${OPENGL_LIBRARIES} ${X11_Xext_LIB})
endif()

if(ENABLE_X11SINK)
    list(APPEND ELEMENT_INCLUDE_DIRS ${X11_INCLUDE_DIR})
    list(APPEND ELEMENT_LIBRARIES ${X11_LIBRARIES})
endif()

if(ENABLE_IPP_COMP_SINK)
    list(APPEND ELEMENT_INCLUDE_DIRS $ENV{IPPROOT}/include/ipp)
    list(APPEND ELEMENT_LIBRARIES ippcore ippi ippcc ipps)
endif()


if(ENABLE_ONEAPI_ENCODER)
    list(APPEND ELEMENT_INCLUDE_DIRS ${ONEVPL_INCLUDE_DIRS})
    list(APPEND ELEMENT_LIBRARIES ${ONEVPL_LIBRARIES})
endif()

if(ENABLE_VAAPI_ENCODER)
    list(APPEND ELEMENT_INCLUDE_DIRS ${LIBVA_INCLUDE_DIRS})
    list(APPEND ELEMENT_LIBRARIES ${LIBVA_LIBRARIES})
endif()

if(ENABLE_VAAPI_DECODER)
    list(APPEND ELEMENT_INCLUDE_DIRS ${LIBVA_INCLUDE_DIRS})
    list(APPEND ELEMENT_LIBRARIES ${LIBVA_LIBRARIES})
endif()

if(HAS_WEBRTC)
    if(LibDataChannel_FOUND)
        # Extract include dirs from the imported target for ELEMENT_INCLUDE_DIRS
        get_target_property(_ldc_includes LibDataChannel::LibDataChannel INTERFACE_INCLUDE_DIRECTORIES)
        if(_ldc_includes)
            list(APPEND ELEMENT_INCLUDE_DIRS ${_ldc_includes})
        endif()
        list(APPEND ELEMENT_LIBRARIES LibDataChannel::LibDataChannel)
    elseif(DATACHANNEL_FOUND)
        list(APPEND ELEMENT_INCLUDE_DIRS ${DATACHANNEL_INCLUDE_DIRS})
        list(APPEND ELEMENT_LIBRARIES ${DATACHANNEL_LIBRARIES})
    endif()
endif()

# ── Include directories ──────────────────────────────────────────────────
set(ZSTREAMER_INCLUDE_DIR
    ${CMAKE_SOURCE_DIR}/include
)

# ── Core library sources ─────────────────────────────────────────────────
set(CORE_SOURCES
    src/zst_caps.c
    src/zst_buffer.c
    src/zst_buffer_pool.c
    src/zst_pad.c
    src/zst_element.c
    src/zst_bin.c
    src/zst_pipeline.c
    src/zst_queue.c
    src/zst_queue_element.c
    src/zst_scheduler.c
    src/zst_bus.c
    src/zst_log.c
    src/zst_allocator.c
    src/zst_allocator_dmabuf.c
    src/zst_allocator_cuda.c
    src/zst_clock.c
    src/zst_timestamp_pacer.c
    src/zst_pad_event.c
    src/zst_stream.c
)

if(ENABLE_PLUGINS)
    list(APPEND CORE_SOURCES src/zst_plugin.c)
endif()

if(HAS_ONEAPI)
    list(APPEND CORE_SOURCES src/zst_allocator_oneapi.cpp)
endif()

if(ENABLE_JETSON)
    list(APPEND CORE_SOURCES src/zst_allocator_jetson.cpp ${STUB_SOURCES})
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
    src/zst_builtins.c
    src/file_sink.c
    src/file_source.c
    src/fake_sink.c
    src/video_test_src.c
    src/audio_test_src.c
    src/audio_mixer.c
    src/srt_parser.c
    src/sdp_demuxer.c
    src/sdp_muxer.c
    src/rtp_payloader.c
    src/rtp_depayloader.c
    src/net_source.c
    src/net_sink.c
    src/rtsp_server.c
    src/nv_video_scaler.c
    src/sc6f0_source.c
    src/zst_ws_server.c
)

if(HAS_V4L2)
    list(APPEND ELEMENT_LIBRARY_SOURCES src/v4l2_source.c src/v4l2_sink.c)
endif()

if(HAS_ALSA)
    list(APPEND ELEMENT_LIBRARY_SOURCES src/alsa_source.c src/alsa_sink.c)
endif()

if(HAS_X264)
    list(APPEND ELEMENT_LIBRARY_SOURCES src/x264_encoder.c)
endif()

if(HAS_X265)
    list(APPEND ELEMENT_LIBRARY_SOURCES src/x265_encoder.c)
endif()

if(HAS_FFMPEG)
    list(APPEND ELEMENT_LIBRARY_SOURCES
        src/h264_decoder.c
        src/h265_encoder.c
        src/h265_decoder.c
        src/vp8_decoder.c
        src/vp8_encoder.c
        src/vp9_decoder.c
        src/vp9_encoder.c
        src/aac_encoder.c
        src/opus_encoder.c
        src/opus_decoder.c
        src/aac_decoder.c
        src/mp4_muxer.c
        src/video_scaler.c
        src/audio_resampler.c
        src/rtsp_source.c
        src/rtsp_sink.c
        src/rtmp_source.c
        src/rtmp_sink.c
        src/mpegts_muxer.c
        src/mpegts_demuxer.c
        src/mp4_demuxer.c
        src/http_source.c
    )
endif()

if(HAS_FREETYPE)
    list(APPEND ELEMENT_LIBRARY_SOURCES src/text_overlay.c src/text_source.c)
endif()

if(HAS_SRT)
    list(APPEND ELEMENT_LIBRARY_SOURCES src/srt_source.c src/srt_sink.c src/srt_common.c)
endif()

if(ENABLE_JETSON)
    list(APPEND ELEMENT_LIBRARY_SOURCES src/nv_video_encoder.c src/nv_video_decoder.c)
endif()

if(ENABLE_ONEAPI_ENCODER)
    list(APPEND ELEMENT_LIBRARY_SOURCES src/oneapi_video_encoder.c)
endif()

if(ENABLE_VAAPI_ENCODER)
    list(APPEND ELEMENT_LIBRARY_SOURCES src/vaapi_video_encoder.c)
endif()

if(ENABLE_VAAPI_DECODER)
    list(APPEND ELEMENT_LIBRARY_SOURCES src/vaapi_video_decoder.c)
endif()

if(ENABLE_GLSINK)
    list(APPEND ELEMENT_LIBRARY_SOURCES src/gl_sink.c)
endif()

if(ENABLE_GLCOMPSINK)
    list(APPEND ELEMENT_LIBRARY_SOURCES src/gl_comp_sink.c)
endif()

if(ENABLE_IPP_COMP_SINK)
    list(APPEND ELEMENT_LIBRARY_SOURCES src/ipp_comp_sink.c)
endif()

if(ENABLE_X11SINK)
    list(APPEND ELEMENT_LIBRARY_SOURCES src/x11_sink.c)
endif()

if(HAS_WEBRTC)
    list(APPEND ELEMENT_LIBRARY_SOURCES src/webrtc_endpoint.c src/zst_webrtc_twcc.c)
endif()

if(ENABLE_ST2110)
    list(APPEND ELEMENT_LIBRARY_SOURCES
        src/st2110_20_payloader.c
        src/st2110_20_depayloader.c
        src/st2110_30_payloader.c
        src/st2110_30_depayloader.c
        src/st2110_40_payloader.c
        src/st2110_40_depayloader.c
        src/ptp_clock.c
        src/st2110_redundancy_mux.c
        src/st2110_redundancy_demux.c
        src/zst_st2110_utils.c
    )
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
    src/v4l2_source.c
    src/v4l2_sink.c
    src/x264_encoder.c
    src/x265_encoder.c
    src/mp4_muxer.c
    src/file_sink.c
    src/fake_sink.c
    src/alsa_source.c
    src/aac_encoder.c
    src/opus_encoder.c
    src/opus_decoder.c
    src/video_scaler.c
    src/nv_video_scaler.c
    src/audio_resampler.c
    src/text_overlay.c
    src/video_test_src.c
    src/audio_test_src.c
    src/file_source.c
    src/text_source.c
    src/h264_decoder.c
    src/h265_encoder.c
    src/h265_decoder.c
    src/aac_decoder.c
    src/net_source.c
    src/net_sink.c
    src/rtmp_source.c
    src/rtmp_sink.c
    src/http_source.c
    src/sdp_demuxer.c
    src/sdp_muxer.c
    src/rtp_payloader.c
    src/rtp_depayloader.c
)

# ── Dynamic Plugins ──────────────────────────────────────────────────────
add_library(zst_filesink SHARED src/file_sink.c)
target_compile_definitions(zst_filesink PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_filesink PRIVATE zstreamer)

add_library(zst_fakesink SHARED src/fake_sink.c)
target_compile_definitions(zst_fakesink PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_fakesink PRIVATE zstreamer)

if(HAS_V4L2)
    add_library(zst_v4l2source SHARED src/v4l2_source.c)
    target_compile_definitions(zst_v4l2source PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_v4l2source PRIVATE zstreamer ${V4L2_LIBRARIES})
    
    add_library(zst_v4l2sink SHARED src/v4l2_sink.c)
    target_compile_definitions(zst_v4l2sink PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_v4l2sink PRIVATE zstreamer ${V4L2_LIBRARIES})
endif()

if(HAS_FFMPEG)
    add_library(zst_rtspsource SHARED src/rtsp_source.c)
    target_compile_definitions(zst_rtspsource PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_rtspsource PRIVATE zstreamer ${AVUTIL_LIBRARIES})
    
    add_library(zst_rtspsink SHARED src/rtsp_sink.c)
    target_compile_definitions(zst_rtspsink PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_rtspsink PRIVATE zstreamer ${AVFORMAT_LIBRARIES} ${AVUTIL_LIBRARIES})
endif()

add_library(zst_rtsp_server SHARED src/rtsp_server.c)
target_compile_definitions(zst_rtsp_server PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_rtsp_server PRIVATE zstreamer)

add_library(zst_ws_server SHARED src/zst_ws_server.c)
target_compile_definitions(zst_ws_server PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_ws_server PRIVATE zstreamer)


if(HAS_ALSA)
    add_library(zst_alsasource SHARED src/alsa_source.c)
    target_compile_definitions(zst_alsasource PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_alsasource PRIVATE zstreamer ${ALSA_LIBRARIES})
    
    add_library(zst_alsasink SHARED src/alsa_sink.c)
    target_compile_definitions(zst_alsasink PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_alsasink PRIVATE zstreamer ${ALSA_LIBRARIES})
endif()

if(HAS_X264)
    add_library(zst_x264encoder SHARED src/x264_encoder.c)
    target_compile_definitions(zst_x264encoder PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_x264encoder PRIVATE zstreamer ${X264_LIBRARIES})
endif()

if(HAS_X265)
    add_library(zst_x265encoder SHARED src/x265_encoder.c)
    target_compile_definitions(zst_x265encoder PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_x265encoder PRIVATE zstreamer ${X265_LIBRARIES})
endif()

if(HAS_FFMPEG)
    add_library(zst_aacencoder SHARED src/aac_encoder.c)
    add_library(zst_opusencoder SHARED src/opus_encoder.c)
    target_compile_definitions(zst_opusencoder PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_opusencoder PRIVATE zstreamer ${AVCODEC_LIBRARIES})
    target_compile_definitions(zst_aacencoder PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_aacencoder PRIVATE zstreamer ${AVCODEC_LIBRARIES})
    
    add_library(zst_mp4muxer SHARED src/mp4_muxer.c)
    target_compile_definitions(zst_mp4muxer PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_mp4muxer PRIVATE zstreamer ${AVFORMAT_LIBRARIES} ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES})
    
    add_library(zst_videoscaler SHARED src/video_scaler.c)
    target_compile_definitions(zst_videoscaler PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_videoscaler PRIVATE zstreamer ${SWSCALE_LIBRARIES} ${AVUTIL_LIBRARIES})
    
    add_library(zst_audioresampler SHARED src/audio_resampler.c)
    target_compile_definitions(zst_audioresampler PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_audioresampler PRIVATE zstreamer ${SWRESAMPLE_LIBRARIES} ${AVUTIL_LIBRARIES})
endif()

if(HAS_FREETYPE)
    add_library(zst_textoverlay SHARED src/text_overlay.c)
    target_compile_definitions(zst_textoverlay PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_textoverlay PRIVATE zstreamer ${FREETYPE_LIBRARIES})
endif()

add_library(zst_srtparser SHARED src/srt_parser.c)
target_compile_definitions(zst_srtparser PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_srtparser PRIVATE zstreamer)

add_library(zst_videotestsrc SHARED src/video_test_src.c)
target_compile_definitions(zst_videotestsrc PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_videotestsrc PRIVATE zstreamer)

add_library(zst_audiotestsrc SHARED src/audio_test_src.c)
target_compile_definitions(zst_audiotestsrc PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_audiotestsrc PRIVATE zstreamer)

add_library(zst_audiomixer SHARED src/audio_mixer.c)
target_compile_definitions(zst_audiomixer PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_audiomixer PRIVATE zstreamer)

add_library(zst_filesource SHARED src/file_source.c)
target_compile_definitions(zst_filesource PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_filesource PRIVATE zstreamer)

if(HAS_FREETYPE)
    add_library(zst_textsource SHARED src/text_source.c)
    target_compile_definitions(zst_textsource PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_textsource PRIVATE zstreamer ${FREETYPE_LIBRARIES})
endif()

add_library(zst_netsrc SHARED src/net_source.c)
target_compile_definitions(zst_netsrc PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_netsrc PRIVATE zstreamer)

add_library(zst_netsink SHARED src/net_sink.c)
target_compile_definitions(zst_netsink PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_netsink PRIVATE zstreamer)

if(ENABLE_GLSINK)
    add_library(zst_glsink SHARED src/gl_sink.c)
    target_compile_definitions(zst_glsink PRIVATE BUILDING_PLUGIN)
    target_include_directories(zst_glsink PRIVATE ${X11_INCLUDE_DIR} ${OPENGL_INCLUDE_DIR})
    target_link_libraries(zst_glsink PRIVATE zstreamer ${X11_LIBRARIES} ${OPENGL_LIBRARIES} ${X11_Xext_LIB})
endif()

if(ENABLE_GLCOMPSINK)
    add_library(zst_glcompsink SHARED src/gl_comp_sink.c)
    target_compile_definitions(zst_glcompsink PRIVATE BUILDING_PLUGIN)
    target_include_directories(zst_glcompsink PRIVATE ${X11_INCLUDE_DIR} ${OPENGL_INCLUDE_DIR})
    target_link_libraries(zst_glcompsink PRIVATE zstreamer ${X11_LIBRARIES} ${OPENGL_LIBRARIES} ${X11_Xext_LIB})
endif()


if(ENABLE_IPP_COMP_SINK)
    add_compile_definitions(HAS_IPP_COMP_SINK=1)
    add_library(zst_ippcompsink SHARED src/ipp_comp_sink.c)
    target_compile_definitions(zst_ippcompsink PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_ippcompsink PRIVATE zstreamer ippcore ippi ippcc ipps)
endif()

if(ENABLE_X11SINK)
    add_library(zst_x11sink SHARED src/x11_sink.c)
    target_compile_definitions(zst_x11sink PRIVATE BUILDING_PLUGIN)
    target_include_directories(zst_x11sink PRIVATE ${X11_INCLUDE_DIR})
    target_link_libraries(zst_x11sink PRIVATE zstreamer ${X11_LIBRARIES})
endif()

if(HAS_FFMPEG)
    add_library(zst_h264decoder SHARED src/h264_decoder.c)
    target_compile_definitions(zst_h264decoder PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_h264decoder PRIVATE zstreamer ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES})
    
    add_library(zst_h265encoder SHARED src/h265_encoder.c)
    target_compile_definitions(zst_h265encoder PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_h265encoder PRIVATE zstreamer ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES})
    
    add_library(zst_h265decoder SHARED src/h265_decoder.c)
    target_compile_definitions(zst_h265decoder PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_h265decoder PRIVATE zstreamer ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES})
    
    add_library(zst_vp8decoder SHARED src/vp8_decoder.c)
    target_compile_definitions(zst_vp8decoder PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_vp8decoder PRIVATE zstreamer ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES})
    
    add_library(zst_vp9decoder SHARED src/vp9_decoder.c)
    target_compile_definitions(zst_vp9decoder PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_vp9decoder PRIVATE zstreamer ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES})
    
    add_library(zst_vp8encoder SHARED src/vp8_encoder.c)
    target_compile_definitions(zst_vp8encoder PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_vp8encoder PRIVATE zstreamer ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES})
    
    add_library(zst_vp9encoder SHARED src/vp9_encoder.c)
    target_compile_definitions(zst_vp9encoder PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_vp9encoder PRIVATE zstreamer ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES})
    
    add_library(zst_aacdecoder SHARED src/aac_decoder.c)
    target_compile_definitions(zst_aacdecoder PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_aacdecoder PRIVATE zstreamer ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES})
    add_library(zst_opusdecoder SHARED src/opus_decoder.c)
    target_compile_definitions(zst_opusdecoder PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_opusdecoder PRIVATE zstreamer ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES})
    
    add_library(zst_rtmpsource SHARED src/rtmp_source.c)
    target_compile_definitions(zst_rtmpsource PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_rtmpsource PRIVATE zstreamer ${AVFORMAT_LIBRARIES} ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES})
    
    add_library(zst_rtmpsink SHARED src/rtmp_sink.c)
    target_compile_definitions(zst_rtmpsink PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_rtmpsink PRIVATE zstreamer ${AVFORMAT_LIBRARIES} ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES})
endif()

if(HAS_SRT)
    add_library(zst_srt SHARED src/srt_source.c src/srt_sink.c src/srt_common.c src/srt_plugin.c)
    target_compile_definitions(zst_srt PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_srt PRIVATE zstreamer ${SRT_LIBRARIES})
endif()

if(HAS_FFMPEG)
    add_library(zst_mpegtsmux SHARED src/mpegts_muxer.c)
    target_compile_definitions(zst_mpegtsmux PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_mpegtsmux PRIVATE zstreamer ${AVFORMAT_LIBRARIES} ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES})
    
    add_library(zst_mpegtsdemux SHARED src/mpegts_demuxer.c)
    target_compile_definitions(zst_mpegtsdemux PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_mpegtsdemux PRIVATE zstreamer ${AVFORMAT_LIBRARIES} ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES})
    
    add_library(zst_mp4demux SHARED src/mp4_demuxer.c)
    target_compile_definitions(zst_mp4demux PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_mp4demux PRIVATE zstreamer ${AVFORMAT_LIBRARIES} ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES})
    
    add_library(zst_httpsource SHARED src/http_source.c)
    target_compile_definitions(zst_httpsource PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_httpsource PRIVATE zstreamer ${AVFORMAT_LIBRARIES} ${AVUTIL_LIBRARIES})
endif()

add_library(zst_sdpdemux SHARED src/sdp_demuxer.c)
target_compile_definitions(zst_sdpdemux PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_sdpdemux PRIVATE zstreamer)

add_library(zst_sdpmuxer SHARED src/sdp_muxer.c)
target_compile_definitions(zst_sdpmuxer PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_sdpmuxer PRIVATE zstreamer)

add_library(zst_rtppay SHARED src/rtp_payloader.c)
target_compile_definitions(zst_rtppay PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_rtppay PRIVATE zstreamer)

add_library(zst_rtpdepay SHARED src/rtp_depayloader.c)
target_compile_definitions(zst_rtpdepay PRIVATE BUILDING_PLUGIN)
target_link_libraries(zst_rtpdepay PRIVATE zstreamer)


if(ENABLE_JETSON)
    add_library(zst_nvenc SHARED src/nv_video_encoder.c)
    target_compile_definitions(zst_nvenc PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_nvenc PRIVATE zstreamer ${JETSON_LIBRARIES})

    add_library(zst_nvdec SHARED src/nv_video_decoder.c)
    target_compile_definitions(zst_nvdec PRIVATE BUILDING_PLUGIN)
    target_link_libraries(zst_nvdec PRIVATE zstreamer ${JETSON_LIBRARIES})

    set(JETSON_PLUGINS zst_nvenc zst_nvdec)
else()
    set(JETSON_PLUGINS "")
endif()

if(ENABLE_ONEAPI_ENCODER)
    add_library(zst_oneapienc SHARED src/oneapi_video_encoder.c)
    target_compile_definitions(zst_oneapienc PRIVATE BUILDING_PLUGIN)
    target_include_directories(zst_oneapienc PRIVATE ${ONEVPL_INCLUDE_DIRS})
    target_link_libraries(zst_oneapienc PRIVATE zstreamer ${ONEVPL_LIBRARIES})
    set(ONEAPI_ENCODER_PLUGINS zst_oneapienc)
else()
    set(ONEAPI_ENCODER_PLUGINS "")
endif()

if(ENABLE_VAAPI_ENCODER)
    add_library(zst_vaapienc SHARED src/vaapi_video_encoder.c)
    target_compile_definitions(zst_vaapienc PRIVATE BUILDING_PLUGIN)
    target_include_directories(zst_vaapienc PRIVATE ${LIBVA_INCLUDE_DIRS})
    target_link_libraries(zst_vaapienc PRIVATE zstreamer ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES} ${LIBVA_LIBRARIES})
    set(VAAPI_ENCODER_PLUGINS zst_vaapienc)
else()
    set(VAAPI_ENCODER_PLUGINS "")
endif()

if(ENABLE_VAAPI_DECODER)
    add_library(zst_vaapidec SHARED src/vaapi_video_decoder.c)
    target_compile_definitions(zst_vaapidec PRIVATE BUILDING_PLUGIN)
    target_include_directories(zst_vaapidec PRIVATE ${LIBVA_INCLUDE_DIRS})
    target_link_libraries(zst_vaapidec PRIVATE zstreamer ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES} ${LIBVA_LIBRARIES})
    set(VAAPI_DECODER_PLUGINS zst_vaapidec)
else()
    set(VAAPI_DECODER_PLUGINS "")
endif()

if(HAS_WEBRTC)
    add_library(zst_webrtc SHARED src/webrtc_endpoint.c src/zst_webrtc_twcc.c)
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

set(PLUGIN_TARGETS zst_filesink zst_fakesink zst_rtsp_server zst_ws_server zst_srtparser zst_videotestsrc zst_audiotestsrc zst_audiomixer zst_filesource zst_netsrc zst_netsink zst_sdpdemux zst_sdpmuxer zst_rtppay zst_rtpdepay ${JETSON_PLUGINS} ${ONEAPI_ENCODER_PLUGINS} ${VAAPI_ENCODER_PLUGINS} ${VAAPI_DECODER_PLUGINS} ${WEBRTC_PLUGINS})

if(HAS_V4L2)
    list(APPEND PLUGIN_TARGETS zst_v4l2source zst_v4l2sink)
endif()

if(HAS_ALSA)
    list(APPEND PLUGIN_TARGETS zst_alsasource zst_alsasink)
endif()

if(HAS_X264)
    list(APPEND PLUGIN_TARGETS zst_x264encoder)
endif()

if(HAS_X265)
    list(APPEND PLUGIN_TARGETS zst_x265encoder)
endif()

if(ENABLE_ONEAPI_ENCODER)
    list(APPEND PLUGIN_TARGETS zst_oneapienc)
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


# ── Tests ────────────────────────────────────────────────────────────────
if(BUILD_TESTS)
    # -- unit / smoke test for core infrastructure --
    add_executable(test_core
        tests/test_core.c
    )
    target_include_directories(test_core
        PRIVATE ${ZSTREAMER_INCLUDE_DIR}
    )
    target_link_libraries(test_core
        PRIVATE zstreamer-elements
    )

    add_executable(test_dynamic
        tests/test_dynamic.c
    )
    target_include_directories(test_dynamic
        PRIVATE ${ZSTREAMER_INCLUDE_DIR}
    )
    target_link_libraries(test_dynamic
        PRIVATE zstreamer-elements
    )
    add_test(NAME test_dynamic COMMAND test_dynamic)

    add_executable(test_reconfigure
        tests/test_reconfigure.c
    )
    target_include_directories(test_reconfigure
        PRIVATE ${ZSTREAMER_INCLUDE_DIR}
    )
    target_link_libraries(test_reconfigure
        PRIVATE zstreamer-elements
    )
    add_test(NAME test_reconfigure COMMAND test_reconfigure)

    add_executable(test_caps_v2
        tests/test_caps_v2.c
    )
    target_include_directories(test_caps_v2
        PRIVATE ${ZSTREAMER_INCLUDE_DIR}
    )
    target_link_libraries(test_caps_v2
        PRIVATE zstreamer-elements
    )
    add_test(NAME test_caps_v2 COMMAND test_caps_v2)

    add_executable(test_videotestsrc_timing
        tests/test_videotestsrc_timing.c
    )
    target_include_directories(test_videotestsrc_timing
        PRIVATE ${ZSTREAMER_INCLUDE_DIR}
    )
    target_link_libraries(test_videotestsrc_timing
        PRIVATE zstreamer-elements m
    )
    add_test(NAME test_videotestsrc_timing
        COMMAND test_videotestsrc_timing
    )

    add_executable(test_audiotestsrc_timing
        tests/test_audiotestsrc_timing.c
    )
    target_include_directories(test_audiotestsrc_timing
        PRIVATE ${ZSTREAMER_INCLUDE_DIR}
    )
    target_link_libraries(test_audiotestsrc_timing
        PRIVATE zstreamer-elements m
    )
    add_test(NAME test_audiotestsrc_timing
        COMMAND test_audiotestsrc_timing
    )

    if(HAS_FFMPEG)
        add_executable(test_mp4demux_timing
            tests/test_mp4demux_timing.c
        )
        target_include_directories(test_mp4demux_timing
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(test_mp4demux_timing
            PRIVATE zstreamer-elements m
        )
        add_test(NAME test_mp4demux_timing
            COMMAND test_mp4demux_timing
        )
    endif()

    add_executable(test_custom_paced_video_source
        tests/test_custom_paced_video_source.c
    )
    target_include_directories(test_custom_paced_video_source
        PRIVATE ${ZSTREAMER_INCLUDE_DIR}
    )
    target_link_libraries(test_custom_paced_video_source
        PRIVATE zstreamer-elements m
    )
    add_test(NAME test_custom_paced_video_source
        COMMAND test_custom_paced_video_source
    )

    add_executable(test_net_source
        tests/test_net_source.c
    )
    target_include_directories(test_net_source
        PRIVATE ${ZSTREAMER_INCLUDE_DIR}
    )
    target_link_libraries(test_net_source
        PRIVATE zstreamer-elements
    )
    add_test(NAME test_net_source COMMAND test_net_source)

    add_executable(test_net_sink
        tests/test_net_sink.c
    )
    target_include_directories(test_net_sink
        PRIVATE ${ZSTREAMER_INCLUDE_DIR}
    )
    target_link_libraries(test_net_sink
        PRIVATE zstreamer-elements
    )
    add_test(NAME test_net_sink COMMAND test_net_sink)

    add_executable(test_ws_server
        tests/test_ws_server.c
    )
    target_include_directories(test_ws_server
        PRIVATE ${ZSTREAMER_INCLUDE_DIR}
    )
    target_link_libraries(test_ws_server
        PRIVATE zstreamer-elements
    )
    add_test(NAME test_ws_server COMMAND test_ws_server)


    if(ENABLE_GLSINK)
        add_executable(test_gl_sink
            tests/test_gl_sink.c
        )
        target_include_directories(test_gl_sink
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(test_gl_sink
            PRIVATE zstreamer-elements
        )
        add_test(NAME test_gl_sink COMMAND test_gl_sink)
    endif()

    if(ENABLE_GLCOMPSINK)
        add_executable(test_gl_comp_sink
            tests/test_gl_comp_sink.c
        )
        target_include_directories(test_gl_comp_sink
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(test_gl_comp_sink
            PRIVATE zstreamer-elements
        )
        add_test(NAME test_gl_comp_sink COMMAND test_gl_comp_sink)
    endif()


    if(ENABLE_IPP_COMP_SINK)
        add_executable(test_ipp_comp_sink
            tests/test_ipp_comp_sink.c
        )
        target_include_directories(test_ipp_comp_sink
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(test_ipp_comp_sink
            PRIVATE zstreamer-elements ippcore ippi ippcc ipps
        )
        add_test(NAME test_ipp_comp_sink COMMAND test_ipp_comp_sink)
    endif()

    if(ENABLE_X11SINK)
        add_executable(test_x11sink
            tests/test_x11sink.c
        )
        target_include_directories(test_x11sink
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(test_x11sink
            PRIVATE zstreamer-elements ${X11_LIBRARIES}
        )
        add_test(NAME test_x11sink COMMAND test_x11sink)
    endif()

    if(HAS_FFMPEG)
        add_executable(test_http_source
            tests/test_http_source.c
        )
        target_include_directories(test_http_source
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(test_http_source
            PRIVATE zstreamer-elements
        )
        add_test(NAME test_http_source COMMAND test_http_source)
    endif()

    if(ENABLE_ONEAPI_ENCODER)
        add_executable(test_oneapi_encoder
            tests/test_oneapi_encoder.c
        )
        target_include_directories(test_oneapi_encoder
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(test_oneapi_encoder
            PRIVATE zstreamer-elements
        )
        add_test(NAME test_oneapi_encoder COMMAND test_oneapi_encoder)
    endif()

    if(ENABLE_VAAPI_ENCODER)
        add_executable(test_vaapi_encoder
            tests/test_vaapi_encoder.c
        )
        target_include_directories(test_vaapi_encoder
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(test_vaapi_encoder
            PRIVATE zstreamer-elements
        )
        add_test(NAME test_vaapi_encoder COMMAND test_vaapi_encoder)

        if(HAS_V4L2)
            add_executable(test_vaapi_dmabuf_integration
                tests/test_vaapi_dmabuf_integration.c
            )
            target_include_directories(test_vaapi_dmabuf_integration
                PRIVATE ${ZSTREAMER_INCLUDE_DIR}
            )
            target_link_libraries(test_vaapi_dmabuf_integration
                PRIVATE zstreamer-elements
            )
            add_test(NAME test_vaapi_dmabuf_integration COMMAND test_vaapi_dmabuf_integration)
        endif()

        add_executable(bench_vaapi
            tests/bench_vaapi.c
        )
        target_include_directories(bench_vaapi
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(bench_vaapi
            PRIVATE zstreamer-elements
        )
    endif()

    if(ENABLE_VAAPI_DECODER)
        add_executable(test_vaapi_decoder
            tests/test_vaapi_decoder.c
        )
        target_include_directories(test_vaapi_decoder
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(test_vaapi_decoder
            PRIVATE zstreamer-elements
        )
        add_test(NAME test_vaapi_decoder COMMAND test_vaapi_decoder)
    endif()

    if(ENABLE_VAAPI_ENCODER)
        add_executable(demo_vaapi_encode
            tests/demo_vaapi_encode.c
        )
        target_include_directories(demo_vaapi_encode
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(demo_vaapi_encode
            PRIVATE zstreamer zstreamer-elements pthread
        )
    endif()

    if(HAS_V4L2)
        add_executable(test_v4l2_loopback
            tests/test_v4l2_loopback.c
        )
        target_include_directories(test_v4l2_loopback
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(test_v4l2_loopback
            PRIVATE zstreamer-elements
        )

        add_executable(test_v4l2_dmabuf_sim
            tests/test_v4l2_dmabuf_sim.c
        )
        target_include_directories(test_v4l2_dmabuf_sim
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(test_v4l2_dmabuf_sim
            PRIVATE zstreamer-elements
        )
    endif()

    # -- full pipeline example --
    if(ENABLE_JETSON)
        add_test(NAME test_nv_video_scaler COMMAND test_nv_video_scaler)

        add_executable(test_nv_video_scaler
            tests/test_nv_video_scaler.c
        )
        target_include_directories(test_nv_video_scaler
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(test_nv_video_scaler
            PRIVATE zstreamer-elements
        )
    endif()

    if(HAS_X264 AND HAS_FFMPEG)
        add_executable(example_record
            tests/example_record.c
        )
        target_include_directories(example_record
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(example_record
            PRIVATE zstreamer-elements
        )

        add_executable(demo_sdp_multicast
            tests/demo_sdp_multicast.c
        )
        target_include_directories(demo_sdp_multicast
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(demo_sdp_multicast
            PRIVATE zstreamer-elements
        )
    endif()

    # -- deterministic 10s colorbar/timecode H.264 + 1kHz AAC MP4 demo --
    # Uses the Element Public API (typed property setters) and
    # zst_register_builtin_elements() for factory-based element creation.
    # zst_register_builtin_elements() lives in libzstreamer-elements and uses
    # strong (non-weak) constructor references, so no --whole-archive needed.
    if(HAS_FFMPEG)
        add_executable(demo_colorbar_mp4
            tests/demo_colorbar_mp4.c
        )
        target_include_directories(demo_colorbar_mp4
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(demo_colorbar_mp4
            PRIVATE zstreamer-elements
        )
    endif()

    # -- RTSP Media-On-Demand demo --
    add_executable(demo_rtsp_mod
        tests/demo_rtsp_mod.c
    )
    target_include_directories(demo_rtsp_mod
        PRIVATE ${ZSTREAMER_INCLUDE_DIR}
    )
    target_link_libraries(demo_rtsp_mod
        PRIVATE zstreamer-elements
    )

    # -- WebRTC signaling loopback test (Phase 2) --
    if(HAS_WEBRTC)
        add_executable(test_webrtc_signaling
            tests/test_webrtc_signaling.c
        )
        target_include_directories(test_webrtc_signaling
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(test_webrtc_signaling
            PRIVATE zstreamer zstreamer-elements LibDataChannel::LibDataChannel
        )
        add_test(NAME test_webrtc_signaling COMMAND test_webrtc_signaling)

        add_executable(test_webrtc_media_send
            tests/test_webrtc_media_send.c
        )
        target_include_directories(test_webrtc_media_send
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(test_webrtc_media_send
            PRIVATE zstreamer zstreamer-elements LibDataChannel::LibDataChannel
        )
        add_test(NAME test_webrtc_media_send COMMAND test_webrtc_media_send)

        add_executable(test_webrtc_media_recv
            tests/test_webrtc_media_recv.c
        )
        target_include_directories(test_webrtc_media_recv
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(test_webrtc_media_recv
            PRIVATE zstreamer zstreamer-elements LibDataChannel::LibDataChannel
        )
        add_test(NAME test_webrtc_media_recv COMMAND test_webrtc_media_recv)

        add_executable(test_webrtc_data_channel
            tests/test_webrtc_data_channel.c
        )
        target_include_directories(test_webrtc_data_channel
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(test_webrtc_data_channel
            PRIVATE zstreamer zstreamer-elements LibDataChannel::LibDataChannel
        )
        add_test(NAME test_webrtc_data_channel COMMAND test_webrtc_data_channel)

        add_executable(test_webrtc_rtcp
            tests/test_webrtc_rtcp.c
        )
        target_include_directories(test_webrtc_rtcp
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(test_webrtc_rtcp
            PRIVATE zstreamer zstreamer-elements LibDataChannel::LibDataChannel
        )
        add_test(NAME test_webrtc_rtcp COMMAND test_webrtc_rtcp)

        add_executable(test_webrtc_multitrack
            tests/test_webrtc_multitrack.c
        )
        target_include_directories(test_webrtc_multitrack
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(test_webrtc_multitrack
            PRIVATE zstreamer zstreamer-elements LibDataChannel::LibDataChannel
        )
        add_test(NAME test_webrtc_multitrack COMMAND test_webrtc_multitrack)

        add_executable(test_webrtc_vp8vp9
            tests/test_webrtc_vp8vp9.c
        )
        target_include_directories(test_webrtc_vp8vp9
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(test_webrtc_vp8vp9
            PRIVATE zstreamer zstreamer-elements LibDataChannel::LibDataChannel
        )
        add_test(NAME test_webrtc_vp8vp9 COMMAND test_webrtc_vp8vp9)

        add_executable(test_webrtc_twcc
            tests/test_webrtc_twcc.c
        )
        target_include_directories(test_webrtc_twcc
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(test_webrtc_twcc
            PRIVATE zstreamer zstreamer-elements LibDataChannel::LibDataChannel m
        )
        add_test(NAME test_webrtc_twcc COMMAND test_webrtc_twcc)

        add_executable(test_webrtc_loopback
            tests/test_webrtc_loopback.c
        )
        target_include_directories(test_webrtc_loopback
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(test_webrtc_loopback
            PRIVATE zstreamer zstreamer-elements LibDataChannel::LibDataChannel
        )
        add_test(NAME test_webrtc_loopback COMMAND test_webrtc_loopback)

        add_executable(test_webrtc_sdpclean
            tests/test_webrtc_sdpclean.c
        )
        target_include_directories(test_webrtc_sdpclean
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(test_webrtc_sdpclean
            PRIVATE zstreamer zstreamer-elements LibDataChannel::LibDataChannel
        )
        add_test(NAME test_webrtc_sdpclean COMMAND test_webrtc_sdpclean)

        add_executable(test_webrtc_restart
            tests/test_webrtc_restart.c
        )
        target_include_directories(test_webrtc_restart
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(test_webrtc_restart
            PRIVATE zstreamer zstreamer-elements LibDataChannel::LibDataChannel
        )
        add_test(NAME test_webrtc_restart COMMAND test_webrtc_restart)

        add_executable(test_webrtc_codec_select
            tests/test_webrtc_codec_select.c
        )
        target_include_directories(test_webrtc_codec_select
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(test_webrtc_codec_select
            PRIVATE zstreamer zstreamer-elements LibDataChannel::LibDataChannel
        )
        add_test(NAME test_webrtc_codec_select COMMAND test_webrtc_codec_select)

        add_executable(webrtc_chrome_server
            examples/webrtc_chrome/server.c
        )
        target_include_directories(webrtc_chrome_server
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(webrtc_chrome_server
            PRIVATE zstreamer zstreamer-elements LibDataChannel::LibDataChannel
        )

        add_executable(webrtc_sender
            examples/webrtc_chrome/webrtc_sender.c
        )
        target_include_directories(webrtc_sender
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(webrtc_sender
            PRIVATE zstreamer zstreamer-elements LibDataChannel::LibDataChannel m
        )

        add_executable(webrtc_receiver
            examples/webrtc_chrome/webrtc_receiver.c
        )
        target_include_directories(webrtc_receiver
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(webrtc_receiver
            PRIVATE zstreamer zstreamer-elements LibDataChannel::LibDataChannel m
        )

        add_executable(test_webrtc_turn
            tests/test_webrtc_turn.c
        )
        target_include_directories(test_webrtc_turn
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(test_webrtc_turn
            PRIVATE zstreamer zstreamer-elements LibDataChannel::LibDataChannel
        )
        add_test(NAME test_webrtc_turn COMMAND test_webrtc_turn)
    endif()

    if(ENABLE_ST2110)
        add_executable(test_st2110
            tests/test_st2110.c
        )
        target_include_directories(test_st2110
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(test_st2110
            PRIVATE zstreamer zstreamer-elements m
        )
        add_test(NAME test_st2110 COMMAND test_st2110)

        add_executable(test_st2110_redundancy
            tests/test_st2110_redundancy.c
        )
        target_include_directories(test_st2110_redundancy
            PRIVATE ${CMAKE_SOURCE_DIR}/include
        )
        target_link_libraries(test_st2110_redundancy
            zstreamer zstreamer-elements
        )
        add_test(NAME test_st2110_redundancy COMMAND test_st2110_redundancy)
    endif()



    if(HAS_FREETYPE)
        add_executable(example_text_overlay
            tests/example_text_overlay.c
        )
        target_include_directories(example_text_overlay
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(example_text_overlay
            PRIVATE zstreamer-elements
        )
    endif()

    # -- GL Compositor demo (requires ENABLE_GLCOMPSINK, run manually with X display) --
    if(ENABLE_GLCOMPSINK)
        add_executable(demo_gl_compsink
            tests/demo_gl_compsink.c
        )
        target_include_directories(demo_gl_compsink
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(demo_gl_compsink
            PRIVATE zstreamer-elements
        )
    endif()

    # Register with CTest
    include(CTest)
    enable_testing()

    add_test(NAME test_core
        COMMAND test_core
    )

    add_test(NAME test_install
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_install.sh ${CMAKE_CURRENT_BINARY_DIR}
    )
endif()

# ── Compiler warnings ────────────────────────────────────────────────────
if(CMAKE_COMPILER_IS_GNUCC OR CMAKE_C_COMPILER_ID MATCHES "Clang")
    add_compile_options(-Wall -Wextra -Wpedantic -Wshadow)
endif()

# ── Installation ─────────────────────────────────────────────────────────
# Install library targets with EXPORT support
install(TARGETS zstreamer zstreamer-elements
    EXPORT zstreamerTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Install dynamic plugins
install(TARGETS ${PLUGIN_TARGETS}
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/zstreamer/plugins
)

# Install public headers
file(GLOB ROOT_HEADERS "include/*.h")
install(FILES ${ROOT_HEADERS}
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/zstreamer
)
install(DIRECTORY include/zstreamer/elements
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/zstreamer
)

# ── pkg-config ───────────────────────────────────────────────────────────
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/zstreamer.pc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/zstreamer.pc"
    @ONLY
)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/zstreamer-elements.pc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/zstreamer-elements.pc"
    @ONLY
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/zstreamer.pc"
    "${CMAKE_CURRENT_BINARY_DIR}/zstreamer-elements.pc"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
)

# ── CMake Export / config ────────────────────────────────────────────────
install(EXPORT zstreamerTargets
    FILE zstreamerTargets.cmake
    NAMESPACE zstreamer::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/zstreamer
)

include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/zstreamerConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY AnyNewerVersion
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/zstreamerConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/zstreamerConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/zstreamer
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/zstreamerConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/zstreamerConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/zstreamer
)

# Append srt_common.c to the srt element targets

# Install AI context files
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/AGENTS.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/llms.txt"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/zstreamer
)

# ── Documentation ────────────────────────────────────────────────────────
find_package(Doxygen)
if(DOXYGEN_FOUND)
    add_custom_target(docs
        COMMAND ${DOXYGEN_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Generating API documentation with Doxygen"
        VERBATIM
    )
endif()

# ── CPack Packaging ───────────────────────────────────────────────────────
set(CPACK_PACKAGE_NAME "zstreamer")
set(CPACK_PACKAGE_VENDOR "zstreamer project")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "zstreamer - A lightweight C/C++ multimedia streaming framework")
set(CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})
set(CPACK_PACKAGE_CONTACT "jules@example.com")
set(CPACK_GENERATOR "TGZ;DEB")
include(CPack)

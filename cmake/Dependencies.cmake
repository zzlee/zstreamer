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

if(ENABLE_DANTE)
    pkg_check_modules(JSON_C QUIET json-c)
    if(NOT JSON_C_FOUND)
        message(FATAL_ERROR "ENABLE_DANTE requires json-c development files (pkg-config module: json-c)")
    endif()
    set(HAS_DANTE ON)
    add_compile_definitions(HAS_DANTE=1)
    message(STATUS "Dante control and video support enabled")
endif()

if(ENABLE_DANTE_DEP)
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR
       NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64|aarch64|arm64)$")
        message(FATAL_ERROR "ENABLE_DANTE_DEP supports only 64-bit x86_64 and AArch64 targets")
    endif()
    set(HAS_DANTE_DEP ON)
    add_compile_definitions(HAS_DANTE_DEP=1)
    message(STATUS "Dante DEP audio support enabled")
endif()

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

pkg_check_modules(SVT_JPEGXS SvtJpegxs)
if(SVT_JPEGXS_FOUND)
    set(HAS_SVT_JPEGXS ON)
    add_compile_definitions(HAS_SVT_JPEGXS=1)
    message(STATUS "SVT-JPEG-XS enabled")
else()
    set(HAS_SVT_JPEGXS OFF)
    message(STATUS "SVT-JPEG-XS disabled")
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

if(ENABLE_ONEAPI_DECODER)
    if(NOT HAS_ONEAPI)
        message(FATAL_ERROR "ENABLE_ONEAPI_DECODER requires ENABLE_ONEAPI with working SYCL support")
    endif()

    pkg_check_modules(ONEVPL vpl)
    pkg_check_modules(LIBVA libva libva-drm)
    if(ONEVPL_FOUND AND LIBVA_FOUND)
        add_compile_definitions(HAS_ONEAPI_DECODER=1)
        message(STATUS "Intel oneAPI video decoder dependencies found (oneVPL + libva)")
    else()
        message(FATAL_ERROR "ENABLE_ONEAPI_DECODER requires oneVPL (vpl) and libva/libva-drm development files")
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
set(ELEMENT_LIBRARY_DIRS)

if(HAS_DANTE)
    list(APPEND ELEMENT_INCLUDE_DIRS ${JSON_C_INCLUDE_DIRS})
    list(APPEND ELEMENT_LIBRARIES ${JSON_C_LIBRARIES})
endif()

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
    list(APPEND ELEMENT_LIBRARY_DIRS
        ${AVFORMAT_LIBRARY_DIRS}
        ${AVCODEC_LIBRARY_DIRS}
        ${AVUTIL_LIBRARY_DIRS}
        ${SWSCALE_LIBRARY_DIRS}
        ${SWRESAMPLE_LIBRARY_DIRS}
    )
endif()

if(HAS_X264)
    list(APPEND ELEMENT_INCLUDE_DIRS ${X264_INCLUDE_DIRS})
    list(APPEND ELEMENT_LIBRARIES ${X264_LIBRARIES})
    list(APPEND ELEMENT_LIBRARY_DIRS ${X264_LIBRARY_DIRS})
endif()

if(HAS_SVT_JPEGXS)
    list(APPEND ELEMENT_INCLUDE_DIRS ${SVT_JPEGXS_INCLUDE_DIRS})
    list(APPEND ELEMENT_LIBRARIES ${SVT_JPEGXS_LIBRARIES})
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
    list(APPEND ELEMENT_LIBRARY_DIRS ${FREETYPE_LIBRARY_DIRS})
endif()

if(HAS_SRT)
    list(APPEND ELEMENT_INCLUDE_DIRS ${SRT_INCLUDE_DIRS})
    list(APPEND ELEMENT_LIBRARIES ${SRT_LIBRARIES})
    list(APPEND ELEMENT_LIBRARY_DIRS ${SRT_LIBRARY_DIRS})
endif()

if(ELEMENT_LIBRARY_DIRS)
    list(REMOVE_DUPLICATES ELEMENT_LIBRARY_DIRS)
    link_directories(${ELEMENT_LIBRARY_DIRS})
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

if(ENABLE_ONEAPI_DECODER)
    list(APPEND ELEMENT_INCLUDE_DIRS ${ONEVPL_INCLUDE_DIRS} ${LIBVA_INCLUDE_DIRS})
    list(APPEND ELEMENT_LIBRARIES ${ONEVPL_LIBRARIES} ${LIBVA_LIBRARIES})
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

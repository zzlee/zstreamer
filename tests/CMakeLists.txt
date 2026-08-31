# ── Tests ────────────────────────────────────────────────────────────────
if(BUILD_TESTS)
    # Tests rely on assert() for validation and setup; Release must not define NDEBUG.
    add_compile_options(-UNDEBUG)

    # -- unit / smoke test for core infrastructure --
    add_executable(test_core
        test_core.c
    )
    target_include_directories(test_core
        PRIVATE ${ZSTREAMER_INCLUDE_DIR}
    )
    target_link_libraries(test_core
        PRIVATE zstreamer zstreamer-elements
    )
    if(ENABLE_PLUGINS)
        target_compile_definitions(test_core PRIVATE ZST_ENABLE_PLUGINS=1)
    endif()

    if(HAS_FFMPEG AND HAS_X264)
        add_executable(test_codec test_codec.c)
        target_include_directories(test_codec PRIVATE ${ZSTREAMER_INCLUDE_DIR})
        target_link_libraries(test_codec PRIVATE zstreamer zstreamer-elements)
        add_test(NAME test_codec COMMAND test_codec)
    endif()

    if(HAS_X265)
        add_executable(test_x265enc test_x265enc.c)
        target_include_directories(test_x265enc PRIVATE ${ZSTREAMER_INCLUDE_DIR})
        target_link_libraries(test_x265enc PRIVATE zstreamer zstreamer-elements)
        add_test(NAME test_x265enc COMMAND test_x265enc)
    endif()

    if(ENABLE_PLUGINS)
        add_executable(test_dynamic
            test_dynamic.c
        )
        target_include_directories(test_dynamic
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(test_dynamic
            PRIVATE zstreamer zstreamer-elements
        )
        add_test(NAME test_dynamic COMMAND test_dynamic)
    endif()

    add_executable(test_reconfigure
        test_reconfigure.c
    )
    target_include_directories(test_reconfigure
        PRIVATE ${ZSTREAMER_INCLUDE_DIR}
    )
    target_link_libraries(test_reconfigure
        PRIVATE zstreamer zstreamer-elements
    )
    add_test(NAME test_reconfigure COMMAND test_reconfigure)

    add_executable(test_caps_v2
        test_caps_v2.c
    )
    target_include_directories(test_caps_v2
        PRIVATE ${ZSTREAMER_INCLUDE_DIR}
    )
    target_link_libraries(test_caps_v2
        PRIVATE zstreamer zstreamer-elements
    )
    add_test(NAME test_caps_v2 COMMAND test_caps_v2)

    add_executable(test_videotestsrc_timing
        test_videotestsrc_timing.c
    )
    target_include_directories(test_videotestsrc_timing
        PRIVATE ${ZSTREAMER_INCLUDE_DIR}
    )
    target_link_libraries(test_videotestsrc_timing
        PRIVATE zstreamer zstreamer-elements m
    )
    add_test(NAME test_videotestsrc_timing
        COMMAND test_videotestsrc_timing
    )

    add_executable(test_audiotestsrc_timing
        test_audiotestsrc_timing.c
    )
    target_include_directories(test_audiotestsrc_timing
        PRIVATE ${ZSTREAMER_INCLUDE_DIR}
    )
    target_link_libraries(test_audiotestsrc_timing
        PRIVATE zstreamer zstreamer-elements m
    )
    add_test(NAME test_audiotestsrc_timing
        COMMAND test_audiotestsrc_timing
    )

    if(HAS_FFMPEG AND HAS_HTTP_SERVER)
        add_executable(test_mp4demux_timing
            test_mp4demux_timing.c
        )
        target_include_directories(test_mp4demux_timing
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(test_mp4demux_timing
            PRIVATE zstreamer zstreamer-elements m
        )
        add_test(NAME test_mp4demux_timing
            COMMAND test_mp4demux_timing
        )
    endif()

    add_executable(test_custom_paced_video_source
        test_custom_paced_video_source.c
    )
    target_include_directories(test_custom_paced_video_source
        PRIVATE ${ZSTREAMER_INCLUDE_DIR}
    )
    target_link_libraries(test_custom_paced_video_source
        PRIVATE zstreamer zstreamer-elements m
    )
    add_test(NAME test_custom_paced_video_source
        COMMAND test_custom_paced_video_source
    )

    add_executable(test_net_source
        test_net_source.c
    )
    target_include_directories(test_net_source
        PRIVATE ${ZSTREAMER_INCLUDE_DIR}
    )
    target_link_libraries(test_net_source
        PRIVATE zstreamer zstreamer-elements
    )
    add_test(NAME test_net_source COMMAND test_net_source)

    add_executable(test_net_sink
        test_net_sink.c
    )
    target_include_directories(test_net_sink
        PRIVATE ${ZSTREAMER_INCLUDE_DIR}
    )
    target_link_libraries(test_net_sink
        PRIVATE zstreamer zstreamer-elements
    )
    add_test(NAME test_net_sink COMMAND test_net_sink)

    if(HAS_FFMPEG AND HAS_HTTP_SERVER)
        add_executable(test_rtp_integration
            test_rtp_integration.c
        )
        target_include_directories(test_rtp_integration
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(test_rtp_integration
            PRIVATE zstreamer zstreamer-elements
        )
        add_test(NAME test_rtp_integration COMMAND test_rtp_integration)
    endif()

    add_executable(test_ws_server
        test_ws_server.c
    )
    target_include_directories(test_ws_server
        PRIVATE ${ZSTREAMER_INCLUDE_DIR}
    )
    target_link_libraries(test_ws_server
        PRIVATE zstreamer zstreamer-elements
    )
    add_test(NAME test_ws_server COMMAND test_ws_server)

    if(HAS_DANTE)
        add_executable(test_dante_session test_dante_session.c)
        target_include_directories(test_dante_session PRIVATE
            ${ZSTREAMER_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/src ${JSON_C_INCLUDE_DIRS})
        target_link_libraries(test_dante_session PRIVATE
            zstreamer zstreamer-elements ${JSON_C_LIBRARIES})
        add_test(NAME test_dante_session COMMAND test_dante_session)
        set_tests_properties(test_dante_session PROPERTIES TIMEOUT 15)

        add_executable(test_dante_udp test_dante_udp.c)
        target_include_directories(test_dante_udp PRIVATE ${ZSTREAMER_INCLUDE_DIR})
        target_link_libraries(test_dante_udp PRIVATE zstreamer zstreamer-elements)
        add_test(NAME test_dante_udp COMMAND test_dante_udp)
        set_tests_properties(test_dante_udp PROPERTIES TIMEOUT 15)

        add_executable(test_dante_video_coordinator test_dante_video_coordinator.c)
        target_include_directories(test_dante_video_coordinator PRIVATE ${ZSTREAMER_INCLUDE_DIR})
        target_link_libraries(test_dante_video_coordinator PRIVATE zstreamer zstreamer-elements)
        add_test(NAME test_dante_video_coordinator COMMAND test_dante_video_coordinator)
        set_tests_properties(test_dante_video_coordinator PROPERTIES TIMEOUT 20)
    endif()

    if(HAS_DANTE_DEP)
        add_executable(test_dante_dep test_dante_dep.c)
        target_include_directories(test_dante_dep PRIVATE
            ${ZSTREAMER_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/src)
        target_link_libraries(test_dante_dep PRIVATE zstreamer zstreamer-elements)
        add_test(NAME test_dante_dep COMMAND test_dante_dep)
        set_tests_properties(test_dante_dep PROPERTIES TIMEOUT 20)
    endif()

    if(HAS_DANTE AND HAS_DANTE_DEP AND HAS_X264)
        add_executable(demo_dante_av_tx demo_dante_av_tx.c)
        target_include_directories(demo_dante_av_tx PRIVATE ${ZSTREAMER_INCLUDE_DIR})
        target_link_libraries(demo_dante_av_tx PRIVATE zstreamer zstreamer-elements)

        add_executable(test_dante_av_tx_mock test_dante_av_tx_mock.c)
        target_include_directories(test_dante_av_tx_mock PRIVATE
            ${ZSTREAMER_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/src)
        target_link_libraries(test_dante_av_tx_mock PRIVATE zstreamer zstreamer-elements)
        add_test(NAME test_dante_av_tx_mock COMMAND test_dante_av_tx_mock)
        set_tests_properties(test_dante_av_tx_mock PROPERTIES TIMEOUT 15)
    endif()

    if(HAS_DANTE AND HAS_DANTE_DEP)
        add_executable(test_dante_av_tx_real test_dante_av_tx_real.c)
        target_include_directories(test_dante_av_tx_real PRIVATE ${ZSTREAMER_INCLUDE_DIR})
        target_link_libraries(test_dante_av_tx_real PRIVATE zstreamer zstreamer-elements pthread)
        # Not added to ctest -- run manually on real hardware:
        #   ./test_dante_av_tx_real [dep_shm_name]
    endif()


    if(ENABLE_GLSINK)
        add_executable(test_gl_sink
            test_gl_sink.c
        )
        target_include_directories(test_gl_sink
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(test_gl_sink
            PRIVATE zstreamer zstreamer-elements
        )
        add_test(NAME test_gl_sink COMMAND test_gl_sink)
    endif()

    if(ENABLE_GLCOMPSINK)
        add_executable(test_gl_comp_sink
            test_gl_comp_sink.c
        )
        target_include_directories(test_gl_comp_sink
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(test_gl_comp_sink
            PRIVATE zstreamer zstreamer-elements
        )
        add_test(NAME test_gl_comp_sink COMMAND test_gl_comp_sink)
    endif()


    if(ENABLE_IPP_COMP_SINK)
        add_executable(test_ipp_comp_sink
            test_ipp_comp_sink.c
        )
        target_include_directories(test_ipp_comp_sink
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(test_ipp_comp_sink
            PRIVATE zstreamer zstreamer-elements ippcore ippi ippcc ipps
        )
        add_test(NAME test_ipp_comp_sink COMMAND test_ipp_comp_sink)
    endif()

    if(ENABLE_X11SINK)
        add_executable(test_x11sink
            test_x11sink.c
        )
        target_include_directories(test_x11sink
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(test_x11sink
            PRIVATE zstreamer zstreamer-elements ${X11_LIBRARIES}
        )
        add_test(NAME test_x11sink COMMAND test_x11sink)
    endif()

    if(HAS_FFMPEG AND HAS_HTTP_SERVER)
        add_executable(test_http_source
            test_http_source.c
        )
        target_include_directories(test_http_source
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(test_http_source
            PRIVATE zstreamer zstreamer-elements
        )
        add_test(NAME test_http_source COMMAND test_http_source)
    endif()

    if(ENABLE_ONEAPI_ENCODER)
        add_executable(test_oneapi_encoder
            test_oneapi_encoder.c
        )
        target_include_directories(test_oneapi_encoder
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(test_oneapi_encoder
            PRIVATE zstreamer zstreamer-elements
        )
        add_test(NAME test_oneapi_encoder COMMAND test_oneapi_encoder)
    endif()

    if(ENABLE_ONEAPI_DECODER)
        add_executable(test_oneapi_decoder
            test_oneapi_decoder.c
        )
        target_include_directories(test_oneapi_decoder
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(test_oneapi_decoder
            PRIVATE zstreamer zstreamer-elements ${ONEVPL_LIBRARIES} ${LIBVA_LIBRARIES}
        )
        add_test(NAME test_oneapi_decoder COMMAND test_oneapi_decoder)
    endif()

    if(ENABLE_VAAPI_ENCODER)
        add_executable(test_vaapi_encoder
            test_vaapi_encoder.c
        )
        target_include_directories(test_vaapi_encoder
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(test_vaapi_encoder
            PRIVATE zstreamer zstreamer-elements
        )
        add_test(NAME test_vaapi_encoder COMMAND test_vaapi_encoder)

        if(HAS_V4L2)
            add_executable(test_vaapi_dmabuf_integration
                test_vaapi_dmabuf_integration.c
            )
            target_include_directories(test_vaapi_dmabuf_integration
                PRIVATE ${ZSTREAMER_INCLUDE_DIR}
            )
            target_link_libraries(test_vaapi_dmabuf_integration
                PRIVATE zstreamer zstreamer-elements
            )
            add_test(NAME test_vaapi_dmabuf_integration COMMAND test_vaapi_dmabuf_integration)
        endif()

        add_executable(bench_vaapi
            bench_vaapi.c
        )
        target_include_directories(bench_vaapi
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(bench_vaapi
            PRIVATE zstreamer zstreamer-elements
        )
    endif()

    if(ENABLE_VAAPI_DECODER)
        add_executable(test_vaapi_decoder
            test_vaapi_decoder.c
        )
        target_include_directories(test_vaapi_decoder
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(test_vaapi_decoder
            PRIVATE zstreamer zstreamer-elements
        )
        add_test(NAME test_vaapi_decoder COMMAND test_vaapi_decoder)
    endif()

    if(ENABLE_VAAPI_ENCODER)
        add_executable(demo_vaapi_encode
            demo_vaapi_encode.c
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
            test_v4l2_loopback.c
        )
        target_include_directories(test_v4l2_loopback
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(test_v4l2_loopback
            PRIVATE zstreamer zstreamer-elements
        )

        add_executable(test_v4l2_dmabuf_sim
            test_v4l2_dmabuf_sim.c
        )
        target_include_directories(test_v4l2_dmabuf_sim
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(test_v4l2_dmabuf_sim
            PRIVATE zstreamer zstreamer-elements
        )
    endif()

    # -- full pipeline example --
    if(ENABLE_JETSON)
        add_test(NAME test_nv_video_scaler COMMAND test_nv_video_scaler)

        add_executable(test_nv_video_scaler
            test_nv_video_scaler.c
        )
        target_include_directories(test_nv_video_scaler
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(test_nv_video_scaler
            PRIVATE zstreamer zstreamer-elements
        )
    endif()

    if(HAS_X264 AND HAS_FFMPEG AND HAS_V4L2 AND HAS_ALSA)
        add_executable(test_codec_h264dec
            test_codec_h264dec.c
        )
        target_include_directories(test_codec_h264dec
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(test_codec_h264dec
            PRIVATE zstreamer zstreamer-elements
        )
        add_test(NAME test_codec_h264dec COMMAND test_codec_h264dec)

        add_executable(example_record
            example_record.c
        )
        target_include_directories(example_record
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(example_record
            PRIVATE zstreamer zstreamer-elements
        )

        add_executable(demo_sdp_multicast
            demo_sdp_multicast.c
        )
        target_include_directories(demo_sdp_multicast
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(demo_sdp_multicast
            PRIVATE zstreamer zstreamer-elements
        )
    endif()

    if(ENABLE_ST2110)
        add_executable(demo_st2110_multicast
            demo_st2110_multicast.c
        )
        target_include_directories(demo_st2110_multicast
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(demo_st2110_multicast
            PRIVATE zstreamer zstreamer-elements
        )
    endif()

    # -- deterministic 10s colorbar/timecode H.264 + 1kHz AAC MP4 demo --
    # Uses the Element Public API (typed property setters) and
    # zst_register_builtin_elements() for factory-based element creation.
    # zst_register_builtin_elements() lives in libzstreamer-elements and uses
    # strong (non-weak) constructor references, so no --whole-archive needed.
    if(HAS_FFMPEG AND HAS_HTTP_SERVER)
        add_executable(demo_colorbar_mp4
            demo_colorbar_mp4.c
        )
        target_include_directories(demo_colorbar_mp4
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(demo_colorbar_mp4
            PRIVATE zstreamer zstreamer-elements
        )
    endif()

    # -- RTSP Media-On-Demand demo --
    add_executable(demo_rtsp_mod
        demo_rtsp_mod.c
    )
    target_include_directories(demo_rtsp_mod
        PRIVATE ${ZSTREAMER_INCLUDE_DIR}
    )
    target_link_libraries(demo_rtsp_mod
        PRIVATE zstreamer zstreamer-elements
    )

    # -- WebRTC signaling loopback test (Phase 2) --
    if(HAS_WEBRTC)
        add_executable(test_webrtc_signaling
            test_webrtc_signaling.c
        )
        target_include_directories(test_webrtc_signaling
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(test_webrtc_signaling
            PRIVATE zstreamer zstreamer-elements LibDataChannel::LibDataChannel
        )
        add_test(NAME test_webrtc_signaling COMMAND test_webrtc_signaling)

        add_executable(test_webrtc_media_send
            test_webrtc_media_send.c
        )
        target_include_directories(test_webrtc_media_send
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(test_webrtc_media_send
            PRIVATE zstreamer zstreamer-elements LibDataChannel::LibDataChannel
        )
        add_test(NAME test_webrtc_media_send COMMAND test_webrtc_media_send)

        add_executable(test_webrtc_media_recv
            test_webrtc_media_recv.c
        )
        target_include_directories(test_webrtc_media_recv
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(test_webrtc_media_recv
            PRIVATE zstreamer zstreamer-elements LibDataChannel::LibDataChannel
        )
        add_test(NAME test_webrtc_media_recv COMMAND test_webrtc_media_recv)

        add_executable(test_webrtc_data_channel
            test_webrtc_data_channel.c
        )
        target_include_directories(test_webrtc_data_channel
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(test_webrtc_data_channel
            PRIVATE zstreamer zstreamer-elements LibDataChannel::LibDataChannel
        )
        add_test(NAME test_webrtc_data_channel COMMAND test_webrtc_data_channel)

        add_executable(test_webrtc_rtcp
            test_webrtc_rtcp.c
        )
        target_include_directories(test_webrtc_rtcp
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(test_webrtc_rtcp
            PRIVATE zstreamer zstreamer-elements LibDataChannel::LibDataChannel
        )
        add_test(NAME test_webrtc_rtcp COMMAND test_webrtc_rtcp)

        add_executable(test_webrtc_multitrack
            test_webrtc_multitrack.c
        )
        target_include_directories(test_webrtc_multitrack
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(test_webrtc_multitrack
            PRIVATE zstreamer zstreamer-elements LibDataChannel::LibDataChannel
        )
        add_test(NAME test_webrtc_multitrack COMMAND test_webrtc_multitrack)

        add_executable(test_webrtc_vp8vp9
            test_webrtc_vp8vp9.c
        )
        target_include_directories(test_webrtc_vp8vp9
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(test_webrtc_vp8vp9
            PRIVATE zstreamer zstreamer-elements LibDataChannel::LibDataChannel
        )
        add_test(NAME test_webrtc_vp8vp9 COMMAND test_webrtc_vp8vp9)

        add_executable(test_webrtc_twcc
            test_webrtc_twcc.c
        )
        target_include_directories(test_webrtc_twcc
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(test_webrtc_twcc
            PRIVATE zstreamer zstreamer-elements LibDataChannel::LibDataChannel m
        )
        add_test(NAME test_webrtc_twcc COMMAND test_webrtc_twcc)

        add_executable(test_webrtc_loopback
            test_webrtc_loopback.c
        )
        target_include_directories(test_webrtc_loopback
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(test_webrtc_loopback
            PRIVATE zstreamer zstreamer-elements LibDataChannel::LibDataChannel
        )
        add_test(NAME test_webrtc_loopback COMMAND test_webrtc_loopback)

        add_executable(test_webrtc_sdpclean
            test_webrtc_sdpclean.c
        )
        target_include_directories(test_webrtc_sdpclean
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(test_webrtc_sdpclean
            PRIVATE zstreamer zstreamer-elements LibDataChannel::LibDataChannel
        )
        add_test(NAME test_webrtc_sdpclean COMMAND test_webrtc_sdpclean)

        add_executable(test_webrtc_restart
            test_webrtc_restart.c
        )
        target_include_directories(test_webrtc_restart
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(test_webrtc_restart
            PRIVATE zstreamer zstreamer-elements LibDataChannel::LibDataChannel
        )
        add_test(NAME test_webrtc_restart COMMAND test_webrtc_restart)

        add_executable(test_webrtc_codec_select
            test_webrtc_codec_select.c
        )
        target_include_directories(test_webrtc_codec_select
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(test_webrtc_codec_select
            PRIVATE zstreamer zstreamer-elements LibDataChannel::LibDataChannel
        )
        add_test(NAME test_webrtc_codec_select COMMAND test_webrtc_codec_select)

        add_executable(webrtc_chrome_server
            ../examples/webrtc_chrome/server.c
        )
        target_include_directories(webrtc_chrome_server
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(webrtc_chrome_server
            PRIVATE zstreamer zstreamer-elements LibDataChannel::LibDataChannel
        )

        add_executable(webrtc_sender
            ../examples/webrtc_chrome/webrtc_sender.c
        )
        target_include_directories(webrtc_sender
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(webrtc_sender
            PRIVATE zstreamer zstreamer-elements LibDataChannel::LibDataChannel m
        )

        add_executable(webrtc_receiver
            ../examples/webrtc_chrome/webrtc_receiver.c
        )
        target_include_directories(webrtc_receiver
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(webrtc_receiver
            PRIVATE zstreamer zstreamer-elements LibDataChannel::LibDataChannel m
        )

        add_executable(test_webrtc_turn
            test_webrtc_turn.c
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
            test_st2110.c
        )
        target_include_directories(test_st2110
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(test_st2110
            PRIVATE zstreamer zstreamer-elements m
        )
        add_test(NAME test_st2110 COMMAND test_st2110)

        add_executable(test_st2110_redundancy
            test_st2110_redundancy.c
        )
        target_include_directories(test_st2110_redundancy
            PRIVATE ${CMAKE_SOURCE_DIR}/include
        )
        target_link_libraries(test_st2110_redundancy
            zstreamer zstreamer-elements
        )
        add_test(NAME test_st2110_redundancy COMMAND test_st2110_redundancy)

        add_executable(test_st2110_ptp
            test_st2110_ptp.c
        )
        target_include_directories(test_st2110_ptp
            PRIVATE ${CMAKE_SOURCE_DIR}/include
        )
        target_link_libraries(test_st2110_ptp
            zstreamer zstreamer-elements
        )
        add_test(NAME test_st2110_ptp COMMAND test_st2110_ptp)

        add_executable(test_st2110_21
            test_st2110_21.c
        )
        target_include_directories(test_st2110_21
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(test_st2110_21
            PRIVATE zstreamer zstreamer-elements
        )
        add_test(NAME test_st2110_21 COMMAND test_st2110_21)
    endif()



    if(HAS_FREETYPE)
        add_executable(example_text_overlay
            example_text_overlay.c
        )
        target_include_directories(example_text_overlay
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(example_text_overlay
            PRIVATE zstreamer zstreamer-elements
        )
    endif()

    # -- GL Compositor demo (requires ENABLE_GLCOMPSINK, run manually with X display) --
    if(ENABLE_GLCOMPSINK)
        add_executable(demo_gl_compsink
            demo_gl_compsink.c
        )
        target_include_directories(demo_gl_compsink
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(demo_gl_compsink
            PRIVATE zstreamer zstreamer-elements
        )
    endif()

    add_test(NAME test_core
        COMMAND test_core
    )

    add_test(NAME test_install
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_install.sh ${CMAKE_BINARY_DIR}
    )

    if(HAS_FFMPEG AND HAS_HTTP_SERVER)
        add_executable(test_hls
            test_hls.c
        )
        target_include_directories(test_hls
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(test_hls
            PRIVATE zstreamer zstreamer-elements
        )
        add_test(NAME test_hls COMMAND test_hls)

        add_executable(demo_hls_server
            demo_hls_server.c
        )
        target_include_directories(demo_hls_server
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(demo_hls_server
            PRIVATE zstreamer zstreamer-elements
        )
    endif()

    if(HAS_SRT AND HAS_FFMPEG AND HAS_X264)
        add_executable(demo_srt_perf
            demo_srt_perf.c
        )
        target_include_directories(demo_srt_perf
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(demo_srt_perf
            PRIVATE zstreamer zstreamer-elements m
        )

        add_executable(test_srt_perf
            test_srt_perf.c
        )
        target_include_directories(test_srt_perf
            PRIVATE ${ZSTREAMER_INCLUDE_DIR}
        )
        target_link_libraries(test_srt_perf
            PRIVATE zstreamer zstreamer-elements m
        )
        add_test(NAME test_srt_perf COMMAND test_srt_perf)
    endif()
endif()

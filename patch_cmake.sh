sed -i '/add_executable(test_rtp_integration/,/add_test(NAME test_rtp_integration COMMAND test_rtp_integration)/ {
    s/add_executable(test_rtp_integration/if(HAS_FFMPEG)\n        add_executable(test_rtp_integration/
    s/add_test(NAME test_rtp_integration COMMAND test_rtp_integration)/add_test(NAME test_rtp_integration COMMAND test_rtp_integration)\n    endif()/
}' CMakeLists.txt

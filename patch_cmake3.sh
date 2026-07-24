sed -i 's/add_executable(test_rtp_integration/if(HAS_FFMPEG AND HAS_X264)\n        add_executable(test_rtp_integration/g' CMakeLists.txt
sed -i 's/add_test(NAME test_rtp_integration COMMAND test_rtp_integration)/add_test(NAME test_rtp_integration COMMAND test_rtp_integration)\n    endif()/g' CMakeLists.txt

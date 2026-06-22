#!/bin/bash
set -e

# Build directory
BUILD_DIR="${1:-build}"
INSTALL_PREFIX="/tmp/zstreamer_install"

echo "=== Running Installation Layout Test ==="
echo "Cleaning old install prefix..."
rm -rf "$INSTALL_PREFIX"

echo "Installing to prefix $INSTALL_PREFIX..."
DESTDIR="" cmake --install "$BUILD_DIR" --prefix "$INSTALL_PREFIX"

echo "Verifying file existence and paths..."

# Helper function to check file
check_file() {
    if [ ! -f "$1" ]; then
        echo "Error: File not found: $1"
        exit 1
    fi
}

# Core and elements libs
if [ -f "$BUILD_DIR/libzstreamer.so" ]; then
    check_file "$INSTALL_PREFIX/lib/libzstreamer.so"
    check_file "$INSTALL_PREFIX/lib/libzstreamer-elements.so"
else
    check_file "$INSTALL_PREFIX/lib/libzstreamer.a"
    check_file "$INSTALL_PREFIX/lib/libzstreamer-elements.a"
fi

# Dynamic plugins
check_file "$INSTALL_PREFIX/lib/zstreamer/plugins/libzst_filesink.so"
check_file "$INSTALL_PREFIX/lib/zstreamer/plugins/libzst_fakesink.so"
check_file "$INSTALL_PREFIX/lib/zstreamer/plugins/libzst_h264encoder.so"

# Public headers
check_file "$INSTALL_PREFIX/include/zstreamer/zst_buffer.h"
check_file "$INSTALL_PREFIX/include/zstreamer/zst_element.h"
check_file "$INSTALL_PREFIX/include/zstreamer/zst_pipeline.h"
check_file "$INSTALL_PREFIX/include/zstreamer/elements/zst_file_sink.h"
check_file "$INSTALL_PREFIX/include/zstreamer/elements/zst_fake_sink.h"

# pkg-config
check_file "$INSTALL_PREFIX/lib/pkgconfig/zstreamer.pc"
check_file "$INSTALL_PREFIX/lib/pkgconfig/zstreamer-elements.pc"

# CMake export configs
check_file "$INSTALL_PREFIX/lib/cmake/zstreamer/zstreamerConfig.cmake"
check_file "$INSTALL_PREFIX/lib/cmake/zstreamer/zstreamerConfigVersion.cmake"
check_file "$INSTALL_PREFIX/lib/cmake/zstreamer/zstreamerTargets.cmake"

echo "All files exist in correct layout!"

# Test linking using pkg-config
export PKG_CONFIG_PATH="$INSTALL_PREFIX/lib/pkgconfig:$PKG_CONFIG_PATH"

echo "pkg-config query zstreamer..."
pkg-config --define-variable=prefix="$INSTALL_PREFIX" --cflags --libs zstreamer

echo "pkg-config query zstreamer-elements..."
pkg-config --define-variable=prefix="$INSTALL_PREFIX" --cflags --libs zstreamer-elements

echo "Creating a simple test main to compile against installed library..."
cat << 'EOF' > /tmp/zst_test_main.c
#include <zstreamer/zst_element.h>
#include <zstreamer/zst_pipeline.h>
#include <zstreamer/elements/zst_file_sink.h>
#include <stdio.h>

int main(void) {
    zst_pipeline_t* pipe = zst_pipeline_create();
    if (!pipe) {
        return 1;
    }
    printf("Successfully created pipeline using installed headers!\n");
    zst_pipeline_destroy(pipe);
    return 0;
}
EOF

echo "Compiling test program..."
gcc /tmp/zst_test_main.c $(pkg-config --define-variable=prefix="$INSTALL_PREFIX" --cflags --libs zstreamer zstreamer-elements) -lavformat -lavcodec -lavutil -lx264 -lasound -lv4l2 -lswscale -lswresample -lfreetype -lpthread -lm -ldl -fsanitize=thread -o /tmp/zst_test_main || gcc /tmp/zst_test_main.c $(pkg-config --define-variable=prefix="$INSTALL_PREFIX" --cflags --libs zstreamer zstreamer-elements) -lavformat -lavcodec -lavutil -lx264 -lasound -lv4l2 -lswscale -lswresample -lfreetype -lpthread -lm -ldl -o /tmp/zst_test_main

echo "Running test program..."
LD_LIBRARY_PATH="$INSTALL_PREFIX/lib:$LD_LIBRARY_PATH" /tmp/zst_test_main

echo "=== Installation Layout Test Passed! ==="

cmake_minimum_required(VERSION 3.16)
project(zstreamer VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_definitions(_GNU_SOURCE)



include(cmake/Options.cmake)
include(cmake/Dependencies.cmake)

# ── Include directories ──────────────────────────────────────────────────
set(ZSTREAMER_INCLUDE_DIR
    ${CMAKE_SOURCE_DIR}/include
)


add_subdirectory(src)
add_subdirectory(tests)

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

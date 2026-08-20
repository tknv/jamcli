cmake_minimum_required(VERSION 3.20)
project(jamcli CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ---------------------------------------------------------------------------
# Version
#
# VERSION is the single source of truth for the release number (bump it
# once per release, nowhere else). It also feeds CPACK_PACKAGE_VERSION
# below, which previously referenced an undefined ${PROJECT_VERSION} since
# project() was never called with a VERSION argument - fixed by setting it
# explicitly here. Git metadata is appended separately (only for the
# runtime `jamcli --version` string, not for package version numbers,
# which must stay purely numeric) when building from a git checkout.
# ---------------------------------------------------------------------------
file(STRINGS "${CMAKE_SOURCE_DIR}/VERSION" JAMCLI_VERSION)
set(PROJECT_VERSION ${JAMCLI_VERSION})

find_package(Git QUIET)
set(JAMCLI_GIT_COMMIT "unknown")
if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --always --dirty
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE JAMCLI_GIT_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
endif()

configure_file(
    ${CMAKE_SOURCE_DIR}/include/jamcli_version.h.in
    ${CMAKE_BINARY_DIR}/generated/jamcli_version.h
    @ONLY
)

# Required compile flags to match Arch Linux libjami.so ABI
add_compile_definitions(
    ASIO_STANDALONE
    MSGPACK_NO_BOOST
    _REENTRANT
)

find_package(PkgConfig REQUIRED)

# --- libjami (the Jami/Ring daemon library, ships jami.h etc.) -------------
find_path(JAMI_INCLUDE_DIR NAMES jami/jami.h)
find_library(JAMI_LIBRARY NAMES jami ring)
if(NOT JAMI_INCLUDE_DIR OR NOT JAMI_LIBRARY)
    message(FATAL_ERROR
        "libjami development files not found. Install the Jami/Ring daemon "
        "dev package, or point CMAKE_PREFIX_PATH / JAMI_INCLUDE_DIR / "
        "JAMI_LIBRARY at a local build.")
endif()

# --- FFmpeg (libavutil, for AVFrame used by the video sink target) --------
pkg_check_modules(AVUTIL REQUIRED IMPORTED_TARGET libavutil)

find_package(Threads REQUIRED)

add_executable(jamcli
    src/main.cpp
    src/app.cpp
    src/config.cpp
    src/utils.cpp
    src/video.cpp
    src/terminal_ui.cpp
)

target_include_directories(jamcli PRIVATE
    ${CMAKE_BINARY_DIR}/generated   # for the generated version.h
    ${AVUTIL_INCLUDE_DIRS}
)
target_link_directories(jamcli PRIVATE ${AVUTIL_LIBRARY_DIRS})
target_compile_options(jamcli PRIVATE ${AVUTIL_CFLAGS_OTHER})

# Link against system libjami
target_link_libraries(jamcli PRIVATE jami Threads::Threads rt ${AVUTIL_LIBRARIES})

# ---------------------------------------------------------------------------
# Installation
# ---------------------------------------------------------------------------

include(GNUInstallDirs)

install(TARGETS jamcli
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Install configuration if it exists.
if(EXISTS "${CMAKE_SOURCE_DIR}/config.yaml")
    install(
        FILES "${CMAKE_SOURCE_DIR}/config.yaml"
        DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/jamcli
    )
endif()

# ---------------------------------------------------------------------------
# CPack
# ---------------------------------------------------------------------------

set(CPACK_PACKAGE_NAME "jamcli")
set(CPACK_PACKAGE_VENDOR "jamcli")
set(CPACK_PACKAGE_CONTACT "jamcli")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Headless CLI client for Jami"
)
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})

set(CPACK_PACKAGE_FILE_NAME
    "${CPACK_PACKAGE_NAME}-${PROJECT_VERSION}-${CMAKE_SYSTEM_PROCESSOR}"
)

# Debian
set(CPACK_DEBIAN_PACKAGE_NAME "jamcli")
set(CPACK_DEBIAN_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE ${CMAKE_SYSTEM_PROCESSOR})
set(CPACK_DEBIAN_PACKAGE_SECTION "net")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_DESCRIPTION
    "Headless command-line client for Jami."
)

# Runtime dependencies.
# Adjust these names if the target distribution uses different package names.
set(CPACK_DEBIAN_PACKAGE_DEPENDS
    "ffmpeg, jami-daemon"
)

# Generate DEB.
set(CPACK_GENERATOR "DEB")

include(CPack)

# Convenience target: `cmake --build build --target print-version`
add_custom_target(print-version
    COMMAND ${CMAKE_COMMAND} -E echo "${JAMCLI_VERSION} (${JAMCLI_GIT_COMMIT})"
)

cmake_minimum_required(VERSION 3.25)
project(KobaltWebKitDependencies LANGUAGES NONE)
include(ExternalProject)
include(FetchContent)
include("${CMAKE_CURRENT_SOURCE_DIR}/../cache.cmake")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/sources.json" sources)
string(JSON url GET "${sources}" wpewebkit url)
string(JSON hash GET "${sources}" wpewebkit sha256)
kobalt_dependency_archive("${KOBALT_WEBKIT_DOWNLOADS}" wpewebkit "${url}" "${hash}" archive)
find_program(PATCH_EXECUTABLE patch REQUIRED)
file(WRITE "${CMAKE_BINARY_DIR}/patches.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/patches/lazy-wpe-egl.patch\n${CMAKE_CURRENT_SOURCE_DIR}/patches/no-libdrm-snapshot.patch\n${CMAKE_CURRENT_SOURCE_DIR}/patches/release-exec-path.patch\n${CMAKE_CURRENT_SOURCE_DIR}/patches/require-requested-sandbox.patch\n")
FetchContent_Declare(wpewebkit
    URL "${archive}" URL_HASH "SHA256=${hash}" DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_DIR "${KOBALT_WEBKIT_OUTPUT}/sources/wpewebkit" SOURCE_SUBDIR kobalt-no-project
    PATCH_COMMAND "${CMAKE_COMMAND}" "-DSOURCE=<SOURCE_DIR>"
        "-DPATCH_EXECUTABLE=${PATCH_EXECUTABLE}" "-DPATCH_LIST=${CMAKE_BINARY_DIR}/patches.txt"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/../apply-patches.cmake")
FetchContent_MakeAvailable(wpewebkit)
ExternalProject_Add(wpewebkit-build
    SOURCE_DIR "${wpewebkit_SOURCE_DIR}" BINARY_DIR "${KOBALT_WEBKIT_OUTPUT}/webkit"
    DOWNLOAD_COMMAND "" UPDATE_COMMAND "" CMAKE_GENERATOR Ninja
    CMAKE_ARGS -C "${CMAKE_CURRENT_SOURCE_DIR}/options.cmake"
        "-DCMAKE_CXX_FLAGS_RELEASE=-O1 -DNDEBUG" "-DCMAKE_C_FLAGS_RELEASE=-O1 -DNDEBUG"
        -DENABLE_MINIBROWSER=OFF
    BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --parallel "${KOBALT_WEBKIT_JOBS}"
    INSTALL_COMMAND "${CMAKE_COMMAND}" --install <BINARY_DIR>
    LOG_CONFIGURE TRUE LOG_BUILD TRUE LOG_INSTALL TRUE LOG_OUTPUT_ON_FAILURE TRUE)

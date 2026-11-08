cmake_minimum_required(VERSION 3.24)
project(kobalt_webkit_m0_dependencies LANGUAGES NONE)
include(ExternalProject)
include(FetchContent)
include("${CMAKE_CURRENT_SOURCE_DIR}/../cache.cmake")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/sources.json" sources)
string(JSON url GET "${sources}" wpewebkit url)
string(JSON hash GET "${sources}" wpewebkit sha256)
kobalt_dependency_archive("${KOBALT_WEBKIT_DOWNLOADS}" wpewebkit "${url}" "${hash}" archive)
set(patch_command "${CMAKE_COMMAND}" -E true)
if(KOBALT_WEBKIT_LAZY_EGL)
    find_program(PATCH_EXECUTABLE patch REQUIRED)
    file(WRITE "${CMAKE_BINARY_DIR}/cpu-patches.txt"
        "${CMAKE_CURRENT_SOURCE_DIR}/lazy-wpe-egl.candidate.patch\n${CMAKE_CURRENT_SOURCE_DIR}/no-libdrm-snapshot.candidate.patch\n")
    set(patch_command "${CMAKE_COMMAND}" "-DSOURCE=<SOURCE_DIR>"
        "-DPATCH_EXECUTABLE=${PATCH_EXECUTABLE}" "-DPATCH_LIST=${CMAKE_BINARY_DIR}/cpu-patches.txt"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/../apply-patches.cmake")
endif()
FetchContent_Declare(wpewebkit
    URL "${archive}" URL_HASH "SHA256=${hash}" DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_DIR "${KOBALT_WEBKIT_OUTPUT}/sources/wpewebkit"
    SOURCE_SUBDIR kobalt-no-project
    PATCH_COMMAND ${patch_command})
FetchContent_MakeAvailable(wpewebkit)
ExternalProject_Add(wpewebkit-build
    SOURCE_DIR "${wpewebkit_SOURCE_DIR}"
    BINARY_DIR "${KOBALT_WEBKIT_OUTPUT}/webkit"
    DOWNLOAD_COMMAND "" UPDATE_COMMAND ""
    CMAKE_GENERATOR Ninja
    CMAKE_ARGS -C "${CMAKE_CURRENT_SOURCE_DIR}/wpe-options.cmake"
        "-DCMAKE_INSTALL_PREFIX=${KOBALT_WEBKIT_OUTPUT}/install"
        "-DCMAKE_CXX_FLAGS_RELEASE=-O1 -DNDEBUG" "-DCMAKE_C_FLAGS_RELEASE=-O1 -DNDEBUG"
        -DENABLE_MINIBROWSER=OFF
    BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --parallel "${KOBALT_WEBKIT_JOBS}"
    INSTALL_COMMAND "${CMAKE_COMMAND}" --install <BINARY_DIR>
    LOG_CONFIGURE TRUE LOG_BUILD TRUE LOG_INSTALL TRUE LOG_OUTPUT_ON_FAILURE TRUE)

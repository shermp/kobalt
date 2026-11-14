cmake_minimum_required(VERSION 3.25)
project(kobalt_webkit_fw4_text LANGUAGES NONE)
include(ExternalProject)
include(FetchContent)
find_program(make_program make REQUIRED)
set(prefix "${KOBALT_WEBKIT_ENTRY}/install")
set(triple arm-kobov4-linux-gnueabihf)
set(tool "${KOBALT_WEBKIT_TOOLCHAIN}/bin/${triple}")
foreach(name icu harfbuzz)
    string(TOUPPER "${name}" upper)
    FetchContent_Declare("${name}"
        URL "${KOBALT_WEBKIT_${upper}_ARCHIVE}"
        URL_HASH "SHA256=${KOBALT_WEBKIT_${upper}_SHA256}"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SOURCE_DIR "${KOBALT_WEBKIT_ENTRY}/sources/${name}"
        SOURCE_SUBDIR kobalt-no-project)
    FetchContent_MakeAvailable("${name}")
endforeach()
set(icu_options --disable-shared --enable-static --with-data-packaging=static
    --disable-samples --disable-tests --disable-extras --disable-icuio)
ExternalProject_Add(dep-icu-host
    SOURCE_DIR "${icu_SOURCE_DIR}/source" BINARY_DIR "${KOBALT_WEBKIT_ENTRY}/build/icu-host"
    DOWNLOAD_COMMAND "" UPDATE_COMMAND ""
    CONFIGURE_COMMAND <SOURCE_DIR>/configure ${icu_options}
    BUILD_COMMAND "${make_program}" -j "${KOBALT_WEBKIT_JOBS}"
    INSTALL_COMMAND "")
ExternalProject_Add(dep-icu
    SOURCE_DIR "${icu_SOURCE_DIR}/source" BINARY_DIR "${KOBALT_WEBKIT_ENTRY}/build/icu"
    DOWNLOAD_COMMAND "" UPDATE_COMMAND ""
    CONFIGURE_COMMAND "${CMAKE_COMMAND}" -E env "CC=${tool}-gcc" "CXX=${tool}-g++"
        "AR=${tool}-ar" "RANLIB=${tool}-ranlib" CFLAGS=-fPIC CXXFLAGS=-fPIC
        <SOURCE_DIR>/configure "--host=${triple}" "--prefix=${prefix}"
        "--with-cross-build=${KOBALT_WEBKIT_ENTRY}/build/icu-host" ${icu_options}
    BUILD_COMMAND "${make_program}" -j "${KOBALT_WEBKIT_JOBS}"
    INSTALL_COMMAND "${make_program}" install
    DEPENDS dep-icu-host)
ExternalProject_Add(dep-harfbuzz
    SOURCE_DIR "${harfbuzz_SOURCE_DIR}" BINARY_DIR "${KOBALT_WEBKIT_ENTRY}/build/harfbuzz"
    DOWNLOAD_COMMAND "" UPDATE_COMMAND ""
    CMAKE_ARGS "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_CURRENT_SOURCE_DIR}/../fw4-toolchain.cmake"
        "-DKOBALT_WEBKIT_TOOLCHAIN=${KOBALT_WEBKIT_TOOLCHAIN}" "-DKOBALT_WEBKIT_PREFIX=${prefix}"
        "-DCMAKE_PREFIX_PATH=${prefix}" "-DCMAKE_INSTALL_PREFIX=${prefix}"
        -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON
        "-DCMAKE_CXX_FLAGS=-isystem ${prefix}/include"
        -DBUILD_SHARED_LIBS=OFF -DHB_HAVE_ICU=ON
        -DHB_BUILD_SUBSET=OFF -DHB_BUILD_RASTER=OFF -DHB_BUILD_VECTOR=OFF -DHB_BUILD_GPU=OFF
    BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --parallel "${KOBALT_WEBKIT_JOBS}"
    DEPENDS dep-icu)

cmake_minimum_required(VERSION 3.25)
project(kobalt_webkit_fw4_glib LANGUAGES NONE)
include(ExternalProject)
include(FetchContent)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(pkg_config NAMES pkg-config REQUIRED)
find_program(make_program NAMES make REQUIRED)
set(prefix "${KOBALT_WEBKIT_ENTRY}/install")
set(triple arm-kobov4-linux-gnueabihf)
set(toolchain "${KOBALT_WEBKIT_ENTRY}/sources/kobov4/${triple}")
set(tool "${toolchain}/bin/${triple}")
configure_file(glib-cross.ini.in glib-cross.ini @ONLY)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/sources.json" sources)
foreach(name kobov4 meson zlib pcre2 libffi glib)
    string(JSON hash GET "${sources}" "${name}" sha256)
    FetchContent_Declare("${name}"
        URL "${KOBALT_WEBKIT_DOWNLOADS}/${name}-${hash}.tar"
        URL_HASH "SHA256=${hash}"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SOURCE_DIR "${KOBALT_WEBKIT_ENTRY}/sources/${name}"
        SOURCE_SUBDIR kobalt-no-project)
    FetchContent_MakeAvailable("${name}")
endforeach()

set(cmake_args
    "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_CURRENT_SOURCE_DIR}/fw4-toolchain.cmake"
    "-DKOBALT_WEBKIT_TOOLCHAIN=${toolchain}" "-DKOBALT_WEBKIT_PREFIX=${prefix}"
    "-DCMAKE_INSTALL_PREFIX=${prefix}" -DCMAKE_INSTALL_LIBDIR=lib
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_SHARED_LIBS=OFF)
foreach(name zlib pcre2)
    set(options)
    if(name STREQUAL zlib)
        list(APPEND options -DZLIB_BUILD_SHARED=OFF -DZLIB_BUILD_TESTING=OFF)
    else()
        list(APPEND options -DPCRE2_BUILD_TESTS=OFF -DPCRE2_BUILD_PCRE2GREP=OFF -DPCRE2_SUPPORT_JIT=OFF)
    endif()
    ExternalProject_Add("dep-${name}"
        SOURCE_DIR "${${name}_SOURCE_DIR}" BINARY_DIR "${KOBALT_WEBKIT_ENTRY}/build/${name}"
        DOWNLOAD_COMMAND "" UPDATE_COMMAND ""
        CMAKE_ARGS ${cmake_args} ${options}
        BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --parallel "${KOBALT_WEBKIT_JOBS}")
endforeach()

ExternalProject_Add(dep-libffi
    SOURCE_DIR "${libffi_SOURCE_DIR}" BINARY_DIR "${KOBALT_WEBKIT_ENTRY}/build/libffi"
    DOWNLOAD_COMMAND "" UPDATE_COMMAND ""
    CONFIGURE_COMMAND "${CMAKE_COMMAND}" -E env
        "CC=${tool}-gcc" "CXX=${tool}-g++" "AR=${tool}-ar" "RANLIB=${tool}-ranlib"
        "CFLAGS=-O2 -fPIC" <SOURCE_DIR>/configure
        "--host=${triple}" "--prefix=${prefix}" --disable-shared --enable-static
        --disable-docs --disable-multi-os-directory
    BUILD_COMMAND "${make_program}" -j "${KOBALT_WEBKIT_JOBS}"
    INSTALL_COMMAND "${make_program}" install)

set(meson "${Python3_EXECUTABLE}" "${meson_SOURCE_DIR}/meson.py")
ExternalProject_Add(dep-glib
    SOURCE_DIR "${glib_SOURCE_DIR}" BINARY_DIR "${KOBALT_WEBKIT_ENTRY}/build/glib"
    DOWNLOAD_COMMAND "" UPDATE_COMMAND ""
    CONFIGURE_COMMAND "${CMAKE_COMMAND}" -E env --unset=PKG_CONFIG_PATH --unset=PKG_CONFIG_SYSROOT_DIR
        ${meson} setup <BINARY_DIR> <SOURCE_DIR> "--cross-file=${CMAKE_BINARY_DIR}/glib-cross.ini"
        "--prefix=${prefix}" --libdir=lib --buildtype=release --wrap-mode=nodownload
        -Ddefault_library=static -Db_staticpic=true -Dtests=false -Dman-pages=disabled
        -Ddocumentation=false -Dnls=disabled -Dlibmount=disabled -Dselinux=disabled
        -Dlibelf=disabled -Dsysprof=disabled
    BUILD_COMMAND ${meson} compile -C <BINARY_DIR> -j "${KOBALT_WEBKIT_JOBS}"
    INSTALL_COMMAND ${meson} install -C <BINARY_DIR> --no-rebuild
    DEPENDS dep-zlib dep-pcre2 dep-libffi)

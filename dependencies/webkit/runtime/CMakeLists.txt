cmake_minimum_required(VERSION 3.25)
project(kobalt_webkit_fw4_runtime LANGUAGES NONE)
include(ExternalProject)
include(FetchContent)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(pkg_config NAMES pkg-config REQUIRED)
find_program(make_program make REQUIRED)
set(prefix "${KOBALT_WEBKIT_ENTRY}/install")
set(triple arm-kobov4-linux-gnueabihf)
set(tool "${KOBALT_WEBKIT_TOOLCHAIN}/bin/${triple}")
set(prefixes "${prefix};${KOBALT_WEBKIT_GLIB_PREFIX};${KOBALT_WEBKIT_TEXT_PREFIX}")
set(pkg_config_dirs)
foreach(dir IN LISTS prefixes)
    list(APPEND pkg_config_dirs "${dir}/lib/pkgconfig")
endforeach()
list(JOIN pkg_config_dirs ":" pkg_config_path)
list(JOIN pkg_config_dirs "', '" meson_pkg_config_dirs)
configure_file(cross.ini.in runtime-cross.ini @ONLY)
set(env "${CMAKE_COMMAND}" -E env --unset=PKG_CONFIG_SYSROOT_DIR --unset=PKG_CONFIG_PATH
    "PKG_CONFIG_LIBDIR=${pkg_config_path}")
set(meson "${Python3_EXECUTABLE}" "${KOBALT_WEBKIT_MESON}/meson.py")
foreach(name IN LISTS KOBALT_WEBKIT_RUNTIME_SOURCES)
    string(REPLACE "-" "_" name_id "${name}")
    FetchContent_Declare("${name_id}"
        URL "${KOBALT_WEBKIT_${name}_ARCHIVE}"
        URL_HASH "SHA256=${KOBALT_WEBKIT_${name}_SHA256}"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SOURCE_DIR "${KOBALT_WEBKIT_ENTRY}/sources/${name}"
        SOURCE_SUBDIR kobalt-no-project)
    FetchContent_MakeAvailable("${name_id}")
    set("${name}_source" "${${name_id}_SOURCE_DIR}")
endforeach()

file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/host-bin")
file(CREATE_LINK "${Python3_EXECUTABLE}" "${CMAKE_BINARY_DIR}/host-bin/python" SYMBOLIC)
string(APPEND env ";PATH=${CMAKE_BINARY_DIR}/host-bin:$ENV{PATH}")
file(COPY "${egl-headers_source}/api/EGL" "${egl-headers_source}/api/KHR"
    DESTINATION "${prefix}/include")

function(runtime_library name style)
    cmake_parse_arguments(lib "" "SUBDIR" "OPTIONS;DEPENDS;MAKE_OPTIONS" ${ARGN})
    set(source "${${name}_source}")
    if(lib_SUBDIR)
        string(APPEND source "/${lib_SUBDIR}")
    endif()
    if(style STREQUAL "cmake")
        string(REPLACE ";" "|" cmake_prefixes "${prefixes}")
        set(configure ${env} "${CMAKE_COMMAND}" -G Ninja -S <SOURCE_DIR> -B <BINARY_DIR>
            "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_CURRENT_SOURCE_DIR}/../fw4-toolchain.cmake"
            "-DKOBALT_WEBKIT_TOOLCHAIN=${KOBALT_WEBKIT_TOOLCHAIN}"
            "-DKOBALT_WEBKIT_PREFIX=${cmake_prefixes}" "-DCMAKE_PREFIX_PATH=${cmake_prefixes}"
            "-DCMAKE_INSTALL_PREFIX=${prefix}" -DCMAKE_INSTALL_LIBDIR=lib
            -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_SHARED_LIBS=OFF)
        set(build "${CMAKE_COMMAND}" --build <BINARY_DIR> --parallel "${KOBALT_WEBKIT_JOBS}")
        set(install "${CMAKE_COMMAND}" --install <BINARY_DIR>)
    elseif(style STREQUAL "meson")
        set(configure ${env} "CPPFLAGS=-I${prefix}/include -I${KOBALT_WEBKIT_TEXT_PREFIX}/include"
            ${meson} setup <BINARY_DIR> <SOURCE_DIR> "--cross-file=${CMAKE_BINARY_DIR}/runtime-cross.ini"
            "--prefix=${prefix}" --libdir=lib --buildtype=release --wrap-mode=nodownload
            -Ddefault_library=static -Db_staticpic=true)
        set(build ${env} ${meson} compile -C <BINARY_DIR> -j "${KOBALT_WEBKIT_JOBS}")
        set(install ${meson} install -C <BINARY_DIR> --no-rebuild)
    else()
        set(configure ${env} "CC=${tool}-gcc" "CXX=${tool}-g++" "AR=${tool}-ar"
            "RANLIB=${tool}-ranlib" "CFLAGS=-O2 -fPIC" "CXXFLAGS=-O2 -fPIC"
            "CPPFLAGS=-I${prefix}/include" "LDFLAGS=-L${prefix}/lib"
            <SOURCE_DIR>/configure "--prefix=${prefix}" "--host=${triple}"
            --disable-shared --enable-static)
        set(build "${make_program}" -j "${KOBALT_WEBKIT_JOBS}" ${lib_MAKE_OPTIONS})
        set(install "${make_program}" install ${lib_MAKE_OPTIONS})
    endif()
    ExternalProject_Add("dep-${name}"
        SOURCE_DIR "${source}" BINARY_DIR "${KOBALT_WEBKIT_ENTRY}/build/${name}"
        DOWNLOAD_COMMAND "" UPDATE_COMMAND "" LIST_SEPARATOR |
        CONFIGURE_COMMAND ${configure} ${lib_OPTIONS}
        BUILD_COMMAND ${build} INSTALL_COMMAND ${install}
        LOG_CONFIGURE ON LOG_BUILD ON LOG_INSTALL ON LOG_OUTPUT_ON_FAILURE ON
        DEPENDS ${lib_DEPENDS})
endfunction()

runtime_library(jpeg cmake OPTIONS -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_TOOLS=OFF
    -DWITH_TESTS=OFF -DWITH_SIMD=OFF)
runtime_library(png cmake OPTIONS -DPNG_SHARED=OFF -DPNG_STATIC=ON -DPNG_TESTS=OFF -DPNG_TOOLS=OFF)
runtime_library(expat cmake SUBDIR expat OPTIONS -DEXPAT_BUILD_TOOLS=OFF -DEXPAT_BUILD_TESTS=OFF
    -DEXPAT_BUILD_EXAMPLES=OFF -DEXPAT_BUILD_DOCS=OFF -DEXPAT_SHARED_LIBS=OFF)
runtime_library(brotli cmake OPTIONS -DBROTLI_DISABLE_TESTS=ON -DBROTLI_BUILD_TOOLS=OFF)
runtime_library(uuid autotools OPTIONS --disable-all-programs --enable-libuuid --without-ncurses)
runtime_library(freetype cmake OPTIONS -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BZIP2=ON
    -DFT_REQUIRE_BROTLI=ON DEPENDS dep-png dep-brotli)
runtime_library(fontconfig autotools OPTIONS --disable-docs --disable-nls
    --with-expat-includes=${prefix}/include --with-expat-lib=${prefix}/lib "LIBS=-L${KOBALT_WEBKIT_GLIB_PREFIX}/lib -lz -lm -lbrotlicommon"
    MAKE_OPTIONS "SUBDIRS=fontconfig fc-case fc-lang src fc-cache fc-cat fc-conflist fc-list fc-match fc-pattern fc-query fc-scan fc-validate conf.d its po po-conf"
    DEPENDS dep-expat dep-freetype dep-uuid)
runtime_library(webp cmake OPTIONS -DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_BUILD_CWEBP=OFF
    -DWEBP_BUILD_DWEBP=OFF -DWEBP_BUILD_GIF2WEBP=OFF -DWEBP_BUILD_IMG2WEBP=OFF
    -DWEBP_BUILD_EXTRAS=OFF -DWEBP_BUILD_VWEBP=OFF -DWEBP_BUILD_WEBPINFO=OFF -DWEBP_BUILD_WEBPMUX=OFF)
runtime_library(xml2 cmake OPTIONS -DLIBXML2_WITH_PYTHON=OFF -DLIBXML2_WITH_LZMA=OFF
    -DLIBXML2_WITH_TESTS=OFF -DLIBXML2_WITH_PROGRAMS=OFF)
runtime_library(sqlite autotools OPTIONS --disable-readline)
runtime_library(nghttp2 cmake OPTIONS -DENABLE_LIB_ONLY=ON -DENABLE_STATIC_LIB=ON -DENABLE_SHARED_LIB=OFF)
runtime_library(psl meson OPTIONS -Druntime=libicu -Dbuiltin=libicu
    "-Dpsl_file=${psl-list_source}/public_suffix_list.dat"
    "-Dpsl_testfile=${psl-list_source}/tests/test_psl.txt"
    "-Dc_link_args=['-lstdc++', '-lm', '-lpthread', '-ldl']")
runtime_library(soup meson OPTIONS -Dtests=false -Dgssapi=disabled -Dntlm=disabled -Dbrotli=disabled
    -Dtls_check=false -Dintrospection=disabled -Dvapi=disabled -Dsysprof=disabled
    -Dpkcs11_tests=disabled "-Dc_link_args=['-lstdc++', '-lm', '-lpthread', '-ldl']"
    DEPENDS dep-nghttp2 dep-psl dep-sqlite)
runtime_library(gpg-error autotools OPTIONS --disable-doc --disable-tests --disable-nls)
runtime_library(gcrypt autotools OPTIONS "--with-libgpg-error-prefix=${prefix}" --disable-asm
    DEPENDS dep-gpg-error)
runtime_library(tasn1 autotools OPTIONS --disable-doc --disable-nls)
runtime_library(seccomp autotools)
runtime_library(epoxy meson OPTIONS -Dglx=no -Degl=yes -Dx11=false -Dtests=false)
runtime_library(xkbcommon meson OPTIONS -Denable-x11=false -Denable-wayland=false
    -Denable-xkbregistry=false -Denable-tools=false -Denable-docs=false
    -Dxkb-config-root=/usr/share/X11/xkb)

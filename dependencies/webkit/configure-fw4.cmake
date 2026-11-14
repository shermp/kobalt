cmake_minimum_required(VERSION 3.25)

get_filename_component(root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
include("${root}/dependencies/cache.cmake")
kobalt_dependency_cache_root("${root}" "$ENV{KOBALT_DEPENDENCY_CACHE}" cache)
foreach(path KOBALT_WEBKIT_TOOLCHAIN KOBALT_WEBKIT_OUTPUT)
    if(NOT IS_ABSOLUTE "${${path}}")
        message(FATAL_ERROR "Pass an absolute ${path}")
    endif()
endforeach()
if(NOT EXISTS "${KOBALT_WEBKIT_TOOLCHAIN}/bin/arm-kobov4-linux-gnueabihf-g++")
    message(FATAL_ERROR "Pass the verified arm-kobov4-linux-gnueabihf toolchain directory")
endif()
if(NOT KOBALT_WEBKIT_PREFIXES)
    message(FATAL_ERROR "Pass the installed FW4 dependency prefixes in KOBALT_WEBKIT_PREFIXES")
endif()
foreach(prefix IN LISTS KOBALT_WEBKIT_PREFIXES)
    if(NOT IS_ABSOLUTE "${prefix}" OR NOT IS_DIRECTORY "${prefix}/lib/pkgconfig")
        message(FATAL_ERROR "Missing FW4 dependency prefix: ${prefix}")
    endif()
    list(APPEND pkg_config_paths "${prefix}/lib/pkgconfig")
endforeach()
list(JOIN pkg_config_paths ":" pkg_config_path)
file(READ "${CMAKE_CURRENT_LIST_DIR}/sources.json" sources)
string(JSON url GET "${sources}" wpewebkit url)
string(JSON hash GET "${sources}" wpewebkit sha256)
kobalt_dependency_archive("${cache}/downloads" wpewebkit "${url}" "${hash}" archive)

find_program(patch NAMES patch REQUIRED)
find_program(glib_compile_resources NAMES glib-compile-resources REQUIRED)
find_program(pkg_config NAMES pkg-config REQUIRED)
set(recipe "${CMAKE_CURRENT_LIST_DIR}")
set(identity "${hash};${KOBALT_WEBKIT_TOOLCHAIN};${KOBALT_WEBKIT_PREFIXES};${CMAKE_VERSION}")
foreach(input fw4-toolchain.cmake headless-options.cmake
    patches/lazy-wpe-egl.patch patches/no-libdrm-snapshot.patch patches/release-exec-path.patch
    patches/static-glib-exports.patch patches/pal-crypto-dependencies.patch patches/bubblewrap-memfd-fw4.patch patches/xdgmime-private-symbols.patch
    ../apply-patches.cmake)
    file(SHA256 "${recipe}/${input}" input_hash)
    string(APPEND identity ";${input_hash}")
endforeach()
string(SHA256 key "${identity}")
set(output "${KOBALT_WEBKIT_OUTPUT}/${key}")
file(MAKE_DIRECTORY "${output}")
file(LOCK "${output}/configure.lock" GUARD PROCESS)
if(NOT EXISTS "${output}/source-ready")
    file(REMOVE_RECURSE "${output}/wpewebkit-2.54.2")
    file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${output}")
    file(WRITE "${output}/patches.txt" "")
    foreach(name lazy-wpe-egl no-libdrm-snapshot release-exec-path static-glib-exports pal-crypto-dependencies bubblewrap-memfd-fw4 xdgmime-private-symbols)
        file(APPEND "${output}/patches.txt" "${recipe}/patches/${name}.patch\n")
    endforeach()
    execute_process(COMMAND "${CMAKE_COMMAND}"
        "-DSOURCE=${output}/wpewebkit-2.54.2" "-DPATCH_EXECUTABLE=${patch}"
        "-DPATCH_LIST=${output}/patches.txt" -P "${root}/dependencies/apply-patches.cmake"
        COMMAND_ERROR_IS_FATAL ANY)
    file(TOUCH "${output}/source-ready")
endif()

set(ENV{PKG_CONFIG_PATH} "")
set(ENV{PKG_CONFIG_LIBDIR} "${pkg_config_path}")
unset(ENV{PKG_CONFIG_SYSROOT_DIR})
# WPE Find modules select archives but omit some private pkg-config libraries.
execute_process(COMMAND "${pkg_config}" --libs --static
    gio-unix-2.0 gmodule-2.0 libsoup-3.0 fontconfig freetype2 harfbuzz-icu
    OUTPUT_VARIABLE static_libraries OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
# WebKit probes for -latomic and later links it by name. Put only the
# verified toolchain archive in this search directory so it wins over the DSO.
execute_process(COMMAND "${KOBALT_WEBKIT_TOOLCHAIN}/bin/arm-kobov4-linux-gnueabihf-g++"
    -print-file-name=libatomic.a OUTPUT_VARIABLE atomic_archive
    OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
if(NOT IS_ABSOLUTE "${atomic_archive}" OR NOT EXISTS "${atomic_archive}")
    message(FATAL_ERROR "The FW4 toolchain must provide a static libatomic archive")
endif()
set(static_compiler_lib "${output}/compiler-lib")
file(MAKE_DIRECTORY "${static_compiler_lib}")
file(CREATE_LINK "${atomic_archive}" "${static_compiler_lib}/libatomic.a" SYMBOLIC)
set(compiler_link_flags "-static-libstdc++ -static-libgcc -L${static_compiler_lib}")
message(STATUS "FW4 WPE configure output: ${output}")
execute_process(COMMAND "${CMAKE_COMMAND}" -G Ninja
    -S "${output}/wpewebkit-2.54.2" -B "${output}/build"
    -C "${recipe}/headless-options.cmake"
    "-DCMAKE_TOOLCHAIN_FILE=${recipe}/fw4-toolchain.cmake"
    "-DKOBALT_WEBKIT_TOOLCHAIN=${KOBALT_WEBKIT_TOOLCHAIN}"
    "-DKOBALT_WEBKIT_PREFIX=${KOBALT_WEBKIT_PREFIXES}"
    "-DCMAKE_PREFIX_PATH=${KOBALT_WEBKIT_PREFIXES}"
    "-DCMAKE_INSTALL_PREFIX=${output}/install"
    "-DGLIB_COMPILE_RESOURCES_EXECUTABLE=${glib_compile_resources}"
    "-DCMAKE_CXX_STANDARD_LIBRARIES=-Wl,--start-group ${static_libraries} -Wl,--end-group"
    "-DCMAKE_SHARED_LINKER_FLAGS=${compiler_link_flags}"
    "-DCMAKE_MODULE_LINKER_FLAGS=${compiler_link_flags}"
    "-DCMAKE_EXE_LINKER_FLAGS=${compiler_link_flags}"
    "-DCMAKE_C_FLAGS_RELEASE=-O1 -DNDEBUG"
    "-DCMAKE_CXX_FLAGS_RELEASE=-O1 -DNDEBUG"
    -DBWRAP_EXECUTABLE=/usr/bin/bwrap -DDBUS_PROXY_EXECUTABLE=/usr/bin/xdg-dbus-proxy
    COMMAND_ERROR_IS_FATAL ANY)

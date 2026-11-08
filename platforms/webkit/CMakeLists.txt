cmake_minimum_required(VERSION 3.25)
project(KobaltWebKit LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_executable(kobalt-webkit ../../src/webkit/launcher.c)
target_compile_options(kobalt-webkit PRIVATE -Wall -Wextra -Werror)
option(KOBALT_WEBKIT_WORKER "Build the WPE worker (disable for native supervisor checks)" ON)
if(KOBALT_WEBKIT_WORKER)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(WPE_DEPS REQUIRED IMPORTED_TARGET glib-2.0 gio-unix-2.0 libsoup-3.0)
    find_package(PNG REQUIRED)
    if(NOT EXISTS "${KOBALT_WEBKIT_PREFIX}/lib/libWPEWebKit-2.0.so")
        message(FATAL_ERROR "Select the built private WebKit prefix")
    endif()
    add_executable(kobalt-webkit-worker ../../src/webkit/renderer.cpp)
    target_include_directories(kobalt-webkit-worker PRIVATE
        "${KOBALT_WEBKIT_PREFIX}/include/wpe-webkit-2.0"
        "${KOBALT_WEBKIT_PREFIX}/include/wpe-webkit-2.0/wpe-platform")
    target_link_libraries(kobalt-webkit-worker PRIVATE
        "${KOBALT_WEBKIT_PREFIX}/lib/libWPEWebKit-2.0.so" PkgConfig::WPE_DEPS PNG::PNG)
    if(CMAKE_CROSSCOMPILING)
        # WebKit also needs the compiler's private libatomic through DT_NEEDED.
        target_link_options(kobalt-webkit-worker PRIVATE
            "-Wl,-rpath-link,${KOBALT_WEBKIT_TOOLCHAIN}/arm-kobov5-linux-gnueabihf/sysroot/lib")
    endif()
    target_compile_options(kobalt-webkit-worker PRIVATE -Wall -Wextra -Werror)
    set_target_properties(kobalt-webkit-worker PROPERTIES BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "$ORIGIN/../private")
endif()
option(KOBALT_WEBKIT_CRYPTO_ISA "Build the focused bundled-crypto ISA diagnostic" OFF)
if(KOBALT_WEBKIT_CRYPTO_ISA)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GCRYPT REQUIRED IMPORTED_TARGET libgcrypt)
    add_executable(webkit-crypto-isa ../../tests/webkit/crypto-isa.c)
    target_link_libraries(webkit-crypto-isa PRIVATE PkgConfig::GCRYPT)
    target_compile_options(webkit-crypto-isa PRIVATE -Wall -Wextra -Werror)
endif()
include(CTest)
if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
    add_executable(webkit-launcher-fixture ../../tests/webkit/launcher-fixture.c)
    target_compile_options(webkit-launcher-fixture PRIVATE -Wall -Wextra -Werror)
    add_test(NAME webkit.supervisor COMMAND "${CMAKE_COMMAND}"
        "-DLAUNCHER=$<TARGET_FILE:kobalt-webkit>" "-DFIXTURE=$<TARGET_FILE:webkit-launcher-fixture>"
        "-DOUTPUT=${CMAKE_BINARY_DIR}/test-tmp" -P "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/webkit/supervisor.cmake")
    set_tests_properties(webkit.supervisor PROPERTIES TIMEOUT 30 LABELS "webkit")
    foreach(check source-bundle sdk-recovery)
        add_test(NAME webkit.${check} COMMAND "${CMAKE_COMMAND}"
            "-DOUTPUT=${CMAKE_BINARY_DIR}/test-${check}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/webkit/${check}.cmake")
        set_tests_properties(webkit.${check} PROPERTIES TIMEOUT 30 LABELS "webkit")
    endforeach()
endif()

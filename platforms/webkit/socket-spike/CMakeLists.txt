cmake_minimum_required(VERSION 3.22)
project(kobalt_socket_spike LANGUAGES CXX)

set(KOBALT_SPIKE_QT_MAJOR "" CACHE STRING "Build the launcher with this Qt major")
option(KOBALT_SPIKE_GLIB "Build the GLib child" ON)

if(KOBALT_SPIKE_QT_MAJOR)
    find_package(Qt${KOBALT_SPIKE_QT_MAJOR} REQUIRED COMPONENTS Core Network)
    add_executable(wpe-socket-launcher launcher.cpp)
    target_compile_features(wpe-socket-launcher PRIVATE cxx_std_17)
    target_compile_options(wpe-socket-launcher PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(wpe-socket-launcher PRIVATE
        Qt${KOBALT_SPIKE_QT_MAJOR}::Core Qt${KOBALT_SPIKE_QT_MAJOR}::Network)
endif()

if(KOBALT_SPIKE_GLIB)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0)
    add_executable(wpe-socket-child child.cpp)
    target_compile_features(wpe-socket-child PRIVATE cxx_std_23)
    target_compile_options(wpe-socket-child PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(wpe-socket-child PRIVATE PkgConfig::GIO)
endif()

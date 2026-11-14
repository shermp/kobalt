cmake_minimum_required(VERSION 3.22)
project(kobalt_webkit_peer LANGUAGES CXX)

set(KOBALT_PEER_QT_MAJOR 6 CACHE STRING "Prepared Qt major version")
find_package(Qt${KOBALT_PEER_QT_MAJOR} REQUIRED COMPONENTS Core DBus Gui)
set(qt Qt${KOBALT_PEER_QT_MAJOR})

add_library(wpe-qt-peer peer.cpp)
target_compile_features(wpe-qt-peer PUBLIC cxx_std_17)
target_compile_options(wpe-qt-peer PRIVATE -Wall -Wextra -Werror)
target_link_libraries(wpe-qt-peer PUBLIC ${qt}::Core ${qt}::DBus ${qt}::Gui)

add_executable(wpe-qt-peer-smoke peer-smoke.cpp)
target_compile_options(wpe-qt-peer-smoke PRIVATE -Wall -Wextra -Werror)
target_link_libraries(wpe-qt-peer-smoke PRIVATE wpe-qt-peer)

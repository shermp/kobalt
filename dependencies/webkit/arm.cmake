include("${CMAKE_CURRENT_LIST_DIR}/kobov5.cmake")
set(KOBALT_WEBKIT_SYSROOT "/opt/wpe-armhf-sysroot" CACHE PATH "glibc-2.35 ARM dependency sysroot")
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES KOBALT_WEBKIT_SYSROOT)
set(CMAKE_SYSROOT "${KOBALT_WEBKIT_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}")
set(CMAKE_LIBRARY_ARCHITECTURE arm-linux-gnueabihf)
string(APPEND CMAKE_C_FLAGS_INIT " -B${CMAKE_SYSROOT}/usr/lib/arm-linux-gnueabihf/ -isystem ${CMAKE_SYSROOT}/usr/include/arm-linux-gnueabihf")
string(APPEND CMAKE_CXX_FLAGS_INIT " -B${CMAKE_SYSROOT}/usr/lib/arm-linux-gnueabihf/ -isystem ${CMAKE_SYSROOT}/usr/include/arm-linux-gnueabihf")
set(runtime "${tc}/arm-kobov5-linux-gnueabihf/sysroot/lib")
set(link_flags "-L${runtime} -L${CMAKE_SYSROOT}/usr/lib/arm-linux-gnueabihf -Wl,-rpath-link,${CMAKE_SYSROOT}/usr/lib/arm-linux-gnueabihf -Wl,-rpath-link,${CMAKE_SYSROOT}/lib/arm-linux-gnueabihf")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${link_flags}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${link_flags}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${link_flags}")
set(ENV{PKG_CONFIG_PATH} "")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${CMAKE_SYSROOT}")
set(ENV{PKG_CONFIG_LIBDIR} "${CMAKE_SYSROOT}/usr/lib/arm-linux-gnueabihf/pkgconfig:${CMAKE_SYSROOT}/usr/share/pkgconfig")
# These target paths are retained in the build; simulator experiments bypass
# their execution only through the explicitly authorized per-run override.
set(BWRAP_EXECUTABLE /usr/bin/bwrap CACHE FILEPATH "Target Bubblewrap path")
set(DBUS_PROXY_EXECUTABLE /usr/bin/xdg-dbus-proxy CACHE FILEPATH "Target D-Bus proxy path")

# Runtime paths are supplied by the isolated launcher. No worktree path is installed.
set(CMAKE_INSTALL_PREFIX /opt/kobalt-webkit CACHE PATH "Renderer installation prefix" FORCE)

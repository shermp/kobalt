#!/bin/sh
# Run in the offline M1 tools container with the M3.3 SDK runtimes staged.
set -eu
cd /work
ulimit -c 0
out=/work/build/webkit-m3.4a
stock=/work/build/webkit-m3.2/stock
case "$1" in
qt5)
    export LC_ALL=C
    qt=/work/build/webkit-m3.3/qt5-runtime
    prefix=$stock
    libs="$stock/lib:$qt/usr/lib:$qt/lib"
    ;;
qt6)
    export LC_ALL=C.UTF-8
    qt=/work/build/webkit-m3.3/qt6-runtime
    prefix=$qt
    libs="$qt/qt:$qt/lib"
    ;;
*) exit 2 ;;
esac
sdk=$1
shift
if [ "$#" -eq 0 ]; then
    set -- normal version oversize truncated missing-fd kill kill kill kill kill
fi
for mode do
    child="$out/fw4/wpe-socket-child"
    if [ "$mode" = webkit ]; then
        package=/work/build/webkit-m2-package
        export QEMU_LD_PREFIX="$stock" QEMU_CPU=cortex-a9
        export WEBKIT_EXEC_PATH="$package/libexec" WEBKIT_INJECTED_BUNDLE_PATH="$package/lib"
        export WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1
        child=/work/build/webkit-m3.2/fw4/wpe-socket-renderer
        set -- "$out/$sdk/wpe-pool-smoke" --webkit
        timeout 30 qemu-arm -cpu cortex-a9 -L "$prefix" -E "LD_LIBRARY_PATH=$libs" \
            "$@" /usr/bin/qemu-arm -cpu cortex-a9 -L "$stock" \
            -E "LD_LIBRARY_PATH=$package/lib" "$child"
        continue
    elif [ "$mode" = pool ]; then
        set -- "$out/$sdk/wpe-pool-smoke"
    else
        set -- "$out/$sdk/wpe-socket-launcher" "$mode"
    fi
    timeout 15 qemu-arm -cpu cortex-a9 -L "$prefix" -E "LD_LIBRARY_PATH=$libs" \
        "$@" /usr/bin/qemu-arm \
        -cpu cortex-a9 -L "$stock" -U LD_LIBRARY_PATH "$child"
done

#!/bin/sh
# Stage only the tested ARM binaries and the resolved userspace closure.
set -eu
if [ "$#" -ne 5 ]; then
    echo "usage: $0 QT5_CLIENT WPE_RENDERER QT5_SYSROOT M2_PACKAGE NEW_OUTPUT" >&2
    exit 2
fi
client=$1
renderer=$2
qt=$3
m2=$4
out=$5
scripts=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
(cd "$m2" && sha256sum -c tests/SHA256SUMS)
mkdir "$out"
mkdir "$out/bin" "$out/qt-lib" "$out/lib" "$out/libexec" "$out/logs"
install -m 755 "$client" "$out/bin/wpe-pool-smoke"
install -m 755 "$renderer" "$out/bin/wpe-socket-renderer"
install -m 755 "$scripts/run" "$scripts/render" "$out/bin/"
install -m 755 "$m2/libexec/WPEWebProcess" "$m2/libexec/WPENetworkProcess" "$out/libexec/"
install -m 644 "$m2/lib/libWPEWebKit-2.0.so.1" "$m2/lib/libWPEInjectedBundle.so" "$out/lib/"
for name in libQt5Core.so.5 libQt5Gui.so.5 libQt5Network.so.5 libz.so.1 libpng16.so.16 \
    libicudata.so.64 libicuuc.so.64 libicui18n.so.64; do
    install -m 644 "$qt/usr/lib/$name" "$out/qt-lib/$name"
done
for name in libstdc++.so.6 libgcc_s.so.1; do
    install -m 644 "$qt/lib/$name" "$out/qt-lib/$name"
done
(cd "$out" && sha256sum bin/* lib/* libexec/* qt-lib/* > SHA256SUMS && sha256sum -c SHA256SUMS)

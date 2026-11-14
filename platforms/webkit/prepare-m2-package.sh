#!/bin/sh
# Package the exact stripped binaries validated in WPE-M1.
set -eu
if [ "$#" -ne 2 ]; then
    echo "usage: $0 M1-candidate-directory NEW-output-directory" >&2
    exit 2
fi
source_dir=$1
out=$2
if [ -e "$out" ] || [ -L "$out" ]; then
    echo "output already exists: $out" >&2
    exit 1
fi
mkdir -p "$out/bin" "$out/libexec" "$out/lib" "$out/share" "$out/tests" "$out/logs"
cp -L "$source_dir/wpe-snapshot-fixture" "$out/tests/wpe-snapshot-fixture"
cp -L "$source_dir/helpers/WPEWebProcess" "$source_dir/helpers/WPENetworkProcess" "$out/libexec/"
cp -L "$source_dir/lib/libWPEWebKit-2.0.so.1" "$out/lib/"
cp -L "$source_dir/modules/libWPEInjectedBundle.so" "$out/lib/"
cp "$(dirname "$0")/run-m2-fixture.sh" "$out/bin/run-fixture"
cat > "$out/tests/SHA256SUMS" <<'SUMS'
2d15e13fca3eaea032be6c931432072a367ab22cc59b83d04242a8e48e6a7e6d  tests/wpe-snapshot-fixture
d3442c1e50c12cc322f367ec74d41848083f70f78cd4fcadb0f2261bc59ab82b  libexec/WPENetworkProcess
a08c5e3b1a82c3fabae92f29b4260f500b72a65f0ea9ad7e53801aeba3baa117  libexec/WPEWebProcess
3fc848a29cb2f6f039a59daf322b677e4e363bc2bbb996d4da8a02ef644ea7c0  lib/libWPEWebKit-2.0.so.1
9cced556ca7571e72fa3dd2172091ac4b3f7dc82cfa4eef2161e5f2539b12f83  lib/libWPEInjectedBundle.so
SUMS
(cd "$out" && sha256sum -c tests/SHA256SUMS)
chmod 755 "$out/bin/run-fixture" "$out/tests/wpe-snapshot-fixture" "$out/libexec/WPEWebProcess" "$out/libexec/WPENetworkProcess"
(cd "$out" && sha256sum bin/run-fixture > tests/LAUNCHER-SHA256SUMS)
echo "Package prepared: $out"

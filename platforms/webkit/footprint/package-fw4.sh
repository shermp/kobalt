#!/bin/sh
# Bundle the already-built M8 candidate with its runtime inputs and notices.
set -eu
export LC_ALL=C
umask 022
test "$#" -eq 6 || {
    echo "Usage: $0 WEBKIT-ENTRY RENDERER GLIB-ENTRY TEXT-ENTRY RUNTIME-ENTRY NEW-OUTPUT" >&2
    exit 2
}
webkit=$1 renderer=$2 glib=$3 text=$4 runtime=$5 out=$6
root=$(CDPATH= cd -- "$(dirname "$0")/../../.." && pwd)
prefix=$webkit/install
source=$webkit/wpewebkit-2.54.2
grep -qx 'ENABLE_BUBBLEWRAP_SANDBOX:BOOL=OFF' "$webkit/build/CMakeCache.txt"
grep -qx 'ENABLE_C_LOOP:BOOL=ON' "$webkit/build/CMakeCache.txt"
grep -qx 'ENABLE_JIT:BOOL=OFF' "$webkit/build/CMakeCache.txt"
mkdir "$out"
bundle=$out/wpe
mkdir -p "$bundle/lib" "$bundle/libexec" "$bundle/fonts" "$bundle/notices" "$bundle/provenance"
install -m 755 "$renderer" "$bundle/wpe-book-renderer"
for program in WPEWebProcess WPENetworkProcess; do
    install -m 755 "$prefix/libexec/wpe-webkit-2.0/$program" "$bundle/libexec/$program"
done
install -m 644 "$prefix/lib/libWPEWebKit-2.0.so.1" \
    "$prefix/lib/wpe-webkit-2.0/injected-bundle/libWPEInjectedBundle.so" "$bundle/lib/"
for style in Regular Italic Bold BoldItalic; do
    install -m 644 "$root/assets/fonts/NotoSerif-$style.ttf" "$bundle/fonts/"
done
install -m 644 "$root/assets/notices/Noto-OFL" "$bundle/notices/Noto-OFL.txt"
install -m 644 "$root/LICENSE" "$bundle/notices/Kobalt-LICENSE"
install -m 644 "$root/dependencies/webkit/sources.json" "$bundle/provenance/fw4-sources.json"
install -m 644 "$root/dependencies/webkit/text/sources.json" "$bundle/provenance/text-sources.json"
install -m 644 "$root/dependencies/webkit/runtime/sources.json" "$bundle/provenance/runtime-sources.json"
install -m 644 "$root/assets/manifest.json" "$bundle/provenance/kobalt-assets.json"
install -m 644 "$root/dependencies/manifest.json" "$bundle/provenance/kobalt-dependencies.json"
grep -E '^(ENABLE_(BUBBLEWRAP_SANDBOX|C_LOOP|JIT|DFG_JIT|FTL_JIT|SAMPLING_PROFILER|WEBASSEMBLY)|CMAKE_(C|CXX)_FLAGS_RELEASE):' \
    "$webkit/build/CMakeCache.txt" >"$bundle/provenance/build-options.txt"
install -m 644 "$root/dependencies/webkit/configure-fw4.cmake" \
    "$root/dependencies/webkit/headless-options.cmake" "$bundle/provenance/"
mkdir "$bundle/provenance/patches"
install -m 644 "$root/dependencies/webkit/patches/"*.patch "$bundle/provenance/patches/"

notice()
{
    label=$1
    origin=$2
    shift 2
    mkdir -p "$bundle/notices/$label"
    for file do
        install -m 644 "$origin/$file" "$bundle/notices/$label/"
    done
}
notice WebKit "$source/Source/WebCore" LICENSE-APPLE LICENSE-LGPL-2 LICENSE-LGPL-2.1
notice JavaScriptCore "$source/Source/JavaScriptCore" COPYING.LIB
notice WTF "$source/Source/WTF" LICENSE-LLVM.txt LICENSE-dragonbox.txt LICENSE-libc++.txt LICENSE-simde.txt
notice Skia "$source/Source/ThirdParty/skia" LICENSE
notice GCC-runtime "$glib/sources/kobov4/arm-kobov4-linux-gnueabihf/share/licenses/gcc" \
    COPYING3 COPYING.RUNTIME
notice GLib "$glib/sources/glib" COPYING
notice libffi "$glib/sources/libffi" LICENSE
notice PCRE2 "$glib/sources/pcre2" COPYING
notice zlib "$glib/sources/zlib" LICENSE
notice ICU "$text/sources/icu" LICENSE
notice HarfBuzz "$text/sources/harfbuzz" COPYING
notice Brotli "$runtime/sources/brotli" LICENSE
notice epoxy "$runtime/sources/epoxy" COPYING
sed -n '8,17p' "$runtime/sources/egl-headers/api/EGL/egl.h" >"$bundle/notices/Khronos-EGL.txt"
notice Expat "$runtime/sources/expat" expat/COPYING
notice Fontconfig "$runtime/sources/fontconfig" COPYING
notice FreeType "$runtime/sources/freetype" LICENSE.TXT docs/FTL.TXT
notice libgcrypt "$runtime/sources/gcrypt" COPYING COPYING.LIB
notice libgpg-error "$runtime/sources/gpg-error" COPYING COPYING.LIB
notice libjpeg-turbo "$runtime/sources/jpeg" LICENSE.md README.ijg
notice nghttp2 "$runtime/sources/nghttp2" COPYING
notice libpng "$runtime/sources/png" LICENSE
notice libpsl "$runtime/sources/psl" LICENSE COPYING src/LICENSE.chromium
notice publicsuffix-list "$runtime/sources/psl-list" LICENSE
notice libseccomp "$runtime/sources/seccomp" LICENSE
notice libsoup "$runtime/sources/soup" COPYING
notice libtasn1 "$runtime/sources/tasn1" COPYING doc/COPYING.LESSER
notice libuuid "$runtime/sources/uuid" libuuid/COPYING
notice WebP "$runtime/sources/webp" COPYING PATENTS
notice xkbcommon "$runtime/sources/xkbcommon" LICENSE
notice libxml2 "$runtime/sources/xml2" COPYING
sed -n '28,36p' "$runtime/sources/sqlite/sqlite3.c" >"$bundle/notices/SQLite.txt"

cat >"$bundle/fonts.conf" <<'FONTCONFIG'
<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "urn:fontconfig:fonts.dtd">
<fontconfig>
  <dir>fonts</dir>
  <cachedir prefix="xdg">fontconfig</cachedir>
  <alias><family>serif</family><prefer><family>Noto Serif</family></prefer></alias>
</fontconfig>
FONTCONFIG
cat >"$bundle/run" <<'LAUNCHER'
#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
# Fontconfig 2.13 resolves a relative <dir> in the process working directory.
cd "$root"
unset LD_PRELOAD
export LD_LIBRARY_PATH="$root/lib"
export FONTCONFIG_FILE="$root/fonts.conf"
export WEBKIT_EXEC_PATH="$root/libexec"
export WEBKIT_INJECTED_BUNDLE_PATH="$root/lib"
export LIBGL_DRIVERS_PATH=/nonexistent __EGL_VENDOR_LIBRARY_FILENAMES=/nonexistent
exec "$root/wpe-book-renderer" "$@"
LAUNCHER
chmod 755 "$bundle/run"
(
    cd "$bundle"
    for elf in wpe-book-renderer libexec/* lib/*; do
        printf '\n%s\n' "$elf"
        wc -c <"$elf"
        readelf -lW "$elf" | grep 'Requesting program interpreter' || true
        readelf -dW "$elf" | grep -E 'NEEDED|SONAME|RPATH|RUNPATH'
        readelf -VW "$elf" | grep -oE 'GLIBC_[0-9]+(\.[0-9]+)+' | sort -Vu | tail -n 1
    done
) >"$bundle/provenance/elf-inventory.txt"
(
    cd "$bundle"
    find . -type f ! -name SHA256SUMS -print0 | LC_ALL=C sort -z | xargs -0 sha256sum >SHA256SUMS
    sha256sum -c SHA256SUMS
)
tar --sort=name --mtime=@0 --numeric-owner --owner=0 --group=0 -C "$out" -cf "$out/wpe-fw4.tar" wpe
gzip -n "$out/wpe-fw4.tar"
sha256sum "$out/wpe-fw4.tar.gz"

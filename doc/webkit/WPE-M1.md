# WPE-M1 standalone snapshot runtime

Worktree base `cb58467f` (WPE-M0), plan base `0b8f3c6014c7bdb8891c7384d85f4f257d0fd3d0`, latest inspected upstream Blaze `8de6ea8127a472378829910bfe59ac05f9571350`. The [WPE-M0 handoff](WPE-M0.md) records the approved Nia/kobo7 4.38.23852 simulator, Kobo glibc 2.19 and the `arm-kobov4-linux-gnueabihf` GCC 14.4 toolchain.

## Native snapshot fixture

The standalone CMake target links the WPE 2.0 public API, explicitly requests a visible snapshot, copies its BGRA8888 rows into owned memory and checks every active pixel. The fixture expects white squares on red or blue backgrounds. It checks red → blue → red navigation, exact viewport size after a resize event, a color change scheduled by `requestAnimationFrame`, cancellation of an outstanding snapshot, and a valid snapshot after cancellation. The fixture uses its own HTML and JavaScript. It blocks on GLib events with an overall deadline.

Two runs against the prior native WPE 2.54.2 cache produced identical hashes. Symbol inspection confirmed `webkit_web_view_get_snapshot`, its finish function and `webkit_image_as_bytes`. A supervised run confirmed that helper processes exited. These runs use the existing native shared WebKit build as a test reference.

| Result | Dimensions and stride | SHA-256 of tightly packed rows |
| --- | --- | --- |
| Initial and return to red | 160 × 120, 640 bytes | `5a866ff21f6cbef0e875097b8c460faff4c234bb73d0d808b7305360af69a2f4` |
| Blue navigation | 160 × 120, 640 bytes | `759e13abb3757e0b105d1ca7ae25f1499bc4ecf1a2da352693a8c88ea71c74a1` |
| Resize with red background | 208 × 136, 832 bytes | `213597716d3f34fb74d8d0419856d4c053b1cc8e551cea94a94386539c05406a` |
| Delayed blue mutation and post-cancellation recovery | 208 × 136, 832 bytes | `8dea712c80ce98bdca9dbe8d68f0d46c19f8b19f46667c9cb65cd1b1b23b4a35` |

The input is the previously built native reference at `webkit-m0-native-b0ff50682b8215ee178a74943bdaf2648bdd9bd85d646406ee712343fb6b83ec/install` in the shared dependency cache. The standalone fixture can also use a separately prepared WPE 2.54.2 headless installation. In the Linux tools container, point `prefix` at that installation and run from the Kobalt worktree.

```sh
export PKG_CONFIG_PATH="$prefix/lib/pkgconfig"
cmake -GNinja -S platforms/webkit -B build/webkit-m1/native
cmake --build build/webkit-m1/native
LD_LIBRARY_PATH="$prefix/lib" LIBGL_DRIVERS_PATH=/nonexistent \
  __EGL_VENDOR_LIBRARY_FILENAMES=/nonexistent \
  timeout 40 build/webkit-m1/native/wpe-snapshot-fixture
```

The test used Podman `--init --network none --userns keep-id`. The container init reaped the exited helper tree. The native reference has the renderer and both native helpers linked to `libWPEWebKit-2.0.so.1`. Its injected bundle is a separate module linked to the same library. The local ELF output and native results are under `build/webkit-m1/`.

## FW4 source builds

The dependency recipes build position-independent static archives in the existing content-addressed cache. They use the existing native tools image `sha256:662599b1c68c64b57e6a3d8f35994592a3f4ac6042b3a66c82651db1f36428d7`, containing GCC 13.3 and CMake 3.28.3, for the host generators. The target compiler, Meson, upstream archives and snapshot source archives are checksum-pinned. HarfBuzz, FreeType, libjpeg-turbo and libpng reuse the application's source pins, with separate renderer build options. Initial builds need network access for uncached downloads. Cache-hit runs can use Podman `--network none`.

From the worktree, mount the checkout and the existing shared dependency cache at the paths used by the build. Set `cache` to the main checkout's `build/dependency-cache`.

```sh
image=662599b1c68c64b57e6a3d8f35994592a3f4ac6042b3a66c82651db1f36428d7
podman run --rm --userns keep-id --cpus 8 -v "$PWD:/work:rw" \
  -v "$cache:/dependency-cache:rw" -e KOBALT_DEPENDENCY_CACHE=/dependency-cache \
  --entrypoint sh "$image" -ec '
    cd /work
    out=/work/build/webkit-m1
    image=662599b1c68c64b57e6a3d8f35994592a3f4ac6042b3a66c82651db1f36428d7
    cmake -DKOBALT_WEBKIT_BUILD_IMAGE="$image" -DKOBALT_WEBKIT_OUTPUT_FILE="$out/glib-entry" \
      -P dependencies/webkit/build-glib.cmake
    glib=$(cat "$out/glib-entry")
    cmake -DKOBALT_WEBKIT_BUILD_IMAGE="$image" -DKOBALT_WEBKIT_GLIB_ENTRY="$glib" \
      -DKOBALT_WEBKIT_OUTPUT_FILE="$out/text-entry" -P dependencies/webkit/build-text.cmake
    text=$(cat "$out/text-entry")
    cmake -DKOBALT_WEBKIT_BUILD_IMAGE="$image" -DKOBALT_WEBKIT_GLIB_ENTRY="$glib" \
      -DKOBALT_WEBKIT_TEXT_ENTRY="$text" -DKOBALT_WEBKIT_OUTPUT_FILE="$out/runtime-entry" \
      -P dependencies/webkit/build-runtime.cmake
    runtime=$(cat "$out/runtime-entry")
    cmake -DKOBALT_WEBKIT_TOOLCHAIN="$glib/sources/kobov4/arm-kobov4-linux-gnueabihf" \
      -DKOBALT_WEBKIT_PREFIXES="$runtime/install;$text/install;$glib/install" \
      -DKOBALT_WEBKIT_OUTPUT="$out/fw4" -P dependencies/webkit/configure-fw4.cmake
  '
```

GLib 2.80.5, libffi, PCRE2 and zlib built as ARM archives. The resulting `gdbus` and `gresource` link statically to those archives, with the Kobo ARM loader, libc, libm, libdl, libresolv, libpthread and compiler `libgcc_s.so.1` in their dynamic closure. Their highest required glibc symbol version was 2.17. Both printed help successfully in the disposable Nia simulator under the reconstructed Kobo glibc 2.19 loader, with `libgcc_s.so.1` from the cross toolchain. Nickel responded after the check, and the instance, container and system volume were removed. ICU 70.1 and HarfBuzz 15.1.0 also built as ARM archives, including `libicudata.a` and `libharfbuzz-icu.a`.

The completed additional runtime prefix contains static archives for Brotli, Epoxy, Expat, Fontconfig, FreeType, libgcrypt, libgpg-error, JPEG, nghttp2, PNG, libpsl, libseccomp, libsoup, SQLite, libtasn1, libuuid, WebP, xkbcommon and libxml2. The three completed prefixes contain no shared libraries. The cross-configure found the required archives, confirmed FreeType's WOFF2 support and used the native GLib resource generator. The generated WPE config enables `ENABLE_2022_GLIB_API`, `ENABLE_WPE_PLATFORM_HEADLESS` and `USE_SKIA`.

The WebKit recipe selects the modern WPE headless port, 2022 GLib API and Skia. It applies the lazy-EGL and no-libdrm snapshot corrections audited in WPE-M0, makes release builds honor a run-local helper directory, exports embedded GLib symbols for the standalone fixture, and declares PAL's libgcrypt and libtasn1 dependencies for an isolated cross sysroot. For this fixture it omits system hyphenation, XSLT, color management, media, display-server backends and desktop integrations. It keeps Bubblewrap support compiled in. Link-time Epoxy and Khronos EGL headers support upstream compilation. The native fixture completed with unusable EGL driver paths and without a graphics device.

To build and stage the fixture, use the path printed by `configure-fw4.cmake` as `webkit`. Run in the same tools container with the three FW4 cache prefixes available.

```sh
export PKG_CONFIG_LIBDIR="$runtime/install/lib/pkgconfig:$text/install/lib/pkgconfig:$glib/install/lib/pkgconfig"
cmake --build "$webkit/build" --parallel 10
cmake --install "$webkit/build" --strip
prefix="$webkit/install"
export PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig:$PKG_CONFIG_LIBDIR"
cmake -GNinja -S /work/platforms/webkit -B "$out/fixture-fw4" \
  -DCMAKE_TOOLCHAIN_FILE=/work/dependencies/webkit/fw4-toolchain.cmake \
  -DKOBALT_WEBKIT_TOOLCHAIN="$glib/sources/kobov4/arm-kobov4-linux-gnueabihf" \
  -DKOBALT_WEBKIT_PREFIX="$prefix;$runtime/install;$text/install;$glib/install" \
  -DKOBALT_WEBKIT_EMBEDDED_GLIB=ON
cmake --build "$out/fixture-fw4"
```

## FW4 compile and simulator results

The compiler failure after PAL came from `BubblewrapLauncher.cpp`. The FW4 kernel headers define `MFD_ALLOW_SEALING`, but glibc 2.19 lacks the `memfd_create` wrapper and the fcntl seal constants. The patch separates those checks and uses the kernel syscall while retaining all four seals and the existing failure handling. The affected translation unit compiled. The next link found two private xdgmime symbols defined by both WebKit and static GIO. Prefixing those two WebKit symbols resolved the collision. WebKit, both process helpers, the injected bundle and the standalone fixture then linked successfully.

The ARM version of the fixture passed on Nia/kobo7 4.38.23852 under QEMU Cortex-A9 and Kobo glibc 2.19. All dimensions, strides and hashes matched the native results above, including the fresh snapshot after cancellation. Nickel responded after the run. VirtualNickel's offline socket filter was active. WebKit reported that Bubblewrap was unavailable inside the container and used its automatic container fallback. Fontconfig reported a missing default config. This color-only fixture did not require fonts. The disposable instance, container and firmware volume were removed.

The staged experiment contains the fixture, `WPEWebProcess`, `WPENetworkProcess`, `libWPEWebKit-2.0.so.1`, `libWPEInjectedBundle.so` and toolchain copies of `libatomic.so.1`, `libgcc_s.so.1` and `libstdc++.so.6`. The first run confirmed that dynamic `libatomic.so.1` was required. The eight ELF files total 99,138,136 bytes; their exact sizes, digests, required libraries and versioned symbols are recorded in `build/webkit-m1/fw4-elf.log`. The highest required glibc symbol version across the staged ELFs is 2.18. The ARM WebKit library exports both snapshot entry points and `webkit_image_as_bytes`.

Local reproduction files are `build/webkit-m1/prepare-fw4-fixture.sh`, `run-fw4-guest.sh`, `run-fw4-fixture.py` and `audit-fw4-fixture.sh` in that same directory. The guest command uses the temporary stock-loader layout from WPE-M0 and a private library search path. The runner uses a 768 MiB container cap. The ARM link and pixel evidence are preserved in `fw4-webkit-link.log` and `fw4-snapshot-retry.log`. Source checks passed. Qt and application suites were outside this standalone slice.

## Remaining evidence

WPE-M1 still requires a linkage comparison and review before adopting a private shared runtime. Combined PSS, runtime font provenance, module lookup coverage, and in-container helper teardown beyond disposal remain unverified. The simulator's sandbox fallback supplies no physical-kernel evidence. No WPE-M1 files are staged, committed, pushed or deployed. WPE-M2 has not started.

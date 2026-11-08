# M0 dependency record

The proof uses WPE WebKit **2.54.2**, koxtoolchain **2026.08 kobov5** and an isolated Ubuntu 22.04 ARM dependency sysroot. Kobalt and Qt SDKs remain unchanged. This is a feasibility dependency stack, not a release package or a completed M1 reproducibility claim.

## Exact inputs

[Source pins](../../dependencies/webkit-m0/sources.json) and [host tools](../../dependencies/webkit-m0/host-tools.json) record the archive URLs and SHA-256 digests. The existing `dependencies/cache.cmake` download helpers verify the archives and share them through the main checkout's dependency cache.

| Input | SHA-256 |
| --- | --- |
| WPE WebKit 2.54.2 | `f3284e19e47fcb5ac8c63c2e1c113dbd65b2423eb8a545f5b345707829004f56` |
| koxtoolchain 2026.08 kobov5 | `a9eba0aaf17e5cc94fdabb46c2ae06d9bb41936b869ea6479d325c9f79b14a64` |
| CMake 3.31.8 Linux x86-64 | `630615d8e98ac33eba7fbe472626dff5c899c85af3c024585ae109166a6909d0` |

WebKit's digest was checked against the [official release](https://wpewebkit.org/release/wpewebkit-2.54.2.html), and the toolchain against its GitHub release asset digest. Signatures were not independently verified.

Build image IDs, base-image digests and reconstruction commands are in [build.md](build.md). Exact installed packages are retained in [arm-packages.tsv](evidence/arm-packages.tsv) and [native-packages.tsv](evidence/native-packages.tsv). The Containerfiles install from moving Ubuntu repositories; the retained local images reproduce this investigation's package inputs, but rebuilding a Containerfile later can select different versions. M1 must lock package/source inputs and distribution material.

The completed ARM dependency set is `webkit-m0-armhf-61ebcc82cd3b82ebd8d4929b49d4c5a4e990c54c7a4e50c12eba45736dbe1fb4`. Its identity includes image ID, CMake version, source pins, recipes and both patches. Its `complete` marker is present. Never modify this published entry. The native experiment was corrected incrementally before publication and remains unpublished; its key must not be reused as a completed final-recipe entry.

## Configuration and closure

[wpe-options.cmake](../../dependencies/webkit-m0/wpe-options.cmake) configured and compiled on both architectures. It enables modern WPEPlatform/headless and sandbox support; disables the legacy API, WebGL, WebGPU, GPU process, DRM, GBM, Vulkan, Wayland, GStreamer, video, audio, WebRTC and selected desktop facilities; and retains JavaScriptCore, SVG, MathML and ordinary document features. AVIF and JPEG XL are also disabled for this minimal stack. FreeType provides WOFF2 support. These private WebKit options require checking at each upstream update.

Important target versions include GLib 2.72.4, ICU 70.1, HarfBuzz 2.7.4, Soup 3.0.7, FreeType 2.11.1, Fontconfig 2.13.1, Epoxy 1.5.10, PNG 1.6.37, WebP 1.2.2, SQLite 3.37.2, libxml2 2.9.13, libxslt 1.1.34, libgcrypt 1.9.4, libtasn1 4.18.0 and libseccomp 2.5.3. Full Ubuntu revisions are in the inventories. Compiler runtimes come from GCC 14.4.0, including libstdc++ 6.0.33 and libatomic.

[arm-library-inputs.tsv](evidence/arm-library-inputs.tsv) records each private library's input SHA-256 and origin before stripping. [arm-bundle-audit.json](evidence/arm-bundle-audit.json) records stripped ELF checksums, byte sizes and required symbol maxima. The measured bundle is 100997695 bytes, including fixtures, and all ELF files are ARM32. No glibc or loader replacement is included. Mandatory WebKit subprocesses and the injected-bundle module retain their installed layout.

Epoxy is an upstream build/link prerequisite even with the tested CPU path. Its presence does not establish GL use: the runtime maps contain no GL/EGL/Vulkan/DRM/GStreamer modules. This bundle is the resolved mandatory ELF closure, not a claim to have minimized every upstream library or covered arbitrary optional `dlopen()` features.

## Candidate patches

- [lazy-wpe-egl.candidate.patch](../../dependencies/webkit-m0/lazy-wpe-egl.candidate.patch): defer modern WPE's EGL initialization until preferences require acceleration, using the existing preference-time path.
- [no-libdrm-snapshot.candidate.patch](../../dependencies/webkit-m0/no-libdrm-snapshot.candidate.patch): compile the DMA-BUF format branch only where its DRM definitions are available; retain the SHM capture branch without libdrm.

Both patches compile and render on native x86-64 and ARM32. They are separate from the verified pristine source and applied by the existing `dependencies/apply-patches.cmake` helper. Default GPU/Vulkan and legacy configurations are untested; no upstream approval is claimed. The separate simulator child-prefix diff is unapplied.

## Licenses and distribution work

[arm-package-licenses.json](evidence/arm-package-licenses.json) maps bundled Ubuntu libraries to exact packages, copyright-file paths and hashes, and machine-readable license labels where supplied. It is an inspection index, not a substitute for the notices themselves or a completed legal audit.

| Component | License material to retain and audit |
| --- | --- |
| WebCore / JavaScriptCore / WTF | LGPL-2.1 terms and permissive portions; component `COPYING.LIB` and individual file notices |
| Bundled Skia and other WebKit third-party code | `Source/ThirdParty/skia/LICENSE`, third-party notices and the exact enabled source set |
| GLib, Soup, HarfBuzz, XML/XSLT, ICU, fonts and image libraries | Package-specific LGPL, MIT/BSD, ICU, FreeType, zlib and other notices indexed above |
| libgcrypt / libgpg-error / crypto closure | Component LGPL and other source notices; inspect exact bundled versions |
| GCC libstdc++, libgcc and libatomic | GPL terms with applicable GCC runtime exceptions; toolchain `share/licenses/gcc/` |
| Embedded Instrument Serif fixture | Existing Kobalt asset and its OFL notice under `assets/fonts/` |
| CMake and host generators | Host-only tool licenses; they are not in the renderer bundle |

Before redistribution, collect corresponding sources, patches, build recipes, notices and relinking/replacement material as required by each component. There is no source-distribution compliance certification or production runtime packaging in M0.

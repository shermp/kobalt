# M1 source and distribution audit

The standalone source export is corresponding-source material for the selected
renderer bundle. It is not approval to publish the renderer or its SDK. The
current implementation keeps the first-party reader and its data unchanged.

## Material and provenance

| Material | Binding to the runtime | Included material |
| --- | --- | --- |
| Kobalt helper | Working-source hashes and base commit | MIT license, helper/launcher sources, CMake recipes, tests and fixture |
| WPE WebKit | Official 2.54.2 archive SHA-256 and dependency key | Pristine release, all Kobalt patches, CPU configuration and component licenses |
| Ubuntu runtime libraries | Binary/source package versions from the actual ELF closure | Exact `.dsc`, pristine archive and Debian patch archives for each source version |
| GCC runtime libraries | kobov5 2026.08 archive, embedded configuration/build log | GCC 14.4.0 source, exact crosstool-NG/koxtoolchain recipe commits and their patches |
| Instrument Serif | Fixture font checksum | Original font and OFL notice |
| Prepared SDK | OCI archive/config SHA-256 and image identity | Separate local recovery archive; it is not included in the source tar |

Ubuntu source pins come from authenticated indexes at the fixed snapshot
`20261108T000000Z`, with the prepared image's Ubuntu archive keyring. No trust or
validity override was used. The retained InRelease hashes and compiler recipe
commits are in `platforms/webkit/source-provenance.json`. The source snapshot is a
build provenance record, not a claim that those package versions remain current.
See [Ubuntu's snapshot documentation](https://snapshot.ubuntu.com/).

The compiler build log identifies the bundled GCC patches. The pinned
crosstool-NG source supplies those patches and the SHA-256 for the pristine GCC
archive. This is distinct from independently verifying a GNU release signature.

The export checks every selected library's source name/version against the
source inventory. Source groups preserve their original filenames for
`dpkg-source`. Source imports verify declared archive hashes before a dependency
cache can be used. Full bundle verification also checks pristine first-party
files; imports permit the recipient to modify application code and patches.

## Notices and replacement

WebKit has mixed LGPL and permissive portions; its [licensing page](https://webkit.org/licensing-webkit/)
is an overview, while the pinned release's files determine the applicable terms.
The export preserves the complete pristine sources and patches. Runtime packaging
retains Kobalt's MIT license, component license files and the exact Ubuntu package
copyright files.

The upstream files changed by Kobalt have BSD-style headers. Their full original
headers are now retained separately under `share/licenses/webkit-modifications/`,
with the corresponding patch name:

- `WebProcessGLib.cpp`, `ProcessExecutablePathGLib.cpp` and `ProcessLauncherGLib.cpp`: Apple/Motorola notices.
- `DrawingAreaCoordinatedGraphicsGLib.cpp`: Apple/Nokia/Igalia notices.
- `AcceleratedBackingStore.cpp`: Igalia notice.

The GCC runtime libraries must retain their own license and applicable exception,
rather than being described simply as GPL or permissive. See the [libstdc++ license
and Runtime Library Exception](https://gcc.gnu.org/onlinedocs/libstdc++/manual/license.html).
Compiler runtime source and downstream build recipes accompany this source export.
The entire binary toolchain and SDK are separate build inputs; a public SDK
redistribution would require auditing their complete contents too.

Private libraries remain ordinary shared objects with their SONAMEs. The launcher
selects the bundle's private directory without a signature check that prevents
replacement. Compatible replacement libraries can therefore be installed there.
Corresponding sources, patch/build instructions and notices must remain associated
with any redistributed binary. Publishing binaries without arranging their source
and notice delivery is outside this development artifact's status.

The selected-configuration audit is recorded in the
[distribution inventory](distribution-inventory.json). It maps the actual ELF
closure to exact source versions, corresponding archives and hashes of the full
notice files. The bundled-component inventory was checked against the completed
build's source/header dependencies, rather than treating every directory in the
WebKit tar as enabled.

The observed bundled inputs include Skia/skcms, xdgmime, WTF's V8 dtoa and
fast_float, and bmalloc's libpas/mimalloc. Skia/skcms and V8 dtoa retain BSD terms;
xdgmime provides an LGPL/AFL alternative; fast_float's amalgamated header carries
MIT and Boost alternatives alongside its Apache license; libpas is BSD and
mimalloc MIT. Per-file terms remain in the complete source material. The library
inventory preserves the original Ubuntu copyright texts, including mixed-license
packages; GPL terms for a package's separate utilities are not assigned wholesale
to its shared libraries.

Packaging now retains `NOTICE`, `AUTHORS` and `COPYRIGHT` files as well as
`LICENSE`/`COPYING`. The source-pinned `platforms/webkit/notices.json` identifies
header-only notice groups. Their complete leading comment groups are retained
and deduplicated by content, including editor-mode comments before a license and
amalgamated line-comment licenses. Native regressions cover both formats and
reject a truncated block. New source pins require review of this manifest.

`ACKNOWLEDGEMENTS.txt` includes the FreeType and Independent JPEG Group credits.
The fixture's font now carries its own Instrument Serif copyright/OFL file,
verified against `assets/fonts/instrument-serif.json`. The earlier generic
`assets/fonts/OFL.txt` describes other fonts and was insufficient attribution for
this fixture. Both the runtime and source export now carry the correct file.
GCC's GPLv3 text and Runtime Library Exception are also retained explicitly.

For distribution, pair the binary with the exact complete source bundle, its
patches/build scripts, original notices, checksums and replacement instructions.
The runtime's index does not replace per-file terms. The audit covers this M1
configuration; enabled-feature/source changes require review. Public SDK
redistribution and release publication remain separate decisions. This is a
record of delivered material and obligations, not a compliance certification.

## ARM instruction-set finding

The selected bundle's `libgcrypt.so.20` advertises ARMv8 attributes. Inspection of
the pinned 1.9.4 source shows AES, SHA1/SHA2 and PMULL routines selected through
ARM hardware-feature flags (`src/hwf-arm.c` and the cipher dispatch code).
The Debian patch inventory was retained for comparison with those upstream paths.

The focused `webkit-crypto-isa` diagnostic runs the bundled library's built-in
self-tests and a SHA-256 known-answer check with firmware glibc 2.35. It passed
under Cortex-A7 with `HWCAP2=0` and QEMU `max` with `HWCAP2=0x7f`, while retaining
the existing offline restriction. This addresses the observed crypto-dispatch
concern for those exercised paths; it is not physical-device or exhaustive ISA
validation.

With the normal ARM build container's pinned host CMake/toolchain environment,
run `cmake --preset crypto-isa` and `cmake --build --preset crypto-isa` from
`platforms/webkit/`. This uses a separate diagnostic output and does not build WPE. Run it through `qemu-arm -L <firmware root>` with the renderer
bundle's `private/` directory first in the guest library path. The current raw
results are `build/webkit/crypto-isa.log`, `crypto-source-patches.json` and
`isa-attributes.txt`. The saved firmware volume was mounted read-only and never
booted or changed by this diagnostic.

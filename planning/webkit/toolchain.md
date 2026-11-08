# Toolchain and ABI

Keep prepared kobuild Qt6 for Kobalt. Its release pin remains in `platforms/sdk-image.sh`; no SDK was rebuilt or replaced. The prepared kobuild image is not locally cached. The existing simulator Qt6 builder was used only to supply CMake for standalone probes.

| Input | Established result |
| --- | --- |
| WebKit 2.54.2 | Global CMake >= 3.20; the ARM32 allocator requires >= 3.25; Ninja; C++23; explicit GCC >= 12.2 check |
| Existing Qt6 builder | CMake 3.22.1, GCC 11; native WebKit configuration with Ninja fails the GCC check |
| Isolated native build | Ubuntu 24.04, GCC 13.3.0 and CMake 3.28; native CPU-rendering proof passes |
| ARM host tools | Pinned official CMake 3.31.8 binary; Ubuntu 22.04 ARM dependency sysroot with glibc 2.35 |
| koxtoolchain 2026.08 `kobov5` | Executed GCC 14.4.0; glibc 2.35 sysroot; libstdc++ 6.0.33 |
| Firmware root tested | Libra Colour 6.0.274403; glibc 2.35; libstdc++ 6.0.29 |

Sources: release `6a4407a3cbb5fb2e3513e63c0fd12fcdbe1380af` of [koxtoolchain](https://github.com/koreader/koxtoolchain/releases/tag/2026.08); its `gen-tc.sh` pins crosstool-ng revision `34844bc8e985ad1ba26b072a5b58264967072e19`. That revision's `samples/arm-kobov5-linux-gnueabihf/crosstool.config` selects Cortex-A7, Thumb, NEON, hard float, glibc 2.35 and a 4.9 minimum kernel.

The compiler reports defaults `-mcpu=cortex-a7`, `-march=armv7ve+neon`, `-mfpu=neon`, hard float and Thumb. The [probe toolchain file](../../dependencies/webkit-m0/kobov5.cmake) explicitly requests ARMv7-A, NEON and hard float. Linked startup/runtime objects still carry v7VE/integer-division attributes; inspect the **whole** linked result rather than assuming a flag overrides prebuilt objects. The whole-bundle [ABI audit](evidence/arm-elf-abi.json) also finds ARMv8/crypto-NEON attributes in Ubuntu's `libgcrypt.so.20.3.4`; all other bundled objects advertise ARMv7. A follow-up with the helper and inherited children using QEMU `cortex-a7` passed render, repaint, repeated startup, injected failure and fresh startup with the same pixels. This establishes the tested ARMv7 workload, not every optimized crypto entry point or physical CPU compatibility. Audit source dispatch or select explicit target-library builds before production.

The built executable is little-endian ARM ELF32, EABI5, hard float, interpreter `/lib/ld-linux-armhf.so.3`. It requires glibc symbols through `GLIBC_2.34`; the private C++ runtime also requires libm `GLIBC_2.35`. These resolve against the tested firmware. The private probe library requires `GLIBCXX_3.4.30`, absent from stock libstdc++.

Bundle private libstdc++ and libgcc only in the helper's environment. Do not change Nickel's library path or preload those libraries into Nickel. The complete ARM WebKit build and firmware execution also passed. The isolated dependency sysroot contains only ARM packages; configuration's implicit include/link paths were checked for host contamination. Native Python/Ruby/Perl and GLib generators run on the build host; no ARM generator needs to run during this build. Source generation includes the ARM LLInt assembly.

The final stripped ELF closure requires at most `GLIBC_2.35`, `GLIBCXX_3.4.30` and `CXXABI_1.3.9`; see [the per-file audit](evidence/arm-bundle-audit.json). Firmware 6.0.274403 supplies libc, libm, the loader and related glibc-family libraries; modern libstdc++, libgcc and libatomic remain private. Runtime loading and WebKit child execution succeeded in a running disposable firmware device. This proves the tested firmware/host configuration, not every firmware 6 release or physical ARM CPU.

The dependency build used eight compiler jobs, `-O1 -DNDEBUG`, and the host's available memory. Peak compiler memory was not separately instrumented. Linkers warned that transitive `libatomic.so.1` was outside their rpath-link roots; the bundle resolver includes the toolchain's library, and firmware runtime loading passed. Tightening that build-time search path belongs in a new dependency recipe/key, never an edit to the completed cache entry.

The generated ARM `LLIntAssembly.h` selects ARMv7, disables JIT, and does not select the C++ C-loop. This configuration uses the assembly LLInt interpreter. Do not infer JIT availability solely from the CMake cache default; platform headers override it on 32-bit targets.

# M0 feasibility — 8 November 2026

**Conditionally viable. WPE WebKit 2.54.2 builds for ARM32 and produces correct CPU-rendered page images inside running Kobo firmware 6.0.274403.** The proof requires two small WebKit patches, private libraries, an explicit simulator-only sandbox exception and a native test supervisor. These conditions are bounded, but this is not production readiness or physical-device validation.

M0 investigation is complete. **M1 requires separate approval.** No production Kobalt code, CREngine behavior, Qt SDK or reading data changed. No tracked VirtualNickel file changed.

## Verified behavior

- Ubuntu 24.04.5 on x86-64 WSL2; both requested repositories accessible.
- Official WebKit, koxtoolchain and host CMake archives verified by pinned SHA-256. Dependency builds use the existing content-addressed cache; the completed ARM set includes both patches in its identity.
- GCC 14.4.0 produces ARM32 hard-float WebKit and probe binaries. The private ELF closure needs glibc no newer than firmware's 2.35. Firmware libc/loader are retained; modern libstdc++, libgcc and libatomic remain private.
- Native Ubuntu CPU rendering passes with Bubblewrap sandboxing enabled and no usable GPU/EGL/display server. A negative control cannot initialize EGL. Observed buffers are SHM and process maps contain no GL/EGL/Vulkan/DRM/GStreamer modules.
- The ARM proof runs alongside stock Nickel in a disposable Libra Colour device through VirtualNickel's existing Device API. It renders local XHTML, embedded font, PNG, SVG, MathML and trusted JavaScript into a visually inspected 800×1100 image.
- Repeated starts and a start after an injected WebProcess crash produce identical PNG bytes. A DOM update changes the image. WebProcess failure returns an error, Nickel remains responsive, and a native supervisor reaps the owned children. Cleanup checks include zombies.
- Runs under 768 MiB and 512 MiB container limits with one CPU pass without OOM events. Only session-owned devices were created; all were stopped and deleted with their system volumes. Pre-existing saved devices were not launched or altered.

The [ARM image](../../build/webkit-m0/firmware-results-512m/cold-1.png) has SHA-256 `89252a681e502ab38fe3f8f178d89c696030f05085a5dc0f3d774cbdaadb0008`. [Rendering](rendering.md), [toolchain](toolchain.md), [dependencies](dependencies.md), [simulator findings](virtualnickel.md), [architecture](architecture.md) and [build instructions](build.md) contain the detailed evidence and limits.

## Resource findings

| Measurement | Result for the controlled ARM fixture |
| --- | --- |
| Stripped bundle including fixtures | 100997695 bytes, about 96.3 MiB |
| Renderer tree idle PSS with QEMU `max` | About 235 MiB, including QEMU overhead and timeout; excludes Nickel and the small Python supervisor |
| First snapshot | About 2.2–2.4 s from probe entry, before PNG encoding |
| Subsequent DOM-update snapshot | About 36–44 ms, before PNG encoding |
| PNG encoding/write | About 133–156 ms |
| Settled idle CPU | About 0.2–0.4% of one CPU over five-second windows |
| Whole-device cgroup peak | About 523 MiB at the 768 MiB cap; about 504 MiB at the 512 MiB cap |

A separate Cortex-A7-model check also passed, including child execution and post-crash startup, with identical pixels. Its renderer-tree PSS was about 252 MiB and whole-container peak about 552 MiB at the 768 MiB cap; CPU-model-dependent QEMU overhead changes the footprint. The 512 MiB pass above used `max`, not Cortex-A7.

These are QEMU diagnostic measurements, not physical page-turn, e-ink, power or battery estimates. The 512 MiB result has little memory margin for even this small fixture. Cgroup peaks include stock Nickel, QEMU and charged caches; they are not renderer-only memory. Native PSS was about 109 MiB. No extrapolation to large books or acceptable hardware performance is justified yet.

## Conditions and remaining risks

1. **Two release corrections:** [lazy EGL initialization](../../dependencies/webkit-m0/lazy-wpe-egl.candidate.patch) and [the no-libdrm snapshot guard](../../dependencies/webkit-m0/no-libdrm-snapshot.candidate.patch) are compiled and exercised on both architectures. Default GPU/Vulkan/legacy configurations and upstream acceptance remain unverified. No custom compositor or major fork is needed for this proof.
2. **Sandbox compatibility:** ARM namespace/seccomp operations fail under QEMU 10.0.13. The user explicitly authorized the per-run `WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1` experiment, guarded by `--allow-unsandboxed-experiment`. Sandbox support stays compiled in; native runs retain it. The offline filter, container network restrictions and ordinary security policy remain in place. Internal WebKit isolation is absent in those ARM experiment runs; a production/device sandbox solution remains unresolved.
3. **Child launch and ownership:** inherited `QEMU_LD_PREFIX` and the host's binfmt setup make WebKit subprocesses work here without editing the simulator. This is not portable child-launch support for every host. Without a native child reaper the exited WebKit subprocesses remained zombies. The run-owned supervisor resolves that test lifecycle issue; production helper supervision is still to design.
4. **Packaging and resources:** installed subprocess/module paths are fixed to the experiment directory. Moving-package relocatability, complete source/package locks, licenses, failure timeouts and full runtime resource policy are M1 work. Memory use warrants physical validation before any default-backend decision. The bundle includes libgcrypt ARMv8 attributes; the Cortex-A7 fixture passes, but a complete target-ISA/source-dispatch audit is still required.

## Scope and validation

Repository inspection covered EngineDocument, ReaderSession/ReaderFrame, publication/resource ownership, positions, workers and existing build/test boundaries. ABI probes preceded WebKit compilation. Native render, repaint, missing-resource failure, override rejection and cleanup checks passed. ARM render, repaint, repeat startup, injected crash, post-crash restart, resource limits and cleanup passed. Existing native results were reused while investigating ARM. Before committing, the C++ probes were formatted with clang-format 21.1.8 and rebuilt on both architectures; both rendering executables remained byte-identical to the validated binaries. Unrelated Kobalt and simulator suites were not rerun because their production sources were unchanged.

This fixture is an XHTML page, not an integrated EPUB backend. EPUB parsing/pagination, arbitrary-book script policy, annotations, positions, IPC, suspend/resume, large-book pressure, production sandboxing and physical hardware remain later work. The existing CREngine implementation remains operational and untouched.

# Kobalt — WebKit Rendering Backend Implementation Plan

## 1. Objective

Implement a modern WebKit-based EPUB rendering backend for [Kobalt](https://github.com/nicoverbruggen/kobalt), initially alongside the existing CREngine implementation.

The target is **Kobo firmware 6.x (ARM32, Qt 6.5.2)**.

Use [WPE WebKit](https://wpewebkit.org/), initially evaluating version **2.54.2**, with software-only rendering and unnecessary browser functionality disabled.

The implementation should preserve Kobalt's existing reader interface, functionality, performance characteristics and reading data.

### Requirements

- Target Kobo firmware 6.x exclusively.
- Use modern upstream WebKit, not QtWebKit.
- Render EPUB content without requiring GPU hardware.
- Disable WebGL, WebGPU, video, audio and other unnecessary browser features wherever technically possible.
- Retain modern HTML, CSS, SVG, MathML, typography and JavaScriptCore functionality.
- Preserve existing Kobalt reading features, including pagination, positions, annotations and search.
- Keep the existing CREngine implementation operational until WebKit is validated.
- Prioritize upstream-compatible changes and manageable long-term maintenance.

### Out of scope

- Firmware 3.x/4.x compatibility.
- DRM-protected EPUB support.
- Multimedia playback.
- Browser functionality unrelated to EPUB reading.
- Save-state or session serialization beyond normal Kobalt reading-state persistence.
- Replacing the Qt interface or Kobo display infrastructure.

## 2. Development environment

Development and testing will use:

| Component | Environment |
|---|---|
| Host | Windows |
| Linux | WSL2, Ubuntu 24.04 |
| Container runtime | Docker or Podman |
| Architecture | ARM32, hard-float |
| Firmware | Kobo 6.x |
| Application SDK | [kobuild](https://github.com/nicoverbruggen/kobuild) Qt6 |
| WebKit toolchain candidate | [koxtoolchain 2026.08](https://github.com/koreader/koxtoolchain/releases/tag/2026.08) `kobov5` |
| Initial runtime testing | VirtualNickel |
| Hardware testing | Later milestone |

VirtualNickel is already installed and operational in WSL2. It runs actual Kobo ARM32 firmware through QEMU.

The agent will have local checkouts of Kobalt and VirtualNickel. Do not require access to VirtualNickel's private GitHub repository.

Do not reinstall or replace existing development infrastructure without establishing that it is necessary.

## 3. Engineering rules

1. Read Kobalt's `AGENTS.md`, `doc/building.md` and relevant validation documentation before starting implementation.
2. Follow Kobalt's existing contribution, formatting, architecture, dependency and testing conventions.
3. Use CMake and the existing build infrastructure. Do not introduce an independent application build framework.
4. Keep third-party source pins, patches and recipes in the existing dependency system, using its content-addressed cache.
5. Use the prepared `kobuild` SDK for Kobalt application builds. Do not rebuild Kobo's Qt SDK.
6. A separate modern compiler and private dependency stack may be used for the WebKit helper.
7. Prefer upstream WebKit without modification. Keep required patches separate, minimal and documented.
8. Avoid unnecessary abstractions, broad refactoring and speculative compatibility code.
9. **Never block Kobalt's main thread on rendering, IPC, disk writes or worker synchronization during reading.**
10. Preserve CREngine functionality and existing user data throughout development.
11. Reuse VirtualNickel's testing infrastructure instead of developing a competing simulator or automation system.
12. Do not modify VirtualNickel unless necessary. Keep required changes minimal and independently testable.
13. Do not silently weaken sandboxing, network restrictions or runtime security to make WebKit work.
14. Do not commit changes without explicit user review and approval.
15. Stop at each milestone's review gate. Do not proceed automatically.

All feasibility findings must distinguish verified behavior from assumptions.

## 4. Proposed architecture

The preferred architecture is a separate WebKit renderer process communicating asynchronously with Kobalt.

```text
Nickel (Kobo firmware 6.x)
  |
  +-- Kobalt Qt interface
       |
       +-- ReaderSession
            |
            +-- Document backend abstraction
                 |
                 +-- CREngine backend
                 |
                 +-- WebKit backend proxy
                      |
                      | Async IPC / shared memory
                      v
                 kobalt-webkit
                      |
                      +-- WPE WebKit
                      +-- JavaScriptCore
                      +-- CPU rasterization
                      +-- EPUB pagination
                      +-- DOM selection/geometry
                      |
                      +-- WebKit subprocesses
```

The process boundary is intended to isolate WebKit's runtime dependencies from Nickel and avoid mixing incompatible Qt, GLib or other libraries.

Keep this architecture provisional until M0 verifies its feasibility.

Use Kobalt's existing image-oriented rendering pipeline wherever practical. Prefer returning CPU-owned page images rather than embedding a native WebKit view in the Qt interface.

---

# Implementation milestones

## M0 — WebKit platform feasibility

**Goal:** Determine whether modern WPE WebKit can run and render correctly on Kobo firmware 6.x without GPU hardware.

Do not refactor Kobalt or integrate a production rendering backend during this milestone.

### M0.1 — Repository inspection

Inspect Kobalt to identify:

- `EngineDocument`, `ReaderSession` and `ReaderFrame` responsibilities.
- Threading, rendering and cache ownership.
- EPUB resource loading and publication handling.
- Pagination, positions, navigation and annotation interfaces.
- Kobo-specific reading-position and display integrations.
- Existing tests, CMake dependencies and build conventions.

Inspect the local VirtualNickel source:

- Firmware 6.x ARM32 execution under QEMU.
- ARM child-process launching and dynamic linking.
- Library search paths, sysroot and environment setup.
- Seccomp, namespaces, IPC and process-management restrictions.
- Deployment and automated test interfaces.
- Existing frame capture, runtime logging and resource instrumentation.

VirtualNickel's README indicates firmware 6.x ARM child-executable support is incomplete. Determine the precise limitation and the smallest possible remedy.

### M0.2 — Toolchain and ABI investigation

Compare:

- `kobuild` Qt6: GCC 11, glibc 2.35-based application SDK.
- `koxtoolchain` 2026.08 `kobov5`: potential modern WebKit compiler/toolchain.
- Any newer compatible cross-compiler required by WPE WebKit.

Determine:

- Compiler and C++ standard-library requirements.
- ARM ISA, FPU and calling-convention compatibility.
- Required glibc and GLIBCXX symbol versions.
- Runtime dependencies and their availability on firmware 6.x.
- Host tools needed during cross-compilation.
- Whether private libraries can be bundled safely.

Prefer retaining the firmware-provided glibc rather than introducing a replacement libc and dynamic loader.

Test simple ARM32 executables and dynamically linked dependencies before building WebKit.

### M0.3 — WPE WebKit configuration investigation

Obtain the official WPE WebKit 2.54.2 source and verify its integrity.

Inspect the release's actual CMake configuration, dependencies and rendering code.

Investigate disabling:

| Feature | Desired setting |
|---|---|
| WebGL / WebGPU | Disabled |
| GPU process | Disabled |
| GBM / DRM | Disabled |
| Vulkan / hardware acceleration | Disabled |
| Video playback | Disabled |
| Web Audio | Disabled |
| WebRTC / media streams | Disabled |
| GStreamer | Disabled if possible |
| WebXR | Disabled |
| Speech synthesis | Disabled |
| Unnecessary desktop integrations | Disabled |
| JavaScriptCore | Enabled |
| SVG / MathML / CSS features | Enabled |
| WPEPlatform headless support | Enabled |

Determine whether the current Skia compositor supports entirely CPU-based page rendering without EGL, OpenGL or Vulkan contexts.

Investigate both:

1. The public WPE WebKit snapshot APIs.
2. WPEPlatform shared-memory buffers (`WPEBufferSHM`).

Prefer existing public APIs over a custom rendering backend or modifications to WebKit's compositor.

**A build that disables WebGL is not sufficient evidence of GPU-free rendering.**

### M0.4 — Host-native proof of concept

Build a minimal WPE configuration on Ubuntu 24.04.

Create an isolated proof-of-concept executable that:

1. Initializes headless WPE WebKit.
2. Loads a local XHTML document.
3. Applies CSS and embedded fonts.
4. Executes trusted JavaScript.
5. Renders at a specified viewport size.
6. Captures the rendered pixels to an image file.
7. Shuts down cleanly, including subprocesses.

Include text, images, SVG and representative EPUB typography.

Verify that the renderer does not require an operational GPU, EGL display, Wayland server or multimedia service.

If CPU rendering fails, investigate the limitation before proceeding.

### M0.5 — ARM32 cross-build

Cross-compile the proof of concept and required runtime dependencies for Kobo firmware 6.x.

- Use the selected modern toolchain.
- Bundle required libraries privately.
- Keep the dependency graph as small as practical.
- Check ELF architecture, interpreter and symbol requirements.
- Avoid host-library contamination.
- Record build flags, patches and exact source versions.

Verify that the resulting binaries target the firmware runtime ABI.

### M0.6 — VirtualNickel execution

Run the ARM32 proof of concept inside VirtualNickel firmware 6.x.

If necessary, minimally extend VirtualNickel's ARM subprocess support, with separate tests and user review before modifying the simulator.

Verify:

- Starting the standalone renderer.
- Launching WebKit child processes under QEMU.
- Dynamic loading of private libraries.
- Unix-domain IPC and shared memory.
- Local XHTML loading and JavaScript execution.
- Correct page images from CPU rendering.
- Repeated startup and shutdown.
- Failure handling and subprocess cleanup.

Do not weaken VirtualNickel's external-network restrictions.

A successful run under a generic ARM Linux image does not replace testing inside the firmware environment.

### M0.7 — Resource evaluation

Measure:

- Total memory use across all renderer processes.
- Executable and bundled dependency sizes.
- Startup and first-render latency.
- Subsequent render latency.
- Idle CPU consumption.
- Temporary memory allocations and caches.
- Behavior under VirtualNickel's memory and CPU limits.

Clearly identify results affected by QEMU overhead or simulator limitations.

Do not infer physical e-ink page-turn timing or battery consumption from these measurements.

### M0 deliverables

Create `planning/webkit/` with:

- `feasibility.md` — conclusions, verified results and blockers.
- `toolchain.md` — selected toolchain and ABI compatibility.
- `dependencies.md` — pinned dependencies, patches and licenses.
- `rendering.md` — software-rendering investigation and proof.
- `virtualnickel.md` — simulator execution, limitations and required changes.
- `architecture.md` — proposed Kobalt integration architecture.
- `build.md` — reproducible build instructions.

Retain prototype source, configuration logs, diagnostics and rendered test images outside production Kobalt code.

### M0 review gate

Present one of these conclusions:

- **Viable:** WebKit builds and renders correctly under simulated firmware 6.x.
- **Conditionally viable:** Clearly bounded changes are required.
- **Blocked:** The rendering or runtime requirements are impractical.

If blocked, evaluate whether another maintained WPE release or a small upstream-compatible change resolves the problem.

Do not embark on a major WebKit fork merely to pass M0.

**Stop for user review before M1 or any commit.**

---

## M1 — Reproducible WebKit build and runtime packaging

**Goal:** Turn the proof of concept into a maintainable build and deployment component.

Tasks:

- Integrate pinned WebKit sources and dependencies with Kobalt's existing dependency infrastructure.
- Create reproducible ARM32 builds.
- Package the renderer executable and private libraries.
- Establish process startup, environment isolation and shutdown.
- Implement minimal asynchronous IPC and error reporting.
- Support renderer crashes without crashing Nickel.
- Preserve WebKit's required subprocess model.
- Audit licensing and source-distribution obligations.

Avoid changing Kobalt's GUI or reader behavior.

**Deliverable:** A reproducibly built, independently deployable WebKit renderer that operates inside VirtualNickel.

**Review gate:** Build, installation, execution and cleanup verified.

## M2 — EPUB rendering backend

**Goal:** Implement the essential EPUB rendering functionality independently of Kobalt's GUI.

Tasks:

- Reuse Kobalt's publication/resource infrastructure where appropriate.
- Load EPUB spine documents securely.
- Implement CSS pagination and chapter transitions.
- Support page navigation and viewport changes.
- Apply reading appearance, fonts, spacing and margins.
- Render pages into CPU-owned pixel buffers.
- Provide logical reading locations and page geometry.
- Support internal links and basic EPUB navigation.
- Introduce caching and pre-rendering only where justified.

Investigate EPUB.js as a reference for pagination and EPUB CFI, but avoid unnecessary duplication of native EPUB handling.

Disable arbitrary book-supplied JavaScript by default. Allow only the trusted scripts required for the renderer's DOM bridge, with appropriate content isolation.

**Deliverable:** Standalone EPUB renderer supporting reliable pagination, page capture and navigation.

**Review gate:** Representative EPUBs render correctly under VirtualNickel.

## M3 — Kobalt backend abstraction and integration

**Goal:** Integrate WebKit without disrupting CREngine or the existing Qt interface.

Tasks:

- Extract the minimum backend-independent document interface.
- Adapt CREngine to that interface without changing its behavior.
- Implement a WebKit backend proxy.
- Preserve `ReaderSession` and immutable `ReaderFrame` behavior.
- Translate WebKit page buffers into the existing image pipeline.
- Handle asynchronous rendering, cancellation and stale results.
- Implement backend selection with CREngine as fallback.
- Integrate renderer lifecycle with opening, closing, sleep and shutdown.

Honor Kobalt's main-thread blocking rules.

Use versioned IPC messages and shared-memory image transfer where appropriate. Avoid generic RPC frameworks unless required by demonstrated complexity.

**Deliverable:** Kobalt can open and navigate EPUBs using either backend.

**Review gate:** Existing CREngine tests pass; WebKit operates through Kobalt in VirtualNickel.

## M4 — Reading features and data compatibility

**Goal:** Bring WebKit to functional parity with Kobalt's essential reader features.

Implement:

- Persistent reading positions.
- Table of contents and chapter navigation.
- Text selection and hit-testing.
- Bookmarks, highlights and annotations.
- Annotation rectangles and selection handles.
- Search and text extraction.
- Reading statistics and progress.
- Page previews and cache invalidation.
- Advanced typography and writing directions.
- Renderer failure recovery.

Define a backend-independent, versioned source-position representation. Investigate EPUB CFI with supplementary text assertions.

Preserve existing CREngine XPointers and Kobo-native reading records. Develop and test conversion where necessary.

Never destructively migrate existing records without a validated fallback.

**Deliverable:** WebKit supports Kobalt's essential reader features and reliably restores saved state.

**Review gate:** Feature parity demonstrated with automated tests. See the
[M4 verification and limits](m4-parity.md).

## M5 — Automated regression testing

**Goal:** Establish reliable testing through VirtualNickel and Kobalt's existing test infrastructure.

Extend existing tests rather than introducing a new test framework.

Test:

- Book opening and closing.
- Pagination and page turning.
- Font, margin and orientation changes.
- Chapter navigation and links.
- Selection and annotation geometry.
- Reading-position persistence.
- Search behavior.
- Complex CSS, SVG, MathML, RTL and CJK.
- Large EPUBs and long chapters.
- Renderer crashes, hangs and recovery.
- Repeated process startup/shutdown.
- Memory pressure and resource cleanup.
- Suspend/resume behavior.

Use VirtualNickel's Python device API, controlled input and framebuffer capture.

Prefer assertions against document state, source positions and rendering geometry. Use pixel comparisons for visual regressions.

Keep fixtures, test settings and display dimensions deterministic.

**Deliverable:** Automated regression coverage for WebKit and unchanged CREngine behavior.

**Review gate:** Relevant existing and new test suites pass.

## M6 — Performance and physical Kobo validation

**Goal:** Determine whether the implementation is suitable for actual firmware 6.x devices.

Tasks:

- Test on physical Kobo hardware when available.
- Measure total runtime memory and idle consumption.
- Measure startup, pagination and page-turn latency.
- Validate e-ink image quality and refresh behavior.
- Investigate pre-rendering and caching effectiveness.
- Verify sleep/wake and battery-related behavior.
- Test low-memory conditions and renderer recovery.
- Compare WebKit with CREngine for typography, compatibility and performance.

Optimize only after obtaining measurements.

Do not remove CREngine or make WebKit the default until the user approves the measured results.

**Deliverable:** Hardware validation report, remaining issues and recommendation for production readiness.

**Review gate:** User decides whether WebKit should become the default renderer.

---

## Milestone workflow

For every milestone:

1. Inspect the relevant existing implementation and documentation.
2. Develop a minimal approach consistent with Kobalt's architecture.
3. Implement only the approved milestone scope.
4. Run applicable unit, integration and VirtualNickel tests.
5. Review changes for unnecessary complexity and duplicated infrastructure.
6. Produce a concise summary of changes, verification and unresolved issues.
7. Present the diff and results for user review.
8. **Do not commit until explicitly approved.**
9. Do not begin the next milestone until authorized.

Record which tests were run and which could not be run. Do not report simulated results as physical-device validation.

## Immediate instructions to the agent

**Begin with M0 only.**

Inspect the locally available Kobalt and VirtualNickel repositories, investigate the real WPE WebKit 2.54.2 rendering implementation and its build options, and establish a minimal CPU-only rendering proof of concept.

Evaluate `koxtoolchain`'s `kobov5` toolchain for WebKit and retain `kobuild` Qt6 for Kobalt.

Use the existing Ubuntu 24.04 WSL2 environment and VirtualNickel installation.

Do not modify production Kobalt code or perform substantial VirtualNickel changes during initial feasibility work.

Document the findings and stop for review.

**The central question for M0 is whether modern WPE WebKit can produce correct CPU-rendered EPUB pages within the actual Kobo firmware 6.x ARM32 runtime, with an acceptable dependency and resource footprint.**

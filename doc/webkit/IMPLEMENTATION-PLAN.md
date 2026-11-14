# Kobalt WPE WebKit engine — agent implementation plan

**Status:** New implementation project; planning only. **Date:** 2026-11-14.  
**Primary implementation environment:** VirtualNickel running Kobo Firmware 4.x.  
**Hardware reference:** Kobo Forma (N782), Firmware 4.38.23852.  
**Execution model:** **One agent at a time; no orchestrator, delegation, or subagents.** Start with **Sol Medium** for most work. Use **Terra Medium** for well-scoped checks, fixtures, documentation and mechanical edits; **Sol High** for genuinely difficult design, toolchain, WebKit or integration problems. **Sol xHigh** is an exceptional, user-selected escalation only, never the default. All model labels below are recommendations for the single active agent, not instructions to spawn another agent.

## 0. Terminology and authority

This is a **standalone plan**. The milestones of *this* project are always named **WPE-M0** through **WPE-M8**. The earlier experimental branch [`feature/webkit-wpe-m4`](https://github.com/shermp/kobalt/tree/feature/webkit-wpe-m4) is called the **PoC**; **PoC-M4** always means work on that historical branch, **never WPE-M4**. Likewise, **Blaze Stage A3/A4/etc.** refers to upstream's engine migration, not any WPE milestone. Do not import old milestone requirements by number.

**Sources of truth, in order:** (1) the user's decisions in this plan; (2) `AGENTS.md`, current platform build/validation rules and code in the selected Kobalt checkout; (3) the actual WPE/WebKit source and public APIs at the pinned revision; (4) the current upstream engine interface and documentation, which are **not stable**; (5) the PoC for selective implementation/test references only. Confirm exact source revisions before work. Do not assume that a passing PoC test proves this implementation works.

Reference points observed on 2026-11-14 (refresh before use): [`shermp/kobalt` `develop`](https://github.com/shermp/kobalt/tree/develop) `f0befcaffd74910ce1dd472ad5ab9251c38b2bb2`; [`feature/blaze-engine`](https://github.com/shermp/kobalt/tree/feature/blaze-engine) `a4bb4d1cf86acaa404064dca184a0e6708a43b00`. These are **reference SHAs, not checkout locks**. WPE WebKit **2.54.2** is the initial candidate, subject to a documented version/ABI feasibility gate.

## 1. Non-negotiable decisions

1. **Explicit WPE WebKit snapshots, not compositor callbacks.** Use the headless WPEPlatform, CPU/Skia path when supported, `webkit_web_view_get_snapshot(WEBKIT_SNAPSHOT_REGION_VISIBLE, ...)` and the resulting `WebKitImage` pixel buffer. No direct `buffer-rendered` subscriptions, compositor-callback barriers, PNG frame transport or GPU/EGL/display-server dependency without evidence and approval. Snapshot completion alone does **not** prove the pixels correspond to the requested page.
2. **A standalone native renderer process** (and the WebKit subprocesses it requires), separate from Nickel and Qt; GLib/GDBus on the service side; Qt5/Qt6 client uses **asynchronous** peer-to-peer D-Bus. Successful visual requests return the completed state and an **owned immutable frame FD together**. The Qt client validates dimensions/stride/format and keeps mapping ownership safe when the renderer exits.
3. **Primary development and regression in VirtualNickel FW4**, ideally with a userspace matching the Forma. Native Linux is useful for fast independent tests. **The Forma is a separate early ABI/kernel gate and later real hardware, e-ink, memory and performance gate**, not the everyday test runner. If VirtualNickel runs a different ABI, record exactly what it does and do not claim binary equivalence.
4. **Prefer static binaries and static dependencies**, including the renderer/supervisor and helper executables where practical. A WebKit multiprocess runtime does not imply a single executable. Investigate the full transitive ELF and `dlopen` closure, compare size and combined memory costs, and request review before accepting a minimally shared private runtime. Do not force a fully static glibc/WebKit build if demonstrably incompatible or substantially worse.
5. **All substantial deployable files live under** `/mnt/onboard/.adds/kobalt-webkit/` (VFAT). The Forma root filesystem has about **42.7 MB free**; do not stage/install WPE, system libraries or large logs there. Verify `exec` mount options; do not use symlinks, POSIX ownership/modes, Unix sockets or locks on VFAT. Use a verified POSIX-capable temporary directory for sockets and private run state, checking free space and RAM.
6. **Keep the existing koblaze/CREngine reader working and default.** No new general engine framework, no wholesale branch merge, no interim `Engine` implementation filled with dummy CREngine operations. Upstream's `feature/blaze-engine` API is changing: independent renderer/transport work proceeds without importing it. Host integration is gated on an actually suitable interface.
7. **Preserve source-based reading positions** and lazy pagination. The canonical future host position is upstream `kobalt::Location`; WebKit-native positions are internal. **PoC-M4** `SourceV1` tokens (`kobalt-source:1:`) are legacy import fixtures, not a competing canonical saved format. Preserve CREngine XPointers and other user data.
8. No book-controlled JavaScript, unrestricted network access or untrusted local-resource paths in the privileged bridge. Bind access to the authorized publication, enforce limits and document the absence of an OS sandbox if the kernel cannot support it. Do not alter Nickel startup or global runtime environment.

### Boundaries

```text
Kobalt ReaderSession / future upstream engine contract (WPE-M5 only)
                  |
   thin Kobalt-specific adapter (Qt5 / Qt6 compatible)
                  |
   independent QtCore + QtDBus transport client
                  | async peer D-Bus + Unix FD
   standalone GLib/GDBus WPE renderer supervisor
                  |
   persistent active-spine DOM / CSS columns
   explicit WebKit snapshot -> immutable FD-backed pixels
                  |
           WebKit helper processes
```

Renderer sources and tooling belong under `platforms/webkit/`; pinned third-party sources/patches/recipes in `dependencies/`, using Kobalt's existing content-addressed dependency-cache policy. Do **not** compile or patch Qt within an application build. Keep native renderer C++23 independent of Qt and Nickel headers; Qt5 integration must respect its older toolchain/C++ library. The host controls saved data, session generation, user interface, e-ink refresh and engine selection.

## 2. Agent operating procedure

- **Start only WPE-M0.** Before each new milestone, re-read the plan, check repository state, current `AGENTS.md`, `doc/building.md`, `doc/validation.md`, `doc/storage.md` where relevant, and record both the selected base SHA and the latest Blaze SHA. Create/use a dedicated branch and worktree following actual `AGENTS.md`; suggested `feature/webkit-snapshot-engine` in `worktrees/webkit-snapshot-engine`. Do not edit the shared `develop` checkout. Do not overwrite an existing worktree.
- **One agent, one reviewed milestone at a time.** The active agent executes the listed tasks sequentially itself. **Do not use an orchestrator, spawn subagents, hand off tasks to another agent, or run parallel agent sessions.** Model names in the task table are *suggested settings for that same agent*; changing effort/model is a user choice, not an automatic escalation. Avoid unnecessary abstraction, unrelated refactors and a parallel build framework.
- **Cost-conscious model choice:** Default to **Sol Medium**; use **Terra Medium** for routine, well-defined tests and mechanical changes where appropriate. Use **Sol High** only for a concrete difficult problem that merits deeper reasoning. Propose **Sol xHigh** to the user only if Sol High is insufficient and the expected benefit justifies its cost. Do not repeatedly reread the whole codebase, rerun verified builds, or request unnecessary model changes.
- **Never stage, commit, push, merge, deploy to the Forma, or advance to the next milestone without explicit user approval** for that action. Reading the Forma's configuration is allowed when the operator provides access, but do not change device configuration or other `.adds` applications. Before physical deployment, present exact payload and commands for review. Do not store the device password, credentials or absolute host details in source/plans/logs.
- Every milestone ends with: changed files; reproducible commands and results; source/build/firmware identities; objective pass/fail evidence; dependencies and binary closure; known risks; **unverified/blocked** items; next proposal; and an explicit `staged=no, committed=no, pushed=no, next milestone started=no`. Retain failure evidence; clean only run-owned processes and scratch, using repository-prescribed cleanup.
- Follow Kobalt's existing source checks, main-thread/blocking rules, dependency-cache rules, Qt5/Qt6 syntax/compile checks, Pewter tests, and validation guard. Do not change source-check allowlists without user approval. Reuse previously valid test results where permitted; do not rerun expensive suites without cause.

¹ **Suggested model** is guidance for selecting the *one* active agent's model/effort for a task. It is not an instruction to launch, delegate to or switch to a second agent. A milestone may be completed entirely with Sol Medium when adequate; seek approval before higher-effort escalation.

## 3. Work packages and acceptance gates

### WPE-M0 — Establish FW4 development and architecture constraints

**Goal:** Reproducible VirtualNickel setup and an evidence-based target/runtime design; no WPE integration yet. **Start with Sol Medium.**

| Task | Suggested model¹ | Work / deliverable |
|---|---|---|
| M0.1 | Sol Medium | Audit local worktree, `develop`, Kobalt build rules, PoC-M4 code/fixtures, and existing VirtualNickel capabilities. Record exact revisions and only the reusable pieces. Set up a reproducible FW4 simulator profile and minimal run/test command; prefer Forma FW 4.38.23852 userspace when possible. |
| M0.2 | Terra Medium | Read-only inventory of VirtualNickel and Forma: CPU/hard-float ABI, ELF interpreter, glibc/libstdc++, kernel, `/proc/meminfo`, mounts, executable paths, process/shm/socket capabilities. Compare what the simulator can and cannot verify. |
| M0.3 | Sol High | Audit WPE 2.54.2 headless snapshot compile/runtime prerequisites for FW4. List renderer, WebProcess, NetworkProcess, GLib/GIO/JSC/Skia, plugins and resource dependencies. Evaluate static archives and the smallest private `.so` fallback without speculative large ports. |
| M0.4 | Sol Medium | Read *implemented* `src/engine/engine.h`, `src/engine/query.h`, `src/publication/location.h`, `ReaderSession` and *proposed* `doc/engines/contract.md` on Blaze. Produce a seam matrix: implemented, proposed, missing, required for WPE-M5. No host-reader code. |

**Gate:** A repeatable simulator command; target triple/ABI/loader report; a static-first build decision with explicitly unverified points; a host-interface seam matrix. **Stop if VirtualNickel cannot execute suitable FW4 binaries; propose a verified alternative or simulator adaptation for approval.**

### WPE-M1 — Build and validate the independent snapshot runtime

| Task | Suggested model¹ | Work / deliverable |
|---|---|---|
| M1.1 | Sol High | Add pinned reproducible WPE/GLib/JSC/Skia build recipes using approved toolchain. Build the most-static viable renderer and required subprocesses for FW4. Verify exports for the **2022 GLib snapshot API**; do not assume an arbitrary build toggle works. |
| M1.2 | Sol Medium | Implement a minimal headless HTML fixture runner with GLib event loop and explicit visible snapshot; copy raw pixel data into an owned frame; output deterministic hash and fixed pixel assertions plus dimensions, stride and format. No GTK, Cog, FDO or custom compositor backend. |
| M1.3 | Terra Medium | Validate red/white/blue or distinctive A→B→A pages; initial snapshot, resize, repeated navigation, delayed mutation and cancellation. Pixel correctness must be observed after the requested DOM/layout revision; do not use arbitrary waits or `buffer-rendered` callbacks to claim freshness. Verify clean subprocess teardown. |
| M1.4 | Sol High | Audit every ELF interpreter, SONAME, `NEEDED`, `dlopen` plugin, resource directory, process helper and static archive. Compare deployment size and measured process memory against a minimally dynamic alternative if static is problematic. Produce a short fallback decision for user review if needed. |

**Gate:** Real WebKit snapshots with verified changing pixels, not merely a successful link/blank image. Fully recorded runtime closure and simulator-run tests; no unreviewed shared-library compromise.

### WPE-M2 — Early physical Forma kernel/ABI gate

| Task | Suggested model¹ | Work / deliverable |
|---|---|---|
| M2.1 | Terra Medium | Prepare a reproducible isolated install manifest under `/mnt/onboard/.adds/kobalt-webkit/{bin,libexec,lib,share,tests,logs}`. Test VFAT execution, real-file layout (no symlink assumptions), explicit private runtime paths, POSIX `/tmp`/`run` socket location and root-space preservation. Present deployment instructions for approval. |
| M2.2 | Sol Medium | **After explicit device-deployment approval**, run exactly the WPE-M1 snapshot fixture binaries on the Forma. Check loader/kernel compatibility, subprocess creation, snapshot A→B→A pixels, FD/memory path, clean exit, logs and install/uninstall scope. Do not alter Nickel or other applications. |
| M2.3 | Sol High (only if blocked) | Isolate ABI/kernel/runtime incompatibilities, distinguish missing loader or glibc symbols from actual syscalls, memory and graphics issues, and propose the smallest verified fix rather than an unbounded port. |

**Gate:** The actual Forma can execute and render snapshots from a matching FW4 build. Record target/simulator parity and resource baseline. **This is a hard gate before WPE-M3 and WPE-M4**, unless the user explicitly approves a documented exception.

### WPE-M3 — Async D-Bus transport and owned frame handoff

| Task | Suggested model¹ | Work / deliverable |
|---|---|---|
| M3.1 | Sol High | Finalize a small, versioned, **independent** peer D-Bus contract: `Hello`, `Open`, `Turn`, `Navigate`, `SetViewport`, `SetAppearance`, `Query`, `Close`, `Cancel`, `Shutdown`; optional indexing progress. Each successful visual reply contains `(document/request/layout identity, state, frame FD, width, height, stride, format)` **atomically**. Errors do not publish stale state. |
| M3.2 | Sol Medium | Implement service calls with GLib/GDBus, foreground serialization and `GCancellable`, explicit document/layout/request generations and stale-result rejection. The GLib event loop must stay responsive to cancellation and shutdown. |
| M3.3 | Sol Medium | Implement standalone QtCore/QtDBus/QtGui client in the prepared **FW4 Qt5** and **FW5/6 Qt6** SDKs. Prove actual Qt5 FD-passing API support in compile/link/run tests. Use asynchronous requests; validate FD list indexing, stride, pixel byte order, bounds/overflow and mapping lifetime before exposing an immutable `QImage`. |
| M3.4 | Terra Medium | Test duplicate/late replies, A→B→A, cancellation, disconnect/restart, renderer death while a frame is mapped, invalid FDs/metadata, memory/FD leaks and cleanup. No GUI-thread wait, nested event loop or synchronous wait disguised as async. |

**Gate:** A simulator-driven Qt5 client receives correct, immutable snapshots over D-Bus; matching Qt6 build validated; an older reply cannot replace a newer frame. No Kobalt `Engine` headers are used yet.

### WPE-M4 — EPUB, retained pagination and source-location proof

**This is WPE-M4, not PoC-M4.** PoC-M4 is only an experimental code and fixture reference.

| Task | Suggested model¹ | Work / deliverable |
|---|---|---|
| M4.1 | Sol Medium | Add bounded EPUB ZIP/OPF/spine/resource loading in the standalone runtime, reusing vetted PoC-M4 utilities where appropriate. Validate publication identity, encrypted/obfuscated resources, path normalization, MIME/size limits, local resource policy and no remote-book access. Avoid duplicating a general *Kobalt host* publication framework. |
| M4.2 | Sol High | Design the smallest persistent **active-spine** WebKit layout: CSS columns/viewport offset for turns, layout-only page counts, lazy spine loading, optional explicit whole-book page indexing without rendering each page. Define exact invalidation on font/viewport/content changes and location restoration. |
| M4.3 | Sol Medium | Implement turns, chapter transitions, CSS appearance and viewport changes with explicit fresh snapshots; visible page first; cancellable/limited background indexing. Prove no eager whole-book scan on normal open/turn and no per-page snapshot during counting. |
| M4.4 | Sol High | Implement/test WebKit DOM ↔ **Blaze `kobalt::Location`** semantics: href; element-child indexes from body; Unicode **code-point** offsets excluding upstream `locationUncounted`; `after` affinity; exact/approximate/unresolved. Test surrogate pairs, combining marks, whitespace, inline elements, parser normalization, malformed XHTML and reflow. |
| M4.5 | Terra Medium | Import **PoC-M4 SourceV1** tokens only as compatibility test fixtures; test UTF-16-to-code-point conversion and failure quality. Preserve unknown/unresolved tokens; do not save host profile data. |

**Gate:** EPUB first-page/turn/restore and lazy pagination in VirtualNickel; deterministic cross-layout source-position fixtures and explicit precision results. Keep host persistence and upstream integration untouched.

### WPE-M5 — Upstream engine adapter integration (conditional gate)

**Entry condition:** the selected Kobalt integration base provides an **approved** engine-neutral path for async frames, source positions, unknown/estimated page totals, cancellation and registration. The existence of `Engine` alone does not satisfy the gate. Upstream may be merged or supplied as a separately approved branch; never merge `feature/blaze-engine` automatically.

| Task | Suggested model¹ | Work / deliverable |
|---|---|---|
| M5.1 | Sol High | Re-read current `feature/blaze-engine` implementation/docs and selected host base. Verify actual `ReaderSession` scheduling, async frame ownership, `Query`/`PageInfo` consumption, `Location` persistence, capabilities, worker/renderer lifecycle, factory and test seams. Identify gaps with concrete tests. |
| M5.2 | Sol High | If gaps remain, stop and propose the **smallest upstream interface changes**, with sample call flow and focused tests, for user review. **Do not** stub the CREngine-heavy interface, block the worker on D-Bus, or invent a second scheduler. |
| M5.3 | Sol Medium | Once gate is met and integration authorized, add a thin `src/engine/webkit` adapter consuming the tested transport and canonical location; keep WebKit/IPC implementation outside `src/reading`, storage and UI. Register as optional engine; koblaze remains default. |
| M5.4 | Terra Medium | Run fake-engine/contract tests (when available), shared native suites, Kobo Qt5/Qt6 compile/link and Pewter checks; validate no main-thread blocking and no default-engine regressions. |

**Gate:** WPE is selectable through the **actual approved upstream seam**, with async state/pixels and host cancellation. If API is not ready, report **WPE-M5 blocked** and keep completed WPE-M0–M4 independently usable.

### WPE-M6 — Persistence, navigation and interoperability

| Task | Suggested model¹ | Work / deliverable |
|---|---|---|
| M6.1 | Sol High | Map upstream storage, navigation and position codecs. Specify non-destructive migration for canonical `Location`, CREngine XPointer/`dom_version`, platform Nickel positions and optional PoC-M4 SourceV1 legacy tokens. Avoid overwriting unrecognized or approximate locations. |
| M6.2 | Sol Medium | Implement bookmark, TOC, return-link, chapter navigation, source-location save/restore and any approved schema hooks. Keep renderer free of host profile writes. |
| M6.3 | Terra Medium | Test CREngine-only, WPE-only, mixed/legacy and corrupted fixture profiles; cold reopen, engine switch, idempotent migration, fingerprint mismatch, reflow, KOSync/Nickel representations and save rollback. |

**Gate:** No data loss or cross-engine position corruption; unresolved legacy locations stay recoverable; all writes use approved host APIs.

### WPE-M7 — Reader features, robustness and secondary firmware

| Task | Suggested model¹ | Work / deliverable |
|---|---|---|
| M7.1 | Sol Medium | Implement features in small vertical slices: visible-page links and notes, search batches, token selection, highlights/range geometry, previews, appearance controls and navigation; each slice gets targeted fixtures. Optional features report unsupported rather than fake success. |
| M7.2 | Sol Medium | Test rapid turn/appearance/close/open, cancellation, renderer crash and restart, reflow, low memory and frame retention. Reuse last **confirmed** image/location until a new valid reply. |
| M7.3 | Sol Medium | Review upstream API drift and firmware 5/6 compatibility without broad redesign. Assess which optional features merit deferral by actual functionality and memory cost. |

**Gate:** Essential reader actions and recovery work in FW4 VirtualNickel, with an explicit capability/deferral matrix and no loss of existing koblaze behavior.

### WPE-M8 — Physical optimization, release and maintainability

| Task | Suggested model¹ | Work / deliverable |
|---|---|---|
| M8.1 | Sol Medium | Measure **on the Forma**: combined renderer/helper PSS/RSS/high-water, cold/warm first frame, warm turn, chapter turn, font reflow, CPU, FD counts, repeated openings, disk footprint and `/` free space. Distinguish e-ink refresh from renderer latency and simulator timing. |
| M8.2 | Sol High | Compare static vs private-dynamic builds, idle/peak memory, startup and reproducibility. Decide release configuration with user review; document unavoidable `.so` modules, loader paths, bundled licenses and security limits. |
| M8.3 | Terra Medium | Finalize safe VFAT package install/update/uninstall, bounded logs, process recovery, FW4 validation and FW5/6 compatibility tests. Complete repository-prescribed release checks. Do not silently change default engine or Nickel startup. |

**Gate:** Reviewed real-device compatibility and resource measurements, repeatable reproducible build and install, no system-partition pressure, known limitations documented and user approval before any release/merge.

## 4. Cross-milestone verification contract

**Pixel correctness:** Initial frame, A→B→A, distinct colored/text regions, resize, rapid supersession and font/viewport reflow. Assert actual pixels/hashes and exact request/layout attribution; never accept a white/stale image because the snapshot callback completed.

**Memory/frame lifetime:** Avoid PNG, intermediate frame files and unnecessary copies. Copy `WebKitImage` pixels into one owned immutable FD-backed frame; validate premultiplied BGRA/architecture order, row stride and bounds. Use sealing where supported; otherwise prove immutability by ownership and access restrictions. Consumer memory mappings remain valid after renderer death. Measure all processes, not only the supervisor.

**Pagination/location:** Normal page turn translates within a retained layout; unknown spine counts stay unknown/estimated rather than forcing full layouts. Source positions survive changes of font/viewport. Compare WebKit vs upstream `Location` precisely; do not conflate JavaScript UTF-16 offsets with code points. Page numbers and percent are **not** persistent locations.

**Device and security:** Execute the same target binaries on VirtualNickel and Forma **only when the simulator truly supports the target ABI**. Verify the runtime/link closure for subprocesses, not just the main ELF. Store substantial files only under `.adds`, use a POSIX run directory and enforce isolated runtime paths. Prevent EPUB archive traversal, untrusted network and user scripts in privileged contexts; do not equate a URL allowlist with a kernel sandbox.

**Repository tests:** Before host integration, keep the normal koblaze app untouched. After host edits, use the current required source checks, `platforms/kobo/syntax.cmake`, Qt5 and Qt6 prepared SDK checks, relevant `ctest` labels, Pewter, storage migration and release-evaluation scripts. Do not change expected output to manufacture passes.

## 5. Milestone handoff format (required)

```text
Milestone: WPE-M# — <name>
Single agent; model/effort actually used (and any user-approved changes): ...
Source base SHA: ...    Blaze reference SHA: ...
Simulator FW/profile and ABI: ...
Forma FW/ABI/kernel (if used): ...
Changed files: ...
Reproduction commands/tests and observed results: ...
Static archives/private .so/ELF closure and deploy bytes: ...
Frames: pixel assertions, revisions, FD lifetime: ...
Memory/time metrics: ... (label simulated vs physical)
Verified: ...
Unverified/blocked: ...
Risks/decisions needing owner review: ...
Staged: NO; committed: NO; pushed: NO; next milestone started: NO
```

## 6. Starting instruction for the agent

> Execute **WPE-M0 only** from this plan on a dedicated worktree based on the current Kobalt `develop` checkout. First inspect the actual repository instructions and the existing VirtualNickel environment, then gather the FW4 ABI/kernel and static-link feasibility evidence and assess the evolving Blaze interface as a read-only reference. Use **Sol Medium** as the single active agent; proceed sequentially through all M0 tasks without delegation or spawning another agent. Use **Terra Medium** only if the user deliberately selects it for a well-scoped task. If a concrete WPE dependency or upstream seam question proves too difficult, report the blocker and recommend **Sol High** to the user rather than escalating automatically. Do not implement WPE-M1, change host reader code, deploy onto the Forma, stage or commit. Return the WPE-M0 handoff report and stop for review.

## 7. Reference sources (not normative prior plans)

- Kobalt: [`develop`](https://github.com/shermp/kobalt/tree/develop), [`AGENTS.md`](https://github.com/shermp/kobalt/blob/develop/AGENTS.md), [`doc/building.md`](https://github.com/shermp/kobalt/blob/develop/doc/building.md).
- Evolving upstream implementation: [`feature/blaze-engine`](https://github.com/shermp/kobalt/tree/feature/blaze-engine), [`engine.h`](https://github.com/shermp/kobalt/blob/feature/blaze-engine/src/engine/engine.h), [`query.h`](https://github.com/shermp/kobalt/blob/feature/blaze-engine/src/engine/query.h), [`location.h`](https://github.com/shermp/kobalt/blob/feature/blaze-engine/src/publication/location.h), [`contract.md`](https://github.com/shermp/kobalt/blob/feature/blaze-engine/doc/engines/contract.md), [`migration.md`](https://github.com/shermp/kobalt/blob/feature/blaze-engine/doc/engines/migration.md).
- Historical implementation/test reference, **not** a design baseline: [`PoC-M4 / feature/webkit-wpe-m4`](https://github.com/shermp/kobalt/tree/feature/webkit-wpe-m4).
- WPE 2.54.2 source: [`WebKitWebView.cpp` snapshot implementation](https://github.com/WebKit/WebKit/blob/wpewebkit-2.54.2/Source/WebKit/UIProcess/API/glib/WebKitWebView.cpp), [`WebKitImage.cpp` pixel interface](https://github.com/WebKit/WebKit/blob/wpewebkit-2.54.2/Source/WebKit/UIProcess/API/glib/WebKitImage.cpp).

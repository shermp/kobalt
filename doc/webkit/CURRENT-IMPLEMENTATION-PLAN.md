# Kobalt WPE WebKit — single-agent implementation plan

**Design status:** proposed replacement for the post-WPE-M3.3 transport and integration work; not authorization to change code, stage, commit, push, merge or deploy.  
**Primary:** Kobo firmware 4.x under VirtualNickel, Qt 5.2.1; targeted physical validation on the Kobo Forma. **Secondary:** Kobo Qt 6.5.2. Older kernels remain a compatibility goal, subject to physical/runtime probes.  
**Agent:** one GPT-6 Sol Low or Medium agent, one slice at a time. A new chat may continue the same plan using the preceding handoff; do not run parallel agents against the same worktree.

## 1. Starting point and authority

Continue **`feature/webkit-snapshot-engine`**, not the obsolete direct-compositor PoC or an M0 restart. At the 15 November 2026 review, the observed WPE branch head was `2e5f3c835116a4d583c55837d7cdb1b91812adc7`; reference branches were `develop` at `71fc998b17c053f6720f88fc2039c23933179634` and `feature/blaze-engine` at `52c522ae23d96df22a993e96406abacb87a3133d`. **Recheck live branches, worktree state and uncommitted files at the start of each slice**; these SHAs are observations, not locks.

Read only the material relevant to the slice: `AGENTS.md`, `doc/building.md`, the applicable files in `doc/validation/`, `doc/webkit/WPE-M3.1.md` through `WPE-M3.3a.md`, and changed source. `doc/webkit/IMPLEMENTATION-PLAN.md` and the two prior attached plans are historical references; this document supersedes their remaining D-Bus/immutable-frame transport decisions. Follow repository source checks, pinned SDKs, worktree/dependency-cache rules, main-thread policy, and source ownership. Never patch or rebuild Kobo Qt as a shortcut.

**Evidence already obtained:** the ARM32 WPE 2.54.2 headless snapshot runtime ran on FW4 and the Forma with Bubblewrap disabled; `Renderer1`/Qt6 FD transport worked under emulation; Qt5 D-Bus visual FD decoding failed; M3.3a's named-tmpfs frames worked under Qt5/Qt6 emulation but Qt5 crashed when the renderer was killed. The Forma's POSIX `shm_open`/`mmap`/`shm_unlink` probe passed despite no visible `/dev/shm`. **Not proved:** the new IPC, physical Qt5 frame transport, retained EPUB DOM, text/font rendering, whole-reader integration, long-run memory or older-kernel compatibility.

## 2. Fixed architecture

```text
Kobalt / Qt 5.2.1 or Qt 6.5.2
  ReaderSession / optional Blaze integration / UI / saved data
  Existing Publication, ZIP, OPF/navigation and authorized resources
  WebKitClient (QtCore + QtNetwork + QtGui)
    QProcess: launches and observes immediate renderer child
    QLocalSocket: owns parent half of connected AF_UNIX SOCK_STREAM socketpair
    FramePool: owns two shm_open() buffers, read-only maps and QImage leases
        |                  |
        | framed control   | shm descriptors inherited at exec, once per launch
        v                  v
WPE renderer (standalone C++23 + GLib/GIO, no Qt)
  GSocket: owns child half of socketpair
  read/write maps of the two fixed-capacity buffers
  WebKitWebView: one retained active-spine DOM
  native WebKit snapshot -> validate -> row-copy into selected free slot
  small, trusted DOM-only JavaScript bridge when public native APIs lack DOM access
  WPEWebProcess / WPENetworkProcess (not owners of IPC or shared-memory FDs)
```

- **One `socketpair()`, two endpoints, both full duplex.** Kobalt wraps exactly one endpoint in `QLocalSocket::setSocketDescriptor(..., ConnectedState, ReadWrite)`; the renderer wraps the other in a `GSocket`. There is no listener, socket pathname, session/system D-Bus bus, stdout protocol or SCM_RIGHTS transfer. Keep stderr independent for diagnostics.
- **One `QProcess` per renderer lifetime.** Kobalt owns the pool; restarting the child makes a *new socketpair*, but can reuse existing shared buffers subject to active leases. Use QProcess signals plus socket EOF/errors; preserve process-tree cleanup of WebKit helpers, not just the immediate PID.
- **Two reusable buffer slots as the initial design, not an unlimited queue.** `shm_open(O_CREAT|O_EXCL|O_RDWR,0600)` with a fresh unpredictable name, followed immediately by `shm_unlink()`, `ftruncate()` to checked fixed size, and read-only Kobalt mappings. The renderer inherits the descriptors on startup, validates/maps writable, then closes its copies of the shared-memory FDs before launching any WebKit helpers. The pool does not have persistent names or depend on a visible `/dev/shm` directory.
- **Explicit FD inheritance.** POSIX `shm_open` descriptors are close-on-exec on Linux; create *all* parent-owned FDs close-on-exec. Pass the actual child endpoint and SHM FD numbers as trusted process arguments. The Qt5 `QProcess::setupChildProcess()` override (or Qt6 child modifier) changes `FD_CLOEXEC` **only in the forked child** for those three descriptors immediately before `exec`; do not clear it globally in the parent or assume FDs 3/4 are available. Keep the child hook restricted to safe low-level calls (`fcntl`, no allocation, Qt API, logging or locks); a failed inheritance must fail the handshake. Verify the actual SDK implementations. The renderer sets `FD_CLOEXEC` on its control socket immediately after startup; SHM FDs are closed after mapping. Ensure neither endpoint nor SHM FD leaks into WebKit helper `exec`s.
- **No OS sandbox claim.** Compile the next approved WPE rebuild with `ENABLE_BUBBLEWRAP_SANDBOX=OFF`; until then use the already tested per-process environment override only for controlled fixtures. The host must restrict publication resources, network access, scripts and navigation. Keep koblaze/CREngine functional and default.

**Fallback rule:** If `shm_open()` fails on a *verified target*, investigate a verified private tmpfs `mkstemp()` + immediate `unlink()` producing the same inherited FD semantics. Do not add `memfd_create()`, persistent named SHM, D-Bus FD transport or multiple production control protocols merely as speculative compatibility layers.

## 3. Transport and frame contract

### 3.1 Small binary protocol, independent of Qt types

Use one checked-in protocol definition/document and a compact C/C++ implementation. `AF_UNIX/SOCK_STREAM` delivers bytes, **not messages**: encode a fixed, little-endian header (`magic`, `version`, `type`, `payloadLength`, `requestId`) and a bounded explicitly encoded payload; implement incremental reads, partial writes, EOF and malformed-input rejection. Do **not** serialize `QVariant`, `QDataStream` defaults, pointers, file descriptors, ABI-dependent structs or arbitrary JS values. Set a tight control-message maximum (initial ceiling 1 MiB, with smaller per-field limits). One writer queue per endpoint, with no synchronous GUI waits. Use `QLocalSocket::readyRead()` / `bytesWritten()` and GLib's nonblocking socket sources.

Keep the useful `Renderer1` semantics without its D-Bus wire format:

- `Hello`/capabilities; `Open`; `Turn`; `Navigate`; `SetViewport`; `SetAppearance`; `Query`; `Cancel`; `Close`; `Shutdown`; success/error replies.
- A session/connection generation, nonzero request/document IDs, `layoutRevision` and `frameRevision`; strictly validate stale IDs, out-of-order results, bounds and version. Only one foreground visual request at a time; allow cancellation and shutdown to reach the GLib loop while it is pending. A cancelled or failed request never advances confirmed state. After an *uncertain* commit/disconnect, retire/reopen the session rather than guessing its revision.
- Client reserves an available slot and includes `poolGeneration` and `slotIndex` in each visual request. Renderer writes **only** that slot, and replies once with committed IDs/revisions, location/state, slot index, width, height, stride, length and `bgra8888-premultiplied`. The reply is the publication event; no second `FrameReady` signal and no per-frame FD or pathname. Queries/errors never publish pixels.
- Validate width, height, stride, multiplication overflow, exact mapped capacity, pixel layout/endian, slot status, generation and all request/revision fields **before** publishing a `QImage`. No `msync()` is needed for interprocess visibility; protocol ordering and exclusive slot ownership are required. A response cannot be accepted from a retired renderer generation.

### 3.2 Buffer ownership and crash invariants

Each slot is `Free`, `Writing`, or `Leased`; `Writing` belongs to exactly one in-flight request and `Leased` to existing displayed/retained Qt images. Wrap the read-only mapping in a `QImage` with a lifetime/cleanup lease that runs when its last shared-data reference disappears; ensure Qt5 and Qt6 copying, detaching and painting behave correctly. Kobalt retains pool mappings independently from individual `QImage`s. Never overwrite a leased slot, even after cancellation, resize or renderer death. On no free slot, **defer/coalesce** requests asynchronously rather than overwrite or wait on the GUI thread. Avoid unbounded retained frames.

A crash during a write can dirty only an **unpublished** slot; discard that candidate, fully overwrite before any later publication and keep the previous frame visible. Close/reap the dead process tree, invalidate the old socket/generation, then start a new renderer with a new control socket and the existing SHM descriptors. A frame retained by Qt must remain readable after renderer death and across restarts. Pool resizing requires a new allocation/generation and safe retirement of old mappings when all leases drain; never `ftruncate()` a live mapping. Allocate for an explicitly bounded target viewport, not an oversized global default; compare total PSS before claiming memory savings.

### 3.3 Rendering correctness

Keep `webkit_web_view_get_snapshot(WEBKIT_SNAPSHOT_REGION_VISIBLE, ...)` and `WebKitImage` on headless CPU/Skia. Avoid the obsolete compositor `buffer-rendered` path. The existing M3 color fixture creates a new view per operation; it does **not** establish freshness after turning pages in a retained DOM. A completed snapshot callback, timestamp, sequence number or matching hash does not itself prove the requested pixels were rendered. Validate operation-to-frame attribution with **same-view** first page, red→blue→red, two-column movement, delayed font/image, appearance change, resize, rapid cancel/supersede and recovery tests. Hold at this gate if stale pixels persist; investigate a minimal WebKit lifecycle/snapshot fix before EPUB integration.

## 4. Implementation slices (sequential; stop for review after each)

| Slice | Model | Work | Required acceptance evidence |
|---|---|---|---|
| **WPE-M3.3b — Baseline and transport switch gate** | Sol Low/Medium | Confirm worktree/SHAs, preserve M3.3/M3.3a fixture and failing Qt5 SIGKILL reproduction. Isolate the old crash enough to distinguish old QtDBus teardown from `QImage` lifetime; **do not spend a milestone rewriting QtDBus**. Record concise replacement IPC contract and test matrix. | Repeatable baseline; no accidental staging/commits; user approves abandoning D-Bus production transport. |
| **WPE-M3.4a — QProcess/socketpair spike** | Sol Medium | Implement a standalone Qt5/Qt6 launcher/client + minimal GLib child under `platforms/webkit/`. Verify `socketpair()`/`QLocalSocket`/`GSocket`, close-on-exec behavior, client-child FD ownership, handshake, partial reads/writes, bidirectional cancellation, normal exit and SIGKILL. No WebKit or frame pool yet. | Qt5.2.1 and Qt6 ARM smoke tests pass; repeated SIGKILL does not crash Qt; EOF and child reaping work; unrelated descriptors stay closed. |
| **WPE-M3.4b — Inherited POSIX SHM and QImage leases** | Sol Medium | Add two anonymous-after-unlink `shm_open()` slots to Qt peer; launch a mock child with socket + SHM inherited; map writer/reader, use exact size and QImage cleanup leases. Probe no `/dev/shm` assumption. | Full-frame A→B→A, retained old frame, detach/copy, no-free-slot behavior, cancellation while writing, child kill/restart, pool retirement; no torn published pixels or FD leaks. |
| **WPE-M3.4c — WPE renderer wire replacement** | Sol Medium | Connect real WPE snapshot output to reserved slots and the new framed control protocol. Reuse existing request/revision/cancellation tests. Keep old Renderer1/RendererShm1 source and tests as comparison controls, not second production implementation. | Correct ARM FW4 Qt5/Qt6 pixels, stale/malformed response rejection, layout/revision rollback, crash recovery and helper-FD audit. No D-Bus required on the new client/service path. |
| **WPE-M3.5 — Forma transport gate** | Sol Medium | After **separate physical deployment approval**, run actual Qt5 socketpair/SHM renderer and kill/restart test on Forma; record kernel/loader and package closure. | Real FW4 device: correct frame and retained mapping, clean process-tree teardown, no Nickel disruption or root-partition install. Compare VirtualNickel results. |
| **WPE-M4.1 — Publication access reuse** | Sol Medium | Inventory current `Publication::inspect`, OPF/spine/TOC code, `ZipResource`, `ResourceCache`, `ResourceServer` at chosen host SHA. Add a minimal validated manifest and scoped resource adapter for XHTML/CSS/images/fonts. A fake provider is fine for standalone tests; no second general EPUB parser. | Real EPUB fixture loads; only authorized members can be requested; path traversal, remote URLs, MIME/size/DRM failures rejected; no duplicate archive extraction. |
| **WPE-M4.2 — Retained paginated WebView** | Sol Medium | One active-spine WebView/DOM, native CSS/settings/view/snapshot APIs, lazy chapter transitions, column turns, reflow and rollback. DOM-only isolated trusted JS where needed. | Actual text/fonts on FW4 VirtualNickel; same-view pixel freshness, first-page correctness, turn forward/back, resize, settings, cancel and recovery pass; no eager whole-book layout. |
| **WPE-M4.3 — Locations and page model** | Sol Medium | Map DOM source elements/Unicode offsets to Blaze `Location` (href, body-rooted element-child path, counted code-point offset, affinity); handle UTF-16 JS offsets and collapsed whitespace explicitly. Provide chapter/page queries and bounded estimates; cancel background indexing between units without rendering every page. | Exact/approximate/unresolved location tests, Unicode/RTL and reflow restoration, unknown global page count represented honestly; correct source anchors in multi-book fixtures. |
| **WPE-M5.1 — Blaze integration seam review** | Sol Medium | Read *implemented* `Engine`, `ReaderSession`, `PageInfo`, `Location`, resource lifetimes, source registration and UI flow at latest Blaze SHA. Produce a short interface map and the smallest asynchronous completion/capability change needed. | Explicit user approval of integration base/seam; do not stub the many CREngine-shaped synchronous methods or add a second generic engine framework. |
| **WPE-M5.2 — Optional Kobalt integration** | Sol Medium | Add only thin `src/engine/webkit/` host glue, engine selection and source/build wiring. ReaderSession remains owner of session generations, UI state and e-ink refresh; koblaze remains the default. | Qt5/Qt6 compile; open, turn, reflow, close through real Kobalt UI in VirtualNickel FW4; no blocking on main thread or nested waiting event loops; existing engine tests pass. |
| **WPE-M6 — Persistence and navigation** | Sol Medium | Use Kobalt's existing metadata, TOC and location/persistence machinery. Handle chapter jumps, return links, bookmarks, save/reopen and engine switching. Treat PoC-M4 `kobalt-source:1:` as optional legacy input, not a new canonical host format. | Existing CREngine XPointer/`dom_version` data preserved; source locations restore after font/viewport changes; older and unsupported records not silently rewritten. |
| **WPE-M7 — Essential reader features and stress** | Sol Low for bounded fixtures; Medium for implementation | Add current-page links/notes, selection/geometry, ranges/highlights, search and previews one feature at a time. Report unsupported capabilities rather than fake page totals. Stress startup/shutdown, long books, cancellation, repeated turns and faults. | Focused reader/annotation/storage tests and device-appropriate scenarios; no regressions of koblaze path. |
| **WPE-M8 — Footprint, old-kernel and release assessment** | Sol Medium | Compare two-slot versus single-slot+copy with full process-tree PSS/high-water, WPE startup/turn latency, binary closure and leak/FD counts. Rebuild minimal WPE feature set. Probe the **oldest intended kernel/firmware** individually; physical Forma final text/refresh and memory measurements. | Measured tradeoffs, complete ELF/`dlopen` and license inventory, reproducible package, explicit release/default-engine decision from owner. |

**Do not advance beyond the M3 transport gate while socket/slot safety fails; do not advance from M4.2 to locations/host integration while retained-view pixel freshness fails.** M5 integration is conditional on approval of a real asynchronous Blaze seam. Report a blocked gate and stop rather than invent a workaround.

## 5. Host EPUB, JavaScript and security policy

Kobalt owns publication fingerprint, allowed archive members, OPF/spine/TOC, media types, path canonicalization, font constraints, saved data, and per-book resource authorization. Prefer a narrowly extended existing `ResourceServer`/resource abstraction, or a `kobalt-epub:` WebKit URI-scheme adapter that fetches **only** validated, current-book resources from it. Never expose arbitrary file URLs, external network, Kobalt profile data, or an EPUB-controlled native bridge. Bound decompression, request count, returned bytes, URI traversal and MIME types; test obfuscated/unsupported fonts and protected books against host admission policy.

Prefer native WebKit C/C++ APIs for load, snapshots, sizing/settings, CSS injection, resource controls, process policy and navigation. A small **versioned, trusted DOM bridge** may use JavaScript solely for DOM-only geometry, source positions, columns, hit testing or selection. Check whether isolated-world evaluation remains possible with page JS disabled; otherwise explicitly document the required sanitization/CSP and risk. No book-supplied scripts, external navigation or network access. With Bubblewrap off, WebKit processes are **not OS-sandboxed**: a resource policy is not equivalent to kernel isolation. Production untrusted-book security requires separate review.

## 6. Verification and cost rules

1. **Primary:** VirtualNickel FW4 with the stock Kobo Qt5 ABI and armhf loader. Run targeted Qt5 smoke/recovery tests at every M3 slice; later run actual text/EPUB/reader flows. Identify emulation versus firmware behavior in every report.
2. **Secondary:** Qt6 SDK ARM smoke, negative cases and comparison controls. Do not rely on Qt6 success for Qt5 approval.
3. **Physical:** Forma only at specifically authorized gates: initial M3.5 SHM/socketpair/lifecycle, M4 text/fonts if needed, final M8 user-visible, memory and e-ink metrics. Never assume a sampled PSS is a peak or install substantial binaries on the small root partition.
4. **Negative tests:** wrong protocol version/length, truncated/oversized payload, duplicate/late request, invalid slot, wrong dimensions/stride/format, stale pool/document/revisions, write while leased, abrupt child death, helpers holding unwanted FDs, reconnect after uncertain result, invalid EPUB member paths and unexpected network requests.
5. **Instrumentation:** startup/first-frame/turn/reflow latency; frames copied; live QImage leases; slot bytes; socket buffered bytes; FD and helper counts; total client+renderer+WebProcess+NetworkProcess PSS/peak; ELF and runtime closure. Compare like-for-like native, QEMU, VirtualNickel and physical results separately. Do not optimize from a short QEMU timing sample.
6. **Build and regression:** Use repo-prescribed source checks, pinned `kobuild` Qt5/Qt6 SDKs, relevant `ctest` labels, and existing reader/Pewter/storage checks. Do not extend an allowlist or change unrelated snapshots to make a test pass. Keep generated data in the worktree's `build/` and dependencies in the content-addressed cache.

**Build-size experiments (M8 unless a rebuild is already required):** Start from working WPE 2.54.2. Disable Bubblewrap at compile time and compare disabling WPE legacy/DRM/Wayland/Cog, GPU/WebGL/WebGPU, multimedia/WebRTC, spellcheck/WebDriver and other unused features, one coherent group at a time. For ARM32, **measure** `ENABLE_JIT=OFF`, `ENABLE_DFG_JIT=OFF`, `ENABLE_FTL_JIT=OFF`, `ENABLE_WEBASSEMBLY=OFF` with a compatible `ENABLE_C_LOOP` configuration: do not assume the ARM Thumb-2 JIT was already absent. Also evaluate caches, allocator, LTO/size flags and static-vs-private-shared runtime with actual font/image/DOM correctness. Prove config flags in generated build files; keep the smallest **measured** viable closure.

## 7. One-agent procedure and mandatory handoff

- **Start with WPE-M3.3b only.** At each slice recheck current branch/dirty files, confirm exactly which preexisting work is user-approved, read the narrow source/validation set, make the smallest change with targeted regression, then stop. Additional chats may continue using the latest handoff; they must not skip review gates.
- Use **Sol Low** for mechanical fixtures, source inventories, test scripts and packaging; **Sol Medium** for Qt5 process/FD lifetime, binary protocol, frame leases, WPE snapshot/DOM correctness, EPUB bridge and async adapter. No subagents, speculative frameworks, needless refactors or model escalation.
- **Before any stage/commit/push/merge/physical deployment or next slice, ask for explicit approval.** Approval to implement a slice is not approval to commit it. Keep experimental tests/source under `platforms/webkit/` and future host adapter under `src/engine/webkit/`; do not modify Nickel system settings or other `.adds` applications.
- End each slice with **only** this handoff; no long narrative or repetitive code dumps:

```text
Slice: WPE-Mx.y — <name> | WPE base SHA / Blaze SHA: ...
Changed: <paths and short purpose>
Tests: <exact command(s), PASS/FAIL, Qt5/Qt6/VirtualNickel/Forma identified>
Evidence: <only essential verified observations/measurements>
Blocked/risks: <none or concrete issue>
Review needed: <one specific decision/approval>
Staged NO | Committed NO | Pushed NO | Next slice NO
```

### First agent prompt

> Continue Kobalt `feature/webkit-snapshot-engine` using this plan. **Implement WPE-M3.3b only.** Verify the worktree, `AGENTS.md`, the branch SHAs, `doc/webkit/WPE-M3.3.md`, `doc/webkit/WPE-M3.3a.md`, `platforms/webkit/qt-client/peer.cpp` and the relevant smoke tests. Preserve the existing results; reproduce and minimally characterize the Qt 5.2.1 SIGKILL crash without spending this slice repairing obsolete QtDBus. Document the exact socketpair/QProcess/QLocalSocket/GSocket protocol replacement boundaries, inheritance and test requirements. Do not start M3.4a, edit the live Renderer1 wire XML, stage, commit, push or deploy. Return the prescribed concise handoff and stop for review.

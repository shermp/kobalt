# WPE-M3.1 Renderer1 peer protocol

The [introspection XML](../../platforms/webkit/renderer1.xml) defines the method signatures. This document defines their meaning. Renderer1 is a standalone protocol for one client and one renderer, independent of Kobalt's C++ engine API.

## Connection and version

Use a private Unix domain socket in a mode-0700 directory on a POSIX filesystem such as the device's verified `/tmp`. The launcher provides the address to the client. GDBus serves one authenticated peer of the expected Unix UID, with no anonymous access, message bus or well-known service name. Calls use the object path `/org/kobalt/Renderer1` and interface `org.kobalt.Renderer1`. Closing the connection retires the session and cancels its outstanding work. The client starts a fresh renderer for a new session.

Before any other protocol call, the client uses `Hello(1)`. A successful reply returns `protocolVersion=1` and the capabilities below. Other versions return `UnsupportedVersion`. Both peers must negotiate Unix FD passing in the D-Bus transport. If the service sees that negotiation failed, `Hello` returns `UnixFdUnavailable`. A failed handshake cannot open a document. A repeated successful `Hello(1)` returns the same capabilities and has no side effects.

| Required capability key | D-Bus type | Meaning |
| --- | --- | --- |
| `sourceKinds` | `as` | Supported `Open` source kinds. The standalone M3 renderer advertises `fixture`. |
| `maxWidth`, `maxHeight` | `u` each | Maximum pixel dimensions, each in 1–4096. |
| `maxFrameBytes` | `u` | Maximum mapped frame bytes, at most 32 MiB and at least 4 bytes. |

The only v1 frame format is `bgra8888-premultiplied`. Unknown output dictionary keys are ignored. A missing required key, duplicate key, or wrong type for a known key invalidates the reply. Unknown input keys and values are rejected unless a later documented capability explicitly enables them. Additive optional features require capabilities. Changes to existing signatures or meaning require a new interface version.

## Request and frame identity

- Each call carrying a `requestId` uses a strictly increasing nonzero value on that connection. The renderer records the high-water request ID on receipt, including calls that later return `Busy` or fail argument validation. IDs at or below that mark return `InvalidArgument`. A client that cannot allocate another ID opens a new connection. `Cancel` names an existing request and does not consume an ID.
- A successful `Open` assigns a nonzero `documentId` from a monotonically increasing connection-local sequence. `Open` can replace the current document. The previous document stays current until the new one commits. `Close` retires the named document ID, and IDs are never reused on the same connection.
- A new document begins at `layoutRevision=1`, `frameRevision=1`. The layout revision increases when pagination or geometry changes, including a change of active spine. A turn within the same geometry can retain its layout revision. Each subsequent visual success increments the frame revision by one, including a valid no-op and a return to identical pixels. Revisions never wrap. Reopen before exhausting the counters.
- `Turn`, `Navigate`, `SetViewport`, `SetAppearance`, and `Query` require the current document ID and exact `baseFrameRevision`. Check both before touching renderer state. The server returns `StaleState` for an old document or frame, including commands based on an earlier page within the same layout. `Close` needs only the current document ID so it can close after a late success.

Every visual method returns one D-Bus reply with signature `tttta{sv}huuus`. It carries the echoed request ID, document ID, layout revision, frame revision, state dictionary, one FD handle, width, height, stride and format, in that order. The returned state describes the returned pixels. The server completes the requested DOM/layout change and verifies that the snapshot belongs to that operation before making the reply. A snapshot callback or identical hash alone cannot establish freshness.

The client keeps the connection generation and pending request identity alongside its last confirmed frame. It checks query replies against the exact identity read by the query and discards them if that frame is no longer current. It validates the entire reply before publishing pixels and state together. For an update it checks the document and base frame against the pending call, the next frame revision, and a nondecreasing layout revision. For `Open` it expects a fresh document ID and initial revisions. It drops duplicate results, replies for retired connections, and replies for requests that the UI has superseded. If the server advanced on a valid success the UI chose to discard, the client must account for the new base before sending another relative operation. It can also reopen. After an uncertain transport failure or malformed success it retires the connection and reopens if needed. Its previously confirmed image can stay visible throughout recovery.

## Operations

| Method | Contract |
| --- | --- |
| `Open` | Takes a source kind, source, fingerprint, viewport in physical pixels, complete appearance overrides and optional saved token. For `fixture`, source is the built-in `color-pages` fixture and fingerprint is empty. It starts at the first page unless a valid token selects another page. A failed open leaves the prior document current. |
| `Turn` | Delta is exactly `-1` or `+1`. At a boundary it returns `OutOfRange` without changing the current frame. |
| `Navigate` | Kind is `start` or `end` with an empty target, or `token` with an opaque token for the current source. A valid request to the current page still returns a frame. Other navigation modes need an explicit extension. |
| `SetViewport` | Takes the full new pixel dimensions within the advertised bounds. The service rejects sizes that cannot fit a valid frame within the advertised byte limit. It preserves the last confirmed source position during reflow. |
| `SetAppearance` | Replaces the full override dictionary. The M3 fixture accepts only the empty dictionary. Additional settings require documented typed keys and a capability. Reflow preserves the last confirmed source position. |
| `Query` | A read of the named committed frame. The v1 base kind is `state`, with empty arguments. The result is the same state dictionary and identity as the selected frame, except for the echoed query request ID. It sends no FD and changes no revision. |
| `Close` | Retires the current document and its runtime resources. Success has an empty reply and the client can issue a new `Open`. A retired or unknown document returns `StaleState`. |
| `Cancel` | Targets a pending visual or query request. True means cancellation won before commit. The original call completes with `Cancelled` and no frame. False means the target is unknown, already cancelled, or already decided. The caller still handles the original reply. |
| `Shutdown` | Stops accepting foreground work and cancels pending uncommitted work. After cleanup the service sends the empty reply, flushes it and closes the peer. The launcher supervises a timeout or unexpected disconnect. |

M3 serializes foreground work. While a foreground call is active, another foreground call returns `Busy`. `Hello`, `Cancel` and `Shutdown` remain responsive. In particular, a client that wants to close during a snapshot cancels it, settles the original reply, then closes the document. It does not wait synchronously on the UI thread. A client may also terminate the session instead of issuing more calls.

Commit is the single point on the service event loop at which the complete result and frame are ready, identity and cancellation are rechecked, and the renderer makes the new state current and queues success. Cancellation after commit returns false. A cancelled or failed operation must leave the prior committed snapshot and interaction state usable, including restoring any provisional DOM changes before admitting another foreground call. If restoration fails, retire the connection and document. Late callbacks are discarded and their resources released. A renderer disconnect after commit can make the outcome uncertain on the client, so the next connection cannot continue the old revision sequence.

For example, if a turn from frame 8 is cancelled before commit, frame 8 is still current and the next turn can use base 8. If the turn commits first, it returns frame 9 and cancellation returns false. The client settles that success before sending a turn from base 9. Resending a turn from base 8 would return `StaleState`.

The v1 base defines no indexing methods or progress signals. Later indexing support must be opt-in and identify the document, layout and job on each update. It must not use a signal to publish a visual frame.

## State and source tokens

Visual replies and `Query("state")` use these keys. Any optional values describe the committed frame's layout only.

| Key | D-Bus type | Meaning |
| --- | --- | --- |
| `sourceToken` | `s` | Required. Opaque, source-bound location, or empty when unresolved. Fixture tokens are `fixture:color-pages:0` and `fixture:color-pages:1`. |
| `locationPrecision` | `s` | Required. One of `exact`, `approximate`, `unresolved`. The token is nonempty for the first two and empty for the last. The fixture reports exact positions. |
| `canTurnBack`, `canTurnForward` | `b` each | Required. Whether the matching relative page turn is available. |
| `pageIndex` | `u` | Optional, zero-based whole-source page index when known. |
| `pageCount` | `u` | Optional, positive exact whole-source page count when already known. If both page fields are present, the index is less than the count. |

An empty input `savedToken` selects the start. A nonempty token that cannot be resolved returns `UnresolvedLocation`; it cannot silently select the start. A later EPUB source kind must define authorization of the publication path, fingerprint verification, and source location resolution before advertising support. A token is never interpreted as a path or URI. The protocol carries no Kobalt profile data, and conversion to canonical Kobalt locations is separate WPE-M4 work.

## Frame byte layout and ownership

Each visual success carries exactly one Unix FD at handle index zero. The wire handle is an index into the descriptors attached to that same reply. GDBus must use an FD list; Qt must validate the decoded Unix descriptor instead of treating the handle as an ordinary integer. Control replies and errors carry no descriptors. See the [D-Bus wire specification](https://dbus.freedesktop.org/doc/dbus-specification.html).

Pixels start at offset zero in top-down rows. Each pixel is four bytes in blue, green, red, alpha order. Color channels are premultiplied by alpha, and row padding is zeroed. Width and height match the current request's resulting viewport. Stride is a multiple of four and at least `width * 4`. Both peers calculate products in checked wide arithmetic and verify that `stride * height` fits the advertised limit and native mapping and image APIs. Before mapping read-only, the receiver checks that the FD is valid, regular and mappable, and that `fstat` reports exactly that length. It rejects unsupported dimensions, format, byte order or stride. The little-endian Qt representation is `QImage::Format_ARGB32_Premultiplied`.

The service copies each snapshot into a distinct owned buffer. It sends a write/grow/shrink-sealed backing object where supported. A fallback may use a private tmpfs file, reopen it read-only, unlink it, then close every writable descriptor and unmap every writable alias before export. The producer never reuses a published backing object or retains a writable alias. A read-only FD alone does not prove that a different process lacks a writable alias, so the fallback requires verified ownership and is limited to the authenticated trusted renderer. VFAT is not frame backing storage. Failure to establish immutability returns an error.

The receiver releases discarded replies and their FDs promptly. The accepted read-only map is owned by the immutable frame and remains valid through image copies and renderer exit. Unmap on last release. Closing the renderer or accepting a newer frame must not invalidate an older mapped image held by the consumer.

## Errors

Application error names use the prefix `org.kobalt.Renderer1.Error.`. The D-Bus error message is diagnostic text and carries no state, frame or recovery instructions that a client must parse.

| Suffix | Meaning |
| --- | --- |
| `NotReady`, `UnsupportedVersion`, `UnixFdUnavailable` | Handshake required, incompatible version, or FD transfer unavailable. |
| `InvalidArgument`, `Unsupported` | Invalid value or request ID, or an unadvertised operation or option. |
| `Busy`, `StaleState`, `Cancelled`, `ShuttingDown` | Foreground call already active, document/base mismatch, accepted cancellation, or session closing. |
| `OutOfRange`, `UnresolvedLocation` | Page boundary or an unresolved source token. |
| `SourceError`, `RenderFailed` | Source could not be opened, or layout, snapshot or immutable frame creation failed. |

## Handoff

Source base `f6698b486497cb7b44933857a0b6612912459f5b`, local Blaze reference `a4bb4d1cf86acaa404064dca184a0e6708a43b00`. Single agent, GPT-6.1 Sol. This slice replaces the draft with `platforms/webkit/renderer1.xml` and links this contract from the implementation plan. No device, simulator, runtime closure, pixels or performance measurements belong to this schema change. The Forma gate remains recorded in [M2.2](WPE-M2.2.md).

On the M1 tools image with GLib 2.80.0, `gdbus-codegen` generated bindings and the generated C passed a compiler syntax check. A separate check verified every XML input and output signature and confirmed that generated completion functions take a `GUnixFDList` for exactly the visual methods. To reproduce the generation and compiler check from the worktree root, run the following.

```sh
mkdir -p build/webkit-m3.1
podman run --rm --network none --userns keep-id \
  -v "$PWD/platforms/webkit:/schema:ro" -v "$PWD/build/webkit-m3.1:/out:rw" \
  --entrypoint sh 662599b1c68c64b57e6a3d8f35994592a3f4ac6042b3a66c82651db1f36428d7 -ec '
    cd /out
    gdbus-codegen --interface-prefix org.kobalt. --c-namespace Kobalt \
      --generate-c-code renderer1 /schema/renderer1.xml
    cc -Wall -Wextra -Werror -fsyntax-only $(pkg-config --cflags gio-unix-2.0) renderer1.c
  '
```

The next slice is M3.2, implementing service dispatch and cancellation against the XML. Live peer authentication, Qt5/Qt6 FD transfer, frame freshness, backing ownership and renderer-death behavior need implementation and tests in M3.2–M3.4.

Staged: NO. Committed: NO. Pushed: NO. Next slice started: NO.

# WPE-M4.1 publication access reuse

The host can now open an EPUB as a worker-owned resource scope. The scope records its publication fingerprint, ordered XHTML spine, optional EPUB3 navigation member and authorized resource URLs. The checked-in Trevelyan's Type Tester fixture supplied real XHTML, CSS and PNG bytes through the scope. The WebKit renderer does not yet request or render those resources.

## Existing source and reuse

This slice uses the publication implementation at WPE base `072facc641b33e9e96575ad2ea016a936c1c8705`. Local `develop` was at `2a708d19abfbb23aab95fab94451efe8feb001a8` and Blaze at `a4bb4d1cf86acaa404064dca184a0e6708a43b00`. The replacement plan came from the main checkout. One GPT-6.1 Sol agent worked on the slice.

| Existing code | Role in this slice |
| --- | --- |
| `Publication::inspect` | Keeps existing ZIP admission, protection checks, member sizes and directory fingerprint. Its container/OPF walk for font obfuscation now supplies the scoped package metadata too. Ordinary CREngine admission keeps its previous behavior. |
| `ZipResource`, `ZipResourceIndex`, `localResourceFactory` | Reuse the bounded, CRC-checked stream and existing local cache construction. The standalone scope pins one regular file and invalidates readers when its identity or timestamps change. |
| `ResourceCache`, `SourceLease` | Keep one worker-side loader and cache for package metadata and resources. Repeated requests for a cached chapter do not extract it again. Revocation invalidates cached access. |
| `ResourceServer` / `PublicationEndpoint` | The current Nickel path serves a selected provider over loopback. Future host integration can supply the same worker cache and lease to the scope. The standalone local scope does not start a server. |
| `inspectLayout`, CREngine/Blaze reader source and TOC | Layout compatibility is advisory and walks linked content for a separate purpose. Native TOC and preview logic relies on engine state. The scope records the EPUB3 nav member and ordered spine without interpreting TOC links or adding a TOC parser. NCX remains outside the served set. |

The renderer-facing resource key is an exact `kobalt-epub://book-<opening>/…` URL. Access requires an admitted publication, the active lease and a manifest entry with a corresponding ZIP member. The resolver accepts simple escaped filenames while rejecting external sources, archive escapes, encoded separators and ambiguous paths. The request lookup accepts canonical URLs for the current opening only. Container metadata and undeclared archive members cannot be read through the scope.

The initial media policy serves XHTML, CSS, PNG/JPEG/GIF/WebP and explicitly supported plain font types. Unsupported entries are omitted. Unsupported spine entries fail admission. Raster images and fonts get a signature check before delivery. The limits are 2 MiB for metadata, XHTML, CSS and fonts, 8 MiB for raster images, and 4,096 requests or 64 MiB of served data per opening. Obfuscated font payloads are withheld even when the existing engine admission recognizes their embedding scheme.

The scope authorizes bytes. It does not sanitize XHTML or CSS, enforce WebKit navigation or network policy, or provide kernel isolation. Those controls are required before letting the renderer load any untrusted EPUB. This slice adds no renderer, Qt SDK, or firmware dependency and leaves storage formats unchanged.

## Checks

Use the prepared Linux SDK and configure `cmake --preset linux` in `platforms/desktop`. In the repository root, using that build:

```sh
cmake --build build/desktop-linux --target kobalt-local-resources-tests \
  kobalt-layout-compatibility-tests kobalt-zip-resource-tests \
  kobalt-resource-cache-tests kobalt-resource-provider-tests
ctest --test-dir build/desktop-linux \
  -R '^(model\.(local-resources|layout-compatibility|zip-resource|resource-cache)|resource\.resource-provider)$' \
  --output-on-failure
cmake -P platforms/checks/source-checks.cmake
cmake -P platforms/kobo/syntax.cmake src/publication/publication.cpp \
  src/publication/local_resources.cpp src/publication/zip_resource.cpp
```

All selected native tests and source checks passed. The targeted syntax check passed against both Kobo SDK header sets, using Clang 18 in the Linux SDK container. The new resource assertions cover Type Tester, one extraction for repeated cached chapter access, escaped filenames, unlisted members, cross-opening URLs, revocation, changed-file rejection, external and traversal paths, MIME and size limits, and rejected LCP and unsupported encryption. Existing resource-provider tests exercised the legacy font admission path. No physical or VirtualNickel run was needed for this host-side scope.

Changed files are `publication.cpp/.h`, `local_resources.cpp/.h`, `zip_resource.cpp/.h`, `web_resources.h`, the local resource check and its desktop test wiring. The next slice needs approval to connect the scope to WPE with request, navigation and script policy, then prove retained-view freshness.

M3.5 was committed as authorized. M4.1 staged NO, committed NO, pushed NO. M4.2 started NO.

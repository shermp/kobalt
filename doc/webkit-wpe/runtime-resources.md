# M1 runtime-resource audit

The standalone bundle is independently deployable within the selected firmware
runtime, with the system inputs below. It does not promise an identical image
across different firmware/font sets. The fixture's embedded body font is bundled;
generic text and MathML can use firmware fallback fonts.

| Input | Ownership and current scope |
| --- | --- |
| ELF dependencies | Recursive private closure in the package; firmware supplies glibc and `/lib/ld-linux-armhf.so.3`. Package checks reject host/ABI contamination and symbol requirements above glibc 2.35. |
| WebKit resources and ICU data | Compiled resources and bundled `libicudata.so.70`; no separate downloaded ICU data is required for this fixture. |
| XHTML, SVG and embedded font | Package-owned files served through the fixed custom URI handler, with explicit MIME types. The raster and snapshot are run-owned files removed on shutdown. |
| Fontconfig config and fallback fonts | Firmware `/etc/fonts/` and `/usr/share/fonts/`; verified on the selected firmware userspace as described below. The package does not redistribute Kobo's system fonts. |
| Font caches and private user directories | Supervisor-owned temporary HOME/XDG directories. Fontconfig may consult and, where permissions allow, update the system cache; its private fallback cache is disposable and separate from reading data. |
| Shared MIME database | Available in the stock firmware. The fixed handler supplies MIME types itself; general file sniffing/publication loading is outside M1. |
| GIO TLS/proxy modules, settings schemas and trust stores | Not supplied as a general networking stack by this package. The trusted fixture uses local resources. Their compatibility and ownership must be established before enabling external requests. |
| Hyphenation dictionaries and keyboard data | Not packaged or exercised by this fixed fixture. General language hyphenation and keyboard input need their own resource contract when those features are implemented. |
| Bubblewrap and D-Bus proxy | Compiled paths remain `/usr/bin/bwrap` and `/usr/bin/xdg-dbus-proxy`; private target tools are not bundled. Required-mode startup refuses an unavailable sandbox. The authorized explicit device mode bypasses it. |

The build retains `/opt/kobalt-webkit` install/data paths in several optional
WebKit definitions. The launcher explicitly relocates subprocesses and the
injected bundle, and the fixed fixture uses its own resource handler. This audit
does not claim that every optional WebKit data lookup is relocatable. New
features must establish their resource paths and validation before use, without
reading an SDK directory or another checkout at runtime.

## Firmware font provenance

The test-only `webkit-font-resources` diagnostic links the same private
Fontconfig. On a disposable device, the runner temporarily installs it in the
worker slot and invokes it through the unmodified production supervisor. It
therefore sees the actual descriptor/environment isolation and run directory.
No WebKit instance starts for this diagnostic. The runner restores the worker,
verifies the whole package again, checks Nickel responsiveness and deletes the
device/system volume even on failure.

For firmware 6.0.274403 / Libra Colour, the config paths resolved to the device's
own `/firmware/rootfs/etc/fonts/`, and every reported font resolved into its own
`/firmware/rootfs/usr/share/fonts/`. The fallback requests in this probe selected
`Vollkorn-Regular.ttf`. The provenance record retains resolved paths and hashes
of files; config include directories are recorded as directories. No fonts or
configuration were taken from another checkout or an Ubuntu build SDK. This is
userspace/resource evidence, not physical-device timing or a kernel test.

Build the diagnostic with the normal prepared ARM toolchain environment, from
`platforms/webkit/`:

```sh
cmake --preset font-resources
cmake --build --preset font-resources
```

Then run from the repository root with host CMake available:

```sh
python3 tests/webkit/run-firmware.py \
  --virtualnickel /home/sherman/kobo-dev/VirtualNickel --font-resources-only
```

Results include `font-resources.tsv`, `font-provenance.json`, stderr and the
restored package check under `build/webkit/firmware-*/`. The current successful
run is `build/webkit/firmware-14f6285d31/`. An initial provenance-collector failure
on a config include directory was corrected; that run also restored the package
and deleted its disposable device. No resource-driven renderer changes were
needed, so its existing image/crash/cleanup validation remains applicable.

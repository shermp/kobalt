# WPE-M2.1 isolated Forma fixture package

This prepares a reviewable package; it does not install or run it on the Forma. The source base is `0271405ab8c93fb13f71f47b5d95f81a62caff5d`. The five stripped ELF inputs are exactly the candidate verified in [WPE-M1](WPE-M1.md), identified by SHA-256 in the preparation script. The prior simulator used Nia/kobo7 firmware 4.38.23852, ARM hard-float, Kobo glibc 2.19 and QEMU Cortex-A9. Actual Forma firmware, ABI and kernel remain unverified.

## Reproduce and inspect

From this worktree, after reproducing the M1 candidate at the documented location:

```sh
platforms/webkit/prepare-m2-package.sh \
  build/webkit-m1/size-comparison/candidate build/webkit-m2-package
(cd build/webkit-m2-package && sha256sum -c tests/SHA256SUMS && sha256sum -c tests/LAUNCHER-SHA256SUMS)
find build/webkit-m2-package -type l -print
```

The output must not already exist. A digest mismatch fails preparation; remove a failed output with the repository cleanup tool before retrying. The manifest includes the fixture, WebProcess, NetworkProcess, private WPE shared library and injected module. The separately generated launcher digest permits transfer verification. All five payload entries are regular files, including the SONAME file: no symlinks or preservation of Unix mode bits on VFAT are required. The package has `bin`, `libexec`, `lib`, `share`, `tests`, and `logs` directories; empty directories must be copied too. Current file contents total approximately 94.4 MB. The ELF closure and static archive list are in WPE-M1. The ELF fixture contains a build-directory RPATH; that path must not exist on the device. The launcher sets the private library and WebKit helper/module paths explicitly, while the loader and glibc libraries remain device-provided.

The launcher creates a private mode-700 directory under `/tmp` for HOME, XDG data, GIO modules, temporary files and runtime sockets, and removes it on exit. It writes no files to the root filesystem except temporary runtime state; it does not modify Nickel, startup configuration, or other applications. This fixture is color-only, so it does not validate font configuration. The existing M1 simulator result used an offline filter and a container Bubblewrap fallback; neither provides a device sandbox or network-isolation guarantee.

## Proposed device operations for approval

The following is a proposal, not a record of execution. First inspect the mounted onboard storage and device `/tmp`, `/`, mount flags, available memory, firmware, and existing destination. Stop if the destination exists, onboard space is insufficient, `/tmp` is not a POSIX filesystem with sufficient free space, or the onboard mount is `noexec`. The exact headroom and compatibility must be assessed from that inspection; do not copy the package to `/` as a workaround. A Unix socket bind in the host's `/tmp` passed; this says nothing about the device.

With the device storage mounted as `$ONBOARD` on the host and only after explicit deployment approval:

```sh
[ ! -e "$ONBOARD/.adds/kobalt-webkit" ] && [ ! -L "$ONBOARD/.adds/kobalt-webkit" ] || exit 1
cp -R build/webkit-m2-package "$ONBOARD/.adds/kobalt-webkit"
sync
(cd "$ONBOARD/.adds/kobalt-webkit" && sha256sum -c tests/SHA256SUMS && sha256sum -c tests/LAUNCHER-SHA256SUMS)
```

Safely eject before accessing the device. On the device, verify the same hashes and inspect `/proc/mounts`, `df -k / /tmp /mnt/onboard`, and the absence of the embedded build RPATH directory before any execution. Verify an actual executable on the onboard mount and an AF_UNIX socket under `/tmp` as part of the separately approved physical run, alongside the fixture and child cleanup. If `sha256sum` is unavailable on the device, establish another trustworthy verification method before running. The intended fixture command, **only after approval of M2.2**, is:

```sh
/mnt/onboard/.adds/kobalt-webkit/bin/run-fixture > /mnt/onboard/.adds/kobalt-webkit/logs/fixture.log 2>&1
```

Capture its exit status immediately and inspect the log, live processes and free space. Do not claim VFAT execution, socket operation, subprocess cleanup, FD behavior or frame correctness from package preparation. The launcher has no external timeout or forced child reaper; device execution requires an operator-supervised bounded run and cleanup plan. Uninstall only this newly installed directory after checking no process uses it and retaining requested logs; do not remove an existing installation or unrelated files.

## Local results and outstanding gate

Preparation verified all five input hashes and the launcher hash; `sh -n` passed for both scripts and `find` found no links. Host `/tmp` accepted an AF_UNIX bind. The host lacks `mkfs.vfat`, so no VFAT test was performed. The device's executable mount, POSIX socket capacity, root-space impact, runtime library selection, subprocess behavior, and all physical pixel and memory results remain unverified. No device access, install or deployment occurred. The M2.2 gate and the decision to accept the private shared runtime remain for owner review.

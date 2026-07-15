#!/usr/bin/env bash
# Run the non-JIT test surface on emulated ppc64 (both byte orders).
#
#   ./scripts/ppc64/emu-test.sh be   # big-endian (ELFv1)
#   ./scripts/ppc64/emu-test.sh le   # little-endian (ELFv2)
#
# Strategy: cross-compile STATIC binaries at native speed in an x86_64 debian
# container, run them on the host through binfmt/qemu-user-static (see
# cross-test.sh). A full native-arch container build (distro gcc running under
# emulation) also works but is far slower and needs a working cmake toolchain
# inside the emulated image; the cross route is the maintained path.
#
# Host prerequisite (once): sudo dnf install -y qemu-user-static-ppc
set -eu
exec "$(dirname "$0")/cross-test.sh" "${1:?usage: emu-test.sh <be|le>}"

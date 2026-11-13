#!/bin/bash
# Build the daemon and the ALSA client for each Volumio architecture into
# bin/<arch>/: audio-keepalive-daemon and libasound_module_pcm_keepalive.so.
# Players run those files; they build nothing.
#
# Built in the builder image against bookworm's glibc (2.36, Volumio 4's) and ALSA,
# warnings denied. The arm folder is the armv7 build: Volumio's universal Raspberry Pi
# image labels itself arm, as 2.0.x's armhf build served it.
set -euo pipefail
ROOT=$(CDPATH='' cd -- "$(dirname "$0")/.." && pwd)
cd "$ROOT"
if [ "${KA_BUILDER:-}" != 1 ]; then
  exec "$ROOT/scripts/builder.sh" scripts/ship.sh "$@"
fi

DAEMON=audio-keepalive-daemon
MODULE=libasound_module_pcm_keepalive.so
VERSION=$(sed -n 's/^ *"version": "\(.*\)",*$/\1/p' plugin/package.json | head -n1)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

ship() {
  local arch=$1 cross=$2 machine=$3
  echo "ship: $arch (${cross:-native})"
  mkdir -p "$WORK/$arch"
  cp ./*.c ./*.h Makefile "$WORK/$arch/"
  make -s -C "$WORK/$arch" ${cross:+CROSS_COMPILE=$cross} VERSION="$VERSION" all strip
  install -D -m 755 "$WORK/$arch/$DAEMON" "bin/$arch/$DAEMON"
  install -D -m 644 "$WORK/$arch/$MODULE" "bin/$arch/$MODULE"
  for f in "bin/$arch/$DAEMON" "bin/$arch/$MODULE"; do
    file -b "$f" | grep -q "$machine" || { echo "ship: $f is not $machine code: $(file -b "$f")" >&2; exit 1; }
    # Nothing newer than bookworm's glibc 2.36, which Volumio 4 runs.
    newest=$(readelf -W --dyn-syms "$f" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -n1)
    case "$newest" in
      GLIBC_2.3[7-9]*|GLIBC_2.[4-9]*) echo "ship: $f needs $newest" >&2; exit 1 ;;
    esac
  done
  readelf -W --dyn-syms "bin/$arch/$MODULE" | grep -q ' _snd_pcm_keepalive_open$' \
    || { echo "ship: bin/$arch/$MODULE does not export _snd_pcm_keepalive_open" >&2; exit 1; }
}
ship x64 "" "x86-64"
ship armv7 arm-linux-gnueabihf- "ARM, EABI5"
ship armv8 aarch64-linux-gnu- "ARM aarch64"
rm -rf bin/arm && cp -a bin/armv7 bin/arm

echo "ship: armv7"
readelf -A "bin/armv7/$DAEMON" | grep -q 'Tag_CPU_arch: v7' || { echo "ship: bin/armv7/$DAEMON is not ARMv7 code" >&2; exit 1; }
echo "ship: version"
said=$("bin/x64/$DAEMON" --version)
[ "$said" = "$DAEMON $VERSION" ] || { echo "ship: the daemon says '$said', plugin/package.json is $VERSION" >&2; exit 1; }
file bin/*/*

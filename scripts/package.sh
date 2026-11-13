#!/bin/bash
# The plugin as the zip a player installs: dist/audio_keepalive-<version>.zip, with the
# plugin's files at its root, the daemon in bin/<arch> and the ALSA client in
# asound/<arch> for every architecture, the licence and build.json naming the commit.
# No node_modules: the plugin uses only Node's own modules and Volumio's kew and v-conf.
#
# The version is plugin/package.json's; the daemon carries the same one.
set -euo pipefail
ROOT=$(CDPATH='' cd -- "$(dirname "$0")/.." && pwd)
cd "$ROOT"
VERSION=$(sed -n 's/^ *"version": "\(.*\)",*$/\1/p' plugin/package.json | head -n1)
NAME=audio_keepalive
STAGE=$ROOT/dist/stage/$NAME
ARCHES="arm armv7 armv8 x64"
rm -rf "$ROOT/dist/stage"
mkdir -p "$STAGE" "$ROOT/dist"
cp -r "$ROOT/plugin/." "$STAGE/"
rm -rf "${STAGE:?}/node_modules" "${STAGE:?}/package-lock.json" "${STAGE:?}/bin"
rm -f "$STAGE"/asound/*.conf "$STAGE"/asound/*.so
cp "$ROOT/LICENSE" "$STAGE/LICENSE"

# The build the zip carries: the commit and the time.
printf '{ "commit": "%s", "built": "%s" }\n' \
  "$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)" \
  "$(date -u +%Y-%m-%dT%H:%M:%SZ)" > "$STAGE/build.json"

for arch in $ARCHES; do
  for file in audio-keepalive-daemon libasound_module_pcm_keepalive.so; do
    bin="$ROOT/bin/$arch/$file"
    [ -f "$bin" ] || { echo "package: $bin is missing; run scripts/ship.sh first" >&2; exit 1; }
  done
  install -D -m 755 "$ROOT/bin/$arch/audio-keepalive-daemon" "$STAGE/bin/$arch/audio-keepalive-daemon"
  install -D -m 644 "$ROOT/bin/$arch/libasound_module_pcm_keepalive.so" "$STAGE/asound/$arch/libasound_module_pcm_keepalive.so"
done

if [ -e "$STAGE/node_modules" ] || grep -q '"dependencies"' "$STAGE/package.json"; then
  echo "package: the plugin must not carry node modules; the zip was not made" >&2; exit 1
fi

ZIP=$ROOT/dist/$NAME-$VERSION.zip
rm -f "$ZIP"
( cd "$STAGE" && zip -qr "$ZIP" . )
rm -rf "$ROOT/dist/stage"
echo "package: $ZIP: $(stat -c %s "$ZIP") bytes, sha256 $(sha256sum "$ZIP" | cut -d' ' -f1)"

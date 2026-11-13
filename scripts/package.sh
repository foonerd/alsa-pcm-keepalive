#!/bin/bash
# The plugin as the zip a player installs: dist/audio_keepalive-<version>.zip, with the
# plugin's files at its root, the daemon and the ALSA client from bin/<arch> for every
# architecture, the licence, build.json naming the commit, and the node modules.
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
rm -f "$STAGE"/asound/*.conf
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
  install -D -m 644 "$ROOT/bin/$arch/libasound_module_pcm_keepalive.so" "$STAGE/bin/$arch/libasound_module_pcm_keepalive.so"
done

# Node modules: with npm on this machine, directly; otherwise in a container with the
# Node major Volumio ships. A zip without them is no plugin: the install must succeed,
# and every module package.json names must be there.
if command -v npm >/dev/null 2>&1; then
  ( cd "$STAGE" && npm install --omit=dev --no-audit --no-fund --loglevel=error >/dev/null )
else
  docker run --rm -v "$STAGE:/plugin" -w /plugin node:20-bookworm-slim sh -c \
    "npm install --omit=dev --no-audit --no-fund --loglevel=error >/dev/null; rc=\$?; chown -R $(id -u):$(id -g) /plugin/node_modules /plugin/package-lock.json 2>/dev/null; exit \$rc" \
    || { echo "package: npm install failed; the zip was not made" >&2; exit 1; }
fi
rm -f "$STAGE/package-lock.json"
for module in $(sed -n '/"dependencies"/,/}/p' "$STAGE/package.json" | sed -n 's/^ *"\([^"]*\)": *"[^"]*",*$/\1/p'); do
  [ -f "$STAGE/node_modules/$module/package.json" ] || { echo "package: the module $module is not in the stage; the zip was not made" >&2; exit 1; }
done

ZIP=$ROOT/dist/$NAME-$VERSION.zip
rm -f "$ZIP"
( cd "$STAGE" && zip -qr "$ZIP" . )
rm -rf "$ROOT/dist/stage"
echo "package: $ZIP: $(stat -c %s "$ZIP") bytes, sha256 $(sha256sum "$ZIP" | cut -d' ' -f1)"

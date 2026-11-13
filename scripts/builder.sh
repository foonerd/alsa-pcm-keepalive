#!/bin/bash
# Run a command in the checkout inside the builder image (scripts/builder/Dockerfile):
# Debian bookworm, the glibc and ALSA Volumio 4 ships, with the armhf and arm64 cross
# compilers. The image is built here when it is missing or the Dockerfile changed.
#   scripts/builder.sh make test
set -euo pipefail
ROOT=$(CDPATH='' cd -- "$(dirname "$0")/.." && pwd)
IMAGE=alsa-keepalive-builder:$(sha256sum "$ROOT/scripts/builder/Dockerfile" | cut -c1-12)
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
  echo "builder: building $IMAGE"
  docker build -q -t "$IMAGE" "$ROOT/scripts/builder" >/dev/null
fi
exec docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$ROOT:/src" -w /src "$IMAGE" "$@"

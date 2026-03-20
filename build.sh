#!/bin/bash
#
# build.sh - Cross-compile ALSA keepalive proxy for all Volumio targets
#
# Builds armhf (Raspberry Pi), arm64, and amd64 binaries using Docker.
# Output: dist/ directory with architecture-suffixed .so files.
#
# Usage:
#   ./build.sh          Build all architectures
#   ./build.sh armhf    Build armhf only
#   ./build.sh arm64    Build arm64 only
#   ./build.sh amd64    Build amd64 only
#

set -e

IMAGE_NAME="alsa-keepalive-builder"
PLUGIN="libasound_module_pcm_keepalive.so"

TARGET="${1:-all}"

echo "=== Building Docker image ==="
docker build -t "${IMAGE_NAME}" .

mkdir -p dist

build_arch() {
    local arch="$1"
    local cross="$2"

    echo ""
    echo "=== Building ${arch} ==="

    local strip_cmd="strip"
    local cross_arg=""

    if [ -n "${cross}" ]; then
        strip_cmd="${cross}strip"
        cross_arg="CROSS_COMPILE=${cross}"
    fi

    docker run --rm \
        -v "$(pwd)/dist:/dist" \
        "${IMAGE_NAME}" \
        bash -c "
            make clean && \
            make ${cross_arg} && \
            ${strip_cmd} ${PLUGIN} && \
            cp ${PLUGIN} /dist/${PLUGIN}.${arch}
        "

    echo "  Built: dist/${PLUGIN}.${arch}"
    file "dist/${PLUGIN}.${arch}"
}

case "${TARGET}" in
    armhf)
        build_arch "armhf" "arm-linux-gnueabihf-"
        ;;
    arm64)
        build_arch "arm64" "aarch64-linux-gnu-"
        ;;
    amd64)
        build_arch "amd64" ""
        ;;
    all)
        build_arch "armhf" "arm-linux-gnueabihf-"
        build_arch "arm64" "aarch64-linux-gnu-"
        build_arch "amd64" ""
        ;;
    *)
        echo "Unknown target: ${TARGET}"
        echo "Usage: $0 [armhf|arm64|amd64|all]"
        exit 1
        ;;
esac

echo ""
echo "=== Build complete ==="
ls -la dist/

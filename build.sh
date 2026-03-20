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
    local pkg_config_path="$3"

    echo ""
    echo "=== Building ${arch} ==="

    if [ -n "${pkg_config_path}" ]; then
        docker run --rm \
            -v "$(pwd)/dist:/dist" \
            "${IMAGE_NAME}" \
            bash -c "
                make clean && \
                PKG_CONFIG_PATH=${pkg_config_path} \
                PKG_CONFIG_ALLOW_CROSS=1 \
                make CROSS_COMPILE=${cross} && \
                ${cross}strip ${PLUGIN} && \
                cp ${PLUGIN} /dist/${PLUGIN}.${arch}
            "
    else
        docker run --rm \
            -v "$(pwd)/dist:/dist" \
            "${IMAGE_NAME}" \
            bash -c "
                make clean && \
                make && \
                strip ${PLUGIN} && \
                cp ${PLUGIN} /dist/${PLUGIN}.${arch}
            "
    fi

    echo "  Built: dist/${PLUGIN}.${arch}"
    file "dist/${PLUGIN}.${arch}"
}

case "${TARGET}" in
    armhf)
        build_arch "armhf" "arm-linux-gnueabihf-" \
            "/usr/lib/arm-linux-gnueabihf/pkgconfig"
        ;;
    arm64)
        build_arch "arm64" "aarch64-linux-gnu-" \
            "/usr/lib/aarch64-linux-gnu/pkgconfig"
        ;;
    amd64)
        build_arch "amd64" "" ""
        ;;
    all)
        build_arch "armhf" "arm-linux-gnueabihf-" \
            "/usr/lib/arm-linux-gnueabihf/pkgconfig"
        build_arch "arm64" "aarch64-linux-gnu-" \
            "/usr/lib/aarch64-linux-gnu/pkgconfig"
        build_arch "amd64" "" ""
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

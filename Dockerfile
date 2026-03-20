FROM debian:bookworm-slim

# Enable multiarch for cross-compilation targets
RUN dpkg --add-architecture armhf && \
    dpkg --add-architecture arm64

# Install native build tools, cross-compilers, and target libraries.
#
# crossbuild-essential-<arch> provides the cross-compiler AND the
# target libc6-dev headers (bits/wordsize.h, etc.) in the correct
# sysroot. Without these, the cross-compiler finds host amd64
# system headers and fails on architecture-specific includes.
#
# libasound2-dev:<arch> provides target ALSA headers and libs
# via Debian multiarch paths.
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        pkg-config \
        crossbuild-essential-armhf \
        crossbuild-essential-arm64 \
        libasound2-dev \
        libasound2-dev:armhf \
        libasound2-dev:arm64 \
    && rm -rf /var/lib/apt/lists/*

# ALSA headers are architecture-independent and installed to
# /usr/include/alsa/ by libasound2-dev. The cross-compilers do
# not search /usr/include/ (that would pull in host amd64 system
# headers and break with bits/wordsize.h mismatch).
#
# Symlink ALSA headers into each cross sysroot so the cross-compiler
# finds them naturally alongside the target libc headers.
RUN ln -s /usr/include/alsa /usr/arm-linux-gnueabihf/include/alsa && \
    ln -s /usr/include/alsa /usr/aarch64-linux-gnu/include/alsa

WORKDIR /build
COPY libasound_module_pcm_keepalive.c Makefile ./

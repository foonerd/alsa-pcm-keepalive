FROM debian:bookworm-slim

# Cross-compilation toolchains and ALSA dev libraries for all targets
RUN dpkg --add-architecture armhf && \
    dpkg --add-architecture arm64 && \
    apt-get update && \
    apt-get install -y --no-install-recommends \
        build-essential \
        gcc-arm-linux-gnueabihf \
        gcc-aarch64-linux-gnu \
        libasound2-dev \
        libasound2-dev:armhf \
        libasound2-dev:arm64 \
        pkg-config \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY libasound_module_pcm_keepalive.c Makefile ./

# armhf needs explicit pkg-config path for cross-compile
ENV PKG_CONFIG_PATH_armhf=/usr/lib/arm-linux-gnueabihf/pkgconfig
ENV PKG_CONFIG_PATH_arm64=/usr/lib/aarch64-linux-gnu/pkgconfig

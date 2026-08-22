FROM debian:bookworm-slim

RUN dpkg --add-architecture arm64 && \
    apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        binutils-aarch64-linux-gnu \
        bzip2 \
        ca-certificates \
        cmake \
        curl \
        g++ \
        g++-aarch64-linux-gnu \
        gcc \
        gcc-aarch64-linux-gnu \
        git \
        libasound2-dev:arm64 \
        libegl-dev:arm64 \
        libudev-dev:arm64 \
        libvulkan-dev:arm64 \
        libwayland-dev:arm64 \
        libxkbcommon-dev:arm64 \
        make \
        ninja-build \
        pkg-config \
        python3 \
        wayland-protocols \
        xz-utils && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /src

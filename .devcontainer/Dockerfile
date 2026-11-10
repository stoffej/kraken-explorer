# Build environment for Kraken Explorer: the same Ubuntu and packages as CI
# (.github/workflows/cmake.yml, BUILD_DEPS + GUI_DEPS), plus git for the version tag and ccache.
# scripts/build_docker.sh builds in it without the devcontainer CLI.
#
# Pinned so every machine gets the same compiler and libraries: the base image by digest, the
# packages by an Ubuntu archive snapshot (snapshot.ubuntu.com). Bump both together, on purpose.
FROM ubuntu:24.04@sha256:fe81560990fae86dc4743202b2cb4d3df98d0bcf949173cecef15ce1127ef852

ARG UBUNTU_SNAPSHOT=20261110T000000Z
# The snapshot is https-only: CA certificates first, from the plain archive (they never reach a build).
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends ca-certificates \
    && apt-get update --snapshot "$UBUNTU_SNAPSHOT" \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends --snapshot "$UBUNTU_SNAPSHOT" \
        build-essential cmake ninja-build pkg-config git ca-certificates ccache \
        libusb-1.0-0-dev zlib1g-dev libnl-3-dev libnl-route-3-dev python3-dev pybind11-dev libgl-dev \
        libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev \
        libwayland-dev libxkbcommon-dev wayland-protocols \
    && rm -rf /var/lib/apt/lists/*

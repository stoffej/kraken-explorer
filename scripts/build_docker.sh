#!/bin/bash
# Builds in the .devcontainer image (Ubuntu 24.04, CI's packages) into build-dc/, as your own
# user, with the host's ccache directory. nice 10, -j$(nproc) of the machine it runs on (JOBS overrides).
#   scripts/build_docker.sh                       configure (first time) + build everything
#   scripts/build_docker.sh --target kraken-explorer
#   scripts/build_docker.sh ctest --test-dir build-dc --output-on-failure -j"$(nproc)"
# Host network: the vcan / can interfaces are visible to the tests.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
IMAGE=kraken-explorer-build
mkdir -p "$HOME/.cache/ccache"
docker build -q -t "$IMAGE" "$ROOT/.devcontainer" >/dev/null
if [ $# -gt 0 ] && [ "${1#-}" = "$1" ]; then
    CMD=("$@")                                   # a command of its own (ctest, ...)
else
    CMD=(sh -c '[ -f build-dc/build.ninja ] || cmake -S . -B build-dc -G Ninja; exec cmake --build build-dc -j"${JOBS:-$(nproc)}" "$@"' sh "$@")
fi
exec docker run --rm --net=host --user "$(id -u):$(id -g)" -e HOME=/tmp -e JOBS="${JOBS:-}" -e CCACHE_DIR=/ccache \
    -v "$ROOT:$ROOT" -v "$HOME/.cache/ccache:/ccache" -w "$ROOT" \
    "$IMAGE" nice -n 10 "${CMD[@]}"

#!/usr/bin/env bash
# Build folly from source and install it into third_party/folly-install, so that
# `cmake -DWITH_FOLLY=ON` (see scripts/build.sh) can find it.
#
# Usage:
#   ./scripts/setup_folly.sh                 # build + install (deps must already be present)
#   ./scripts/setup_folly.sh --install-deps  # also `sudo apt-get install` the dependencies
#
# Environment:
#   FOLLY_REF   folly git tag/branch to build   (default: v2026.09.14.00)
#   CXX         C++ compiler; folly needs C++20, i.e. GCC >= 11 (12+ preferred), e.g. CXX=g++-12
#   JOBS        parallel build jobs             (default: all cores)
#
# Needs internet access (clones github.com/facebook/folly). Takes ~10-30 minutes.
set -euo pipefail

FOLLY_REF="${FOLLY_REF:-v2026.09.14.00}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/third_party/folly-src"
PREFIX="$ROOT/third_party/folly-install"
JOBS="${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu)}"

if [[ "${1:-}" == "--install-deps" ]]; then
    sudo apt-get update
    sudo apt-get install -y g++ cmake git libboost-all-dev libevent-dev \
        libdouble-conversion-dev libgoogle-glog-dev libgflags-dev libfmt-dev \
        libssl-dev libsodium-dev libzstd-dev liblz4-dev liblzma-dev libsnappy-dev \
        zlib1g-dev libiberty-dev libunwind-dev
fi

mkdir -p "$ROOT/third_party"
if [[ ! -d "$SRC/.git" ]]; then
    git clone --depth 1 --branch "$FOLLY_REF" https://github.com/facebook/folly.git "$SRC"
fi

cmake -S "$SRC" -B "$SRC/_build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DBUILD_TESTS=OFF
cmake --build "$SRC/_build" -j"$JOBS"
cmake --install "$SRC/_build"

echo
echo "folly installed to $PREFIX"
echo "Next: ./scripts/build.sh"

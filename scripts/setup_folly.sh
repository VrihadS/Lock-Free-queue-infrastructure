#!/usr/bin/env bash
# Build folly from source and install it into third_party/folly-install, so that
# `cmake -DWITH_FOLLY=ON` (see scripts/build.sh) can find it.
#
# Usage:
#   ./scripts/setup_folly.sh                 # build + install (deps must already be present)
#   ./scripts/setup_folly.sh --install-deps  # install dependencies via dnf or apt (then exits on dnf; re-run)
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

# Newer compiler on RHEL-family distros (Rocky/Alma/RHEL ship an old default g++).
# build.sh does the same, so folly and the benchmark use the same toolchain.
if [[ -f /opt/rh/gcc-toolset-12/enable ]]; then
    # shellcheck disable=SC1091
    source /opt/rh/gcc-toolset-12/enable
fi

if [[ "${1:-}" == "--install-deps" ]]; then
    if command -v dnf >/dev/null; then
        # Rocky / Alma / RHEL / Fedora
        sudo dnf install -y epel-release dnf-plugins-core
        sudo dnf config-manager --set-enabled crb 2>/dev/null \
            || sudo dnf config-manager --set-enabled powertools   # Rocky 9 / Rocky 8 name
        sudo dnf install -y gcc-toolset-12-gcc-c++ cmake git boost-devel libevent-devel \
            double-conversion-devel glog-devel gflags-devel fmt-devel openssl-devel \
            libsodium-devel libzstd-devel lz4-devel xz-devel snappy-devel zlib-devel \
            binutils-devel libunwind-devel
        echo "Installed gcc-toolset-12. Re-run this script without --install-deps"
        echo "(it will pick the new compiler up automatically)."
        exit 0
    elif command -v apt-get >/dev/null; then
        sudo apt-get update
        sudo apt-get install -y g++ cmake git libboost-all-dev libevent-dev \
            libdouble-conversion-dev libgoogle-glog-dev libgflags-dev libfmt-dev \
            libssl-dev libsodium-dev libzstd-dev liblz4-dev liblzma-dev libsnappy-dev \
            zlib1g-dev libiberty-dev libunwind-dev
    else
        echo "No dnf or apt-get found; install folly's dependencies manually." >&2
        exit 1
    fi
fi

echo "Using compiler: $(${CXX:-c++} --version | head -1)"

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

#!/usr/bin/env bash
# Configure and build everything into ./build (override with BUILD_DIR=...).
# If folly was installed by scripts/setup_folly.sh, the benchmark includes
# folly::MPMCQueue; otherwise it builds without it.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# Same toolchain as setup_folly.sh (must match the compiler folly was built with).
if [[ -f /opt/rh/gcc-toolset-12/enable ]]; then
    # shellcheck disable=SC1091
    source /opt/rh/gcc-toolset-12/enable
fi
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
FOLLY_PREFIX="$ROOT/third_party/folly-install"
JOBS="${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu)}"

ARGS=(-DCMAKE_BUILD_TYPE=Release)
if [[ -d "$FOLLY_PREFIX" ]]; then
    ARGS+=(-DWITH_FOLLY=ON -DCMAKE_PREFIX_PATH="$FOLLY_PREFIX")
else
    echo "NOTE: $FOLLY_PREFIX not found -> building WITHOUT folly (run scripts/setup_folly.sh first)"
fi

cmake -S "$ROOT" -B "$BUILD_DIR" "${ARGS[@]}"
cmake --build "$BUILD_DIR" -j"$JOBS"

echo
echo "Built. Edit bench.conf to set cores, then run:"
echo "  $BUILD_DIR/bench_nblfq $ROOT/bench.conf"

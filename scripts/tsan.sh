#!/usr/bin/env bash
# Build with ThreadSanitizer + UBSan (the Debug configuration) into ./build_tsan and run
# the tests. Any sanitizer report makes the run fail.
#
#   ./scripts/tsan.sh            # test_spsc and test_nblfq
#   ./scripts/tsan.sh --bench    # also a short bench_nblfq run (covers the Vyukov queue,
#                                # which has no unit tests); uses cores 1-4, no pinning needed
#
# Sanity check that TSan works on this machine before trusting a clean result:
#   cat > race.cpp <<'X'
#   #include <thread>
#   int x; int main(){ std::thread a([]{for(int i=0;i<1000;++i)++x;}),
#                      b([]{for(int i=0;i<1000;++i)++x;}); a.join(); b.join(); }
#   X
#   c++ -std=c++17 -g -fsanitize=thread race.cpp -o race && ./race   # must print a data race
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
B="${BUILD_DIR:-$ROOT/build_tsan}"
JOBS="${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu)}"

# Same toolchain as the other scripts (RHEL-family default g++ is old).
if [[ -f /opt/rh/gcc-toolset-12/enable ]]; then
    # shellcheck disable=SC1091
    source /opt/rh/gcc-toolset-12/enable
fi

targets=(test_spsc test_nblfq)
[[ "${1:-}" == "--bench" ]] && targets+=(bench_nblfq)

cmake -S "$ROOT" -B "$B" -DCMAKE_BUILD_TYPE=Debug >/dev/null
cmake --build "$B" -j"$JOBS" --target "${targets[@]}"

for t in test_spsc test_nblfq; do
    echo "== $t (TSan + UBSan)"
    "$B/$t"
done

if [[ "${1:-}" == "--bench" ]]; then
    conf="$(mktemp)"
    trap 'rm -f "$conf"' EXIT
    printf 'producer_cores = 1,2\nconsumer_cores = 3,4\nops = 20000\n' > "$conf"
    echo "== bench_nblfq, short run (TSan + UBSan)"
    "$B/bench_nblfq" "$conf" > /dev/null
fi
echo "No sanitizer reports."

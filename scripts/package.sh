#!/usr/bin/env bash
# Package the built benchmarks plus every non-glibc shared library they need into
# dist/bench_bundle.tar.gz, so they can run on a machine with NO build tools, NO folly
# and NO root: just untar and run. Linux only. Run on the machine you built on, after
# scripts/build.sh.
#
# On the target machine:
#   tar xzf bench_bundle.tar.gz && cd bench_bundle
#   $EDITOR bench.conf          # set the isolated cores
#   ./run.sh                    # or ./run.sh path/to/other.conf
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
OUT="$ROOT/dist/bench_bundle"

[[ "$(uname)" == "Linux" ]] || { echo "package.sh must run on Linux (needs ldd)" >&2; exit 1; }
for b in bench_nblfq bench_spsc test_nblfq test_spsc; do
    [[ -x "$BUILD_DIR/$b" ]] || { echo "missing $BUILD_DIR/$b; run scripts/build.sh first" >&2; exit 1; }
done

rm -rf "$OUT" && mkdir -p "$OUT/lib"
cp "$BUILD_DIR"/{bench_nblfq,bench_spsc,test_nblfq,test_spsc} "$OUT/"
cp "$ROOT/bench.conf" "$OUT/"

# glibc itself must come from the target system; everything else is bundled.
SKIP='/(ld-linux[^/]*|libc|libm|libdl|librt|libpthread|libutil|libresolv|libnsl)\.so[^/]*$'
for b in bench_nblfq bench_spsc test_nblfq test_spsc; do
    ldd "$OUT/$b" | awk '/=> \// {print $3}' | while read -r lib; do
        [[ "$lib" =~ $SKIP ]] && continue
        cp -n "$lib" "$OUT/lib/"
    done
done
if ldd "$OUT/bench_nblfq" | grep -q 'not found'; then
    echo "WARNING: unresolved libraries:" >&2; ldd "$OUT/bench_nblfq" | grep 'not found' >&2
fi

cat > "$OUT/run.sh" <<'RUN'
#!/usr/bin/env bash
# Runs the bundled benchmark using the bundled libraries only for this process.
# Nothing is installed or modified outside this directory.
DIR="$(cd "$(dirname "$0")" && pwd)"
export LD_LIBRARY_PATH="$DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$DIR/bench_nblfq" "${1:-$DIR/bench.conf}"
RUN
chmod +x "$OUT/run.sh"

# Record the build host's glibc: the target must have the same or newer.
{ echo "built on: $(hostname)"; ldd --version 2>&1 | sed -n 1p; } > "$OUT/BUILD_INFO.txt"

tar -C "$ROOT/dist" -czf "$ROOT/dist/bench_bundle.tar.gz" bench_bundle
echo "Created $ROOT/dist/bench_bundle.tar.gz ($(du -sh "$ROOT/dist/bench_bundle.tar.gz" | cut -f1))"
echo "Target needs glibc >= build host's (see BUILD_INFO.txt) and x86_64."

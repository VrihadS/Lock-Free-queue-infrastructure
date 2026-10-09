#!/usr/bin/env bash
# Remove everything this repo's scripts created INSIDE the repo directory:
#   third_party/ (folly sources + install), build/, build_*/, dist/, run_*.txt
# It never touches anything outside the repo and never uses sudo.
#
# Usage:
#   ./scripts/clean.sh        # lists what it would delete and asks for confirmation
#   ./scripts/clean.sh -y     # no prompt
#
# System packages installed by `setup_folly.sh --install-deps` are NOT removed here;
# see the reminder printed at the end.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# Refuse to run unless ROOT really is this project.
if [[ ! -f "$ROOT/CMakeLists.txt" || ! -d "$ROOT/scripts" || ! -f "$ROOT/include/nblfq.hpp" ]]; then
    echo "Refusing to clean: $ROOT does not look like the queue repo." >&2
    exit 1
fi

if pgrep -x -l 'bench_nblfq|bench_spsc' >/dev/null; then
    echo "A benchmark is still running; stop it first:" >&2
    pgrep -x -l 'bench_nblfq|bench_spsc' >&2
    exit 1
fi

shopt -s nullglob
targets=()
for p in "$ROOT/third_party" "$ROOT/build" "$ROOT"/build_* "$ROOT/dist" "$ROOT"/run_*.txt; do
    [[ -e "$p" ]] && targets+=("$p")
done

if [[ ${#targets[@]} -eq 0 ]]; then
    echo "Nothing to clean."
else
    echo "Will delete:"
    for t in "${targets[@]}"; do
        echo "  $(du -sh "$t" 2>/dev/null | cut -f1)	$t"
    done
    if [[ "${1:-}" != "-y" ]]; then
        read -r -p "Proceed? [y/N] " ans
        [[ "$ans" == "y" || "$ans" == "Y" ]] || { echo "Aborted."; exit 1; }
    fi
    rm -rf -- "${targets[@]}"
    echo "Done."
fi

cat <<'MSG'

Not removed (system-wide, needs sudo, review before running):
  sudo dnf history list | head
  sudo dnf history info <ID>
  sudo dnf history undo <ID>       # answer N first to review the package list
  sudo dnf config-manager --set-disabled crb   # only if CRB was off before (powertools on Rocky 8)
See README.md, "Uninstalling".
MSG

# Running on a server

Three situations, from simplest to most restricted.

## 1. A machine where you can install packages and build

```bash
# Optional: build folly so bench_nblfq includes folly::MPMCQueue (once, ~10-30 min,
# needs internet). On Rocky/RHEL this also installs gcc-toolset-12 and exits; re-run it.
./scripts/setup_folly.sh --install-deps      # uses sudo (dnf or apt-get)
./scripts/setup_folly.sh

$EDITOR bench.conf                           # cores, see below
./scripts/build.sh                           # uses folly automatically if it was set up
./build/test_nblfq && ./build/test_spsc
./build/bench_nblfq bench.conf
```

Folly needs C++20, i.e. GCC 11 or newer (12+ preferred). Only `bench_nblfq` is built as
C++20 and only when folly is enabled. Without `setup_folly.sh`, `build.sh` builds
everything except the folly comparison, and the output header says so.

## 2. Configuring cores: `bench.conf`

```
producer_cores = 14, 15, 8, 9
consumer_cores = 10, 11, 12, 13
ops = 1000000
```

- Producer `i` is pinned to `producer_cores[i]` and consumer `i` to `consumer_cores[i]`.
- 1P+1C (and the SPSC reference) use the first entry of each list. The 2P+2C and 4P+4C runs
  are skipped unless each list has at least 2 / 4 entries.
- A core may not appear twice. Pinning failures print a `WARNING`; discard such a run.
- Find isolated cores with `cat /sys/devices/system/cpu/isolated`, and hyperthread
  siblings with `lscpu -e`. The benchmark spins at 100% on every core it uses, so pick
  cores that nothing else relies on.
- `bench_spsc` does not read `bench.conf`: pass cores as arguments,
  `./build/bench_spsc 500000 <producer core> <consumer core>`. Its defaults are cores 2
  and 4; make sure those are cores you intend to use, or pass your own.

## 3. A machine with no root and no toolchain (for example production)

Build on a dev machine of the same OS and architecture, then ship a self-contained bundle.

```bash
# build machine (Linux)
./scripts/build.sh
./scripts/package.sh                         # -> dist/bench_bundle.tar.gz
sha256sum dist/bench_bundle.tar.gz

# target machine, as an ordinary user
tar xzf bench_bundle.tar.gz && cd bench_bundle
./test_nblfq && ./test_spsc                  # confirms the libraries load
$EDITOR bench.conf
./run.sh
```

- The bundle holds the binaries, `bench.conf`, and every shared library the binaries need
  except glibc. `run.sh` sets `LD_LIBRARY_PATH` for its own process only; nothing is
  installed or modified outside the unpacked folder.
- The target needs the same or a newer glibc than the build machine (see
  `BUILD_INFO.txt`) and the same CPU architecture. The build uses `-march=native`, so a
  different CPU generation can fail with `Illegal instruction`.
- Check that the cores you choose are idle before running:
  `ps -eLo psr,pcpu,comm | awk '$1==14 || $1==10'` (substitute your cores).
- Remove everything with `rm -rf bench_bundle bench_bundle.tar.gz`.

## Uninstalling

Everything the scripts create lives inside the repo folder, except system packages from
`setup_folly.sh --install-deps`.

```bash
./scripts/clean.sh          # deletes third_party/, build/, build_*/, dist/, run_*.txt (asks first; -y skips)
```

System packages (needs sudo; on a production machine review each step):

```bash
sudo dnf history list | head -10     # find the install transactions
sudo dnf history info <ID>           # see exactly what each installed
sudo dnf history undo <ID>           # newest first; answer N to review the list, then repeat and answer y
# only if CRB was disabled before you started (powertools on Rocky 8):
sudo dnf config-manager --set-disabled crb
```

`undo` removes only what that transaction installed (with its dependencies), so tools that
were already present are left alone. Avoid `dnf history rollback`. `gcc-toolset-12` lives
in `/opt/rh` and does not change the default `gcc`.

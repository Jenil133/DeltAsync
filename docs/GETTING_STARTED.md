# Getting started

Every command on this page was run as written from the project folder. The
numbers shown next to commands are what those runs printed; your timings
will differ, the byte counts will not, except for the interrupted push,
whose total depends on where the cut landed.

## Requirements

- A C++17 compiler (Apple clang, GCC 13 or later, Clang 18 or later are used
  regularly), CMake 3.20 or later and Ninja.
- Nothing at runtime beyond the C++ standard library, POSIX sockets and
  threads. SHA-256 is implemented in tree.
- GoogleTest 1.17.0 is downloaded by CMake for the tests
  (`tests/CMakeLists.txt`, with a pinned SHA-256 of the archive).
- Python 3 for the helper scripts and their self-test; Docker for the Linux
  runs and the `tc netem` benchmark.

## Build

The presets in `CMakePresets.json` build into `build/<preset>/`:

| Preset | Configuration |
| --- | --- |
| `debug` | `-DCMAKE_BUILD_TYPE=Debug` |
| `release` | `-DCMAKE_BUILD_TYPE=Release` |
| `asan` | Debug, `-O1`, `-fsanitize=address,undefined` |
| `tsan` | Debug, `-O1`, `-fsanitize=thread` |

```sh
cmake --preset release
cmake --build --preset release -j 4
build/release/deltasync --version      # deltasync 0.1.0
```

Without presets (for example inside the Docker image, whose CMake is 3.28):

```sh
cmake -S . -B build/plain -G Ninja -DCMAKE_BUILD_TYPE=Release -DDELTASYNC_BUILD_TESTS=OFF
cmake --build build/plain -j 4
```

`DELTASYNC_BUILD_TESTS=OFF` skips the GoogleTest download; `DELTASYNC_SANITIZE=address,undefined`
or `thread` adds the sanitizer flags to every target, GoogleTest included.

## Test

```sh
cmake --preset debug && cmake --build --preset debug -j 4
ctest --preset debug -j 4              # 100% tests passed out of 318
ctest --preset debug-stress -j 2       # 100% tests passed out of 4
```

Tests carry two labels. `unit` is everything in the default run: the
GoogleTest suites, the allocation-counting binary and `scripts.selftest`
(the Python helpers). `stress` is four long tests (a 1 GiB and a
4 GiB + 3 byte SHA-256 message, 3,000 random edit sequences, scattered
edits to a 256 MiB file), which the `<preset>-stress` test presets select
and every other preset excludes:

```sh
ctest --preset debug -L unit -N        # lists the 318 default tests
ctest --preset debug -LE stress -j 4   # same as the default run
ctest --preset debug -L stress -j 2    # same as debug-stress
ctest --preset release -R LargeFileTest -V   # the 64 MiB / 4 KiB edit numbers
```

The last command prints, among its output:

```
overwrite: file_bytes=67108864 block_size=8192 signature_bytes=163853 delta_bytes=19059 literal_bytes=16384 copy_ops=64 chunks=64
insert: file_bytes=67108864 block_size=8192 signature_bytes=163853 delta_bytes=15002 literal_bytes=12288 copy_ops=65 chunks=65
delete: file_bytes=67108864 block_size=8192 signature_bytes=163853 delta_bytes=14962 literal_bytes=12288 copy_ops=64 chunks=64
```

### Sanitizers

```sh
cmake --preset asan && cmake --build --preset asan -j 4 && ctest --preset asan -j 4   # 314 passed
cmake --preset tsan && cmake --build --preset tsan -j 4 && ctest --preset tsan -j 4   # 314 passed
```

The sanitizer presets run 314 tests instead of 318: the four allocation
tests replace the global `operator new`, which the sanitizer runtimes also
do, so that binary is only built without them. The test presets set
`ASAN_OPTIONS`, `UBSAN_OPTIONS` and `TSAN_OPTIONS` to halt on the first
report, so a passing run means no report at all.

### Linux, in Docker

`docker/Dockerfile` defines the `deltasync-dev` image (Ubuntu 24.04, GCC 13
by default plus g++-14, Clang 18, CMake, Ninja, Python 3, iproute2). Build
it once, then run the suite with either compiler against the working tree:

```sh
docker build -t deltasync-dev docker/
docker run --rm --mount type=bind,source="$PWD",target=/src deltasync-dev:latest bash -c '
  cmake -S /src -B /src/build/linux-gcc-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=g++ &&
  cmake --build /src/build/linux-gcc-debug -j 4 &&
  ctest --test-dir /src/build/linux-gcc-debug -LE stress -j 4 &&
  ctest --test-dir /src/build/linux-gcc-debug -L stress -j 2'
```

Use `-DCMAKE_CXX_COMPILER=clang++` (and another build directory) for Clang.
Both compilers pass 318 default and 4 stress tests.

## Command line

`deltasync --help` lists every command and option. Sizes take `K`, `M` and
`G` suffixes (powers of 1024). Exit status is 0 on success, 1 when the
operation failed (an integrity, format, I/O or network error) and 2 for
invalid usage.

### Test files

`scripts/make_test_files.py` writes a deterministic pseudo-random file and a
copy with one edit (an overwrite by default; `--edit-kind insert|delete`),
and prints both SHA-256 digests:

```sh
python3 scripts/make_test_files.py --size 16M --edit-size 4K --base old.bin --edited new.bin
```

### Signature, delta, patch

The three local steps, like `rdiff`; `-s`/`--stats` prints one line of
`key=value` pairs to standard error:

```sh
build/release/deltasync signature --stats old.bin old.sig
build/release/deltasync delta --stats old.sig new.bin new.delta
build/release/deltasync patch --stats old.bin new.delta rebuilt.bin
cmp new.bin rebuilt.bin
```

With the 16 MiB files above (4 KiB blocks by default at that size):

```
signature: basis_bytes=16777216 block_size=4096 blocks=4096 signature_bytes=81933 seconds=0.062
delta: target_bytes=16777216 matched_bytes=16773120 literal_bytes=4096 delta_bytes=4802 copy_ops=16 literal_ops=1 chunks=16 block_size=4096 weak_hits=4095 false_matches=0 seconds=0.150
patch: output_bytes=16777216 copied_bytes=16773120 literal_bytes=4096 chunks_verified=16 seconds=0.115
```

`patch` writes to a temporary file next to the output and renames it into
place only after the whole-file digest verifies; the output may be the basis
itself. `signature --block-size SIZE` (1 byte to 16 MiB) overrides the
default policy, `--strong-len N` keeps 8 to 32 bytes of SHA-256 per block,
and `delta --chunk-size SIZE` (a multiple of 64, default 1M) sets how much
output each `CHUNK` digest covers. `SIGNATURE` and `DELTA` may be `-` for
standard input or output.

### Serve and push

The receiver holds the old versions under `--root`; the sender pushes one
file. `--port 0` picks a free port and prints it on standard output; `--once`
handles a single connection and exits with its status.

```sh
mkdir -p remote && cp old.bin remote/data.bin
build/release/deltasync serve --port 0 --root remote --bind 127.0.0.1 --once &
# prints: listening on 127.0.0.1:PORT
build/release/deltasync push new.bin --host 127.0.0.1 --port PORT --name data.bin --stats --stats-json push.json
cmp new.bin remote/data.bin
```

The sender printed, and `push.json` holds the same fields:

```
push: mode=delta file_size=16777216 bytes_sent=4966 bytes_received=82020 signature_bytes=81933 delta_bytes=4802 literal_bytes=4096 matched_bytes=16773120 resumed_from=0 attempts=1 elapsed_ms=370
```

and the receiver logged one line for the session on standard error:

```
deltasync serve: peer=127.0.0.1:PORT name=data.bin ok mode=delta file_size=16777216 had_basis=1 bytes_received=4966 bytes_sent=82020 signature_bytes=81933 delta_bytes=4802 resumed_from=0
```

`bytes_sent` and `bytes_received` are every byte written to and read from
the socket (TCP payload, frame headers and handshake included). Other push
options: `--whole-file` sends every byte (the baseline the benchmarks use),
`--block-size` asks the receiver for a signature block size, `--chunk-size`
sets the resume granularity, `--retries N` reconnects after a network
failure or a busy receiver, and `--timeout-ms N` (both commands, default
30000) is the inactivity timeout. The receiver serves up to 16 connections
at once, refuses a second push of a name that is being written with
`ERROR busy`, and stops on `SIGINT` or `SIGTERM`.

### Cutting a connection, then resuming

`deltasync_proxy` is a TCP forwarder that closes a connection after exactly
N forwarded bytes. The run below pushes a 16 MiB file through it, loses the
connection after 8 MiB, and lets `--retries` reconnect through the proxy;
`--drop-connections 1` leaves that second connection uncut:

```sh
rm -f remote/data.bin
build/release/deltasync serve --port 0 --root remote --bind 127.0.0.1 &
build/release/deltasync_proxy --listen-port 0 --target-host 127.0.0.1 --target-port PORT \
  --drop-after-bytes 8388608 --drop-direction up --drop-connections 1 --max-connections 2 &
# prints: listening on 127.0.0.1:PROXY_PORT
build/release/deltasync push new.bin --host 127.0.0.1 --port PROXY_PORT --name data.bin --whole-file --retries 3 --stats
cmp new.bin remote/data.bin
```

```
push: mode=whole-file file_size=16777216 bytes_sent=19401087 bytes_received=133 signature_bytes=0 delta_bytes=9438108 literal_bytes=9437184 matched_bytes=0 resumed_from=7340032 attempts=2 elapsed_ms=886
deltasync_proxy: connection 1 closed: up=8388608 down=56 cut=yes
deltasync_proxy: connection 2 closed: up=9438417 down=77 cut=no
```

The second attempt resumed from 7,340,032 bytes (7 MiB): the receiver had
verified and journaled seven 1 MiB chunks of the 8 MiB that crossed before
the cut, and only the remaining 9 MiB travelled again. With `--retries`,
byte counts add up all attempts, and the first attempt's share depends on
how much the sender had written when the proxy cut the connection, so
`bytes_sent` differs from run to run (three further runs printed 19,663,236,
18,876,789 and 19,401,087); `resumed_from`, `attempts` and the proxy's two
lines do not. Between attempts the receiver keeps the
verified data in a hidden `.data.bin.partial` with a 128-byte
`.data.bin.journal` next to it; both disappear when the push completes.
Other proxy options: `--listen-host A` (default 127.0.0.1),
`--drop-direction up|down|both`, `--delay-ms D` before each forwarded
piece, `--max-connections K` to exit after K connections.

## Benchmarks

The `tc netem` grid runs inside the `deltasync-dev` container with
`NET_ADMIN`, shaping only the container's loopback. `--smoke` is a
short functional check (1 MiB file, delays 0 and 20 ms, losses 0 and
1 %) written to `build/netem-smoke/netem.csv`; the two runs behind
`bench/results/` are the next two commands (about 13 and 7 minutes).
`JOBS` limits the build's parallelism.

```sh
docker build -t deltasync-dev docker/
JOBS=4 scripts/run_netem_bench_docker.sh --smoke
JOBS=4 scripts/run_netem_bench_docker.sh --size 64M --edit-size 4K --repeats 3 --out /src/build/netem-final/netem.csv
JOBS=4 scripts/run_netem_bench_docker.sh --no-netem --size 1G --edit-size 4K --repeats 3 --delays 0 --losses 0 --out /src/build/netem-final/headline.csv
python3 scripts/csv_to_markdown.py bench/results/netem.csv
python3 scripts/csv_to_markdown.py bench/results/no-netem-1g.csv
```

`--out` paths are inside the container, where the project is mounted at
`/src`; the CSVs were copied unchanged to `bench/results/`. Every other
option of `scripts/netem_bench.sh` (`--delays`, `--losses`, `--size`,
`--edit-size`, `--seed`, `--repeats`, `--drop-fraction`, `--timeout-ms`,
`--no-netem`) passes through; `scripts/netem_bench.sh --help` lists them.
What is measured, the environment and the result tables are in
[BENCHMARKS.md](BENCHMARKS.md).

## Figures

The SVGs in `docs/assets/` are generated, in a light and a dark variant
each, by two standard-library-only scripts. Rerunning them on the same CSVs
reproduces the files byte for byte:

```sh
python3 scripts/plot_netem.py bench/results/netem.csv --headline bench/results/no-netem-1g.csv
python3 scripts/draw_readme_figures.py
python3 -B scripts/test_scripts.py        # self-test of every script; also ctest -R scripts.selftest
```

`plot_netem.py` draws the bytes-on-the-wire comparison, the delay x loss
heatmap and the resume figure from the CSVs; `draw_readme_figures.py`
draws the banner, the block-grid figure (its numbers are the `LargeFileTest`
output above) and the rolling-window explainer. Both print the values they
plotted.

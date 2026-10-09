# Benchmarks

How Deltasync's transfer size and time are measured under added latency and
packet loss, and how to reproduce every number. Results are added to this
file only from runs of the commands below, together with the environment they
ran in.

## What is measured

Each point of the grid **delay x loss** runs four pushes of the same edited
file against a local `deltasync serve` on the loopback interface:

| Scenario | Receiver starts with | Command (sender side) | Shows |
| --- | --- | --- | --- |
| `whole-file` | the old version | `deltasync push target.bin --whole-file ...` | full-copy baseline over the same protocol and framing |
| `delta` | the old version | `deltasync push target.bin ...` | only the changed regions travel |
| `interrupted` | nothing | `deltasync push target.bin --whole-file --retries 0 ...` through `deltasync_proxy --drop-after-bytes N` | a transfer cut part way |
| `resumed` | the state the interrupted push left | `deltasync push target.bin --whole-file ...` directly | continuing from the last verified chunk instead of from zero |

The old version is a deterministic pseudo-random file (`--size`, default
16 MiB, seed 1). The new version overwrites `--edit-size` bytes (default
4 KiB) in its middle with new random bytes. Both come from
`scripts/make_test_files.py`, which prints their SHA-256 digests; the same
size and seed always give the same files.

The interrupted push is cut by the proxy after `--drop-fraction` (default
0.5) of the file size has been forwarded from sender to receiver. The script
waits until the receiver has logged the end of that session before the
resumed push, which then continues from the receiver's last verified chunk
(`resumed_from` in its row); a row whose push restarted at 0 says so in its
note.

### Metrics

All per-push numbers come from `deltasync push --stats-json`:

- **socket bytes** = `bytes_sent + bytes_received`: every byte the sender
  wrote to or read from its socket, including the handshake, framing,
  the receiver's signature, its `PROGRESS` frames and the final
  acknowledgement. This is TCP payload: TCP/IP headers and retransmissions
  are not counted, so under packet loss the traffic on the link is larger
  than this figure. A failed push writes no statistics, so for the
  `interrupted` row `bytes_sent` and `bytes_received` are the proxy's own
  counts of forwarded payload bytes (sender to receiver, and receiver to
  sender), and socket bytes is their sum.
- **sent** (`bytes_sent`) is the sender-to-receiver direction (handshake,
  file information and the delta or whole-file data); **received**
  (`bytes_received`) is the receiver-to-sender direction (handshake, the
  signature of the receiver's old version, `RESUME`, `PROGRESS` and the
  final acknowledgement).
- `signature_bytes`, `delta_bytes`, `literal_bytes`, `matched_bytes`,
  `resumed_from`, `attempts`: as reported by the sender.
- **push elapsed ms**: `elapsed_ms` measured inside the sender.
- **wall ms**: wall-clock time around the whole `deltasync push` process,
  measured by the script (includes process start-up and file hashing).
- **ok**: the push exited 0 and the receiver's file is byte-identical
  (`cmp`) to the sender's.

### Network emulation

The benchmark runs in a Linux container with `NET_ADMIN`, and shapes only the
container's loopback interface:

```
tc qdisc replace dev lo root netem delay <D>ms loss <L>%
```

over D in {0, 20, 50, 100} ms and L in {0, 0.5, 1, 2} %. Both directions of
a loopback connection pass through `lo`, so the round-trip time is about
2 x D and loss applies to data and acknowledgements alike. The qdisc is
removed after each grid point and when the script exits.

At high loss and delay, TCP throughput collapses (roughly
MSS / RTT x 1.22 / sqrt(p)), so whole-file transfers at the corner of the
grid take minutes; this is the effect the delta and resumed rows are meant
to avoid.

## Commands

Everything runs from the project folder. The runner builds the release
configuration inside the `deltasync-dev` image (Ubuntu 24.04; GCC 13 by
default) into `build/linux-gcc-netem-release`, then runs the grid. The image
is defined by `docker/Dockerfile` and has to be built once first; the runner
stops with a message saying so when it is missing:

```
docker build -t deltasync-dev docker/
scripts/run_netem_bench_docker.sh
python3 scripts/csv_to_markdown.py bench/results/netem.csv
```

`scripts/run_netem_bench_docker.sh` amounts to:

```
docker run --rm --cap-add NET_ADMIN \
  --mount type=bind,source="$PWD",target=/src deltasync-dev:latest bash -c '
    cmake -S /src -B /src/build/linux-gcc-netem-release -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++ -DDELTASYNC_BUILD_TESTS=OFF
    cmake --build /src/build/linux-gcc-netem-release -j 2 --target deltasync deltasync_proxy
    /src/scripts/netem_bench.sh --bin-dir /src/build/linux-gcc-netem-release'
```

Useful options (passed through to `scripts/netem_bench.sh`):

```
scripts/run_netem_bench_docker.sh --size 64M --repeats 3
scripts/run_netem_bench_docker.sh --delays "0 50" --losses "0 1"
scripts/run_netem_bench_docker.sh --smoke     # 1 MiB functional check; writes build/netem-smoke/netem.csv
CXX_NAME=clang++ scripts/run_netem_bench_docker.sh
```

`serve` and `push` run with deltasync's default 30 s inactivity timeout
unless `--timeout-ms N` is passed, so the numbers reflect what a user gets
with default settings. `scripts/netem_bench.sh --help` lists every option. Without `NET_ADMIN`,
`--no-netem` runs the same scenarios on an unshaped loopback (functional
check only).

### Output

`bench/results/netem.csv` has one row per push. The file name is fixed, and
the rows hold only the grid point, the push's statistics, timings and the
verification result: no host name, user name, path or date. Runs with
other options are written with `--out`; the 1 GiB run below is
`bench/results/no-netem-1g.csv`, in the same format.

```
delay_ms,loss_pct,scenario,run,exit_code,mode,file_size,bytes_sent,bytes_received,
socket_bytes,signature_bytes,delta_bytes,literal_bytes,matched_bytes,resumed_from,
attempts,elapsed_ms,wall_ms,verified,note
```

`scripts/csv_to_markdown.py` groups rows by (delay, loss, scenario) and
prints medians over the successful runs (the `interrupted` scenario keeps its
failed runs, since failing is the point).

## Fault-injection proxy

`deltasync_proxy` is a small TCP forwarder built with the project:

```
deltasync_proxy --listen-port P --target-host H --target-port T \
  [--listen-host A] [--drop-after-bytes N] [--drop-direction up|down|both] \
  [--drop-connections K] [--delay-ms D] [--max-connections K]
```

It forwards both directions, serves any number of connections (one thread per
direction per connection), and with `--drop-after-bytes` forwards exactly N
bytes in the counted direction(s) before shutting both sides of that
connection down. `--drop-connections K` limits the cut to the first K
connections, so a sender's automatic retry through the same proxy goes
through. With `--listen-port 0` it picks a free port and reports it on the
first line of standard output (`listening on 127.0.0.1:<port>`); each closed
connection is summarised on standard error
(`connection 1 closed: up=U down=D cut=yes`).

## Environment

Recorded with each published run: CPU model and OS, Docker version,
container image and compiler (the runner prints `uname -srm`, the compiler
version and `nproc` before the grid), the exact command line, and the
machine's load. Host names, user names and paths are left out.

## Results

Two runs, both with the release build inside the container:

- a **grid** over delay {0, 20, 50, 100} ms x loss {0, 0.5, 1, 2} % with a
  64 MiB file and a 4 KiB edit, three repetitions per point
  (`bench/results/netem.csv`, 192 rows);
- a **headline** run on an unshaped loopback with a 1 GiB file and a
  4 KiB edit, three repetitions (`bench/results/no-netem-1g.csv`, 12 rows).

Every `whole-file`, `delta` and `resumed` push in both runs exited 0 and
left the receiver's file byte-identical to the sender's (`ok` = 3 of 3 at
every point); every `interrupted` push failed, as intended. Tables show the
median of the three runs (three keeps the whole grid within about a quarter
of an hour; byte counts hardly vary between runs, times do, see the
caveats).

### Environment

- Host: Apple M5 (10 cores: 4 performance + 6 efficiency), 16 GB RAM,
  macOS 26.5.
- Docker Desktop, engine 29.7.2; its Linux VM has 10 vCPUs and 7.7 GiB of
  memory; kernel `Linux 7.0.12-linuxkit aarch64`.
- Image `deltasync-dev` (Ubuntu 24.04, `docker/Dockerfile`), g++ 13.3.0,
  `-DCMAKE_BUILD_TYPE=Release`, `nproc` 10. Test data and the receiver's
  directory live in the container's `/tmp`.
- The machine was **not idle**: another, unrelated build job was running on
  the host at the same time. Load averages (1, 5, 15 min) were 3.61 3.87
  4.67 before the grid and 5.21 6.06 5.86 after it (801 s), 4.55 5.90 5.81
  before the headline run and 6.06 8.34 7.19 after it (393 s).

### Commands

From the project folder, with the image already built
(`docker build -t deltasync-dev docker/`). `JOBS=4` only limits the
build's parallelism. The CSVs were written to `build/` first and copied
unchanged to `bench/results/`:

```
JOBS=4 scripts/run_netem_bench_docker.sh --size 64M --edit-size 4K --repeats 3 \
  --out /src/build/netem-final/netem.csv
JOBS=4 scripts/run_netem_bench_docker.sh --no-netem --size 1G --edit-size 4K --repeats 3 \
  --delays 0 --losses 0 --out /src/build/netem-final/headline.csv
python3 scripts/csv_to_markdown.py bench/results/no-netem-1g.csv
python3 scripts/csv_to_markdown.py bench/results/netem.csv
```

The grid used the script's defaults otherwise: seed 1, edit in the middle
of the file, interrupted push cut after half the file size
(`--drop-fraction 0.5`) of sender-to-receiver bytes, default 1 MiB chunks,
default 30 s inactivity timeout. `--no-netem` leaves `lo` unshaped.

### Headline: 1 GiB file, 4 KiB edit, no netem

| scenario | runs | ok | sent (sender to receiver) | received (receiver to sender) | socket bytes | resumed from | wall ms (median; min-max) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| whole-file | 3 | 3 | 1,073,866,885 | 350 | 1,073,867,235 | 0 | 29,895 (24,232-42,098) |
| delta | 3 | 3 | 80,777 | 656,352 | 737,129 | 0 | 34,484 (33,733-49,913) |
| interrupted (proxy counts) | 3 | 0 | 536,870,912 | 173 | 536,871,085 | - | 15,742 (13,427-28,646) |
| resumed | 3 | 3 | 537,982,211 | 259 | 537,982,470 | 535,822,336 | 23,273 (18,345-23,688) |

- **Delta after a 4 KiB edit:** the sender sent 80,777 bytes for a
  1,073,741,824-byte file (0.0075 %; 75,573 bytes of delta, of which
  32,768 literal and 1,073,709,056 matched). The receiver sent back its
  signature of the old version (655,375 bytes) plus control frames, so
  both directions together carried 737,129 bytes (0.069 % of the file),
  against 1,073,867,235 for the whole-file push of the same edit.
- **Interrupted, then resumed:** the proxy cut the push after
  536,870,912 sender-to-receiver bytes (512 MiB). The resumed push started
  at the receiver's last verified chunk, 535,822,336 (511 MiB) in two runs
  and 534,773,760 (510 MiB) in one, and sent 537,982,211 and 539,030,909
  bytes: 50.1 % and 50.2 % of the 1,073,866,885 bytes a full retry sends.
  Counting both attempts, 986,238 and 2,034,936 bytes more were sent than
  one uninterrupted push: the part of the cut transfer that the proxy had
  forwarded but that the receiver had not verified and checkpointed as a
  chunk when the connection closed (just under 1 MiB and 2 MiB).
- **Time:** on an unshaped loopback the network is not the bottleneck, and
  the delta push was not faster than copying the whole file (34.5 s vs
  29.9 s median). It has more local work to do: the receiver reads and
  hashes its old version to build the signature, and the sender runs the
  rolling search over the whole new file (not profiled further). The time
  gain appears once the link is slow, as in the grid below.

### Grid: 64 MiB file, 4 KiB edit, tc netem on `lo`

Byte counts are the same at every grid point, apart from the receiver's
13-byte `PROGRESS` frames (their number depends on how long it works) and
the resume offset:

| scenario | sent (sender to receiver) | received (receiver to sender) | share of the whole-file push |
| --- | ---: | ---: | ---: |
| whole-file | 67,116,804 | 77-246 | 100 % |
| delta | 11,270 | 163,983-163,996 (signature 163,853) | 0.017 % sent; 0.26 % both directions |
| interrupted (proxy counts) | 33,554,432 | 56-186 | cut at half the file size |
| resumed | 34,607,169 (46 runs), 35,655,867 (2 runs) | 77-168 | 51.6 % / 53.1 % |

The file is 67,108,864 bytes. The resumed push started at 32,505,856
(31 MiB, the last full chunk before the cut) in 46 of 48 runs, and at
31,457,280 (30 MiB) in two (0 ms / 1 % run 2 and 100 ms / 2 % run 2).
The proxy counts what it passed on to its socket towards the receiver, not
what the receiver had read and verified when the connection was torn down;
the resume offset is what the receiver had actually verified and made
durable.

Median wall time per push (ms) over three runs:

| delay ms | loss % | added RTT ms | whole-file | delta | whole-file / delta | interrupted | resumed |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 0 | 0 | 0 | 1,365 | 1,940 | 0.7x | 834 | 1,041 |
| 0 | 0.5 | 0 | 1,660 | 1,978 | 0.8x | 974 | 1,234 |
| 0 | 1 | 0 | 1,420 | 1,901 | 0.7x | 1,159 | 1,168 |
| 0 | 2 | 0 | 1,917 | 2,104 | 0.9x | 833 | 2,030 |
| 20 | 0 | 40 | 2,088 | 2,891 | 0.7x | 2,145 | 1,850 |
| 20 | 0.5 | 40 | 2,359 | 2,337 | 1.0x | 2,652 | 1,764 |
| 20 | 1 | 40 | 3,325 | 2,082 | 1.6x | 1,721 | 1,822 |
| 20 | 2 | 40 | 4,223 | 2,226 | 1.9x | 4,290 | 2,274 |
| 50 | 0 | 100 | 4,159 | 2,411 | 1.7x | 4,085 | 2,934 |
| 50 | 0.5 | 100 | 5,629 | 2,557 | 2.2x | 3,428 | 2,918 |
| 50 | 1 | 100 | 6,086 | 2,169 | 2.8x | 5,189 | 4,033 |
| 50 | 2 | 100 | 11,470 | 2,213 | 5.2x | 7,493 | 5,895 |
| 100 | 0 | 200 | 7,232 | 2,490 | 2.9x | 10,562 | 5,371 |
| 100 | 0.5 | 200 | 11,570 | 2,848 | 4.1x | 7,300 | 5,670 |
| 100 | 1 | 200 | 10,153 | 3,443 | 2.9x | 9,005 | 7,632 |
| 100 | 2 | 200 | 12,359 | 3,492 | 3.5x | 12,013 | 6,482 |

The added RTT is 2 x the netem delay, because both directions of a
loopback connection pass through `lo`; it comes on top of the loopback's
own round trip, which `ping -c 10 -i 0.2 127.0.0.1` in the same image
without netem puts at a fraction of a millisecond that varies with host
load, and which is not subtracted. "whole-file / delta" divides the two
medians. The full per-scenario table (medians of socket bytes, both
directions, resume offset, push elapsed and wall time) is the output of
`scripts/csv_to_markdown.py bench/results/netem.csv`:

<details>
<summary>Full grid table</summary>

| delay ms | loss % | scenario | runs | ok | socket bytes | sent | received | resumed from | push elapsed ms | wall ms | notes |
| ---: | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| 0 | 0 | whole-file | 3 | 3 | 67,116,881 | 67,116,804 | 77 | 0 | 1,351 | 1,365 |  |
| 0 | 0 | delta | 3 | 3 | 175,253 | 11,270 | 163,983 | 0 | 1,929 | 1,940 |  |
| 0 | 0 | interrupted | 3 | 0 | 33,554,488 | 33,554,432 | 56 | - | - | 834 |  |
| 0 | 0 | resumed | 3 | 3 | 34,607,246 | 34,607,169 | 77 | 32,505,856 | 1,034 | 1,041 |  |
| 0 | 0.5 | whole-file | 3 | 3 | 67,116,894 | 67,116,804 | 90 | 0 | 1,652 | 1,660 |  |
| 0 | 0.5 | delta | 3 | 3 | 175,253 | 11,270 | 163,983 | 0 | 1,971 | 1,978 |  |
| 0 | 0.5 | interrupted | 3 | 0 | 33,554,488 | 33,554,432 | 56 | - | - | 974 |  |
| 0 | 0.5 | resumed | 3 | 3 | 34,607,246 | 34,607,169 | 77 | 32,505,856 | 1,230 | 1,234 |  |
| 0 | 1 | whole-file | 3 | 3 | 67,116,894 | 67,116,804 | 90 | 0 | 1,415 | 1,420 |  |
| 0 | 1 | delta | 3 | 3 | 175,253 | 11,270 | 163,983 | 0 | 1,895 | 1,901 |  |
| 0 | 1 | interrupted | 3 | 0 | 33,554,488 | 33,554,432 | 56 | - | - | 1,159 |  |
| 0 | 1 | resumed | 3 | 3 | 34,607,246 | 34,607,169 | 77 | 32,505,856 | 1,165 | 1,168 |  |
| 0 | 2 | whole-file | 3 | 3 | 67,116,894 | 67,116,804 | 90 | 0 | 1,907 | 1,917 |  |
| 0 | 2 | delta | 3 | 3 | 175,253 | 11,270 | 163,983 | 0 | 2,099 | 2,104 |  |
| 0 | 2 | interrupted | 3 | 0 | 33,554,488 | 33,554,432 | 56 | - | - | 833 |  |
| 0 | 2 | resumed | 3 | 3 | 34,607,259 | 34,607,169 | 90 | 32,505,856 | 2,025 | 2,030 |  |
| 20 | 0 | whole-file | 3 | 3 | 67,116,894 | 67,116,804 | 90 | 0 | 2,076 | 2,088 |  |
| 20 | 0 | delta | 3 | 3 | 175,253 | 11,270 | 163,983 | 0 | 2,886 | 2,891 |  |
| 20 | 0 | interrupted | 3 | 0 | 33,554,501 | 33,554,432 | 69 | - | - | 2,145 |  |
| 20 | 0 | resumed | 3 | 3 | 34,607,246 | 34,607,169 | 77 | 32,505,856 | 1,843 | 1,850 |  |
| 20 | 0.5 | whole-file | 3 | 3 | 67,116,894 | 67,116,804 | 90 | 0 | 2,353 | 2,359 |  |
| 20 | 0.5 | delta | 3 | 3 | 175,253 | 11,270 | 163,983 | 0 | 2,330 | 2,337 |  |
| 20 | 0.5 | interrupted | 3 | 0 | 33,554,514 | 33,554,432 | 82 | - | - | 2,652 |  |
| 20 | 0.5 | resumed | 3 | 3 | 34,607,259 | 34,607,169 | 90 | 32,505,856 | 1,758 | 1,764 |  |
| 20 | 1 | whole-file | 3 | 3 | 67,116,907 | 67,116,804 | 103 | 0 | 3,314 | 3,325 |  |
| 20 | 1 | delta | 3 | 3 | 175,253 | 11,270 | 163,983 | 0 | 2,076 | 2,082 |  |
| 20 | 1 | interrupted | 3 | 0 | 33,554,501 | 33,554,432 | 69 | - | - | 1,721 |  |
| 20 | 1 | resumed | 3 | 3 | 34,607,259 | 34,607,169 | 90 | 32,505,856 | 1,816 | 1,822 |  |
| 20 | 2 | whole-file | 3 | 3 | 67,116,920 | 67,116,804 | 116 | 0 | 4,217 | 4,223 |  |
| 20 | 2 | delta | 3 | 3 | 175,253 | 11,270 | 163,983 | 0 | 2,216 | 2,226 |  |
| 20 | 2 | interrupted | 3 | 0 | 33,554,527 | 33,554,432 | 95 | - | - | 4,290 |  |
| 20 | 2 | resumed | 3 | 3 | 34,607,259 | 34,607,169 | 90 | 32,505,856 | 2,265 | 2,274 |  |
| 50 | 0 | whole-file | 3 | 3 | 67,116,920 | 67,116,804 | 116 | 0 | 4,150 | 4,159 |  |
| 50 | 0 | delta | 3 | 3 | 175,253 | 11,270 | 163,983 | 0 | 2,403 | 2,411 |  |
| 50 | 0 | interrupted | 3 | 0 | 33,554,527 | 33,554,432 | 95 | - | - | 4,085 |  |
| 50 | 0 | resumed | 3 | 3 | 34,607,272 | 34,607,169 | 103 | 32,505,856 | 2,927 | 2,934 |  |
| 50 | 0.5 | whole-file | 3 | 3 | 67,116,933 | 67,116,804 | 129 | 0 | 5,615 | 5,629 |  |
| 50 | 0.5 | delta | 3 | 3 | 175,253 | 11,270 | 163,983 | 0 | 2,550 | 2,557 |  |
| 50 | 0.5 | interrupted | 3 | 0 | 33,554,514 | 33,554,432 | 82 | - | - | 3,428 |  |
| 50 | 0.5 | resumed | 3 | 3 | 34,607,272 | 34,607,169 | 103 | 32,505,856 | 2,915 | 2,918 |  |
| 50 | 1 | whole-file | 3 | 3 | 67,116,946 | 67,116,804 | 142 | 0 | 6,079 | 6,086 |  |
| 50 | 1 | delta | 3 | 3 | 175,253 | 11,270 | 163,983 | 0 | 2,163 | 2,169 |  |
| 50 | 1 | interrupted | 3 | 0 | 33,554,527 | 33,554,432 | 95 | - | - | 5,189 |  |
| 50 | 1 | resumed | 3 | 3 | 34,607,285 | 34,607,169 | 116 | 32,505,856 | 4,028 | 4,033 |  |
| 50 | 2 | whole-file | 3 | 3 | 67,117,011 | 67,116,804 | 207 | 0 | 11,457 | 11,470 |  |
| 50 | 2 | delta | 3 | 3 | 175,253 | 11,270 | 163,983 | 0 | 2,208 | 2,213 |  |
| 50 | 2 | interrupted | 3 | 0 | 33,554,566 | 33,554,432 | 134 | - | - | 7,493 |  |
| 50 | 2 | resumed | 3 | 3 | 34,607,298 | 34,607,169 | 129 | 32,505,856 | 5,869 | 5,895 |  |
| 100 | 0 | whole-file | 3 | 3 | 67,116,959 | 67,116,804 | 155 | 0 | 7,220 | 7,232 |  |
| 100 | 0 | delta | 3 | 3 | 175,253 | 11,270 | 163,983 | 0 | 2,485 | 2,490 |  |
| 100 | 0 | interrupted | 3 | 0 | 33,554,566 | 33,554,432 | 134 | - | - | 10,562 |  |
| 100 | 0 | resumed | 3 | 3 | 34,607,298 | 34,607,169 | 129 | 32,505,856 | 5,344 | 5,371 |  |
| 100 | 0.5 | whole-file | 3 | 3 | 67,117,011 | 67,116,804 | 207 | 0 | 11,554 | 11,570 |  |
| 100 | 0.5 | delta | 3 | 3 | 175,253 | 11,270 | 163,983 | 0 | 2,841 | 2,848 |  |
| 100 | 0.5 | interrupted | 3 | 0 | 33,554,540 | 33,554,432 | 108 | - | - | 7,300 |  |
| 100 | 0.5 | resumed | 3 | 3 | 34,607,298 | 34,607,169 | 129 | 32,505,856 | 5,655 | 5,670 |  |
| 100 | 1 | whole-file | 3 | 3 | 67,116,985 | 67,116,804 | 181 | 0 | 10,138 | 10,153 |  |
| 100 | 1 | delta | 3 | 3 | 175,266 | 11,270 | 163,996 | 0 | 3,424 | 3,443 |  |
| 100 | 1 | interrupted | 3 | 0 | 33,554,566 | 33,554,432 | 134 | - | - | 9,005 |  |
| 100 | 1 | resumed | 3 | 3 | 34,607,311 | 34,607,169 | 142 | 32,505,856 | 7,608 | 7,632 |  |
| 100 | 2 | whole-file | 3 | 3 | 67,117,011 | 67,116,804 | 207 | 0 | 12,341 | 12,359 |  |
| 100 | 2 | delta | 3 | 3 | 175,266 | 11,270 | 163,996 | 0 | 3,483 | 3,492 |  |
| 100 | 2 | interrupted | 3 | 0 | 33,554,592 | 33,554,432 | 160 | - | - | 12,013 |  |
| 100 | 2 | resumed | 3 | 3 | 34,607,337 | 34,607,169 | 142 | 32,505,856 | 6,466 | 6,482 |  |

</details>

### Interpretation

- **Bytes.** After a 4 KiB edit, the delta push sends kilobytes instead of
  the file: 11,270 bytes for 64 MiB and 80,777 bytes for 1 GiB, against
  67,116,804 and 1,073,866,885 for the whole-file push. The larger share of
  the traffic flows the other way, as the receiver's signature of its old
  version (163,853 and 655,375 bytes), so the total for both directions is
  0.26 % and 0.069 % of the file. These counts do not depend on delay, loss
  or machine load.
- **Time.** Without added delay the delta push is slower than the
  whole-file copy (0.7x to 0.9x at 0 ms; 34.5 s vs 29.9 s for 1 GiB):
  loopback moves bytes faster than the extra local work of signature and
  rolling search. As the RTT grows, the whole-file push slows down roughly
  with the RTT and, at 50 and 100 ms, with loss (TCP's window growth and
  loss recovery both work per round trip), while the delta push stays
  between 1.9 and 3.5 s (medians) across the whole grid. At 100 ms of RTT
  the delta push was 1.7x to 5.2x faster than the whole-file copy, and at
  200 ms 2.9x to 4.1x. With
  added delay the wall times are dominated by the emulated network, not by
  CPU, so the concurrent load matters less there than at 0 ms.
- **Resume.** An interrupted push loses at most the data that was not yet
  in a verified, checkpointed chunk: the resumed push started at the last
  verified 1 MiB chunk before the cut (one chunk earlier in 2 of 48 grid
  runs and 1 of 3 headline runs) and sent 51.6-53.1 % (64 MiB) or
  50.1-50.2 % (1 GiB) of the bytes of a full retry. Its median wall time
  was 49 % to 89 % of the whole-file push at the same grid point, except at
  0 ms / 2 % (106 %, within the run-to-run spread there). Time does not
  halve with the bytes, partly because the sender hashes the whole file
  before connecting and the receiver rehashes its verified prefix before
  answering with the resume offset.

### Caveats

- **Socket payload, not link traffic.** All byte counts are TCP payload
  read or written by the sender (or forwarded by the proxy). TCP/IP
  headers, acknowledgements and retransmissions are not counted, so under
  loss the bytes on the link are higher than these figures.
- **Loopback emulation.** netem shapes the container's `lo`, so delay and
  loss apply to both directions, including acknowledgements; the RTT is
  2 x the configured delay. There is no bandwidth limit: at 0 ms the
  "network" is a memory copy, which is why the delta push loses there.
- **Busy machine.** Another unrelated build job ran on the host during both
  runs (load averages above), and the times include process start-up,
  hashing the file before connecting, and the receiver's per-chunk `fsync`.
  Wall times at 0 ms and in the headline run are CPU- and disk-bound and
  vary between runs (whole-file 1 GiB: 24.2 s to 42.1 s); the byte counts
  are unaffected.
- **Three runs.** Medians of three runs at each point; under loss the
  spread is wide (whole-file 64 MiB at 100 ms / 1 %: 9,658 to 14,794 ms),
  so individual ratios in the time table are approximate, but the trend
  with RTT is consistent.
- **One edit pattern.** One 4 KiB overwrite in the middle of a random file;
  edits spread across many blocks, inserts that shift data, or
  compressible data give different deltas.
- **Resume granularity is the chunk.** "Last verified block" means the last
  verified chunk (1 MiB by default, made of the blocks and literal data
  the delta wrote), not a single signature block. The interrupted pushes
  here are whole-file pushes to an empty receiver; a resumed delta push
  also resends the receiver's signature on each attempt.

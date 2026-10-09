#!/usr/bin/env bash
# Measures deltasync on loopback under emulated latency and packet loss.
#
# Runs inside a Linux container started with --cap-add NET_ADMIN (see
# scripts/run_netem_bench_docker.sh). For every point of the grid
# delay x loss it sets
#
#   tc qdisc replace dev lo root netem delay <D>ms loss <L>%
#
# and runs four pushes against a local `deltasync serve`:
#
#   whole-file   push --whole-file of the edited file (full-copy baseline over
#                the same protocol; the receiver holds the old version)
#   delta        push of the edited file; the receiver holds the old version
#   interrupted  push of the whole edited file to an empty receiver through
#                deltasync_proxy, which cuts the connection after
#                --drop-after-bytes bytes (sender to receiver)
#   resumed      the same push again, directly; it should continue from the
#                receiver's last verified chunk
#
# Socket byte counts and times come from `push --stats-json`; for the
# interrupted attempt, which writes no statistics, the proxy's counts of
# forwarded bytes are used instead (up = sender to receiver, down = receiver
# to sender). Socket bytes are TCP payload: TCP/IP headers and
# retransmissions are not included. Every row also records whether the receiver's file is
# byte-identical to the sender's afterwards.
#
# netem on lo applies to both directions, so a delay of D ms gives a round
# trip of about 2*D ms, and loss L applies to packets in both directions.
#
# Usage: scripts/netem_bench.sh [options]
#   --bin-dir DIR       directory holding deltasync and deltasync_proxy (required)
#   --out FILE          CSV to write (default bench/results/netem.csv)
#   --work-dir DIR      scratch directory (default: a new directory under $TMPDIR)
#   --delays "LIST"     delays in ms (default "0 20 50 100")
#   --losses "LIST"     loss in percent (default "0 0.5 1 2")
#   --size SIZE         test file size (default 16M)
#   --edit-size SIZE    size of the edit for the delta push (default 4K)
#   --seed N            test data seed (default 1)
#   --repeats N         runs per grid point and scenario (default 1)
#   --drop-fraction F   cut the interrupted push after F * file size bytes
#                       (default 0.5)
#   --timeout-ms N      passed to serve and push (default: not passed, so both
#                       use deltasync's own 30 s inactivity timeout)
#   --port N            server port (default 0: any free port; the proxy
#                       always picks a free port)
#   --no-netem          skip tc (for a functional smoke run without NET_ADMIN)
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
project_dir=$(cd "$script_dir/.." && pwd)

bin_dir=""
out="$project_dir/bench/results/netem.csv"
work_dir=""
delays="0 20 50 100"
losses="0 0.5 1 2"
size="16M"
edit_size="4K"
seed=1
repeats=1
drop_fraction="0.5"
timeout_ms=""
port=0
use_netem=1

die() { echo "netem_bench: $*" >&2; exit 1; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --bin-dir) bin_dir=$2; shift 2 ;;
    --out) out=$2; shift 2 ;;
    --work-dir) work_dir=$2; shift 2 ;;
    --delays) delays=$2; shift 2 ;;
    --losses) losses=$2; shift 2 ;;
    --size) size=$2; shift 2 ;;
    --edit-size) edit_size=$2; shift 2 ;;
    --seed) seed=$2; shift 2 ;;
    --repeats) repeats=$2; shift 2 ;;
    --drop-fraction) drop_fraction=$2; shift 2 ;;
    --timeout-ms) timeout_ms=$2; shift 2 ;;
    --port) port=$2; shift 2 ;;
    --no-netem) use_netem=0; shift ;;
    -h|--help) sed -n '2,/^set -euo/p' "${BASH_SOURCE[0]}" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
    *) die "unknown option: $1" ;;
  esac
done

[[ -n "$bin_dir" ]] || die "--bin-dir is required"
deltasync="$bin_dir/deltasync"
proxy="$bin_dir/deltasync_proxy"
[[ -x "$deltasync" ]] || die "not executable: $deltasync"
[[ -x "$proxy" ]] || die "not executable: $proxy"
command -v python3 >/dev/null || die "python3 is required"

if [[ -z "$work_dir" ]]; then
  work_dir=$(mktemp -d "${TMPDIR:-/tmp}/netem_bench.XXXXXX")
fi
mkdir -p "$work_dir" "$(dirname "$out")"
server_root="$work_dir/server"
data_dir="$work_dir/data"
stats_dir="$work_dir/stats"
mkdir -p "$server_root" "$data_dir" "$stats_dir"

server_pid=""
proxy_pid=""
cleanup() {
  [[ -n "$proxy_pid" ]] && kill "$proxy_pid" 2>/dev/null || true
  [[ -n "$server_pid" ]] && kill "$server_pid" 2>/dev/null || true
  wait 2>/dev/null || true
  if [[ $use_netem -eq 1 ]]; then
    tc qdisc del dev lo root 2>/dev/null || true
  fi
}
trap cleanup EXIT

# Milliseconds from a monotonic-enough clock. GNU date is used when it
# supports %N (Linux); python3 otherwise, at the cost of its start-up time.
if [[ $(date +%N 2>/dev/null) =~ ^[0-9]+$ ]]; then
  now_ms() { echo $(( $(date +%s%N) / 1000000 )); }
else
  now_ms() { python3 -c 'import time; print(int(time.time() * 1000))'; }
fi

# Waits (bounded) for the "listening on" line that deltasync serve and
# deltasync_proxy print once their socket is ready. Connecting to probe the
# port instead would count as a real client.
wait_for_listening() {
  local out=$1 pid=$2 i
  for ((i = 0; i < 400; i++)); do
    grep -q '^listening on ' "$out" 2>/dev/null && return 0
    kill -0 "$pid" 2>/dev/null || return 1
    sleep 0.05
  done
  return 1
}

# The port from a "listening on <address>:<port>" line.
listening_port() {
  sed -n 's/^listening on .*:\([0-9][0-9]*\)$/\1/p' "$1" | head -n 1
}

# Waits up to ~5 s for a process to exit, then kills it.
reap() {
  local pid=$1 i
  for ((i = 0; i < 100; i++)); do
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.05
  done
  kill "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true
}

set_netem() {
  local delay=$1 loss=$2
  [[ $use_netem -eq 1 ]] || return 0
  tc qdisc replace dev lo root netem delay "${delay}ms" loss "${loss}%"
}

clear_netem() {
  [[ $use_netem -eq 1 ]] || return 0
  tc qdisc del dev lo root 2>/dev/null || true
}

# --- test data ---------------------------------------------------------------
basis="$data_dir/basis.bin"
target="$data_dir/target.bin"
python3 "$script_dir/make_test_files.py" --size "$size" --seed "$seed" \
  --edit-size "$edit_size" --base "$basis" --edited "$target" >"$work_dir/files.sha256"
file_bytes=$(wc -c <"$target" | tr -d ' ')
drop_after=$(python3 -c "print(int($file_bytes * float('$drop_fraction')))")
remote_name="data.bin"

if [[ $use_netem -eq 1 ]]; then
  tc qdisc replace dev lo root netem delay 0ms loss 0% ||
    die "tc netem failed; run the container with --cap-add NET_ADMIN (or pass --no-netem)"
  clear_netem
fi

timeout_args=()
[[ -n $timeout_ms ]] && timeout_args=(--timeout-ms "$timeout_ms")

# --- receiver ----------------------------------------------------------------
"$deltasync" serve --port "$port" --root "$server_root" --bind 127.0.0.1 \
  ${timeout_args[@]+"${timeout_args[@]}"} >"$work_dir/serve.out" 2>"$work_dir/serve.log" &
server_pid=$!
wait_for_listening "$work_dir/serve.out" "$server_pid" ||
  die "deltasync serve did not start; see $work_dir/serve.log"
port=$(listening_port "$work_dir/serve.out")

reset_receiver() {  # reset_receiver with-basis|empty
  rm -rf "$server_root"
  mkdir -p "$server_root"
  if [[ $1 == with-basis ]]; then
    cp "$basis" "$server_root/$remote_name"
  fi
}

# Sessions deltasync serve has logged so far (one line per connection).
session_count() {
  grep -c ' serve: peer=' "$work_dir/serve.log" 2>/dev/null || true
}

# Waits (bounded) until serve has logged at least N sessions. After a cut
# connection the receiver may still be finishing that session, and holds the
# name until it does; a push started earlier would be turned away as busy.
wait_for_sessions() {
  local want=$1 i
  for ((i = 0; i < 1200; i++)); do
    (( $(session_count) >= want )) && return 0
    kill -0 "$server_pid" 2>/dev/null || return 1
    sleep 0.05
  done
  return 1
}

receiver_matches() {
  if cmp -s "$target" "$server_root/$remote_name"; then echo yes; else echo no; fi
}

columns="delay_ms,loss_pct,scenario,run,exit_code,mode,file_size,bytes_sent,bytes_received,socket_bytes,signature_bytes,delta_bytes,literal_bytes,matched_bytes,resumed_from,attempts,elapsed_ms,wall_ms,verified,note"
echo "$columns" >"$out"

# Appends one CSV row. Statistics come from the push's --stats-json file when
# it exists; missing values stay empty instead of being guessed.
emit_row() {
  local delay=$1 loss=$2 scenario=$3 run=$4 code=$5 stats=$6 wall=$7 verified=$8 \
        note=$9 proxy_up=${10:-} proxy_down=${11:-}
  python3 - "$stats" "$delay" "$loss" "$scenario" "$run" "$code" "$wall" "$verified" \
    "$note" "$proxy_up" "$proxy_down" >>"$out" <<'EOF'
import csv, json, os, sys
stats_path, delay, loss, scenario, run, code, wall, verified, note, up, down = sys.argv[1:]
stats = {}
if os.path.exists(stats_path) and os.path.getsize(stats_path) > 0:
    try:
        with open(stats_path) as f:
            stats = json.load(f)
    except ValueError as e:
        note = (note + "; " if note else "") + "unreadable stats: %s" % e
# A push that failed wrote no statistics; the proxy's counts stand in for
# the two directions.
if up and down and "bytes_sent" not in stats:
    stats["bytes_sent"], stats["bytes_received"] = up, down
keys = ["mode", "file_size", "bytes_sent", "bytes_received"]
row = [delay, loss, scenario, run, code] + [stats.get(k, "") for k in keys]
if "bytes_sent" in stats and "bytes_received" in stats:
    on_wire = int(stats["bytes_sent"]) + int(stats["bytes_received"])
else:
    on_wire = ""
row.append(on_wire)
row += [stats.get(k, "") for k in ("signature_bytes", "delta_bytes", "literal_bytes",
                                     "matched_bytes", "resumed_from", "attempts",
                                     "elapsed_ms")]
row += [wall, verified, note]
csv.writer(sys.stdout, lineterminator="\n").writerow(row)
EOF
}

# Runs one push; sets push_code, push_wall_ms and leaves output in $push_log.
run_push() {  # run_push <stats file> <log file> <push args...>
  local stats=$1 log=$2
  shift 2
  rm -f "$stats"
  local start end
  start=$(now_ms)
  set +e
  "$deltasync" push "$target" --name "$remote_name" --stats-json "$stats" \
    ${timeout_args[@]+"${timeout_args[@]}"} "$@" >"$log" 2>&1
  push_code=$?
  set -e
  end=$(now_ms)
  push_wall_ms=$((end - start))
}

echo "netem_bench: work dir $work_dir, file $file_bytes bytes, cut after $drop_after bytes" >&2

for delay in $delays; do
  for loss in $losses; do
    set_netem "$delay" "$loss"
    for ((run = 1; run <= repeats; run++)); do
      tag="d${delay}_l${loss}_r${run}"

      # Full-copy baseline: same protocol, whole-file mode.
      reset_receiver with-basis
      run_push "$stats_dir/$tag.whole.json" "$stats_dir/$tag.whole.log" \
        --host 127.0.0.1 --port "$port" --whole-file
      emit_row "$delay" "$loss" whole-file "$run" "$push_code" "$stats_dir/$tag.whole.json" \
        "$push_wall_ms" "$(receiver_matches)" ""

      # Delta after a small edit.
      reset_receiver with-basis
      run_push "$stats_dir/$tag.delta.json" "$stats_dir/$tag.delta.log" \
        --host 127.0.0.1 --port "$port"
      emit_row "$delay" "$loss" delta "$run" "$push_code" "$stats_dir/$tag.delta.json" \
        "$push_wall_ms" "$(receiver_matches)" ""

      # Interrupted push through the proxy, then a resumed push.
      reset_receiver empty
      sessions_before=$(session_count)
      "$proxy" --listen-port 0 --target-host 127.0.0.1 --target-port "$port" \
        --drop-after-bytes "$drop_after" --drop-direction up --max-connections 1 \
        >"$stats_dir/$tag.proxy.out" 2>"$stats_dir/$tag.proxy.err" &
      proxy_pid=$!
      wait_for_listening "$stats_dir/$tag.proxy.out" "$proxy_pid" ||
        die "deltasync_proxy did not start; see $stats_dir/$tag.proxy.err"
      proxy_port=$(listening_port "$stats_dir/$tag.proxy.out")
      run_push "$stats_dir/$tag.cut.json" "$stats_dir/$tag.cut.log" \
        --host 127.0.0.1 --port "$proxy_port" --whole-file --retries 0
      reap "$proxy_pid"
      proxy_pid=""
      # "connection 1 closed: up=U down=D cut=yes" -> U and D
      read -r proxy_up proxy_down < <(sed -n \
        's/.*closed: up=\([0-9]*\) down=\([0-9]*\).*/\1 \2/p' \
        "$stats_dir/$tag.proxy.err" | head -n 1) || true
      note="proxy forwarded $(( ${proxy_up:-0} + ${proxy_down:-0} )) bytes"
      [[ -n ${proxy_up:-} ]] || note="proxy forwarded ? bytes"
      if [[ $push_code -eq 0 ]]; then
        note="$note; push did not fail (cut point past the end of the transfer?)"
      fi
      emit_row "$delay" "$loss" interrupted "$run" "$push_code" "$stats_dir/$tag.cut.json" \
        "$push_wall_ms" "$(receiver_matches)" "$note" "${proxy_up:-}" "${proxy_down:-}"

      wait_for_sessions $((sessions_before + 1)) ||
        die "deltasync serve did not finish the interrupted session; see $work_dir/serve.log"
      serve_lines=$(wc -l <"$work_dir/serve.log" | tr -d ' ')
      run_push "$stats_dir/$tag.resume.json" "$stats_dir/$tag.resume.log" \
        --host 127.0.0.1 --port "$port" --whole-file
      note=""
      if { cat "$stats_dir/$tag.resume.log"; tail -n +"$((serve_lines + 1))" "$work_dir/serve.log"; } |
           grep -qi "resume not supported"; then
        note="resume not supported by this build; full retry"
      elif [[ -f "$stats_dir/$tag.resume.json" ]] &&
           python3 -c 'import json,sys; sys.exit(0 if int(json.load(open(sys.argv[1])).get("resumed_from", 0)) == 0 else 1)' \
             "$stats_dir/$tag.resume.json" 2>/dev/null; then
        note="restarted from 0"
      fi
      emit_row "$delay" "$loss" resumed "$run" "$push_code" "$stats_dir/$tag.resume.json" \
        "$push_wall_ms" "$(receiver_matches)" "$note"
    done
    clear_netem
  done
done

echo "netem_bench: wrote $out" >&2

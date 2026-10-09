#!/usr/bin/env bash
# Builds the release configuration inside the deltasync-dev Linux image and
# runs scripts/netem_bench.sh there with NET_ADMIN, so tc netem can shape the
# container's loopback interface (the host network is never touched).
#
# Usage: scripts/run_netem_bench_docker.sh [--smoke] [netem_bench.sh options]
#
#   --smoke   tiny functional run (1 MiB file, delays 0 20, losses 0 1),
#             written to build/netem-smoke/netem.csv instead of
#             bench/results/netem.csv; not for publishing numbers
#
# Environment:
#   IMAGE      container image (default deltasync-dev:latest; build it with
#              docker build -t deltasync-dev docker/)
#   CXX_NAME   compiler inside the image, g++ or clang++ (default g++)
#   JOBS       build parallelism (default 2)
#
# The build goes to build/linux-gcc-netem-release (or linux-clang-...) in the
# project folder; CMake is configured with explicit flags instead of presets
# so it works with the image's CMake.
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
project_dir=$(cd "$script_dir/.." && pwd)
image=${IMAGE:-deltasync-dev:latest}
cxx=${CXX_NAME:-g++}
jobs=${JOBS:-2}

case "$cxx" in
  g++*) build_name="linux-gcc-netem-release" ;;
  clang++*) build_name="linux-clang-netem-release" ;;
  *) echo "run_netem_bench_docker: unsupported CXX_NAME '$cxx'" >&2; exit 2 ;;
esac

bench_args=()
if [[ ${1:-} == --smoke ]]; then
  shift
  bench_args=(--size 1M --delays "0 20" --losses "0 1"
              --out /src/build/netem-smoke/netem.csv)
fi
bench_args+=("$@")

command -v docker >/dev/null 2>&1 ||
  { echo "run_netem_bench_docker: docker is not installed or not on PATH" >&2; exit 1; }
if ! docker image inspect "$image" >/dev/null 2>&1; then
  {
    echo "run_netem_bench_docker: image $image not found."
    echo "Build it first, from the project folder:"
    echo "  docker build -t deltasync-dev docker/"
    echo "or set IMAGE to an image that exists."
  } >&2
  exit 1
fi

docker run --rm --cap-add NET_ADMIN \
  --mount type=bind,source="$project_dir",target=/src \
  -e BUILD_NAME="$build_name" -e CXX_NAME="$cxx" -e JOBS="$jobs" \
  "$image" bash -c '
    set -euo pipefail
    build=/src/build/$BUILD_NAME
    cmake -S /src -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_COMPILER="$CXX_NAME" -DDELTASYNC_BUILD_TESTS=OFF >/dev/null
    cmake --build "$build" -j "$JOBS" --target deltasync deltasync_proxy
    echo "== environment"
    uname -srm
    "$CXX_NAME" --version | head -n 1
    nproc
    echo "== benchmark"
    TMPDIR=/tmp /src/scripts/netem_bench.sh --bin-dir "$build" "$@"
  ' run_netem_bench ${bench_args[@]+"${bench_args[@]}"}

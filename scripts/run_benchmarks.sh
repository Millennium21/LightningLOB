#!/usr/bin/env bash
# scripts/run_benchmarks.sh - build (if needed) and run the full Google
# Benchmark suite with sensible defaults, saving a JSON copy of the results.
#
# Usage:
#   scripts/run_benchmarks.sh [-- <extra benchmark flags>]
#
# Examples:
#   scripts/run_benchmarks.sh
#   scripts/run_benchmarks.sh -- --benchmark_filter=CancelOrder
set -euo pipefail

cd "$(dirname "$0")/.."

BUILD_DIR="${BUILD_DIR:-build}"
RESULTS_DIR="${RESULTS_DIR:-benchmark_results}"
mkdir -p "${RESULTS_DIR}"

if [[ ! -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
    echo "==> No configured build found in ${BUILD_DIR}/, configuring Release build"
    cmake -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release
fi

echo "==> Building lightninglob_benchmarks"
JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
cmake --build "${BUILD_DIR}" --target lightninglob_benchmarks -j"${JOBS}"

TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
JSON_OUT="${RESULTS_DIR}/results_${TIMESTAMP}.json"

echo "==> Running benchmarks (results also saved to ${JSON_OUT})"
"${BUILD_DIR}/benchmarks/lightninglob_benchmarks" \
    --benchmark_out="${JSON_OUT}" \
    --benchmark_out_format=json \
    "$@"

echo "==> Done."

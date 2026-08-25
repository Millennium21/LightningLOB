#!/usr/bin/env bash
# scripts/build.sh - configure and build LightningLOB.
#
# Usage:
#   scripts/build.sh [Debug|Release|RelWithDebInfo]   (default: Release)
#
# Environment overrides:
#   BUILD_DIR       (default: build)
#   NATIVE_ARCH=1   add -march=native
#   SANITIZER=address|thread|undefined
set -euo pipefail

cd "$(dirname "$0")/.."

BUILD_TYPE="${1:-Release}"
BUILD_DIR="${BUILD_DIR:-build}"
JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

CMAKE_ARGS=(-DCMAKE_BUILD_TYPE="${BUILD_TYPE}")

if [[ "${NATIVE_ARCH:-0}" == "1" ]]; then
    CMAKE_ARGS+=(-DLIGHTNINGLOB_NATIVE_ARCH=ON)
fi
if [[ -n "${SANITIZER:-}" ]]; then
    CMAKE_ARGS+=(-DLIGHTNINGLOB_SANITIZER="${SANITIZER}")
fi

echo "==> Configuring (${BUILD_TYPE}) in ${BUILD_DIR}/"
cmake -B "${BUILD_DIR}" "${CMAKE_ARGS[@]}"

echo "==> Building with ${JOBS} parallel jobs"
cmake --build "${BUILD_DIR}" -j"${JOBS}"

echo "==> Done. Binaries:"
echo "    ${BUILD_DIR}/lightninglob_cli"
echo "    ${BUILD_DIR}/tests/lightninglob_tests"
echo "    ${BUILD_DIR}/benchmarks/lightninglob_benchmarks"

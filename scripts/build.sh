#!/usr/bin/env bash
# Configures and builds pipeeq.
set -euo pipefail
cd "$(dirname "$0")/.."

BUILD_DIR="${BUILD_DIR:-build}"

cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-RelWithDebInfo}"
cmake --build "$BUILD_DIR" -j"$(nproc)"

echo
echo "Build complete. Running test suites:"
ctest --test-dir "$BUILD_DIR" --output-on-failure

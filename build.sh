#!/usr/bin/env bash
# ServiceScope - Linux/Mac build script
# Prerequisites: cmake, g++ or clang++
#
# Usage:
#   ./build.sh          — Build with default generator
#   ./build.sh ninja    — Build with Ninja
#   ./build.sh clean    — Clean rebuild

set -euo pipefail

BUILD_DIR="build"
GENERATOR=""

if [ "${1:-}" = "ninja" ]; then
    GENERATOR="-G Ninja"
elif [ "${1:-}" = "clean" ]; then
    rm -rf "$BUILD_DIR"
    echo "Cleaned build directory."
    shift
fi

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

echo "=== ServiceScope Build ==="
echo ""

echo "[1/2] Running CMake..."
cmake .. $GENERATOR -DCMAKE_BUILD_TYPE=Release

echo ""
echo "[2/2] Building..."
cmake --build . --parallel

echo ""
echo "=== Build complete ==="
echo "Binary: build/servicescope"
echo ""
echo "Run: ./servicescope [port] [threads]"
echo ""

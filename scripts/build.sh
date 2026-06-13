#!/bin/bash
# INVENT 2027 — Build Script
# Natska Rule++ — Zero Kernel Preemption HFT Engine

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build"

echo "============================================================"
echo "  INVENT 2027 — Build Script"
echo "============================================================"
echo ""

if command -v cmake &> /dev/null; then
    echo "[BUILD] Using CMake (preferred)"
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
    cmake .. -DCMAKE_BUILD_TYPE=Release
    make -j$(nproc)
    echo ""
    echo "[BUILD] Binary: $BUILD_DIR/natska_engine"
    echo "[BUILD] Run: sudo $BUILD_DIR/natska_engine"
elif command -v make &> /dev/null; then
    echo "[BUILD] Using GNU Make (fallback)"
    cd "$PROJECT_DIR"
    make clean && make
    echo ""
    echo "[BUILD] Binary: $PROJECT_DIR/natska_engine"
    echo "[BUILD] Run: sudo $PROJECT_DIR/natska_engine"
else
    echo "[ERROR] Neither cmake nor make found. Install build-essential."
    exit 1
fi

echo ""
echo "[BUILD] Complete"

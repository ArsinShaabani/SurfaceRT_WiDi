#!/usr/bin/env bash
# SurfaceRT-WiDi - one-command build for Linux / macOS / WSL.
#
#   ./build.sh          build the reference sink, the synthetic sender and tests
#   ./build.sh test     build and run the portable core test suite
#   ./build.sh clean    remove build output
#
# Requires only a C++17 compiler (g++ or clang++) and make; nothing else.
set -euo pipefail
cd "$(dirname "$0")"

MODE="${1:-all}"
case "$MODE" in
    all)   make -s ;;
    test)  make -s test ;;
    check) make -s test ;;
    clean) make -s clean ;;
    *)
        echo "usage: ./build.sh [all|test|clean]" >&2
        exit 2
        ;;
esac
echo "[ok] done ($MODE)"

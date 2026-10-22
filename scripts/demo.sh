#!/usr/bin/env bash
# Configures and builds if needed, then runs the demo scenario.
set -euo pipefail
cd "$(dirname "$0")/.."
[[ -x build/svcc_demo ]] || { cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null && cmake --build build -j >/dev/null; }
./build/svcc_demo "$@"

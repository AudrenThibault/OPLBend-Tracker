#!/bin/sh
# Build and run the engine on the Mac (tests/host_engine.c): renders the demo song to build/host_demo.wav.
set -e
cd "$(dirname "$0")/.."
mkdir -p build
PY="${PYTHON:-python3}"
[ -f build/gen/tk_tables.h ] || "$PY" tools/gen_tables.py build/gen/tk_tables.h
cc -O2 -Wall -Wno-unused-function -Ibuild/gen -o build/host_engine tests/host_engine.c -lm
./build/host_engine "$@"

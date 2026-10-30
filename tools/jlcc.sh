#!/bin/sh
# Run a JieLi toolchain binary (pi32v2/bin/<tool>) in Docker, from the tracker root, with paths relative to it.
#   tools/jlcc.sh clang++ -target pi32v2 ... file.cpp
cd "$(dirname "$0")/.."
TOOL="$1"; shift
exec docker run --rm --platform linux/amd64 -v "$PWD:/work" -v "$HOME/.jieli/toolchain:/opt/jieli:ro" -w /work \
  debian:bookworm-slim "/opt/jieli/pi32v2/bin/$TOOL" "$@"

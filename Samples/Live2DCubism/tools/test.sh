#!/usr/bin/env bash
# Builds the plugin, the mock Core and the harness, then runs the unit and the
# plugin tests. POSIX only (the plugin test talks to the harness over FIFOs).
#   tools/test.sh [folder holding Engine/Native/PxPluginAbi.h]
set -euo pipefail

here="$(cd "$(dirname "$0")/.." && pwd)"
engine="${1:-$here/../../PrismatiXEngine}"
build="$(mktemp -d)"
trap 'rm -rf "$build"' EXIT

cmake -S "$here/native" -B "$build" -DPX_ENGINE_INCLUDE="$engine" -DPX_LIVE2D_TESTS=ON >/dev/null
cmake --build "$build" >/dev/null

case "$(uname -s)" in
  Darwin) library="libpx_live2d.dylib"; arch="darwin-$(uname -m | sed 's/x86_64/x64/')" ;;
  *)      library="libpx_live2d.so";    arch="linux-x64" ;;
esac

cd "$here/extension"
LIVE2D_NATIVE_BUILD="$build" LIVE2D_PLUGIN="$here/extension/native/$arch/$library" node --test test/*.test.ts

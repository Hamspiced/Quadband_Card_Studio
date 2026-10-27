#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
export CLANG_MODULE_CACHE_PATH="$PWD/.build/ModuleCache"
export SWIFTPM_MODULECACHE_OVERRIDE="$PWD/.build/ModuleCache"
mkdir -p .build/ModuleCache .build/cache .build/config .build/security
swift test --disable-sandbox --cache-path "$PWD/.build/cache" \
  --config-path "$PWD/.build/config" --security-path "$PWD/.build/security" "$@"

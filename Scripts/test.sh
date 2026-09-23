#!/bin/sh
set -eu
task_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cmake -S "$task_root" -B "$task_root/build" -DBUILD_TESTING=ON -DSS_BUILD_TOOLS=ON -DSS_ENABLE_SANITIZERS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build "$task_root/build"
ctest --test-dir "$task_root/build" --output-on-failure
cd "$task_root"
CLANG_MODULE_CACHE_PATH="$task_root/.build/module-cache" \
SWIFTPM_MODULECACHE_OVERRIDE="$task_root/.build/module-cache" \
swift test --disable-sandbox --cache-path .build/cache --config-path .build/config --security-path .build/security

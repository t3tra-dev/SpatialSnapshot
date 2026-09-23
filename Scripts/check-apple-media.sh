#!/bin/sh
set -eu
task_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$task_root"
export CLANG_MODULE_CACHE_PATH="$task_root/.build/module-cache"
export SWIFTPM_MODULECACHE_OVERRIDE="$task_root/.build/module-cache"
export SPATIALSNAPSHOT_MEDIA_TESTS=1
if [ "${1:-}" = "--generate-demo" ]; then
    export SPATIALSNAPSHOT_DEMO_OUTPUT="$task_root/Apps/SpatialSnapshotLab/Resources"
elif [ "$#" -ne 0 ]; then
    echo "Usage: sh Scripts/check-apple-media.sh [--generate-demo]" >&2
    exit 2
fi
swift test --disable-sandbox --cache-path .build/cache --config-path .build/config \
    --security-path .build/security --filter 'AppleMediaIntegrationTests|RealityKitAdapterTests'

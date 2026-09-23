#!/bin/sh
set -eu
task_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
task_blender=${BLENDER_BIN:-blender}
if ! command -v "$task_blender" >/dev/null 2>&1; then
    if [ -x /Applications/Blender.app/Contents/MacOS/Blender ]; then
        task_blender=/Applications/Blender.app/Contents/MacOS/Blender
    else
        echo 'Set BLENDER_BIN to a Blender 5.0+ executable.' >&2
        exit 2
    fi
fi
cd "$task_root"
python3 Scripts/package-b3d.py
python3 -m unittest discover -s Tests/BlenderTests -p test_portable.py -v
"$task_blender" --background --factory-startup --python-exit-code 1 --python Tests/BlenderTests/integration.py

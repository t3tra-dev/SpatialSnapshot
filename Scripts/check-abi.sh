#!/bin/sh
set -eu
task_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
task_build=${1:-"$task_root/build"}
python3 "$task_root/Scripts/check_abi.py" "$task_build/libspatialsnapshot.a"

#!/bin/sh
set -eu
task_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
python3 "$task_root/Tests/Conformance/test_conformance.py" --generate
python3 "$task_root/Tests/Bindings/test_bindings.py" --generate

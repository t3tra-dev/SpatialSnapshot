"""Check that every declared public C entry point has an implementation."""
from pathlib import Path
import re
import subprocess
import sys

root=Path(__file__).resolve().parents[1]
header="\n".join(p.read_text() for p in (root/"Sources/SpatialSnapshotC/include/spatialsnapshot").glob("*.h"))
declared=set(re.findall(r"SS_API\s+[^;]*?\b(ss_\w+)\s*\(",header))
symbols=subprocess.check_output(["nm","-g",sys.argv[1]],text=True)
defined=set(re.findall(r"\b[TW]\s+_?(ss_\w+)",symbols))
missing=declared-defined
if missing:
    raise SystemExit("Missing C ABI symbols: "+", ".join(sorted(missing)))
print(f"{len(declared)} public C ABI symbols verified")

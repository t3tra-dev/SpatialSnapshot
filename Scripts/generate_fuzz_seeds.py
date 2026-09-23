"""Create disposable libFuzzer corpora from the independent fixture encoder."""
import importlib.util
from pathlib import Path
import struct
import sys

root=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location("conformance",root/"Tests/Conformance/test_conformance.py")
module=importlib.util.module_from_spec(spec)
sys.modules[spec.name]=module
spec.loader.exec_module(module)
destination=root/"Fuzz/corpus-generated"
for target in ("ssps","geometry","lz4"):
    (destination/target).mkdir(parents=True,exist_ok=True)
for name,(data,_) in module.cases().items():
    if not name.startswith("truncated-"):
        (destination/"ssps"/name).write_bytes(data)
geometry=module.geometry()
(destination/"geometry"/"triangle").write_bytes(geometry)
for name,raw in (("triangle",geometry),("depth",module.depth())):
    (destination/"lz4"/name).write_bytes(struct.pack("<I",len(raw))+module.literal_block(raw))

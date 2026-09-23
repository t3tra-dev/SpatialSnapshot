#!/usr/bin/env python3
"""Independent SSPS encoder and conformance corpus (Python standard library).

No implementation code or C writer is used to construct expectations.
Run with a ssvalidate executable, or --generate to refresh checked-in fixtures.
"""
from dataclasses import dataclass, replace
from pathlib import Path
import json
import math
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SCENE, CAMERA, BEGIN, PUT, REMOVE, END, DEPTH, FINISH = 1, 2, 16, 17, 18, 19, 32, 255


def crc(data):
    value = 0xFFFFFFFF
    for b in data:
        value ^= b
        for _ in range(8):
            value = (value >> 1) ^ (0x82F63B78 if value & 1 else 0)
    return value ^ 0xFFFFFFFF


def literal_block(raw):
    result = bytearray([min(len(raw), 15) << 4])
    if len(raw) >= 15:
        n = len(raw) - 15
        while n >= 255:
            result.append(255)
            n -= 255
        result.append(n)
    return bytes(result) + raw


@dataclass
class Packet:
    type: int
    raw: bytes = b""
    time: int = 0
    flags: int | None = None
    stored: bytes | None = None
    raw_size: int | None = None
    sequence: int | None = None
    extra: bytes = b""
    reserved: int = 0

    def encode(self, sequence):
        flags = self.flags if self.flags is not None else (3 if self.type in (PUT, DEPTH) else 2)
        stored = self.stored if self.stored is not None else literal_block(self.raw) if flags & 1 else self.raw
        header = bytearray(struct.pack("<4sHHIQQIIIII", b"SSPK", 48 + len(self.extra), self.type,
                                      flags, sequence if self.sequence is None else self.sequence, self.time,
                                      len(stored), len(self.raw) if self.raw_size is None else self.raw_size,
                                      crc(stored), 0, self.reserved) + self.extra)
        struct.pack_into("<I", header, 40, crc(header))
        return bytes(header) + stored


def stream(packets, kind=1, minor=0, major=1, extra=b"", flags=0, stream_id=bytes(range(1, 17))):
    header = bytearray(struct.pack("<8sHHHHIHH16sI20s", b"SSPS\r\n\x1a\n", major, minor,
                                   64 + len(extra), kind, flags, 48, 0, stream_id, 0, bytes(20)) + extra)
    struct.pack_into("<I", header, 40, crc(header))
    return bytes(header) + b"".join(p.encode(i) for i, p in enumerate(packets))


def camera(width=4, height=3, fx=2, fy=3, cx=1.5, cy=1, t=(0, 0, 0), q=(0, 0, 0, 1)):
    return struct.pack("<II4f3f4f12x", width, height, fx, fy, cx, cy, *t, *q)


def checkpoint(identifier=1, chunks=0):
    return struct.pack("<QII", identifier, chunks, 0)


def geometry(identifier=1, key=(0, 0, 2), vertices=((0, 0, 0), (0, 65535, 0), (65535, 0, 0)),
             triangles=((0, 2, 1),), classes=(4,)):
    return struct.pack("<QiiiIII", identifier, *key, len(vertices), len(triangles), 0) + b"".join(
        struct.pack("<HHH", *v) for v in vertices) + b"".join(struct.pack("<HHH", *t) for t in triangles) + bytes(classes)


def depth(width=1, height=1, values=(1000,), confidence=(3,), fx=1, fy=1, cx=0, cy=0):
    packed = bytearray((len(values) + 3) // 4)
    for i, value in enumerate(confidence):
        packed[i // 4] |= value << (2 * (i % 4))
    return struct.pack("<II4fII", width, height, fx, fy, cx, cy, 0, 0) + struct.pack(f"<{len(values)}H", *values) + packed


SCENE_RAW = struct.pack("<4f4I", 0, 1, 0, 0, 1, 500000, 1000, 0)
MINIMAL = [Packet(SCENE, SCENE_RAW), Packet(BEGIN, checkpoint()), Packet(END, checkpoint()), Packet(CAMERA, camera()), Packet(FINISH)]
MESH = [Packet(SCENE, SCENE_RAW), Packet(BEGIN, checkpoint(chunks=1)), Packet(PUT, geometry()),
        Packet(END, checkpoint(chunks=1)), Packet(CAMERA, camera()), Packet(DEPTH, depth()), Packet(FINISH)]
VIDEO = MINIMAL[:-1] + [Packet(CAMERA, camera(t=(0.25, 0, 0)), time=1_000_000_000),
                       Packet(CAMERA, camera(t=(0.5, 0, 0)), time=2_000_000_000), Packet(FINISH, time=3_000_000_000)]


def changed(packets, index, **kw):
    result = list(packets)
    result[index] = replace(result[index], **kw)
    return result


def raw_field(raw, offset, fmt, value):
    result = bytearray(raw)
    struct.pack_into("<" + fmt, result, offset, value)
    return bytes(result)


def cases():
    result = {}

    def add(name, packets=None, *, data=None, expected="MALFORMED", kind=1, **kw):
        result[name] = (data if data is not None else stream(packets, kind=kind, **kw), expected)

    add("minimal-still", MINIMAL, expected="OK")
    add("mesh-depth-still", MESH, expected="OK")
    add("minimal-video", VIDEO, kind=2, expected="OK")
    update_video = MESH[:-1] + [Packet(PUT, geometry(identifier=0, key=(0, 0, 3)), time=1_000_000_000),
                               Packet(CAMERA, camera(t=(0.25, 0, 0)), time=1_000_000_000),
                               Packet(REMOVE, struct.pack("<iiiI", 0, 0, 2, 0), time=2_000_000_000),
                               Packet(CAMERA, camera(t=(0.5, 0, 0)), time=2_000_000_000), Packet(FINISH, time=3_000_000_000)]
    add("geometry-video", update_video, kind=2, expected="OK")
    add("future-minor", MINIMAL, minor=65535, expected="OK")
    add("extended-headers", [replace(p, extra=b"extension") for p in MINIMAL], extra=b"future header", minor=1, expected="OK")
    add("maximum-headers", [replace(p, extra=bytes(4048)) for p in MINIMAL], extra=bytes(4032), minor=1, expected="OK")
    extension = Packet(0x8000, b"opaque", flags=0)
    add("unknown-noncritical", MINIMAL[:-1] + [extension, MINIMAL[-1]], expected="OK")
    add("unknown-compressed-skipped", MINIMAL[:-1] + [Packet(0x101, flags=0x80000001, stored=b"invalid LZ4", raw_size=16_777_216), MINIMAL[-1]], expected="OK")
    add("unknown-extension-only-time", VIDEO[:-1] + [replace(extension, time=2_500_000_000), VIDEO[-1]], kind=2, expected="OK")
    add("unknown-classification", changed(MESH, 2, raw=geometry(classes=(254,))), expected="OK")
    add("negative-zero", changed(MINIMAL, 3, raw=camera(t=(-0.0, 0, 0))), expected="OK")
    add("quaternion-tolerance", changed(VIDEO, 4, raw=camera(q=(0, 0, 0, 1.00009))), kind=2, expected="OK")
    add("maximum-camera-raster", changed(MINIMAL, 3, raw=camera(width=16384, height=16384)), expected="OK")
    add("signed-cell-limits", changed(MESH, 2, raw=geometry(key=(-2147483648, 2147483647, -1))), expected="OK")
    cp_video = MESH[:-1] + [Packet(BEGIN, checkpoint(2), time=40_000_000_000), Packet(END, checkpoint(2), time=40_000_000_000),
                           Packet(CAMERA, camera(), time=40_000_000_000), Packet(FINISH, time=40_000_000_001)]
    add("later-empty-checkpoint", cp_video, kind=2, expected="OK")
    add("static-long-video", MINIMAL[:-1] + [Packet(CAMERA, camera(), time=10**12), Packet(FINISH, time=10**12)], kind=2, expected="OK")
    cadence = MESH[:-1] + [Packet(REMOVE, struct.pack("<iiiI", 0, 0, 2, 0), time=30_000_000_000),
                          Packet(CAMERA, camera(), time=30_000_000_000), Packet(FINISH, time=30_000_000_001)]
    add("cadence-exact-boundary", cadence, kind=2, expected="OK")
    # A non-literal block uses a match with offset=1, then five final literals.
    # It decodes a valid 1x1 depth payload with a long reserved/zero run.
    d = depth(values=(0,), confidence=(0,))
    # Emit first 21 bytes, then nine zeros copied from position 20, then 5 literals.
    lz4 = b"\xf5\x06" + d[:21] + b"\x01\x00" + b"\x50" + d[30:]
    assert d[20:30] == bytes(10)
    add("lz4-overlap", changed(MESH, 5, raw=d, stored=lz4), expected="OK")

    add("unsupported-major", MINIMAL, major=2, expected="UNSUPPORTED")
    add("unsupported-kind", MINIMAL, kind=3, expected="UNSUPPORTED")
    add("unknown-critical", MINIMAL[:-1] + [replace(extension, flags=2), MINIMAL[-1]], expected="UNSUPPORTED_CRITICAL_PACKET")
    for index in range(len(MINIMAL)):
        add(f"missing-packet-{index}", MINIMAL[:index] + MINIMAL[index + 1:])
        add(f"duplicate-packet-{index}", MINIMAL[:index] + [MINIMAL[index]] + MINIMAL[index:])
    add("stream-magic", data=b"x" + stream(MINIMAL)[1:])
    add("packet-magic", data=stream(MINIMAL)[:64] + b"x" + stream(MINIMAL)[65:])
    add("stream-crc", data=raw_field(stream(MINIMAL), 40, "I", 0))
    add("packet-crc", data=raw_field(stream(MINIMAL), 64 + 40, "I", 0))
    add("payload-crc", data=raw_field(stream(MINIMAL), 64 + 48, "I", 123))
    add("zero-stream-id", MINIMAL, stream_id=bytes(16))
    add("stream-flags", MINIMAL, flags=1)
    for index in range(len(MINIMAL)):
        add(f"sequence-gap-{index}", changed(MINIMAL, index, sequence=index + 1))
        add(f"flags-{index}", changed(MINIMAL, index, flags=0))
        add(f"packet-reserved-{index}", changed(MINIMAL, index, reserved=1))
    add("sequence-wrap", changed(MINIMAL, 0, sequence=2**64 - 1))
    add("decreasing-time", changed(VIDEO, 5, time=1), kind=2)
    add("raw-too-large", changed(MESH, 2, raw_size=16_777_217))
    add("uncompressed-size-mismatch", changed(MINIMAL, 3, raw_size=63))
    add("geometry-requires-lz4", changed(MESH, 2, flags=2))
    add("depth-requires-lz4", changed(MESH, 5, flags=2))
    for name, compressed in {"empty": b"", "zero-offset": b"\x00\x00\x00", "dictionary": b"\x00\x01\x00", "short": b"\xf0",
                             "bomb": b"\x1fX\x01\x00" + bytes([255])*100, "missing-final-literals": lz4[:-6],
                             "trailing": literal_block(d) + b"\x00", "frame-header": b"\x04\x22\x4d\x18"}.items():
        add("lz4-" + name, changed(MESH, 5, stored=compressed))
    add("lz4-size-mismatch", changed(MESH, 5, raw_size=34))
    add("invalid-type", MINIMAL[:-1] + [replace(extension, type=65535), MINIMAL[-1]])
    add("unknown-before-scene", [extension] + MINIMAL)
    add("unknown-in-checkpoint", MESH[:2] + [extension] + MESH[2:])
    add("unknown-before-first-camera", MINIMAL[:3] + [extension] + MINIMAL[3:])
    add("depth-after-extension", MESH[:5] + [extension] + MESH[5:])
    for offset, fmt, val, name in [(0,"f",math.nan,"gravity-nan"),(4,"f",2,"gravity-norm"),(12,"f",1,"scene-reserved-float"),
                                 (16,"I",2,"coordinate-id"),(20,"I",1,"cell-edge"),(24,"I",1,"depth-unit"),(28,"I",1,"scene-reserved")]:
        add(name, changed(MINIMAL, 0, raw=raw_field(SCENE_RAW,offset,fmt,val)))
    add("scene-size", changed(MINIMAL, 0, raw=SCENE_RAW+b"\x00"))
    add("first-checkpoint-id", changed(MINIMAL, 1, raw=checkpoint(2)))
    add("checkpoint-count-mismatch", changed(MESH, 3, raw=checkpoint()))
    add("checkpoint-id-mismatch", changed(MESH, 3, raw=checkpoint(2,1)))
    add("checkpoint-too-many-chunks", changed(MINIMAL, 1, raw=checkpoint(chunks=262145)))
    add("checkpoint-reserved", changed(MINIMAL, 1, raw=raw_field(checkpoint(),12,"I",1)))
    add("checkpoint-membership", changed(MESH, 2, raw=geometry(identifier=0)))
    add("checkpoint-interleave-camera", MESH[:2]+[MESH[4]]+MESH[2:])
    add("duplicate-checkpoint-cell", changed(MESH[:3]+[MESH[2]]+MESH[3:], 1, raw=checkpoint(chunks=2)))
    add("absent-removal", MINIMAL[:-1]+[Packet(REMOVE,struct.pack("<iiiI",0,0,0,0),time=1),Packet(CAMERA,camera(),time=1),Packet(FINISH,time=2)],kind=2)
    add("remove-in-checkpoint", MESH[:2]+[Packet(REMOVE,struct.pack("<iiiI",0,0,2,0))]+MESH[2:])
    add("ordinary-checkpoint-mix", cp_video[:-2]+[Packet(PUT,geometry(identifier=0),time=40_000_000_000)]+cp_video[-2:],kind=2)
    add("cadence-time-exceeded", [replace(p,time=p.time+1) if p.time else p for p in cadence],kind=2)
    add("geometry-without-camera", cadence[:-2]+cadence[-1:],kind=2)
    add("geometry-after-camera", MESH[:-1]+[Packet(CAMERA,camera(),time=1),Packet(PUT,geometry(identifier=0),time=1),Packet(FINISH,time=2)],kind=2)
    for offset, val, name in [(20,2,"few-vertices"),(20,65536,"many-vertices"),(24,0,"zero-triangles"),(24,262145,"many-triangles"),(28,1,"geometry-reserved")]:
        add(name,changed(MESH,2,raw=raw_field(geometry(),offset,"I",val)))
    add("geometry-size",changed(MESH,2,raw=geometry()+b"\x00"))
    add("index-out-of-bounds",changed(MESH,2,raw=geometry(triangles=((0,3,1),))))
    add("repeated-index",changed(MESH,2,raw=geometry(triangles=((0,0,1),))))
    add("zero-area",changed(MESH,2,raw=geometry(vertices=((0,0,0),(1,0,0),(2,0,0)))))
    add("below-area-threshold",changed(MESH,2,raw=geometry(vertices=((0,0,0),(0,1,0),(1,0,0)))))
    add("duplicate-vertex",changed(MESH,2,raw=geometry(vertices=((0,0,0),(0,0,0),(65535,0,0)))))
    add("unsorted-vertices",changed(MESH,2,raw=geometry(vertices=((0,65535,0),(0,0,0),(65535,0,0)))))
    add("unreferenced-vertex",changed(MESH,2,raw=geometry(vertices=((0,0,0),(0,65535,0),(65535,0,0),(65535,65535,0)))))
    add("noncanonical-triangle",changed(MESH,2,raw=geometry(triangles=((2,1,0),))))
    add("duplicate-triangle",changed(MESH,2,raw=geometry(triangles=((0,2,1),(0,2,1)),classes=(4,4))))
    add("unsorted-triangles",changed(MESH,2,raw=geometry(triangles=((0,2,1),(0,1,2)),classes=(4,4))))
    add("invalid-classification",changed(MESH,2,raw=geometry(classes=(255,))))
    for offset,fmt,val,name in [(0,"I",0,"zero-width"),(4,"I",16385,"large-height"),(8,"f",0,"zero-focal"),(12,"f",-1,"negative-focal"),
                               (16,"f",4,"principal-point"),(20,"f",math.inf,"infinite-intrinsic"),(24,"f",math.nan,"nan-translation"),
                               (48,"f",0,"zero-quaternion"),(48,"f",1.01,"nonunit-quaternion"),(52,"I",1,"camera-reserved")]:
        add(name,changed(MINIMAL,3,raw=raw_field(camera(),offset,fmt,val)))
    add("first-nonidentity-translation",changed(MINIMAL,3,raw=camera(t=(1,0,0))))
    add("noncanonical-quaternion-sign",changed(VIDEO,4,raw=camera(q=(0,0,0,-1))),kind=2)
    add("camera-dimension-change",changed(VIDEO,4,raw=camera(width=5)),kind=2)
    add("camera-duplicate-time",changed(VIDEO,4,time=0),kind=2)
    add("camera-size",changed(MINIMAL,3,raw=camera()+bytes(1)))
    add("depth-without-camera",changed(MESH,5,time=1),kind=2)
    add("duplicate-depth",MESH[:6]+[MESH[5]]+MESH[6:])
    for offset,fmt,val,name in [(0,"I",0,"depth-zero-width"),(4,"I",4097,"depth-large-height"),(0,"I",0xffffffff,"depth-overflow"),
                               (8,"f",math.nan,"depth-nan-focal"),(16,"f",1,"depth-principal-point"),(24,"I",1,"depth-reserved")]:
        add(name,changed(MESH,5,raw=raw_field(depth(),offset,fmt,val)))
    add("depth-pixel-limit",changed(MESH,5,raw=depth(width=4096,height=4096)))
    add("depth-zero-with-confidence",changed(MESH,5,raw=depth(values=(0,),confidence=(3,))))
    add("depth-nonzero-without-confidence",changed(MESH,5,raw=depth(values=(1,),confidence=(0,))))
    add("depth-unused-confidence-bits",changed(MESH,5,raw=depth()[:-1]+b"\xff"))
    add("depth-size",changed(MESH,5,raw=depth()+b"\x00"))
    add("depth-dimensions-change",MESH[:-1]+[Packet(CAMERA,camera(),time=1),Packet(DEPTH,depth(width=2,values=(1,1),confidence=(3,3)),time=1),Packet(FINISH,time=2)],kind=2)
    add("nonempty-stream-end",changed(MINIMAL,4,raw=b"x"))
    add("bytes-after-end",data=stream(MINIMAL)+b"\x00")
    add("concatenation",data=stream(MINIMAL)+stream(MINIMAL))
    add("still-positive-duration",changed(MINIMAL,4,time=1))
    add("video-zero-duration",MINIMAL,kind=2)
    add("stream-header-too-small",data=raw_field(stream(MINIMAL),12,"H",63))
    add("stream-header-too-big",MINIMAL,extra=bytes(4033))
    add("packet-header-too-big",changed(MINIMAL,0,extra=bytes(4049)))
    original=stream(MINIMAL)
    for length in range(len(original)):
        add(f"truncated-{length:04}",data=original[:length])
    return result


def generate():
    corpus=cases()
    selected=["minimal-still","minimal-video","mesh-depth-still","geometry-video","lz4-overlap","later-empty-checkpoint",
              "unknown-compressed-skipped","extended-headers","duplicate-vertex","checkpoint-membership","payload-crc","unknown-critical"]
    manifest={}
    for name in selected:
        data,expected=corpus[name]
        directory="v1" if expected=="OK" else "unsupported" if expected.startswith("UNSUPPORTED") else "malformed"
        relative=f"{directory}/{name}.ssps"
        path=ROOT/"Fixtures"/relative
        path.parent.mkdir(parents=True,exist_ok=True)
        path.write_bytes(data)
        manifest[relative]={"expected":expected,"bytes":len(data)}
    (ROOT/"Fixtures"/"manifest.json").write_text(json.dumps(manifest,indent=2)+"\n")


def test(executable):
    corpus=cases()
    with tempfile.TemporaryDirectory(prefix="ssps-conformance-") as temporary:
        paths=[]
        for name,(data,expected) in corpus.items():
            path=Path(temporary)/(name+".ssps"); path.write_bytes(data)
            paths.append((path,expected))
        failures=[]
        for start in range(0,len(paths),80):
            batch=paths[start:start+80]
            process=subprocess.run([executable,"--json",*[str(path) for path,_ in batch]],capture_output=True,text=True)
            records=[json.loads(line) for line in process.stdout.splitlines()]
            if len(records)!=len(batch):
                raise AssertionError(f"Validator crashed or omitted results: {process.returncode}\n{process.stderr}\n{process.stdout}")
            if process.stderr:
                raise AssertionError(process.stderr)
            for record,(path,expected) in zip(records,batch):
                if record["status"]!=expected: failures.append(f"{path.stem}: expected {expected}, got {record}")
        if failures: raise AssertionError("\n".join(failures))
    print(f"{len(corpus)} independent conformance cases passed")


if __name__=="__main__":
    assert crc(b"123456789")==0xe3069283
    if len(sys.argv)==2 and sys.argv[1]=="--generate": generate()
    elif len(sys.argv)==2: test(sys.argv[1])
    else: raise SystemExit("Usage: test_conformance.py /path/to/ssvalidate | --generate")

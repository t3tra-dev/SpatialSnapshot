"""C ABI consumer with ctypes; no Swift, ARKit, or Python extension module.

python3 Examples/Python/inspect.py build-release/libspatialsnapshot.dylib file.ssps
python3 Examples/Python/inspect.py build-release/libspatialsnapshot.dylib file.mov quicktime
"""
import ctypes as c
import json
from pathlib import Path
import sys


class Vector3(c.Structure):
    _fields_ = [(axis, c.c_double) for axis in ("x", "y", "z")]


class Info(c.Structure):
    _fields_ = [("struct_size", c.c_uint32), ("abi_version", c.c_uint32),
                ("version_major", c.c_uint32), ("version_minor", c.c_uint32),
                ("kind", c.c_uint32), ("stream_id", c.c_uint8 * 16),
                ("duration_ns", c.c_uint64), ("packet_count", c.c_uint64),
                ("camera_count", c.c_uint64), ("checkpoint_count", c.c_uint64),
                ("gravity", Vector3)]


def main(library, file, kind=None):
    lib = c.CDLL(str(Path(library).resolve()))
    lib.ss_version.restype = c.c_uint32
    lib.ss_document_open_memory.argtypes = [c.c_void_p, c.c_size_t, c.c_void_p, c.POINTER(c.c_void_p), c.c_void_p]
    lib.ss_document_open_memory.restype = c.c_int32
    lib.ss_document_get_info.argtypes = [c.c_void_p, c.POINTER(Info)]
    lib.ss_document_get_info.restype = c.c_int32
    lib.ss_document_release.argtypes = [c.c_void_p]
    lib.ss_document_release.restype = None
    lib.ss_status_string.argtypes = [c.c_int32]
    lib.ss_status_string.restype = c.c_char_p
    data = Path(file).read_bytes()
    buffer = c.create_string_buffer(data)
    document = c.c_void_p()
    if kind:
        lib.ss_container_open_memory.argtypes = [c.c_uint32,c.c_void_p,c.c_size_t,c.c_void_p,c.POINTER(c.c_void_p)]
        lib.ss_container_open_memory.restype = c.c_int32
        lib.ss_container_document.argtypes = [c.c_void_p,c.POINTER(c.c_void_p)]
        lib.ss_container_document.restype = c.c_int32
        lib.ss_container_release.argtypes = [c.c_void_p]
        lib.ss_container_release.restype = None
        container = c.c_void_p()
        status = lib.ss_container_open_memory({"heif":1,"quicktime":2}[kind],buffer,len(data),None,c.byref(container))
        if not status: status = lib.ss_container_document(container,c.byref(document))
        lib.ss_container_release(container)
    else:
        status = lib.ss_document_open_memory(buffer, len(data), None, c.byref(document), None)
    if status:
        raise ValueError(lib.ss_status_string(status).decode())
    try:
        info = Info(struct_size=c.sizeof(Info), abi_version=0x00010000)
        status = lib.ss_document_get_info(document, c.byref(info))
        if status:
            raise ValueError(lib.ss_status_string(status).decode())
        print(json.dumps({"kind": info.kind, "duration_ns": info.duration_ns,
                          "camera_count": info.camera_count, "checkpoint_count": info.checkpoint_count,
                          "stream_id": bytes(info.stream_id).hex(),
                          "gravity": [info.gravity.x, info.gravity.y, info.gravity.z]}))
    finally:
        lib.ss_document_release(document)


if __name__ == "__main__":
    if len(sys.argv) not in (3,4):
        raise SystemExit(__doc__)
    main(*sys.argv[1:])

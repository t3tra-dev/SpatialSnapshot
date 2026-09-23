# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned ctypes handles for C ABI 1. No container or SSPS parser is duplicated here."""

from __future__ import annotations

import ctypes as c
from dataclasses import dataclass
from pathlib import Path
import struct
import hashlib
import sys

ABI = 0x00010000
U8P = c.POINTER(c.c_uint8)
U16P = c.POINTER(c.c_uint16)
HEADER = [("struct_size", c.c_uint32), ("abi_version", c.c_uint32)]


class SpatialError(RuntimeError):
    pass


class Vec3(c.Structure):
    _fields_ = [(axis, c.c_double) for axis in ("x", "y", "z")]


class Cell(c.Structure):
    _fields_ = [(axis, c.c_int32) for axis in ("x", "y", "z")]


class Allocator(c.Structure):
    _fields_ = HEADER + [(name, c.c_void_p) for name in ("context", "allocate", "deallocate")]


class OpenOptions(c.Structure):
    _fields_ = HEADER + [(name, c.c_uint64) for name in (
        "max_memory_bytes", "max_geometry_bytes", "max_packets", "max_partition_work"
    )] + [("max_scene_cells", c.c_uint32), ("max_packet_raw_bytes", c.c_uint32), ("allocator", Allocator)]


class Diagnostic(c.Structure):
    _fields_ = HEADER + [("domain", c.c_uint32), ("status", c.c_int32),
        ("file_offset", c.c_uint64), ("ssps_offset", c.c_uint64),
        ("entity_id", c.c_uint32), ("packet_type", c.c_uint32), ("message", c.c_char * 160)]


DIAGNOSTIC = c.CFUNCTYPE(None, c.c_void_p, c.POINTER(Diagnostic))
READ = c.CFUNCTYPE(c.c_int32, c.c_void_p, c.c_uint64, c.c_void_p, c.c_size_t)
SIZE = c.CFUNCTYPE(c.c_uint64, c.c_void_p)


class BindingOptions(c.Structure):
    _fields_ = HEADER + [("resources", OpenOptions), ("diagnostic", DIAGNOSTIC), ("diagnostic_context", c.c_void_p)]


class IO(c.Structure):
    _fields_ = HEADER + [("context", c.c_void_p), ("read_at", READ), ("size", SIZE)]


class Info(c.Structure):
    _fields_ = HEADER + [("version_major", c.c_uint32), ("version_minor", c.c_uint32),
        ("kind", c.c_uint32), ("stream_id", c.c_uint8 * 16),
        ("duration_ns", c.c_uint64), ("packet_count", c.c_uint64),
        ("camera_count", c.c_uint64), ("checkpoint_count", c.c_uint64), ("gravity", Vec3)]


class CameraRecord(c.Structure):
    _fields_ = HEADER + [("timestamp_ns", c.c_uint64), ("raster_width", c.c_uint32), ("raster_height", c.c_uint32)] + [
        (name, c.c_float) for name in ("fx", "fy", "cx", "cy", "tx", "ty", "tz", "qx", "qy", "qz", "qw")]


class MeshView(c.Structure):
    _fields_ = HEADER + [("cell", Cell), ("vertex_count", c.c_uint32), ("triangle_count", c.c_uint32),
        ("quantized_xyz", U16P), ("triangle_indices", U16P), ("classifications", U8P)]


class ContainerInfo(c.Structure):
    _fields_ = HEADER + [(name, c.c_uint32) for name in (
        "kind", "media_id", "metadata_id", "local_key_id", "raster_width", "raster_height"
    )] + [(name, c.c_uint64) for name in ("duration_ns", "sample_count", "checkpoint_count")]


class Sample(c.Structure):
    _fields_ = HEADER + [(name, c.c_uint64) for name in (
        "timestamp_ns", "duration_ns", "media_decode_index", "media_offset", "media_size",
        "bundle_offset", "bundle_size", "ssps_offset")]


def record(cls):
    return cls(struct_size=c.sizeof(cls), abi_version=ABI)


def find_library(configured=""):
    if configured:
        path = Path(configured).expanduser().resolve()
        if path.is_file():
            return path
        raise SpatialError(f"C ライブラリが見つかりません: {path}")
    filename = {"darwin": "libspatialsnapshot.dylib", "win32": "spatialsnapshot.dll"}.get(sys.platform, "libspatialsnapshot.so")
    folder = Path(__file__).resolve().parent
    candidates = [folder / "lib" / filename]
    # Source checkout convenience only; installed extensions use their bundled lib.
    root = folder.parent.parent
    if (root / "Sources/SpatialSnapshotC").is_dir():
        candidates += [root / ".build/b3d/native" / filename, root / "build-release" / filename]
    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()
    raise SpatialError("C ライブラリがありません. Scripts/package-b3d.py で ZIP を作成するか, アドオン設定でライブラリを指定してください.")


class Library:
    def __init__(self, path=""):
        self.path = find_library(path)
        try:
            self.lib = c.CDLL(str(self.path))
        except OSError as error:
            raise SpatialError(f"C ライブラリをロードできません（OS・CPU アーキテクチャを確認してください）: {error}") from error
        declarations = {
            "ss_version": (c.c_uint32, []),
            "ss_status_string": (c.c_char_p, [c.c_int32]),
            "ss_binding_options_init": (None, [c.POINTER(BindingOptions)]),
            "ss_container_open": (c.c_int32, [c.c_uint32, c.POINTER(IO), c.POINTER(BindingOptions), c.POINTER(c.c_void_p)]),
            "ss_container_release": (None, [c.c_void_p]),
            "ss_container_get_info": (c.c_int32, [c.c_void_p, c.POINTER(ContainerInfo)]),
            "ss_container_document": (c.c_int32, [c.c_void_p, c.POINTER(c.c_void_p)]),
            "ss_container_ssps_bytes": (c.c_int32, [c.c_void_p, c.POINTER(U8P), c.POINTER(c.c_size_t)]),
            "ss_container_get_sample": (c.c_int32, [c.c_void_p, c.c_uint64, c.POINTER(Sample)]),
            "ss_document_open_memory": (c.c_int32, [c.c_void_p, c.c_size_t, c.c_void_p, c.POINTER(c.c_void_p), c.c_void_p]),
            "ss_document_release": (None, [c.c_void_p]),
            "ss_document_get_info": (c.c_int32, [c.c_void_p, c.POINTER(Info)]),
            "ss_document_camera": (c.c_int32, [c.c_void_p, c.c_uint64, c.POINTER(CameraRecord)]),
            "ss_scene_cursor_create": (c.c_int32, [c.c_void_p, c.POINTER(c.c_void_p)]),
            "ss_scene_cursor_release": (None, [c.c_void_p]),
            "ss_scene_cursor_seek": (c.c_int32, [c.c_void_p, c.c_uint64]),
            "ss_scene_cursor_chunk_count": (c.c_uint32, [c.c_void_p]),
            "ss_scene_cursor_chunk": (c.c_int32, [c.c_void_p, c.c_uint32, c.POINTER(MeshView)]),
        }
        try:
            for name, (result, arguments) in declarations.items():
                function = getattr(self.lib, name)
                function.restype, function.argtypes = result, arguments
        except AttributeError as error:
            raise SpatialError("SpatialSnapshot C ABI 1 のライブラリを指定してください.") from error
        if self.lib.ss_version() >> 16 != ABI >> 16:
            raise SpatialError("SpatialSnapshot C ABI の major version が一致しません.")

    def check(self, status, details=()):
        if status:
            message = self.lib.ss_status_string(status).decode("utf-8", "replace")
            raise SpatialError(" / ".join([message, *details]))

    def open_media(self, filename):
        filename = Path(filename).resolve()
        kind = {".heic": 1, ".heif": 1, ".mov": 2}.get(filename.suffix.lower())
        if kind is None:
            raise SpatialError("SSPS を含む .heic / .heif / .mov を選んでください.")
        messages, container, document = [], c.c_void_p(), c.c_void_p()
        with filename.open("rb") as file:
            length = filename.stat().st_size

            @READ
            def read(_context, offset, destination, size):
                try:
                    file.seek(offset)
                    data = file.read(size)
                    if len(data) != size:
                        return -7
                    c.memmove(destination, data, size)
                    return 0
                except (OSError, OverflowError, MemoryError) as error:
                    messages.append(str(error))
                    return -7

            @SIZE
            def size(_context):
                return length

            @DIAGNOSTIC
            def diagnostic(_context, value):
                if len(messages) < 8:
                    messages.append(value.contents.message.decode("utf-8", "replace"))

            io = IO(c.sizeof(IO), ABI, None, read, size)
            options = record(BindingOptions)
            self.lib.ss_binding_options_init(c.byref(options))
            options.diagnostic = diagnostic
            try:
                status = self.lib.ss_container_open(kind, c.byref(io), c.byref(options), c.byref(container))
                self.check(status, ["SSPS binding を読み込めません.", *messages])
                info = record(ContainerInfo)
                self.check(self.lib.ss_container_get_info(container, c.byref(info)))
                pointer, byte_count = U8P(), c.c_size_t()
                self.check(self.lib.ss_container_ssps_bytes(container, c.byref(pointer), c.byref(byte_count)))
                ssps = c.string_at(pointer, byte_count.value)
                samples = []
                for index in range(info.sample_count):
                    sample = record(Sample)
                    self.check(self.lib.ss_container_get_sample(container, index, c.byref(sample)))
                    samples.append((sample.timestamp_ns, sample.duration_ns))
                self.check(self.lib.ss_container_document(container, c.byref(document)))
            finally:
                self.lib.ss_container_release(container)
        return Document(self, document, ssps), info, samples

    def open_ssps(self, data):
        document = c.c_void_p()
        buffer = c.create_string_buffer(data)
        self.check(self.lib.ss_document_open_memory(buffer, len(data), None, c.byref(document), None))
        return Document(self, document, data)


@dataclass(frozen=True)
class Camera:
    timestamp_ns: int
    width: int
    height: int
    fx: float
    fy: float
    cx: float
    cy: float
    translation: tuple
    quaternion: tuple  # x, y, z, w


@dataclass(frozen=True)
class Chunk:
    cell: tuple
    xyz: bytes  # native-endian uint16 xyz triples
    indices: bytes  # native-endian uint16 triangle triples
    classifications: bytes


class Document:
    """Not thread-safe: callers serialize this handle and its cursor together."""

    def __init__(self, library, handle, data):
        self.library, self.handle, self.data = library, handle, data
        self.cursor = c.c_void_p()
        try:
            self.info = record(Info)
            library.check(library.lib.ss_document_get_info(handle, c.byref(self.info)))
            library.check(library.lib.ss_scene_cursor_create(handle, c.byref(self.cursor)))
            self.cameras = []
            for index in range(self.info.camera_count):
                value = record(CameraRecord)
                library.check(library.lib.ss_document_camera(handle, index, c.byref(value)))
                self.cameras.append(Camera(value.timestamp_ns, value.raster_width, value.raster_height,
                    value.fx, value.fy, value.cx, value.cy, (value.tx, value.ty, value.tz),
                    (value.qx, value.qy, value.qz, value.qw)))
        except BaseException:
            self.close()
            raise

    def snapshot(self, timestamp):
        if not self.handle:
            raise SpatialError("Document は既に閉じられています.")
        lib, chunks, digest = self.library.lib, [], hashlib.sha256()
        self.library.check(lib.ss_scene_cursor_seek(self.cursor, timestamp))
        for index in range(lib.ss_scene_cursor_chunk_count(self.cursor)):
            view = record(MeshView)
            self.library.check(lib.ss_scene_cursor_chunk(self.cursor, index, c.byref(view)))
            cell = (view.cell.x, view.cell.y, view.cell.z)
            chunk = Chunk(cell, c.string_at(view.quantized_xyz, 6 * view.vertex_count),
                c.string_at(view.triangle_indices, 6 * view.triangle_count),
                c.string_at(view.classifications, view.triangle_count))
            digest.update(struct.pack("=iiiII", *cell, view.vertex_count, view.triangle_count))
            for data in (chunk.xyz, chunk.indices, chunk.classifications):
                digest.update(data)
            chunks.append(chunk)
        return chunks, digest.digest()

    def close(self):
        self.library.lib.ss_scene_cursor_release(self.cursor)
        self.library.lib.ss_document_release(self.handle)
        self.cursor, self.handle = c.c_void_p(), c.c_void_p()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

"""Generate bound HEIC and VFR MOV regression fixtures using the public C writer."""

import ctypes as c
import os
from pathlib import Path

from SpatialSnapshotB3d import media
from SpatialSnapshotB3d.native import HEADER, Library, OpenOptions, Vec3, MeshView, CameraRecord, record, U8P

RANDOM = c.CFUNCTYPE(c.c_int32, c.c_void_p, U8P, c.c_size_t)


class WriterOptions(c.Structure):
    _fields_ = HEADER + [("kind", c.c_uint32), ("gravity", Vec3), ("random_bytes", RANDOM),
                        ("random_context", c.c_void_p), ("resources", OpenOptions)]


class Update(c.Structure):
    _fields_ = HEADER + [("operation", c.c_uint32), ("mesh", MeshView)]


class Triangle(c.Structure):
    _fields_ = HEADER + [("vertices", Vec3 * 3), ("classification", c.c_uint32)]


@RANDOM
def random_bytes(_context, destination, size):
    c.memmove(destination, os.urandom(size), size)
    return 0


def writer_library():
    library = Library()
    lib = library.lib
    declarations = {
        "ss_writer_options_init": (None, [c.POINTER(WriterOptions)]),
        "ss_writer_create": (c.c_int32, [c.POINTER(WriterOptions), c.POINTER(c.c_void_p)]),
        "ss_writer_append_frame": (c.c_int32, [c.c_void_p, c.POINTER(CameraRecord), c.POINTER(Update), c.c_size_t, c.c_void_p]),
        "ss_writer_finish": (c.c_int32, [c.c_void_p, c.c_uint64]),
        "ss_writer_bytes": (c.c_int32, [c.c_void_p, c.POINTER(U8P), c.POINTER(c.c_size_t)]),
        "ss_writer_release": (None, [c.c_void_p]),
        "ss_mesh_partition": (c.c_int32, [c.POINTER(Triangle), c.c_size_t, c.c_void_p, c.POINTER(c.c_void_p)]),
        "ss_mesh_set_count": (c.c_uint32, [c.c_void_p]),
        "ss_mesh_set_chunk": (c.c_int32, [c.c_void_p, c.c_uint32, c.POINTER(MeshView)]),
        "ss_mesh_set_release": (None, [c.c_void_p]),
        "ss_heif_bind_memory": (c.c_int32, [c.c_void_p, c.c_size_t, c.c_void_p, c.c_void_p, c.POINTER(c.c_void_p)]),
        "ss_quicktime_bind_memory": (c.c_int32, [c.c_void_p, c.c_size_t, c.c_uint32, c.c_void_p, c.c_void_p, c.POINTER(c.c_void_p)]),
        "ss_binding_output_bytes": (c.c_int32, [c.c_void_p, c.POINTER(U8P), c.POINTER(c.c_size_t)]),
        "ss_binding_output_release": (None, [c.c_void_p]),
    }
    for name, (result, arguments) in declarations.items():
        function = getattr(lib, name)
        function.restype, function.argtypes = result, arguments
    return library


def gravity_still(host, destination, gravity, vertices):
    """Keep the encoded raster unchanged; only SSPS determines physical down."""
    library = writer_library()
    lib = library.lib
    options = record(WriterOptions)
    lib.ss_writer_options_init(c.byref(options))
    options.kind, options.gravity, options.random_bytes = 1, Vec3(*gravity), random_bytes
    writer, meshes = c.c_void_p(), c.c_void_p()
    library.check(lib.ss_writer_create(c.byref(options), c.byref(writer)))
    try:
        triangle = record(Triangle)
        triangle.vertices, triangle.classification = (Vec3 * 3)(*(Vec3(*v) for v in vertices)), 2
        library.check(lib.ss_mesh_partition(c.byref(triangle), 1, None, c.byref(meshes)))
        count = lib.ss_mesh_set_count(meshes)
        updates = (Update * count)()
        for i in range(count):
            updates[i] = record(Update)
            updates[i].operation, updates[i].mesh = 1, record(MeshView)
            library.check(lib.ss_mesh_set_chunk(meshes, i, c.byref(updates[i].mesh)))
        camera = record(CameraRecord)
        camera.raster_width, camera.raster_height = 640, 480
        camera.fx, camera.fy, camera.cx, camera.cy, camera.qw = 510, 570, 239, 179, 1
        library.check(lib.ss_writer_append_frame(writer, c.byref(camera), updates, count, None))
        library.check(lib.ss_writer_finish(writer, 0))
        pointer, size = U8P(), c.c_size_t()
        library.check(lib.ss_writer_bytes(writer, c.byref(pointer), c.byref(size)))
        ssps = c.string_at(pointer, size.value)
    finally:
        lib.ss_mesh_set_release(meshes)
        lib.ss_writer_release(writer)
    with library.open_ssps(ssps) as document:
        data = Path(host).read_bytes()
        buffer, output = c.create_string_buffer(data), c.c_void_p()
        try:
            library.check(lib.ss_heif_bind_memory(buffer, len(data), document.handle, None, c.byref(output)))
            pointer, size = U8P(), c.c_size_t()
            library.check(lib.ss_binding_output_bytes(output, c.byref(pointer), c.byref(size)))
            Path(destination).write_bytes(c.string_at(pointer, size.value))
            return Path(destination)
        finally:
            lib.ss_binding_output_release(output)


def vfr_movie(directory):
    directory = Path(directory)
    source = directory / "vfr-source.mov"
    media.run([media.executable("ffmpeg"), "-nostdin", "-v", "error", "-f", "lavfi", "-i",
        "testsrc=size=96x64:rate=10:duration=0.3", "-vf",
        r"settb=1/1000000000,setpts=if(eq(N\,0)\,0\,if(eq(N\,1)\,83333331\,199456789))",
        "-fps_mode", "passthrough", "-enc_time_base", "1:1000000000", "-c:v", "libx264", "-bf", "0",
        "-video_track_timescale", "1000000000", "-movie_timescale", "1000000000", "-use_editlist", "0",
        "-pix_fmt", "yuv420p", "-y", str(source)])
    library = writer_library()
    lib = library.lib

    options = record(WriterOptions)
    lib.ss_writer_options_init(c.byref(options))
    options.kind, options.gravity, options.random_bytes = 2, Vec3(0, 1, 0), random_bytes
    writer = c.c_void_p()
    library.check(lib.ss_writer_create(c.byref(options), c.byref(writer)))
    try:
        xyz = (c.c_uint16 * 9)(1000, 1000, 1000, 40000, 1000, 1000, 1000, 40000, 30000)
        indices, classes = (c.c_uint16 * 3)(0, 1, 2), (c.c_uint8 * 1)(2)
        update = record(Update)
        update.mesh = record(MeshView)
        update.mesh.cell.z = 2
        update.mesh.vertex_count, update.mesh.triangle_count = 3, 1
        update.mesh.quantized_xyz, update.mesh.triangle_indices, update.mesh.classifications = xyz, indices, classes
        for index, timestamp in enumerate((0, 83_333_331, 199_456_789)):
            camera = record(CameraRecord)
            camera.timestamp_ns, camera.raster_width, camera.raster_height = timestamp, 96, 64
            camera.fx, camera.fy, camera.cx, camera.cy, camera.qw = 60, 60, 47.5, 31.5, 1
            camera.tx = 0.01 * index
            update.operation = index  # Frame 1 PUT; frame 2 REMOVE.
            library.check(lib.ss_writer_append_frame(writer, c.byref(camera), c.byref(update) if index else None,
                                                     1 if index else 0, None))
        library.check(lib.ss_writer_finish(writer, 299_456_789))
        pointer, size = U8P(), c.c_size_t()
        library.check(lib.ss_writer_bytes(writer, c.byref(pointer), c.byref(size)))
        ssps = c.string_at(pointer, size.value)
    finally:
        lib.ss_writer_release(writer)
    with library.open_ssps(ssps) as document:
        data = source.read_bytes()
        buffer, output = c.create_string_buffer(data), c.c_void_p()
        try:
            library.check(lib.ss_quicktime_bind_memory(buffer, len(data), 0, document.handle, None, c.byref(output)))
            pointer, size = U8P(), c.c_size_t()
            library.check(lib.ss_binding_output_bytes(output, c.byref(pointer), c.byref(size)))
            destination = directory / "vfr.mov"
            destination.write_bytes(c.string_at(pointer, size.value))
            return destination
        finally:
            lib.ss_binding_output_release(output)

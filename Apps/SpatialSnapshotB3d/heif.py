# SPDX-License-Identifier: GPL-3.0-or-later
"""Small optional libheif adapter; the shared library is supplied by the user."""

import ctypes as c
import ctypes.util
from pathlib import Path
import struct
import zlib

from .native import SpatialError


class Error(c.Structure):
    _fields_ = [("code", c.c_int), ("subcode", c.c_int), ("message", c.c_char_p)]


def decode_primary(source, output, configured=""):
    path = configured or ctypes.util.find_library("heif")
    if not path:
        path = next((str(candidate) for candidate in (Path("/opt/homebrew/lib/libheif.dylib"),
                    Path("/usr/local/lib/libheif.dylib")) if candidate.is_file()), None)
    if not path:
        raise SpatialError("HEIC の復号には libheif が必要です. OS にインストールするか, アドオン設定で共有ライブラリを指定してください.")
    try:
        lib = c.CDLL(path)
    except OSError as error:
        raise SpatialError(f"libheif をロードできません: {error}") from error
    signatures = {
        "heif_context_alloc": (c.c_void_p, []),
        "heif_context_free": (None, [c.c_void_p]),
        "heif_context_read_from_file": (Error, [c.c_void_p, c.c_char_p, c.c_void_p]),
        "heif_context_get_primary_image_handle": (Error, [c.c_void_p, c.POINTER(c.c_void_p)]),
        "heif_image_handle_release": (None, [c.c_void_p]),
        "heif_image_handle_get_width": (c.c_int, [c.c_void_p]),
        "heif_image_handle_get_height": (c.c_int, [c.c_void_p]),
        "heif_decode_image": (Error, [c.c_void_p, c.POINTER(c.c_void_p), c.c_int, c.c_int, c.c_void_p]),
        "heif_image_release": (None, [c.c_void_p]),
        "heif_image_get_plane_readonly": (c.c_void_p, [c.c_void_p, c.c_int, c.POINTER(c.c_int)]),
    }
    for name, (result, arguments) in signatures.items():
        function = getattr(lib, name)
        function.restype, function.argtypes = result, arguments

    def check(error):
        if error.code:
            raise SpatialError("libheif: " + error.message.decode("utf-8", "replace"))

    context, handle, image = lib.heif_context_alloc(), c.c_void_p(), c.c_void_p()
    if not context:
        raise SpatialError("libheif のコンテキストを確保できません.")
    try:
        check(lib.heif_context_read_from_file(context, str(source).encode(), None))
        check(lib.heif_context_get_primary_image_handle(context, c.byref(handle)))
        width, height = lib.heif_image_handle_get_width(handle), lib.heif_image_handle_get_height(handle)
        if not 1 <= width <= 16384 or not 1 <= height <= 16384:
            raise SpatialError("HEIF primary のサイズが SSPS v1 の範囲外です.")
        check(lib.heif_decode_image(handle, c.byref(image), 1, 10, None))  # RGB / interleaved RGB8
        stride = c.c_int()
        pixels = lib.heif_image_get_plane_readonly(image, 10, c.byref(stride))  # interleaved channel
        if not pixels or stride.value < width * 3:
            raise SpatialError("libheif が有効な RGB plane を返しませんでした.")
        with Path(output).open("wb") as file:
            file.write(b"\x89PNG\r\n\x1a\n")

            def chunk(kind, data):
                file.write(struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data)))

            chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            chunk(b"sRGB", b"\0")
            compressor = zlib.compressobj(3)
            for row in range(height):
                packed = compressor.compress(b"\0" + c.string_at(pixels + row * stride.value, width * 3))
                if packed:
                    chunk(b"IDAT", packed)
            chunk(b"IDAT", compressor.flush())
            chunk(b"IEND", b"")
    finally:
        if image:
            lib.heif_image_release(image)
        if handle:
            lib.heif_image_handle_release(handle)
        lib.heif_context_free(context)

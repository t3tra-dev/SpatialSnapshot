# SPDX-License-Identifier: GPL-3.0-or-later
"""SpatialSnapshot import for Blender. The portable reader lives in the C core."""

bl_info = {
    "name": "SpatialSnapshot",
    "author": "SpatialSnapshot contributors",
    "version": (0, 1, 0),
    "blender": (5, 0, 0),
    "location": "File > Import > SpatialSnapshot",
    "description": "Import SSPS cameras, meshes and projected images from HEIC and MOV",
    "category": "Import-Export",
}


def register():
    from . import addon
    addon.register()


def unregister():
    from . import addon
    addon.unregister()

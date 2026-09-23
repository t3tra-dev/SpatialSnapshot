"""Smoke test an installed extension in an isolated Blender user profile."""
from pathlib import Path
import bpy

root = Path(__file__).resolve().parents[2]
identifier = "bl_ext.user_default.spatial_snapshot"
entry = bpy.context.preferences.addons.get(identifier)
assert entry is not None, "Extension was not enabled from the installed ZIP"
entry.preferences.cache_directory = str(root / ".build/b3d/cache")
assert not entry.preferences.library_path  # Exercise discovery of the packaged native library.
source = root / "Apps/SpatialSnapshotLab/Resources/Room.heic"
original_scene = bpy.context.scene
original_camera, original_world = original_scene.camera, original_scene.world
scenes_before = set(bpy.data.scenes)
result = bpy.ops.import_scene.spatial_snapshot(filepath=str(source))
assert result == {"FINISHED"}, result
assert bpy.context.scene == original_scene
assert set(bpy.data.scenes) == scenes_before
assert original_scene.camera == original_camera and original_scene.world == original_world
collection = original_scene.ssps_active_capture
assert collection.name in original_scene.collection.children
state = collection.ssps_capture
assert state.kind == "STILL"
assert len(state.mesh.data.polygons) > 0
assert state.image.packed_file
assert not state.last_error
assert bpy.ops.spatial_snapshot.view_capture() == {"FINISHED"}
assert original_scene.camera == state.camera and original_scene.world == state.world
print("Installed extension: active-scene collection import and explicit camera/background switching passed.")

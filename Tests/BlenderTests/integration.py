"""Run in a factory-startup Blender process; never touches the user's open project."""

from pathlib import Path
import sys
import unittest
from unittest.mock import patch

import bpy
import numpy as np
from mathutils import Quaternion, Vector
from bpy_extras.object_utils import world_to_camera_view

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "Apps"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import SpatialSnapshotB3d  # noqa: E402
from SpatialSnapshotB3d import scene, projection, media  # noqa: E402
from SpatialSnapshotB3d.native import Camera, SpatialError  # noqa: E402
from generated_media import vfr_movie, gravity_still  # noqa: E402

OUTPUT = ROOT / ".build/b3d/integration"
CACHE = ROOT / ".build/b3d/cache"
SOURCE = ROOT / "Apps/SpatialSnapshotLab/Resources"
OUTPUT.mkdir(parents=True, exist_ok=True)
SpatialSnapshotB3d.register()


def read_pixels(image):
    values = np.empty(len(image.pixels), dtype=np.float32)
    image.pixels.foreach_get(values)
    return values.reshape((-1, 4))[:, :3]


def render(capture, name):
    if bpy.context.window:
        bpy.context.window.scene = capture
    capture.render.engine = "CYCLES"
    capture.cycles.device = "CPU"
    capture.cycles.samples = 8
    capture.cycles.use_denoising = False
    capture.render.image_settings.file_format = "PNG"
    capture.render.image_settings.color_mode = "RGBA"
    capture.render.filepath = str(OUTPUT / name)
    bpy.ops.render.render(scene=capture.name, write_still=True)
    image = bpy.data.images.load(capture.render.filepath, check_existing=False)
    values = read_pixels(image)
    bpy.data.images.remove(image)
    return values


class BlenderImportTests(unittest.TestCase):
    def setUp(self):
        self.original_scene_name = bpy.context.scene.name
        self.datablocks = ("scenes", "collections", "objects", "meshes", "cameras", "materials",
                           "worlds", "node_groups", "texts", "images")
        self.existing = {name: set(getattr(bpy.data, name).keys()) for name in self.datablocks}
        target = bpy.data.scenes.new(self._testMethodName)
        target.view_settings.view_transform = "Standard"
        target.view_settings.look = "None"
        target.render.engine = "CYCLES"
        bpy.context.window.scene = target

    def tearDown(self):
        scene.clear_runtimes()
        bpy.context.window.scene = bpy.data.scenes[self.original_scene_name]
        for name in self.datablocks:
            items = getattr(bpy.data, name)
            for item in list(items):
                if item.name not in self.existing[name]:
                    items.remove(item, do_unlink=True)

    def test_01_camera_intrinsics_pose_and_gravity(self):
        test_scene = bpy.data.scenes.new("Calibration test")
        obj = bpy.data.objects.new("Calibration camera", bpy.data.cameras.new("Calibration camera"))
        test_scene.collection.objects.link(obj)
        test_scene.camera = obj
        q = Quaternion(Vector((1, 2, 3)).normalized(), 0.37)
        camera = Camera(0, 640, 480, 510, 570, 239, 179, (0.2, -0.3, 0.4), (q.x, q.y, q.z, q.w))
        for gravity in ((0, 1, 0), (1, 0, 0), (0, 0, 1), (0.1, 0.9, 0.3)):
            basis = projection.scene_basis(gravity)
            self.assertLess((basis @ Vector(gravity).normalized() - Vector((0, 0, -1))).length, 1e-6)
            self.assertAlmostEqual(basis.determinant(), 1, places=6)
            projection.set_camera(test_scene, obj, camera, basis)
            test_scene.view_layers[0].update()
            for u, v, depth in ((0, 0, 1), (639, 479, 2), (117.2, 355.8, 3.7), (239, 179, 4)):
                local = Vector(((u - camera.cx) * depth / camera.fx, (v - camera.cy) * depth / camera.fy, depth))
                world = basis @ (q @ local + Vector(camera.translation))
                result = world_to_camera_view(test_scene, obj, world)
                self.assertAlmostEqual(result.x, (u + 0.5) / camera.width, places=5)
                self.assertAlmostEqual(result.y, 1 - (v + 0.5) / camera.height, places=5)
        bpy.data.objects.remove(obj, do_unlink=True)
        bpy.data.scenes.remove(test_scene)

    def test_02_still_mesh_world_and_boundary_render(self):
        original = bpy.context.scene
        existing = bpy.data.objects.new("Existing object", None)
        original.collection.objects.link(existing)
        old_camera = bpy.data.objects.new("Existing camera", bpy.data.cameras.new("Existing camera"))
        original.collection.objects.link(old_camera)
        original.camera = old_camera
        original.world = bpy.data.worlds.new("Existing world")
        old_world = original.world
        original.render.resolution_x, original.render.resolution_y = 1280, 720
        original.frame_start, original.frame_end = 5, 500
        original.frame_set(37)
        before = set(original.objects)
        scenes_before = set(bpy.data.scenes)
        capture = scene.import_capture(SOURCE / "Room.heic", original, CACHE)
        state = capture.ssps_capture
        self.assertEqual(set(bpy.data.scenes), scenes_before)
        self.assertEqual(set(original.objects), before | {state.camera, state.mesh})
        self.assertIn(capture, original.collection.children.values())
        self.assertEqual(bpy.context.scene, original)
        self.assertEqual(original.camera, old_camera)
        self.assertEqual(original.world, old_world)
        self.assertEqual((original.render.resolution_x, original.render.resolution_y), (1280, 720))
        self.assertEqual((original.frame_start, original.frame_end, original.frame_current), (5, 500, 37))
        self.assertEqual(state.start_frame, 37)
        self.assertGreater(len(state.mesh.data.polygons), 0)
        self.assertIsNotNone(state.image.packed_file)
        self.assertEqual(state.material.node_tree.nodes["SSPS Image"].image, state.world.node_tree.nodes["SSPS Image"].image)
        scene.view_capture(original, capture)
        self.assertEqual(state.previous_world, old_world)
        reference = read_pixels(state.image)
        state.mesh.hide_render = True
        background = render(original, "still-world.png")
        print("WORLD ERROR", np.abs(reference - background).mean(), flush=True)
        self.assertLess(float(np.abs(reference - background).mean()), 0.012)
        state.mesh.hide_render = False
        projected = render(original, "still-mesh.png")
        print("MESH ERROR", np.abs(reference - projected).mean(), flush=True)
        self.assertLess(float(np.abs(reference - projected).mean()), 0.012)
        # One oblique triangle leaves the rest to the world, exercising the boundary
        # and perspective projection across a large depth gradient.
        camera = scene.runtime_for(capture).document.cameras[0]
        basis = scene.runtime_for(capture).basis
        vertices = [basis @ Vector(((u-camera.cx)*z/camera.fx, (v-camera.cy)*z/camera.fy, z))
                    for u, v, z in ((30, 30, 1), (575, 64, 4), (65, 430, 2.5))]
        state.mesh.data.clear_geometry()
        state.mesh.data.from_pydata(vertices, [], [(0, 1, 2)])
        state.mesh.data.update()
        boundary = render(original, "still-boundary.png")
        print("BOUNDARY ERROR", np.abs(background - boundary).mean(), flush=True)
        self.assertLess(float(np.abs(background - boundary).mean()), 0.003)

    def test_03_movie_fps_scrubbing_cache_and_reopen(self):
        original = bpy.context.scene
        original.render.fps, original.render.fps_base = 24, 1
        capture = scene.import_capture(SOURCE / "Room.mov", original, CACHE, start_frame=7)
        state = capture.ssps_capture
        self.assertEqual(original.frame_end, 250)
        self.assertEqual(original.frame_current, 1)
        scene.view_capture(original, capture)
        for frame, expected in ((7, 0), (9, 1), (31, 15), (78, 44), (8, 0), (7, 0)):
            original.frame_set(frame)
            self.assertFalse(state.last_error)
            self.assertEqual(state.current_sample, expected)
            self.assertTrue(state.image.filepath.endswith(f"frame-{expected:08d}.png"))
        original.render.fps = 30
        original.frame_set(31)
        self.assertEqual(state.current_sample, 12)
        original.render.fps_base = 1.001
        original.frame_set(37)
        self.assertEqual(state.current_sample, 15)
        reference = read_pixels(state.image)
        rendered = render(original, "movie-moving-camera.png")
        self.assertLess(float(np.abs(reference - rendered).mean()), 0.012)
        moving = state.camera.matrix_world.translation.copy()
        original.frame_set(7)
        self.assertGreater((state.camera.matrix_world.translation - moving).length, 0.01)
        missing = media.frame_path(state.cache_directory, 1)
        backup = missing.with_suffix(".temporarily-missing")
        missing.rename(backup)
        try:
            original.frame_set(10)
            self.assertTrue(state.last_error)
        finally:
            backup.rename(missing)
        original.frame_set(10)
        self.assertFalse(state.last_error)
        name = capture.name
        scene_name = original.name
        path = OUTPUT / "reopen.blend"
        bpy.ops.wm.save_as_mainfile(filepath=str(path), check_existing=False)
        bpy.ops.wm.open_mainfile(filepath=str(path))
        reopened = bpy.data.collections[name]
        bpy.data.scenes[scene_name].frame_set(37)
        self.assertFalse(reopened.ssps_capture.last_error)
        self.assertEqual(reopened.ssps_capture.current_sample, 15)
        self.assertEqual(len([i for i in bpy.data.images if i.name.startswith("SSPS ")]), 1)

    def test_04_vfr_and_geometry_put_remove_during_render(self):
        source = vfr_movie(OUTPUT)
        original = bpy.context.scene
        original.render.fps, original.render.fps_base = 24, 1
        capture = scene.import_capture(source, original, CACHE, start_frame=1)
        state = capture.ssps_capture
        runtime = scene.runtime_for(capture)
        scene.view_capture(original, capture)
        self.assertEqual(runtime.timestamps, [0, 83_333_331, 199_456_789])
        for frame, expected, faces in ((1, 0, 0), (3, 1, 1), (5, 1, 1), (6, 2, 0), (3, 1, 1)):
            original.frame_set(frame)
            self.assertFalse(state.last_error)
            self.assertEqual(state.current_sample, expected)
            self.assertEqual(len(state.mesh.data.polygons), faces)
        original.render.engine = "CYCLES"
        original.cycles.device, original.cycles.samples = "CPU", 4
        original.cycles.use_denoising = False
        original.render.image_settings.file_format = "PNG"
        original.render.filepath = str(OUTPUT / "vfr-animation-")
        original.frame_start, original.frame_end = 1, 6
        bpy.ops.render.render(animation=True, scene=original.name)
        for frame, sample in ((1, 0), (3, 1), (5, 1), (6, 2)):
            actual = bpy.data.images.load(str(OUTPUT / f"vfr-animation-{frame:04d}.png"), check_existing=False)
            expected = bpy.data.images.load(str(media.frame_path(state.cache_directory, sample)), check_existing=False)
            self.assertLess(float(np.abs(read_pixels(actual) - read_pixels(expected)).mean()), 0.04)
            bpy.data.images.remove(actual)
            bpy.data.images.remove(expected)

    def test_05_unbound_import_is_atomic(self):
        before = tuple(len(getattr(bpy.data, name)) for name in self.datablocks)
        with self.assertRaises(SpatialError):
            scene.import_capture(ROOT / "Fixtures/bindings/interop/source.heic", bpy.context.scene, CACHE)
        self.assertEqual(before, tuple(len(getattr(bpy.data, name)) for name in self.datablocks))
        # Also fail after creating image/material/camera datablocks, before linking.
        with patch.object(projection, "set_camera", side_effect=SpatialError("unsupported calibration")):
            with self.assertRaises(SpatialError):
                scene.import_capture(SOURCE / "Room.heic", bpy.context.scene, CACHE)
        self.assertEqual(before, tuple(len(getattr(bpy.data, name)) for name in self.datablocks))

    def test_06_still_orientation_comes_from_ssps_gravity(self):
        # Identical encoded pixels, different SSPS gravity: neither HEIC orientation
        # nor width/height is allowed to decide the scene's up or the camera's roll.
        for index, (gravity, turns) in enumerate((((1, 0, 0), 1), ((-1, 0, 0), 3),
                ((0, -1, 0), 2), ((0, 1, 0), 0), ((0.85, 0.015, 0.53), 1),
                ((0, 0, 1), 0), ((0, 0, -1), 0))):
            with self.subTest(gravity=gravity):
                down = Vector(gravity).normalized()
                reference_axis = min((Vector((1, 0, 0)), Vector((0, 1, 0)), Vector((0, 0, 1))),
                                     key=lambda axis: abs(axis.dot(down)))
                right = reference_axis.cross(down).normalized()
                forward = right.cross(down)
                center = down + Vector((0, 0, 2)) - down * (2 * down.z)
                vertices = (center, center + right * 0.4, center + forward * 0.4)
                source = gravity_still(SOURCE / "Room.heic", OUTPUT / f"gravity-{index}.heic", down, vertices)
                target = bpy.context.scene
                for other in scene.captures(target):
                    other.hide_render = True
                capture = scene.import_capture(source, bpy.context.scene, CACHE)
                state = capture.ssps_capture
                runtime = scene.runtime_for(capture)
                scene.view_capture(target, capture)
                target.view_layers[0].update()
                self.assertLess((runtime.basis @ down - Vector((0, 0, -1))).length, 1e-6)
                self.assertGreater(len(state.mesh.data.polygons), 0)
                for vertex in state.mesh.data.vertices:
                    world = state.mesh.matrix_world @ vertex.co
                    self.assertAlmostEqual(world.z, -1, delta=2e-5)
                self.assertEqual((target.render.resolution_x, target.render.resolution_y),
                                 (480, 640) if turns % 2 else (640, 480))
                camera_down = state.camera.matrix_world.to_3x3().transposed() @ Vector((0, 0, -1))
                if abs(down.z) < 0.9:
                    self.assertLess(camera_down.y, -0.8)
                    self.assertLess(abs(camera_down.x), 0.02)
                if index < 4:
                    expected = np.rot90(read_pixels(state.image).reshape((480, 640, 3)), turns).reshape((-1, 3))
                    actual = render(target, f"gravity-{index}.png")
                    self.assertLess(float(np.abs(expected - actual).mean()), 0.012)

    def test_07_multiple_movies_share_timeline_but_not_capture_state(self):
        target = bpy.context.scene
        target.render.fps, target.render.fps_base = 24, 1
        target.render.resolution_x, target.render.resolution_y = 800, 450
        target.view_settings.view_transform = "AgX"
        target.view_settings.exposure = 0.25
        original_world = bpy.data.worlds.new("Original world")
        target.world = original_world
        first = scene.import_capture(SOURCE / "Room.mov", target, CACHE, start_frame=1)
        second = scene.import_capture(vfr_movie(OUTPUT), target, CACHE, start_frame=3)
        a, b = first.ssps_capture, second.ssps_capture
        self.assertNotEqual(a.metadata, b.metadata)
        self.assertNotEqual(a.world, b.world)
        # Moving/renaming a capture in the Outliner must not lose its timeline.
        folder = bpy.data.collections.new("Nested captures")
        target.collection.children.link(folder)
        folder.children.link(second)
        target.collection.children.unlink(second)
        second.name = "Renamed capture"
        for frame, index_a, index_b in ((1, 0, 0), (3, 1, 0), (5, 2, 1), (8, 4, 2), (5, 2, 1)):
            target.frame_set(frame)
            self.assertFalse(a.last_error or b.last_error)
            self.assertEqual((a.current_sample, b.current_sample), (index_a, index_b))
            self.assertTrue(a.image.filepath.endswith(f"frame-{index_a:08d}.png"))
            self.assertTrue(b.image.filepath.endswith(f"frame-{index_b:08d}.png"))
            self.assertEqual(len(b.mesh.data.polygons), int(index_b == 1))
            self.assertIsNone(target.camera)
            self.assertEqual(target.world, original_world)
            self.assertEqual((target.render.resolution_x, target.render.resolution_y), (800, 450))
        self.assertEqual(target.view_settings.view_transform, "AgX")
        self.assertEqual(target.view_settings.exposure, 0.25)
        scene.view_capture(target, first)
        target.frame_set(8)
        self.assertEqual(target.world, a.world)
        self.assertEqual((target.render.resolution_x, target.render.resolution_y), (640, 480))
        scene.view_capture(target, second)
        target.frame_set(5)
        self.assertEqual(target.world, b.world)
        self.assertEqual((target.render.resolution_x, target.render.resolution_y), (96, 64))
        b.enabled = False
        target.frame_set(8)
        self.assertEqual((a.current_sample, b.current_sample), (4, 1))
        b.enabled = True
        scene_name, first_name, second_name = target.name, first.name, second.name
        path = OUTPUT / "multiple-captures.blend"
        bpy.ops.wm.save_as_mainfile(filepath=str(path), check_existing=False)
        bpy.ops.wm.open_mainfile(filepath=str(path))
        target = bpy.data.scenes[scene_name]
        first, second = bpy.data.collections[first_name], bpy.data.collections[second_name]
        a, b = first.ssps_capture, second.ssps_capture
        target.frame_set(5)
        self.assertEqual((a.current_sample, b.current_sample), (2, 1))
        self.assertEqual(target.ssps_active_capture, second)
        self.assertEqual(target.world, b.world)
        self.assertEqual((target.render.resolution_x, target.render.resolution_y), (96, 64))
        document = scene.runtime_for(second).document
        bpy.data.collections.remove(second)
        target.frame_set(9)
        self.assertFalse(document.handle)
        self.assertFalse(a.last_error)
        self.assertEqual(a.current_sample, 5)
        self.assertEqual(scene.selected_capture(target), first)

    def test_08_scene_units_are_preserved(self):
        target = bpy.context.scene
        target.unit_settings.system = "METRIC"
        target.unit_settings.scale_length = 0.01
        source = gravity_still(SOURCE / "Room.heic", OUTPUT / "centimeter-scene.heic", (0, 1, 0),
                               ((0, 1, 2), (0.4, 1, 2), (0, 1, 2.4)))
        collection = scene.import_capture(source, target, CACHE)
        for vertex in collection.ssps_capture.mesh.data.vertices:
            self.assertAlmostEqual(vertex.co.z, -100, delta=0.002)
        movie = scene.import_capture(vfr_movie(OUTPUT), target, CACHE, start_frame=1)
        target.render.fps, target.render.fps_base = 24, 1
        target.frame_set(6)
        target.view_layers[0].update()
        self.assertEqual(movie.ssps_capture.current_sample, 2)
        self.assertAlmostEqual(movie.ssps_capture.camera.matrix_world.translation.x, 2, places=5)
        self.assertEqual(target.unit_settings.system, "METRIC")
        self.assertAlmostEqual(target.unit_settings.scale_length, 0.01, places=7)

    def test_09_saved_scene_owned_capture_migrates_to_collection(self):
        target = bpy.context.scene
        target.render.fps, target.render.fps_base = 24, 1
        collection = scene.import_capture(SOURCE / "Room.mov", target, CACHE, start_frame=1)
        scene.view_capture(target, collection)
        state = collection.ssps_capture
        # Recreate the saved layout of the original add-on: metadata on Scene only.
        fields = [prop.identifier for prop in state.bl_rna.properties if not prop.is_readonly]
        for name in fields:
            setattr(target.ssps_capture, name, getattr(state, name))
        for name in fields:
            state.property_unset(name)
        target.ssps_active_capture = None
        scene.clear_runtimes()
        scene_name, collection_name = target.name, collection.name
        scenes_before, collections_before = set(bpy.data.scenes.keys()), set(bpy.data.collections.keys())
        path = OUTPUT / "legacy-scene-capture.blend"
        bpy.ops.wm.save_as_mainfile(filepath=str(path), check_existing=False)
        bpy.ops.wm.open_mainfile(filepath=str(path))
        target = bpy.data.scenes[scene_name]
        collection = bpy.data.collections[collection_name]
        self.assertEqual(set(bpy.data.scenes.keys()), scenes_before)
        self.assertEqual(set(bpy.data.collections.keys()), collections_before)
        self.assertFalse(target.ssps_capture.kind)
        self.assertEqual(target.ssps_active_capture, collection)
        target.frame_set(25)
        self.assertEqual(collection.ssps_capture.current_sample, 15)
        self.assertFalse(collection.ssps_capture.last_error)
        self.assertEqual(target.camera, collection.ssps_capture.camera)


result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(BlenderImportTests))
SpatialSnapshotB3d.unregister()
if not result.wasSuccessful():
    raise SystemExit(1)

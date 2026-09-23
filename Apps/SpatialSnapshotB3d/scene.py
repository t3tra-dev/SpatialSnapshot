# SPDX-License-Identifier: GPL-3.0-or-later
"""Blender datablocks and persistent, serial frame evaluation."""

import base64
from dataclasses import replace
import json
from pathlib import Path
import threading

import bpy
from bpy.app.handlers import persistent
import numpy as np

from . import media, projection, timing
from .native import Library, SpatialError

_runtimes = {}
_lock = threading.RLock()

def _library_path():
    return ""


def set_library_provider(provider):
    global _library_path
    _library_path = provider


def clear_runtimes(*_):
    with _lock:
        for runtime in _runtimes.values():
            runtime.document.close()
        _runtimes.clear()


def fill_mesh(mesh, chunks, basis, meters_per_unit=1.0):
    vertices, triangles, classifications, offset = [], [], [], 0
    transform = np.asarray(basis, dtype=np.float64) / meters_per_unit
    for chunk in chunks:
        quantized = np.frombuffer(chunk.xyz, dtype=np.uint16).reshape((-1, 3))
        points = np.asarray(chunk.cell, dtype=np.float64) * 0.5 + quantized * (0.5 / 65535)
        vertices.append(np.einsum("ij,kj->ik", points, transform))
        triangles.append(np.frombuffer(chunk.indices, dtype=np.uint16).astype(np.int32) + offset)
        classifications.append(np.frombuffer(chunk.classifications, dtype=np.uint8).astype(np.int32))
        offset += len(points)
    mesh.clear_geometry()
    if not vertices:
        mesh.update()
        return
    positions = np.concatenate(vertices).astype(np.float32).ravel()
    indices, classes = np.concatenate(triangles), np.concatenate(classifications)
    count = len(classes)
    mesh.vertices.add(len(positions) // 3)
    mesh.vertices.foreach_set("co", positions)
    mesh.loops.add(len(indices))
    mesh.loops.foreach_set("vertex_index", indices)
    mesh.polygons.add(count)
    mesh.polygons.foreach_set("loop_start", np.arange(count, dtype=np.int32) * 3)
    mesh.polygons.foreach_set("loop_total", np.full(count, 3, dtype=np.int32))
    attribute = mesh.attributes.get("ssps_classification") or mesh.attributes.new("ssps_classification", "INT", "FACE")
    attribute.data.foreach_set("value", classes)
    mesh.update()


class Runtime:
    def __init__(self, document):
        self.document = document
        gravity = document.info.gravity
        self.basis = projection.scene_basis((gravity.x, gravity.y, gravity.z))
        # One orientation for the whole capture, also when scrubbing a movie.
        self.quarter_turns = projection.initial_quarter_turns((gravity.x, gravity.y, gravity.z))
        self.timestamps = [c.timestamp_ns for c in document.cameras]
        self.index, self.mesh_hash = None, None

    def camera_at(self, state, index):
        camera = self.document.cameras[index]
        return replace(camera, translation=tuple(value / state.meters_per_unit for value in camera.translation))

    def update(self, scene, collection, force=False):
        state = collection.ssps_capture
        if not all((state.camera, state.mesh, state.material, state.world, state.projection)):
            raise SpatialError("収録のカメラ・メッシュ・マテリアルが削除されています. 同期を無効にするか, 再インポートしてください.")
        index = timing.sample_index(self.timestamps, scene.frame_current, scene.frame_subframe,
                                    state.start_frame, scene.render.fps, scene.render.fps_base)
        # Re-apply camera calibration even when FPS changes leave the source index unchanged.
        camera = self.camera_at(state, index)
        if force or index != self.index:
            path = media.frame_path(bpy.path.abspath(state.cache_directory), index)
            if not path.is_file():
                raise SpatialError(f"画像キャッシュがありません. 「キャッシュを再作成」を実行してください: {path}")
            image = bpy.data.images.load(str(path), check_existing=False)
            image.name = f"SSPS {state.capture_id[:8]} frame {index}"
            image.colorspace_settings.name = "sRGB"
            try:
                if tuple(image.size) != (camera.width, camera.height):
                    raise SpatialError("画像キャッシュのサイズがカメラと一致しません.")
                chunks, digest = self.document.snapshot(camera.timestamp_ns)
                if force or digest != self.mesh_hash:
                    fill_mesh(state.mesh.data, chunks, self.basis, state.meters_per_unit)
                previous = state.image
                for owner in (state.material, state.world):
                    owner.node_tree.nodes["SSPS Image"].image = image
                state.image = image
                if self.document.info.kind == 1:
                    image.pack()  # A still .blend does not need its decoded PNG to render.
                if previous and previous.users == 0:
                    bpy.data.images.remove(previous)
                self.index, self.mesh_hash = index, digest
            except BaseException:
                if image.users == 0:
                    bpy.data.images.remove(image)
                raise
        projection.set_camera(scene, state.camera, projection.display_camera(camera, self.quarter_turns), self.basis,
                              configure_render=scene.camera == state.camera, meters_per_unit=state.meters_per_unit)
        projection.update_projection(state.projection, camera, self.basis)
        state.current_sample = index
        state.timestamp_seconds = camera.timestamp_ns / timing.NANOSECONDS
        state.last_error = ""


def collections_in(scene):
    """Visit each linked collection once, including captures nested by the user."""
    seen, pending = set(), list(reversed(scene.collection.children))
    while pending:
        collection = pending.pop()
        key = collection.as_pointer()
        if key in seen:
            continue
        seen.add(key)
        yield collection
        pending.extend(reversed(collection.children))


def captures(scene):
    return [collection for collection in collections_in(scene) if collection.ssps_capture.kind]


def selected_capture(scene):
    available = captures(scene)
    if scene.ssps_active_capture in available:
        return scene.ssps_active_capture
    return next((collection for collection in available if collection.ssps_capture.camera == scene.camera),
                available[0] if available else None)


def runtime_for(collection):
    key, state = collection.as_pointer(), collection.ssps_capture
    runtime = _runtimes.get(key)
    if runtime is None:
        if not state.metadata:
            raise SpatialError("埋め込まれた SSPS データがありません.")
        try:
            payload = json.loads(state.metadata.as_string())
            if payload["version"] != 1:
                raise ValueError("unsupported project data version")
            data = base64.b64decode(payload["ssps"], validate=True)
        except (ValueError, KeyError, TypeError) as error:
            raise SpatialError(f"プロジェクトの SSPS データが不正です: {error}") from error
        runtime = Runtime(Library(_library_path()).open_ssps(data))
        _runtimes[key] = runtime
    return runtime


def prune_runtimes():
    live = {collection.as_pointer() for scene in bpy.data.scenes for collection in captures(scene)}
    for key in list(_runtimes):
        if key not in live:
            _runtimes.pop(key).document.close()


def update_capture(scene, collection, *, force=False):
    with _lock:
        state = collection.ssps_capture
        if not state.enabled and not force:
            return
        try:
            runtime_for(collection).update(scene, collection, force=force)
        except Exception as error:
            message = str(error)
            if state.last_error != message:
                print(f"SpatialSnapshot: {collection.name}: {message}")
            state.last_error = message


def update_scene(scene):
    with _lock:
        prune_runtimes()
        for collection in captures(scene):
            if collection.ssps_capture.kind == "VIDEO":
                update_capture(scene, collection)


def view_capture(scene, collection):
    """Explicitly select this capture's camera, world and raster for the scene."""
    if collection not in captures(scene):
        raise SpatialError("このシーンにリンクされた収録コレクションを選んでください.")
    state = collection.ssps_capture
    if not state.camera or not state.world:
        raise SpatialError("収録のカメラまたはワールドが削除されています.")
    with _lock:
        runtime = runtime_for(collection)
        if state.kind == "VIDEO" and state.enabled:
            runtime.update(scene, collection)
        camera = projection.display_camera(runtime.camera_at(state, state.current_sample), runtime.quarter_turns)
        # Retain the original World datablock even after saving with a capture active.
        if scene.world != state.world and not state.previous_world:
            state.previous_world = scene.world
        scene.camera, scene.world = state.camera, state.world
        scene.ssps_active_capture = collection
        projection.set_render_camera(scene, camera)


def migrate_legacy_capture(scene):
    """Move saved 0.1.0 scene-owned metadata onto its existing collection."""
    legacy = scene.ssps_capture
    if not legacy.kind:
        return
    collection = next((item for item in collections_in(scene) if not item.ssps_capture.kind and
                       legacy.mesh and legacy.camera and legacy.mesh.name in item.objects and
                       legacy.camera.name in item.objects), None)
    if collection is None:
        collection = bpy.data.collections.new(scene.name + " · SSPS")
        scene.collection.children.link(collection)
        for obj in (legacy.mesh, legacy.camera):
            if obj:
                collection.objects.link(obj)
    fields = [prop.identifier for prop in legacy.bl_rna.properties if not prop.is_readonly]
    for name in fields:
        setattr(collection.ssps_capture, name, getattr(legacy, name))
    for name in fields:
        legacy.property_unset(name)
    scene.ssps_active_capture = collection


@persistent
def frame_changed(scene, _depsgraph=None):
    update_scene(scene)


@persistent
def before_load(_dummy):
    clear_runtimes()


@persistent
def after_load(_dummy):
    clear_runtimes()
    for scene in bpy.data.scenes:
        migrate_legacy_capture(scene)
        # Still geometry, projection nodes and packed image are self-contained.
        update_scene(scene)


@persistent
def after_undo(_dummy):
    clear_runtimes()
    for scene in bpy.data.scenes:
        migrate_legacy_capture(scene)
        update_scene(scene)


@persistent
def before_render(scene):
    update_scene(scene)
    for collection in captures(scene):
        state = collection.ssps_capture
        if state.kind == "VIDEO" and state.enabled and state.last_error:
            raise SpatialError(f"{collection.name}: {state.last_error}")


HANDLERS = (("frame_change_pre", frame_changed), ("load_pre", before_load), ("load_post", after_load),
            ("undo_post", after_undo), ("redo_post", after_undo), ("render_pre", before_render))


def install_handlers():
    for name, callback in HANDLERS:
        collection = getattr(bpy.app.handlers, name)
        if callback not in collection:
            collection.append(callback)


def remove_handlers():
    for name, callback in HANDLERS:
        collection = getattr(bpy.app.handlers, name)
        if callback in collection:
            collection.remove(callback)
    clear_runtimes()


def import_capture(filename, source_scene, cache_root, tools=None, library_path="", start_frame=None):
    """Add an independently managed collection without changing scene settings."""
    tools = tools or media.Tools()
    filename = Path(filename).resolve()
    document, info, samples = Library(library_path).open_media(filename)
    owned = []
    collection = None
    previous_selection = source_scene.ssps_active_capture

    def own(collection, value):
        owned.append((collection, value))
        return value

    try:
        cache = media.prepare_cache(filename, cache_root, info, samples, tools)
        collection = own(bpy.data.collections, bpy.data.collections.new(filename.stem + " · SSPS"))
        state = collection.ssps_capture
        state.start_frame = source_scene.frame_current if start_frame is None else start_frame
        state.meters_per_unit = source_scene.unit_settings.scale_length
        state.source_path, state.cache_directory = str(filename), str(cache)
        state.capture_id = bytes(document.info.stream_id).hex()
        state.kind = "VIDEO" if document.info.kind == 2 else "STILL"
        state.sample_count = len(document.cameras)
        mesh = own(bpy.data.meshes, bpy.data.meshes.new("SSPS Mesh"))
        camera = own(bpy.data.cameras, bpy.data.cameras.new("SSPS Camera"))
        state.mesh = own(bpy.data.objects, bpy.data.objects.new("SSPS Mesh", mesh))
        state.camera = own(bpy.data.objects, bpy.data.objects.new("SSPS Camera", camera))
        collection.objects.link(state.mesh)
        collection.objects.link(state.camera)
        state.projection = own(bpy.data.node_groups, projection.projection_group("SSPS Camera Projection"))
        state.material = own(bpy.data.materials, bpy.data.materials.new("SSPS Projected Image"))
        state.world = own(bpy.data.worlds, bpy.data.worlds.new("SSPS Image Background"))
        projection.image_shader(state.material, state.projection, None)
        projection.image_shader(state.world, state.projection, None, world=True)
        mesh.materials.append(state.material)
        state.metadata = own(bpy.data.texts, bpy.data.texts.new("SSPS Data " + state.capture_id[:8]))
        state.metadata.write(json.dumps({"version": 1, "ssps": base64.b64encode(document.data).decode("ascii")}))
        runtime = Runtime(document)
        runtime.update(source_scene, collection, force=True)
        state.enabled = True
        with _lock:
            prune_runtimes()
            source_scene.collection.children.link(collection)
            source_scene.ssps_active_capture = collection
            _runtimes[collection.as_pointer()] = runtime
        if state.kind == "VIDEO":
            source_scene.render.use_lock_interface = True
        return collection
    except BaseException:
        document.close()
        image = collection.ssps_capture.image if collection else None
        if collection:
            _runtimes.pop(collection.as_pointer(), None)
            if source_scene.ssps_active_capture == collection:
                source_scene.ssps_active_capture = previous_selection
        # These IDs reference each other through collection properties and shader
        # nodes. Remove only this import's IDs together, without dangling owners.
        created = [value for _collection, value in owned]
        if image:
            created.append(image)
        if created:
            bpy.data.batch_remove(ids=created)
        raise


def rebuild_cache(scene, collection, cache_root, tools, library_path=""):
    state = collection.ssps_capture
    document, info, samples = Library(library_path).open_media(bpy.path.abspath(state.source_path))
    with document:
        stored = json.loads(state.metadata.as_string())
        if document.data != base64.b64decode(stored["ssps"], validate=True):
            raise SpatialError("指定したファイルの SSPS がこのシーンと一致しません.")
        cache = media.prepare_cache(bpy.path.abspath(state.source_path), cache_root, info, samples, tools, force=True)
    state.cache_directory = str(cache)
    update_capture(scene, collection, force=True)

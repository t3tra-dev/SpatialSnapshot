# SPDX-License-Identifier: GPL-3.0-or-later
import bpy
from bpy.props import BoolProperty, FloatProperty, IntProperty, PointerProperty, StringProperty
from bpy_extras.io_utils import ImportHelper, poll_file_object_drop
from pathlib import Path

from . import media, scene

PACKAGE = __package__


class SSPSPreferences(bpy.types.AddonPreferences):
    bl_idname = PACKAGE
    library_path: StringProperty(name="SpatialSnapshot C Library", subtype="FILE_PATH")
    ffmpeg_path: StringProperty(name="FFmpeg", subtype="FILE_PATH")
    ffprobe_path: StringProperty(name="FFprobe", subtype="FILE_PATH")
    heif_library: StringProperty(name="libheif (macOS では省略可)", subtype="FILE_PATH")
    cache_directory: StringProperty(name="画像キャッシュ", subtype="DIR_PATH")

    def draw(self, context):
        self.layout.label(text="空欄の場合は同梱ライブラリと OS のツールを使用します.")
        for name in ("library_path", "ffmpeg_path", "ffprobe_path", "heif_library", "cache_directory"):
            self.layout.prop(self, name)


def options():
    entry = bpy.context.preferences.addons.get(PACKAGE)
    prefs = entry.preferences if entry else None

    def path(name):
        value = getattr(prefs, name, "")
        return bpy.path.abspath(value) if value else ""

    cache = path("cache_directory") or str(Path(bpy.utils.user_resource("DATAFILES")) / "SpatialSnapshotB3d")
    return path("library_path"), cache, media.Tools(path("ffmpeg_path"), path("ffprobe_path"), path("heif_library"))


class SSPCapture(bpy.types.PropertyGroup):
    enabled: BoolProperty(name="タイムライン同期", default=False)
    kind: StringProperty()
    capture_id: StringProperty()
    source_path: StringProperty(name="元の収録", subtype="FILE_PATH")
    cache_directory: StringProperty(name="画像キャッシュ", subtype="DIR_PATH")
    start_frame: IntProperty(name="開始フレーム", default=1, min=-1048574, max=1048574)
    current_sample: IntProperty()
    sample_count: IntProperty()
    meters_per_unit: FloatProperty(default=1.0, min=1e-9)
    timestamp_seconds: FloatProperty(precision=6)
    last_error: StringProperty()
    camera: PointerProperty(type=bpy.types.Object)
    mesh: PointerProperty(type=bpy.types.Object)
    projection: PointerProperty(type=bpy.types.NodeTree)
    material: PointerProperty(type=bpy.types.Material)
    world: PointerProperty(type=bpy.types.World)
    previous_world: PointerProperty(type=bpy.types.World)
    metadata: PointerProperty(type=bpy.types.Text)
    image: PointerProperty(type=bpy.types.Image)


class SSPS_OT_import(bpy.types.Operator, ImportHelper):
    bl_idname = "import_scene.spatial_snapshot"
    bl_label = "SpatialSnapshot をインポート"
    bl_options = {"REGISTER", "UNDO"}
    filename_ext = ""
    filter_glob: StringProperty(default="*.heic;*.heif;*.mov", options={"HIDDEN"})

    def execute(self, context):
        library, cache, tools = options()
        context.window_manager.progress_begin(0, 1)
        try:
            imported = scene.import_capture(self.filepath, context.scene, cache, tools, library)
            context.view_layer.update()
            layer_collection = context.view_layer.layer_collection.children.get(imported.name)
            if layer_collection:
                context.view_layer.active_layer_collection = layer_collection
            self.report({"INFO"}, f"{imported.name}: {imported.ssps_capture.sample_count} samples")
            return {"FINISHED"}
        except Exception as error:
            self.report({"ERROR"}, str(error))
            return {"CANCELLED"}
        finally:
            context.window_manager.progress_end()


class SSPS_FH_import(bpy.types.FileHandler):
    bl_idname = "SSPS_FH_import"
    bl_label = "SpatialSnapshot"
    bl_import_operator = SSPS_OT_import.bl_idname
    bl_file_extensions = ".heic;.heif;.mov"

    @classmethod
    def poll_drop(cls, context):
        return poll_file_object_drop(context)


class SSPS_OT_refresh(bpy.types.Operator):
    bl_idname = "spatial_snapshot.refresh"
    bl_label = "現在のフレームを更新"
    bl_options = {"REGISTER", "UNDO"}

    @classmethod
    def poll(cls, context):
        return scene.selected_capture(context.scene) is not None

    def execute(self, context):
        collection = scene.selected_capture(context.scene)
        scene.update_capture(context.scene, collection, force=True)
        error = collection.ssps_capture.last_error
        if error:
            self.report({"ERROR"}, error)
            return {"CANCELLED"}
        return {"FINISHED"}


class SSPS_OT_rebuild_cache(bpy.types.Operator):
    bl_idname = "spatial_snapshot.rebuild_cache"
    bl_label = "キャッシュを再作成"

    @classmethod
    def poll(cls, context):
        return scene.selected_capture(context.scene) is not None

    def execute(self, context):
        library, cache, tools = options()
        collection = scene.selected_capture(context.scene)
        try:
            scene.rebuild_cache(context.scene, collection, cache, tools, library)
            if collection.ssps_capture.last_error:
                raise RuntimeError(collection.ssps_capture.last_error)
            return {"FINISHED"}
        except Exception as error:
            self.report({"ERROR"}, str(error))
            return {"CANCELLED"}


class SSPS_OT_view_capture(bpy.types.Operator):
    bl_idname = "spatial_snapshot.view_capture"
    bl_label = "収録カメラと背景を表示"
    bl_description = "選択した収録のカメラ・ワールド背景・画像サイズに切り替えます"
    bl_options = {"REGISTER", "UNDO"}

    @classmethod
    def poll(cls, context):
        return scene.selected_capture(context.scene) is not None

    def execute(self, context):
        try:
            scene.view_capture(context.scene, scene.selected_capture(context.scene))
            if context.screen:
                for area in context.screen.areas:
                    if area.type == "VIEW_3D":
                        area.spaces.active.region_3d.view_rotation = context.scene.camera.matrix_world.to_quaternion()
                        area.spaces.active.region_3d.view_perspective = "CAMERA"
                        area.spaces.active.shading.type = "MATERIAL"
                        area.spaces.active.shading.use_scene_world = True
            return {"FINISHED"}
        except Exception as error:
            self.report({"ERROR"}, str(error))
            return {"CANCELLED"}


class SSPS_PT_capture(bpy.types.Panel):
    bl_label = "SpatialSnapshot"
    bl_idname = "SSPS_PT_capture"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "SpatialSnapshot"

    def draw(self, context):
        layout = self.layout
        layout.operator(SSPS_OT_import.bl_idname, icon="IMPORT")
        collection = scene.selected_capture(context.scene)
        if collection is None:
            return
        layout.prop(context.scene, "ssps_active_capture", text="収録")
        state = collection.ssps_capture
        layout.operator(SSPS_OT_view_capture.bl_idname, icon="CAMERA_DATA")
        layout.label(text=f"{state.kind} · {state.current_sample + 1} / {state.sample_count}")
        layout.label(text=f"{state.timestamp_seconds:.6f} s")
        if state.kind == "VIDEO":
            layout.prop(state, "enabled")
            layout.prop(state, "start_frame")
            layout.label(text=f"{context.scene.render.fps / context.scene.render.fps_base:g} fps")
        layout.operator(SSPS_OT_refresh.bl_idname)
        layout.prop(state, "source_path")
        layout.prop(state, "cache_directory")
        layout.operator(SSPS_OT_rebuild_cache.bl_idname)
        if state.last_error:
            box = layout.box()
            box.alert = True
            box.label(text="更新できませんでした.", icon="ERROR")
            for start in range(0, len(state.last_error), 42):
                box.label(text=state.last_error[start:start + 42])


CLASSES = (SSPSPreferences, SSPCapture, SSPS_OT_import, SSPS_FH_import, SSPS_OT_refresh,
           SSPS_OT_rebuild_cache, SSPS_OT_view_capture, SSPS_PT_capture)


def menu_import(self, context):
    self.layout.operator(SSPS_OT_import.bl_idname, text="SpatialSnapshot (.heic / .heif / .mov)")


def after_enable():
    # Blender restricts bpy.data during register(); resume saved captures afterwards.
    if hasattr(bpy.types.Scene, "ssps_capture"):
        scene.after_load(None)
    return None


def register():
    for cls in CLASSES:
        bpy.utils.register_class(cls)
    bpy.types.Collection.ssps_capture = PointerProperty(type=SSPCapture)
    bpy.types.Scene.ssps_active_capture = PointerProperty(type=bpy.types.Collection,
        poll=lambda owner, collection: collection in scene.captures(owner))
    bpy.types.Scene.ssps_capture = PointerProperty(type=SSPCapture)  # Read saved scene-owned captures for migration.
    bpy.types.TOPBAR_MT_file_import.append(menu_import)
    scene.set_library_provider(lambda: options()[0])
    scene.install_handlers()
    bpy.app.timers.register(after_enable, first_interval=0.1)


def unregister():
    if bpy.app.timers.is_registered(after_enable):
        bpy.app.timers.unregister(after_enable)
    scene.remove_handlers()
    bpy.types.TOPBAR_MT_file_import.remove(menu_import)
    del bpy.types.Scene.ssps_capture
    del bpy.types.Scene.ssps_active_capture
    del bpy.types.Collection.ssps_capture
    for cls in reversed(CLASSES):
        bpy.utils.unregister_class(cls)

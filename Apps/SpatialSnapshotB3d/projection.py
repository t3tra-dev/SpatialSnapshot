# SPDX-License-Identifier: GPL-3.0-or-later
"""One pinhole projection shared by mesh shading and the world surface."""

from dataclasses import replace
from math import hypot, pi

import bpy
from mathutils import Matrix, Quaternion, Vector

from .native import SpatialError


def scene_basis(gravity):
    up = -Vector(gravity).normalized()
    forward = Vector((0, 0, 1))
    forward -= up * forward.dot(up)
    if forward.length_squared < 1e-10:
        right = Vector((1, 0, 0))
        right -= up * right.dot(up)
        right.normalize()
        forward = up.cross(right)
    else:
        forward.normalize()
        right = forward.cross(up).normalized()
    return Matrix((right, forward, up))  # S -> Blender; gravity becomes world -Z.


def initial_quarter_turns(gravity):
    """Clockwise display rotation, inferred from SSPS, never the HEIC raster."""
    x, y, _z = gravity
    # Looking straight up/down has no reliable screen-space gravity direction.
    if hypot(x, y) < 0.1:
        return 0
    if abs(x) > abs(y):
        return 1 if x > 0 else 3
    return 2 if y < 0 else 0


def display_camera(camera, quarter_turns):
    """Rotate the viewing raster without modifying stored pixels or SSPS poses.

    Mesh/world shaders continue to project through the original capture camera.
    The Blender camera and its calibration rotate together, so the render is an
    exact quarter-turn of that raster, including off-center/non-square intrinsics.
    """
    turns = quarter_turns % 4
    if not turns:
        return camera
    width, height = camera.width, camera.height
    fx, fy, cx, cy = camera.fx, camera.fy, camera.cx, camera.cy
    for _ in range(turns):
        width, height, fx, fy, cx, cy = height, width, fy, fx, height - 1 - cy, cx
    x, y, z, w = camera.quaternion
    rotation = (Quaternion((w, x, y, z)) @ Quaternion((0, 0, 1), -turns * pi / 2)).normalized()
    return replace(camera, width=width, height=height, fx=fx, fy=fy, cx=cx, cy=cy,
                   quaternion=(rotation.x, rotation.y, rotation.z, rotation.w))


def pose(camera, basis):
    x, y, z, w = camera.quaternion
    rotation = basis @ Quaternion((w, x, y, z)).normalized().to_matrix()
    location = basis @ Vector(camera.translation)
    return rotation, location


def set_camera(scene, obj, camera, basis, *, configure_render=True, meters_per_unit=1.0):
    rotation, location = pose(camera, basis)
    obj.matrix_world = (Matrix.Translation(location) @ rotation.to_4x4() @
                        Matrix.Diagonal((1, -1, -1, 1)))
    obj.rotation_mode = "QUATERNION"
    data = obj.data
    data.type, data.sensor_fit = "PERSP", "HORIZONTAL"
    # Pick a sensor width that also keeps Blender's focal-length property in range.
    sensor = 36.0
    focal = camera.fx * sensor / camera.width
    if not 1 <= focal <= 5000:
        sensor = min(100.0, max(1.0, 50.0 * camera.width / camera.fx))
        focal = camera.fx * sensor / camera.width
    ratio = camera.fx / camera.fy
    if not 1 <= focal <= 5000 or not 1 / 200 <= ratio <= 200:
        raise SpatialError("このカメラの内部パラメータは Blender の lens / pixel aspect 範囲で表現できません.")
    data.sensor_width, data.lens = sensor, focal
    data.shift_x = (camera.width / 2 - camera.cx - 0.5) / camera.width
    data.shift_y = (camera.cy + 0.5 - camera.height / 2) * ratio / camera.width
    data.clip_start, data.clip_end = 0.001 / meters_per_unit, 10000 / meters_per_unit
    if configure_render:
        set_render_camera(scene, camera)


def set_render_camera(scene, camera):
    ratio = camera.fx / camera.fy
    scene.render.resolution_x, scene.render.resolution_y = camera.width, camera.height
    scene.render.resolution_percentage = 100
    scene.render.pixel_aspect_x, scene.render.pixel_aspect_y = (1, ratio) if ratio >= 1 else (1 / ratio, 1)


def projection_group(name):
    group = bpy.data.node_groups.new(name, "ShaderNodeTree")
    for direction, socket, name in (("INPUT", "NodeSocketVector", "Vector"),
            ("INPUT", "NodeSocketFloat", "Position"), ("OUTPUT", "NodeSocketVector", "UV"),
            ("OUTPUT", "NodeSocketFloat", "Valid")):
        group.interface.new_socket(name=name, in_out=direction, socket_type=socket)
    nodes, links = group.nodes, group.links
    start, end = nodes.new("NodeGroupInput"), nodes.new("NodeGroupOutput")
    start.location, end.location = (-800, 100), (800, 100)

    def vector(operation, name, left, right=None):
        node = nodes.new("ShaderNodeVectorMath")
        node.operation, node.name = operation, name
        links.new(left, node.inputs[0])
        if right is not None:
            links.new(right, node.inputs[3] if operation == "SCALE" else node.inputs[1])
        return node

    translation = nodes.new("ShaderNodeCombineXYZ")
    translation.name = "Origin"
    scaled = vector("SCALE", "Point origin", translation.outputs[0], start.outputs["Position"])
    local = vector("SUBTRACT", "Relative ray", start.outputs["Vector"], scaled.outputs[0])
    axes = [vector("DOT_PRODUCT", name, local.outputs[0]) for name in ("Right", "Down", "Forward")]

    def math_node(operation, name, left, right=None, value=None):
        node = nodes.new("ShaderNodeMath")
        node.operation, node.name = operation, name
        links.new(left, node.inputs[0])
        if right is not None:
            links.new(right, node.inputs[1])
        if value is not None:
            node.inputs[1].default_value = value
        return node.outputs[0]

    uv = nodes.new("ShaderNodeCombineXYZ")
    for index, (axis, scale, offset) in enumerate(((axes[0], "Scale X", "Offset X"), (axes[1], "Scale Y", "Offset Y"))):
        divided = math_node("DIVIDE", "Perspective", axis.outputs["Value"], axes[2].outputs["Value"])
        scaled = math_node("MULTIPLY", scale, divided)
        shifted = math_node("ADD", offset, scaled)
        links.new(shifted, uv.inputs[index])
    valid = math_node("GREATER_THAN", "In front", axes[2].outputs["Value"], value=1e-7)
    links.new(uv.outputs[0], end.inputs["UV"])
    links.new(valid, end.inputs["Valid"])
    # Stable readable layout, with the coordinate inputs on the left and UV on the right.
    for index, node in enumerate(n for n in nodes if n not in (start, end)):
        node.location = (-560 + (index // 4) * 240, 360 - (index % 4) * 180)
    return group


def update_projection(group, camera, basis):
    rotation, location = pose(camera, basis)
    for i in range(3):
        group.nodes["Origin"].inputs[i].default_value = location[i]
    for index, name in enumerate(("Right", "Down", "Forward")):
        group.nodes[name].inputs[1].default_value = rotation.col[index]
    for name, value in (("Scale X", camera.fx / camera.width), ("Scale Y", -camera.fy / camera.height),
            ("Offset X", (camera.cx + 0.5) / camera.width), ("Offset Y", 1 - (camera.cy + 0.5) / camera.height)):
        group.nodes[name].inputs[1].default_value = value


def image_shader(owner, group, image, *, world=False):
    owner.use_nodes = True
    nodes, links = owner.node_tree.nodes, owner.node_tree.links
    nodes.clear()
    coordinates = nodes.new("ShaderNodeTexCoord" if world else "ShaderNodeNewGeometry")
    projection = nodes.new("ShaderNodeGroup")
    projection.node_tree = group
    projection.inputs["Position"].default_value = 0 if world else 1
    links.new(coordinates.outputs["Generated" if world else "Position"], projection.inputs["Vector"])
    texture = nodes.new("ShaderNodeTexImage")
    texture.name, texture.label, texture.image = "SSPS Image", "Capture image", image
    texture.extension, texture.interpolation = "CLIP", "Linear"
    links.new(projection.outputs["UV"], texture.inputs["Vector"])
    valid = nodes.new("ShaderNodeMath")
    valid.operation = "MULTIPLY"
    links.new(projection.outputs["Valid"], valid.inputs[0])
    links.new(texture.outputs["Alpha"], valid.inputs[1])
    shader = nodes.new("ShaderNodeBackground" if world else "ShaderNodeEmission")
    links.new(texture.outputs["Color"], shader.inputs["Color"])
    if world:
        links.new(valid.outputs[0], shader.inputs["Strength"])
        output = nodes.new("ShaderNodeOutputWorld")
        links.new(shader.outputs[0], output.inputs["Surface"])
    else:
        transparent, mix = nodes.new("ShaderNodeBsdfTransparent"), nodes.new("ShaderNodeMixShader")
        links.new(valid.outputs[0], mix.inputs[0])
        links.new(transparent.outputs[0], mix.inputs[1])
        links.new(shader.outputs[0], mix.inputs[2])
        output = nodes.new("ShaderNodeOutputMaterial")
        links.new(mix.outputs[0], output.inputs["Surface"])
    coordinates.location, projection.location, texture.location = (-600, 0), (-380, 0), (-150, 0)
    shader.location, output.location, valid.location = (100, 40), (560, 40), (80, -220)

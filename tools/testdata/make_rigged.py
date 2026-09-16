# Test rig for the skinning code: a two-bone cylinder with two animation clips.
#
#   /Applications/Blender.app/Contents/MacOS/Blender --background --python tools/testdata/make_rigged.py
#   -> assets/test/rigged.glb  (committed; a few tens of KB)
#
# Shape: a 2 m cylinder standing on the origin, 12 sides, cut into 8 rings so the bend is visible.
# Rig: bones "Lower" (0..1 m) and "Upper" (1..2 m, child), automatic weights.
# Clips: "diam" (rest, 1 s) and "jalan" (the upper half bends 60 deg forward and back, 1 s).
# The engine's own test (tests/test_skin.cpp) checks the parsed numbers; this file only has to be a
# real glTF exporter's output, not a hand-made one.
import math
import os
import sys

import bpy
from mathutils import Euler, Vector

FPS = 24
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, "assets", "test", "rigged.glb")


def reset():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.context.scene.render.fps = FPS


def build_mesh():
    bpy.ops.mesh.primitive_cylinder_add(vertices=12, radius=0.2, depth=2.0, location=(0, 0, 1))
    obj = bpy.context.active_object
    obj.name = "Body"
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.mesh.subdivide(number_cuts=4)
    bpy.ops.object.mode_set(mode="OBJECT")
    mat = bpy.data.materials.new("Skin")
    mat.use_nodes = True
    bsdf = mat.node_tree.nodes["Principled BSDF"]
    bsdf.inputs["Base Color"].default_value = (0.75, 0.35, 0.25, 1.0)
    bsdf.inputs["Roughness"].default_value = 0.7
    bsdf.inputs["Metallic"].default_value = 0.0
    obj.data.materials.append(mat)
    return obj


def build_armature():
    bpy.ops.object.armature_add(location=(0, 0, 0))
    arm = bpy.context.active_object
    arm.name = "Rangka"
    arm.data.name = "Rangka"
    bpy.ops.object.mode_set(mode="EDIT")
    bones = arm.data.edit_bones
    lower = bones[0]
    lower.name = "Lower"
    lower.head = Vector((0, 0, 0))
    lower.tail = Vector((0, 0, 1))
    upper = bones.new("Upper")
    upper.head = Vector((0, 0, 1))
    upper.tail = Vector((0, 0, 2))
    upper.parent = lower
    upper.use_connect = True
    bpy.ops.object.mode_set(mode="OBJECT")
    return arm


def key_pose(arm, action_name, bends):
    """bends: [(frame, degrees)] applied to the Upper bone around X."""
    arm.animation_data_create()
    action = bpy.data.actions.new(action_name)
    action.use_fake_user = True
    arm.animation_data.action = action
    for bone in arm.pose.bones:
        bone.rotation_mode = "XYZ"
        bone.rotation_euler = Euler((0, 0, 0))
    for frame, deg in bends:
        bpy.context.scene.frame_set(frame)
        arm.pose.bones["Upper"].rotation_euler = Euler((math.radians(deg), 0, 0))
        arm.pose.bones["Upper"].keyframe_insert("rotation_euler", frame=frame)
        arm.pose.bones["Lower"].rotation_euler = Euler((0, 0, 0))
        arm.pose.bones["Lower"].keyframe_insert("rotation_euler", frame=frame)
    return action


def main():
    reset()
    body = build_mesh()
    arm = build_armature()
    bpy.ops.object.select_all(action="DESELECT")
    body.select_set(True)
    arm.select_set(True)
    bpy.context.view_layer.objects.active = arm
    bpy.ops.object.parent_set(type="ARMATURE_AUTO")

    bpy.context.view_layer.objects.active = arm
    key_pose(arm, "diam", [(1, 0), (FPS + 1, 0)])
    key_pose(arm, "jalan", [(1, 0), (FPS // 2 + 1, 60), (FPS + 1, 0)])

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    bpy.ops.export_scene.gltf(
        filepath=OUT,
        export_format="GLB",
        export_animation_mode="ACTIONS",
        export_skins=True,
        export_apply=False,
        export_yup=True,
    )
    print("wrote", OUT, os.path.getsize(OUT), "bytes")


main()
sys.exit(0)

import json
import os
import sys
import bpy
import bpy.ops
from idprop.types import IDPropertyGroup

argv = sys.argv
argv = argv[argv.index("--") + 1:]  # get all args after "--"
outfile = argv[0]

# gltfpack breaks armatures that are far from the world origin.
# Move them to (0,0,0) before export, saving the original location
# in a custom property so the engine can restore it via glTF extras.
export_collection = bpy.data.collections.get("export")
if export_collection:
    for obj in export_collection.all_objects:
        if obj.type == 'ARMATURE':
            loc = obj.location.copy()
            if loc.x != 0 or loc.y != 0 or loc.z != 0:
                # Store in glTF Y-up convention (Blender is Z-up):
                #   gltf_x =  blender_x
                #   gltf_y =  blender_z
                #   gltf_z = -blender_y
                obj["armatureLocation"] = [loc.x, loc.z, -loc.y]
                obj.location = (0, 0, 0)
                print(f"Zeroed armature '{obj.name}', saved location "
                      f"({loc.x:.3f}, {loc.y:.3f}, {loc.z:.3f}) to extras")

    # Export custom properties from Blender objects as glTF extras.
    # Currently recognised keys: "player" — marks the armature entity as
    # the player-controlled character so game code can attach a Player
    # component at load time.
    for obj in export_collection.all_objects:
        if "player" in obj and obj["player"]:
            # Ensure it's stored as a simple bool-like int for JSON.
            obj["player"] = 1
            print(f"Marked '{obj.name}' as player")

    # Save light colors into glTF extras so they survive gltfpack
    # (gltfpack strips the color field when it equals white).
    for obj in export_collection.all_objects:
        if obj.type == 'LIGHT' and obj.data:
            c = obj.data.color
            obj["lightColor"] = [c.r, c.g, c.b]
            print(f"Saved light color for '{obj.name}': "
                  f"({c.r:.3f}, {c.g:.3f}, {c.b:.3f})")

    # Save rigid body properties into glTF extras so the engine can
    # create Jolt physics bodies during scene parsing.
    for obj in export_collection.all_objects:
        if obj.rigid_body:
            rb = obj.rigid_body
            obj["rigidBodyShape"]       = rb.collision_shape   # BOX, SPHERE, CAPSULE, …
            obj["rigidBodyType"]        = rb.type              # ACTIVE or PASSIVE
            obj["rigidBodyMass"]        = rb.mass
            obj["rigidBodyFriction"]    = rb.friction
            obj["rigidBodyRestitution"] = rb.restitution
            print(f"Saved rigid body for '{obj.name}': "
                  f"shape={rb.collision_shape} type={rb.type} "
                  f"mass={rb.mass:.2f} friction={rb.friction:.2f} "
                  f"restitution={rb.restitution:.2f}")

bpy.ops.export_scene.gltf(filepath=outfile,
                            export_format="GLB",
                            export_yup=True,
                            export_texcoords=True,
                            export_normals=True,
                            export_tangents=True,
                            export_materials='EXPORT',
                            export_unused_textures=False,
                            export_unused_images=False,
                            export_all_vertex_colors=True,
                            export_attributes=False,
                            use_visible=True,
                            use_renderable=True,
                            use_active_scene=True,
                            export_apply=True,
                            export_extras=True,
                            export_animations=True,
                            # export_force_sampling=True,
                            # export_sampling_interpolation_fallback='LINEAR',
                            # export_animation_mode="ACTIONS",
                            export_optimize_animation_size=True,
                            # export_optimize_animation_keep_anim_armature=True,
                            # export_optimize_animation_keep_anim_object=True,
                            # export_anim_slide_to_zero=True,
                            export_lights=True,
                            export_cameras=True,
                            collection="export")

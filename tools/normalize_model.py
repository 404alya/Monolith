# normalize_model.py -- Blender script: normalise a downloaded model into a clean GLB
#
# Run headless and locked down (never open a downloaded .blend; this script only imports
# glTF / GLB / OBJ / FBX through Blender's own importers):
#
#   BLENDER=${BLENDER:-~/.local/share/Steam/steamapps/common/Blender/blender}
#   "$BLENDER" -b --factory-startup --disable-autoexec --python-exit-code 1 \
#       --python tools/normalize_model.py -- <in.glb|.gltf|.obj|.fbx> <out.glb> [options]
#
# Run it from the repo root (never from a download directory). Options:
#   --origin bottom|center|keep   move the origin: bottom centre of the bounds (default keep)
#   --height H                    uniform scale so the model is H metres tall
#   --scale S                     uniform scale factor (e.g. 0.01 for an FBX in centimetres)
#   --decimate N                  collapse-decimate meshes to about N triangles in total
#   --max-texture PX              downscale every image larger than PX on its longer side
#   --strip-unlit                 rebuild KHR_materials_unlit (emission/background) materials
#                                 as lit Principled BSDF with the same base colour / texture
#   --metallic F                  set the metallic factor of every material without a
#                                 metalness map (e.g. 0 for Kenney kits exported with metallic 1)
#   --report FILE                 also write the change list as JSON (for tools/record_asset.py)
#
# glTF is Y-up; Blender is Z-up internally and its glTF importer / exporter convert both ways,
# so the output is Y-up metres. Transforms are applied (the output has identity node
# transforms), animations and cameras / lights are dropped. Prints every change it made.

import json
import os
import sys

import bpy
from mathutils import Matrix, Vector


def Args():
    import argparse
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    p = argparse.ArgumentParser(prog="normalize_model.py")
    p.add_argument("input")
    p.add_argument("output")
    p.add_argument("--origin", choices=["bottom", "center", "keep"], default="keep")
    g = p.add_mutually_exclusive_group()
    g.add_argument("--height", type=float)
    g.add_argument("--scale", type=float)
    p.add_argument("--decimate", type=int)
    p.add_argument("--max-texture", type=int)
    p.add_argument("--strip-unlit", action="store_true")
    p.add_argument("--metallic", type=float)
    p.add_argument("--report")
    return p.parse_args(argv)


def Fail(msg):
    print("normalize_model: " + msg, file=sys.stderr)
    sys.exit(1)


def MeshObjects():
    return [o for o in bpy.context.scene.objects if o.type == "MESH"]


def Triangles():
    n = 0
    dg = bpy.context.evaluated_depsgraph_get()
    for o in MeshObjects():
        m = o.evaluated_get(dg).to_mesh()
        n += sum(len(p.vertices) - 2 for p in m.polygons)
        o.evaluated_get(dg).to_mesh_clear()
    return n


def Bounds():
    lo = Vector((1e30, 1e30, 1e30))
    hi = -lo
    for o in MeshObjects():
        for v in o.data.vertices:
            w = o.matrix_world @ v.co
            lo = Vector(map(min, lo, w))
            hi = Vector(map(max, hi, w))
    return lo, hi


def Fmt(v):
    return "%.4f %.4f %.4f" % tuple(v)


def YUp(v):
    """Blender (Z-up) vector -> glTF (Y-up): (x, z, -y)."""
    return Vector((v.x, v.z, -v.y))


def YUpBounds(lo, hi):
    a, b = YUp(lo), YUp(hi)
    return Vector(map(min, a, b)), Vector(map(max, a, b))


def main():
    a = Args()
    src = os.path.abspath(a.input)
    dst = os.path.abspath(a.output)
    if not os.path.isfile(src):
        Fail("no such file: " + src)
    if os.path.exists(dst):
        Fail("refusing to overwrite " + dst)
    if not dst.lower().endswith(".glb"):
        Fail("the output must be a .glb")
    ext = os.path.splitext(src)[1].lower()
    changes = []

    bpy.ops.wm.read_factory_settings(use_empty=True)
    if ext in (".glb", ".gltf"):
        bpy.ops.import_scene.gltf(filepath=src)
    elif ext == ".obj":
        bpy.ops.wm.obj_import(filepath=src, up_axis="Y", forward_axis="NEGATIVE_Z")
    elif ext == ".fbx":
        bpy.ops.import_scene.fbx(filepath=src)
    else:
        Fail("unsupported input (glTF/GLB/OBJ/FBX only; never open a .blend): " + ext)

    # Drop everything that isn't a mesh (cameras, lights, empties after parenting is applied)
    for o in list(bpy.context.scene.objects):
        if o.type not in ("MESH", "EMPTY"):
            what = "removed %s object %r" % (o.type.lower(), o.name)
            bpy.data.objects.remove(o, do_unlink=True)
            changes.append(what)
    for o in bpy.context.scene.objects:
        o.animation_data_clear()
    if not MeshObjects():
        Fail("no meshes in " + src)

    tris0 = Triangles()
    lo, hi = Bounds()
    ylo, yhi = YUpBounds(lo, hi)
    print("input: %s, %d triangles, bounds (Y-up) min %s max %s" % (os.path.basename(src), tris0,
          Fmt(ylo), Fmt(yhi)))

    # Apply all transforms: un-parent keeping the world transform, then bake it into the meshes
    bpy.ops.object.select_all(action="DESELECT")
    for o in MeshObjects():
        o.select_set(True)
    bpy.context.view_layer.objects.active = MeshObjects()[0]
    bpy.ops.object.parent_clear(type="CLEAR_KEEP_TRANSFORM")
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    for o in list(bpy.context.scene.objects):
        if o.type == "EMPTY":
            bpy.data.objects.remove(o, do_unlink=True)
    changes.append("applied node transforms (identity transforms in the output)")

    # Decimate
    if a.decimate:
        tris = Triangles()
        if tris > a.decimate:
            ratio = a.decimate / tris
            for o in MeshObjects():
                m = o.modifiers.new("decimate", "DECIMATE")
                m.decimate_type = "COLLAPSE"
                m.ratio = ratio
                bpy.context.view_layer.objects.active = o
                bpy.ops.object.modifier_apply(modifier=m.name)
            changes.append("decimated (collapse, ratio %.4f): %d -> %d triangles" % (ratio, tris, Triangles()))

    # Scale
    lo, hi = Bounds()
    s = 1.0
    if a.height:
        h = hi.z - lo.z
        if h <= 0:
            Fail("model has zero height")
        s = a.height / h
    elif a.scale:
        s = a.scale
    if s != 1.0:
        for o in MeshObjects():
            o.data.transform(Matrix.Scale(s, 4))
            o.data.update()
        changes.append("scaled uniformly by %.6g%s" % (s, " (to %.3f m tall)" % a.height if a.height else ""))

    # Origin
    lo, hi = Bounds()
    if a.origin != "keep":
        c = (lo + hi) / 2
        off = Vector((c.x, c.y, lo.z if a.origin == "bottom" else c.z))
        M = Matrix.Translation(-off)
        for o in MeshObjects():
            o.data.transform(M)
            o.data.update()
        changes.append("moved the origin to the %s of the bounds (shift %s, Y-up)" % (
            "bottom centre" if a.origin == "bottom" else "centre", Fmt(YUp(-off))))

    # Materials
    for mat in bpy.data.materials:
        if mat.node_tree is None:
            continue
        nt = mat.node_tree
        bsdf = next((n for n in nt.nodes if n.type == "BSDF_PRINCIPLED"), None)
        if a.strip_unlit and bsdf is None:
            # The glTF importer builds unlit materials as emission / background mixes; keep the
            # colour (or its image) and rebuild a lit Principled BSDF
            out = next((n for n in nt.nodes if n.type == "OUTPUT_MATERIAL"), None)
            col_src, col = None, (0.8, 0.8, 0.8, 1.0)
            for n in nt.nodes:
                if n.type == "TEX_IMAGE":
                    col_src = n.outputs["Color"]
                elif n.type in ("EMISSION", "BACKGROUND") and not n.inputs["Color"].is_linked:
                    col = tuple(n.inputs["Color"].default_value)
                elif n.type == "RGB":
                    col = tuple(n.outputs[0].default_value)
            bsdf = nt.nodes.new("ShaderNodeBsdfPrincipled")
            if col_src is not None:
                nt.links.new(col_src, bsdf.inputs["Base Color"])
            else:
                bsdf.inputs["Base Color"].default_value = col
            bsdf.inputs["Roughness"].default_value = 0.9
            bsdf.inputs["Metallic"].default_value = 0.0
            if out is None:
                out = nt.nodes.new("ShaderNodeOutputMaterial")
            nt.links.new(bsdf.outputs["BSDF"], out.inputs["Surface"])
            changes.append("material %r: unlit -> lit Principled BSDF" % mat.name)
        if a.metallic is not None and bsdf is not None and not bsdf.inputs["Metallic"].is_linked:
            old = bsdf.inputs["Metallic"].default_value
            if abs(old - a.metallic) > 1e-6:
                bsdf.inputs["Metallic"].default_value = a.metallic
                changes.append("material %r: metallic %.3g -> %.3g" % (mat.name, old, a.metallic))

    # Textures
    if a.max_texture:
        for img in bpy.data.images:
            w, h = img.size
            if max(w, h) > a.max_texture:
                f = a.max_texture / max(w, h)
                img.scale(max(1, round(w * f)), max(1, round(h * f)))
                changes.append("image %r: %dx%d -> %dx%d" % (img.name, w, h, img.size[0], img.size[1]))

    tris1 = Triangles()
    lo, hi = Bounds()
    bpy.ops.export_scene.gltf(filepath=dst, export_format="GLB", export_yup=True,
                              export_apply=True, export_animations=False, export_cameras=False,
                              export_lights=False, export_extras=False)
    ylo, yhi = YUpBounds(lo, hi)
    size = yhi - ylo
    print("output: %s, %d triangles, size %s m (x y z, Y-up)" % (dst, tris1, Fmt(size)))
    print("changes:")
    for c in changes:
        print("  - " + c)
    if a.report:
        with open(a.report, "x") as f:
            json.dump({"input": src, "output": dst, "trianglesIn": tris0, "trianglesOut": tris1,
                       "sizeM": list(size), "changes": changes,
                       "tool": "tools/normalize_model.py (Blender %s)" % bpy.app.version_string},
                      f, indent=2)
            f.write("\n")


try:
    main()
except SystemExit:
    raise
except Exception as e:                  # --python-exit-code 1 turns this into a failing exit
    print("normalize_model: %s: %s" % (type(e).__name__, e), file=sys.stderr)
    raise

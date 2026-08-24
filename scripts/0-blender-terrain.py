#!/usr/bin/env python3
"""
Convert .blend terrain files to compressed .glb assets.

Pipeline per file:  blender → terrain-chunker → gltfpack → zstd

Two modes, detected automatically:
  • Standalone (python3):  orchestrates the full pipeline.
  • Inside Blender (blender --python):  exports the .glb from the open .blend.

Standalone mode expects environment variables from build.sh:
    SCRIPTS_TMP   – cache directory for modification timestamps
"""

import json
import os
import sys

# ─── Detect which mode we're running in ────────────────────────────────────
try:
    import bpy        # noqa: F401 – available only inside Blender
    import bpy.ops
    _INSIDE_BLENDER = True
except ImportError:
    _INSIDE_BLENDER = False


# ═══════════════════════════════════════════════════════════════════════════
#  BLENDER EXPORT MODE
# ═══════════════════════════════════════════════════════════════════════════
def blenderExport():
    argv = sys.argv
    argv = argv[argv.index("--") + 1:]  # get all args after "--"
    outfile = argv[0]
    splatInfoOutfile = argv[1] if len(argv) > 1 else None

    blenderExportTextured(outfile, splatInfoOutfile)


def saveRigidBodyExtras(exportCollection):
    """Save rigid body properties into glTF extras (same as 1-blender-scene.py)."""
    for obj in exportCollection.all_objects:
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


def collectSplatDataFromObject(obj):
    colorInputs = ["red", "green", "blue", "alpha"]
    outputData = {}

    if not obj.data or not hasattr(obj.data, "materials"):
        return outputData

    for mat in obj.data.materials:
        if not mat or not mat.use_nodes:
            continue

        for node in mat.node_tree.nodes:
            if node.type == 'GROUP' and "splat" in node.label.lower():
                splatColorInput = node.inputs["SplatColor"]
                label = splatColorInput.links[0].from_node.image.name

                outputData[label] = {}
                for inputName in colorInputs:
                    inp = next(
                        (s for s in node.inputs if s.name.lower() == inputName),
                        None
                    )

                    imageName = None
                    if inp and inp.links:
                        frm = inp.links[0].from_node
                        if frm.type == 'TEX_IMAGE' and frm.image:
                            imageName = frm.image.name

                    outputData[label][inputName] = imageName

    return outputData


def blenderExportTextured(outfile, splatInfoOutfile=None):
    exportCollection = bpy.data.collections.get("export")
    if not exportCollection:
        print("Export collection not found.")
        sys.exit(1)

    terrainObjects = [
        obj for obj in exportCollection.objects
        if "terrain" in obj.name.lower()
    ]

    if not terrainObjects:
        print("No terrain objects found.")
        sys.exit(1)

    mergedSplatInfo = {}

    for terrain in terrainObjects:
        splatData = collectSplatDataFromObject(terrain)
        if splatData:
            terrain["splatInfo"] = splatData
            print(f"[DEBUG] {terrain.name} splatInfo:")
            print(json.dumps(splatData, indent=2, default=str))
            mergedSplatInfo.update(splatData)

    if splatInfoOutfile:
        with open(splatInfoOutfile, "w", encoding="utf-8") as f:
            json.dump(mergedSplatInfo, f, indent=2)

        vegetationGroups = [name for name in mergedSplatInfo.keys()
                            if isVegetationGroupName(name)]
        vegetationGroupsPath = Path(splatInfoOutfile).with_name(
            Path(splatInfoOutfile).name.replace("terrain-splatinfo", "terrain-vegetation-groups")
        )
        with open(vegetationGroupsPath, "w", encoding="utf-8") as f:
            json.dump(vegetationGroups, f, indent=2)

    saveRigidBodyExtras(exportCollection)

    bpy.ops.export_scene.gltf(filepath=outfile,
                              export_format="GLB",
                              export_yup=True,
                              export_extras=True,
                              export_normals=True,
                              export_texcoords=True,
                              export_tangents=True,
                              export_materials='EXPORT',
                              export_image_format="NONE",
                              export_unused_images=False,
                              use_active_scene=True,
                              export_apply=False,
                              collection="export")


# ═══════════════════════════════════════════════════════════════════════════
#  STANDALONE PIPELINE MODE
# ═══════════════════════════════════════════════════════════════════════════
import subprocess
from pathlib import Path

GLTFPACK          = Path("/home/enes/Projects/c/cpp-thirdparty/meshoptimizer/git/build-linux/gltfpack")
JOLT_SHAPE_BUILDER = Path(__file__).resolve().parent.parent / "tools" / "jolt-shape-builder" / "jolt-shape-builder"
VEGETATION_BUILDER = Path(__file__).resolve().parent.parent / "tools" / "vegetation-builder" / "vegetation-builder"

BLEND_FILES = [
    Path("/home/enes/Projects/assets/Scenes/Terrain/oghuzlands/oghuzlands.blend"),
    # Path("/home/enes/Projects/assets/Scenes/Terrain/8k.blend"),
    # Path("/home/enes/Projects/assets/Scenes/Terrain/8k-new.blend"),
]

OUTPUT_DIR = Path("data/pak_1/models/terrain")

THIS_SCRIPT = Path(__file__).resolve()


def fileSizeHuman(path: Path) -> str:
    size = path.stat().st_size
    for unit in ("B", "K", "M", "G"):
        if size < 1024:
            return f"{size:.0f}{unit}" if unit == "B" else f"{size:.1f}{unit}"
        size /= 1024
    return f"{size:.1f}T"


def run(*args, **kwargs):
    result = subprocess.run(args, **kwargs)
    if result.returncode != 0:
        print(f"Command failed: {' '.join(str(a) for a in args)}", file=sys.stderr)
        sys.exit(1)
    return result


def cachedSplatInfoPath(blendFile: Path, scriptsTmp: Path) -> Path:
    return scriptsTmp / f"{blendFile.stem}.terrain-splatinfo.json"


def cachedVegetationGroupsPath(blendFile: Path, scriptsTmp: Path) -> Path:
    return scriptsTmp / f"{blendFile.stem}.terrain-vegetation-groups.json"


def loadSplatInfo(path: Path) -> dict:
    if not path.exists():
        return {}

    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return {}

    return data if isinstance(data, dict) else {}


def isVegetationGroupName(name: str) -> bool:
    return name.lower().startswith("grass")


def loadCachedSplatGroupNames(blendFile: Path, scriptsTmp: Path) -> list[str]:
    data = loadSplatInfo(cachedSplatInfoPath(blendFile, scriptsTmp))
    return [k for k, v in data.items() if isinstance(k, str) and isinstance(v, dict)]


def loadCachedVegetationGroupNames(blendFile: Path, scriptsTmp: Path) -> list[str]:
    path = cachedVegetationGroupsPath(blendFile, scriptsTmp)
    if not path.exists():
        return []

    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return []

    if not isinstance(data, list):
        return []

    return [v for v in data if isinstance(v, str)]


def newestMtime(blendFile: Path, scriptsTmp: Path) -> int:
    """Return newest mtime_ns across the .blend file and cached splat-info dirs."""
    mtime = blendFile.stat().st_mtime_ns
    blendDir = blendFile.parent

    groupNames = set(loadCachedSplatGroupNames(blendFile, scriptsTmp))
    groupNames.update(loadCachedVegetationGroupNames(blendFile, scriptsTmp))

    for groupName in groupNames:
        groupDir = blendDir / groupName
        if not groupDir.is_dir():
            continue
        for png in groupDir.rglob("*.png"):
            mtime = max(mtime, png.stat().st_mtime_ns)

    return mtime


def needsConvert(blendFile: Path, scriptsTmp: Path) -> bool:
    stampFile = scriptsTmp / blendFile.name
    mtime = newestMtime(blendFile, scriptsTmp)

    if stampFile.exists():
        saved = stampFile.read_text().strip()
        if saved == str(mtime):
            return False

    stampFile.write_text(str(mtime))
    return True


def convertBlendFile(blendFile: Path, scriptsTmp: Path):
    if not blendFile.exists():
        print(f"WARNING: {blendFile} does not exist, skipping.")
        return

    if not needsConvert(blendFile, scriptsTmp):
        return

    stem = blendFile.stem
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

    glb                  = OUTPUT_DIR / f"{stem}.glb"
    packedGlb            = OUTPUT_DIR / f"{stem}.packed.glb"
    dat                  = OUTPUT_DIR / f"{stem}.dat"
    splatInfoJson        = cachedSplatInfoPath(blendFile, scriptsTmp)
    vegetationGroupsJson = cachedVegetationGroupsPath(blendFile, scriptsTmp)

    # ── 1. Blender export ──────────────────────────────────────────────────
    print("#############################################")
    print(f"blend to glb {stem}... ", end="", flush=True)

    blenderCommon = ["blender", str(blendFile), "--background",
                     "--python", str(THIS_SCRIPT), "--"]

    run(*blenderCommon, str(glb), str(splatInfoJson))

    print(fileSizeHuman(glb))

    # ── 2. terrain-chunker ─────────────────────────────────────────────────
    chunkedGlb = OUTPUT_DIR / f"{stem}.chunked.glb"
    TERRAIN_CHUNKER = Path(__file__).resolve().parent.parent / "tools" / "terrain-chunker" / "terrain-chunker"
    CHUNK_GRID_X = 4
    CHUNK_GRID_Y = 4

    print("terrain-chunker...")
    run(str(TERRAIN_CHUNKER), str(glb), str(chunkedGlb), str(CHUNK_GRID_X), str(CHUNK_GRID_Y))

    # Remove pre-chunk glb
    glb.unlink()

    print(f"chunked size: {fileSizeHuman(chunkedGlb)}")

    # ── 3. gltfpack ───────────────────────────────────────────────────────
    print("gltfpack...")

    env = {**os.environ, "KTX_GEN_MIPMAP": "1"}

    run(str(GLTFPACK), "-vpf", "-vn", "16", "-vt", "16", "-cc", "-kn", "-kv", "-ke", "-tc", "-tj", "32",
        "-i", str(chunkedGlb), "-o", str(packedGlb),
        env=env)

    # Remove intermediate chunked glb
    chunkedGlb.unlink()

    print(f"size: {fileSizeHuman(packedGlb)}")

    # ── 4. Build pre-baked Jolt physics shapes (from packed glb) ────────
    joltShapes    = OUTPUT_DIR / f"{stem}.jolt"
    joltShapesZst = OUTPUT_DIR / f"{stem}.jolt.zst"
    joltShapesDat = OUTPUT_DIR / f"{stem}.jolt.dat"

    print("jolt shapes...", end=" ", flush=True)
    run(str(JOLT_SHAPE_BUILDER), str(packedGlb), str(joltShapes))

    # ── 5. Navmesh is built by build-navmesh.sh ─────────────────────────────

    # ── 6. Convert splat textures to ktx2 ─────────────────────────────────
    convertSplatDirs(blendFile, splatInfoJson)

    # ── 7. Build offline vegetation summary/bake scaffold ─────────────────
    # DISABLED: vegetation-builder doesn't support chunked GLB yet
    print("vegetation... SKIPPED (disabled)")

    # ── 8. zstd compression ───────────────────────────────────────────────
    print("zstd...")

    run("zstd", "-q", "-19", "--rm", "-f", "-o", str(dat), str(packedGlb))

    if joltShapes.exists():
        run("zstd", "-q", "-19", "--rm", "-f", str(joltShapes))
        joltShapesZst.rename(joltShapesDat)
        print(f"jolt shapes: {fileSizeHuman(joltShapesDat)}")

    print(f"size: {fileSizeHuman(dat)}")


def isNoopSplatPng(pngPath: Path) -> bool:
    """Return True if the PNG is a noop splatmap tile (solid 0,0,0,255).

    All splat tiles are 1024x1024 RGBA.  A solid-color tile compresses to
    ~20 KB, while any painted tile is significantly larger.  Checking file
    size is orders of magnitude faster than decoding the image.
    """
    NOOP_MAX_SIZE = 20_800  # bytes – noop tiles are ~20,726 bytes
    return pngPath.stat().st_size <= NOOP_MAX_SIZE


def resolveSplatGroupDirs(blendFile: Path, splatInfo: dict) -> list[tuple[str, Path, bool]]:
    blendDir = blendFile.parent
    result = []

    for groupName, groupValue in splatInfo.items():
        if not isinstance(groupName, str) or not isinstance(groupValue, dict):
            continue
        groupDir = blendDir / groupName
        if not groupDir.is_dir():
            print(f"WARNING: splatInfo group '{groupName}' has no directory at {groupDir}")
            continue
        result.append((groupName, groupDir, isVegetationGroupName(groupName)))

    return result


def convertSplatDirs(blendFile: Path, splatInfoJson: Path):
    """Resolve terrain texture directories from exported splatInfo keys.

    For each top-level key in splatInfo (for example grass1, roads1), look for a
    sibling directory with the same name next to the .blend file and convert its
    active UDIM PNGs to KTX2. Groups named grass* are also flagged as vegetation
    map groups for future preprocessing. Skips noop tiles (solid 0,0,0,255) and
    removes stale .ktx2 files for them.
    """
    stem = blendFile.stem
    convertScript = THIS_SCRIPT.parent / "convert-model-textures.py"

    if not splatInfoJson.exists():
        return

    try:
        splatInfo = json.loads(splatInfoJson.read_text(encoding="utf-8"))
    except Exception as e:
        print(f"WARNING: failed to read splat info {splatInfoJson}: {e}")
        return

    if not isinstance(splatInfo, dict) or not splatInfo:
        return

    resolvedGroups = resolveSplatGroupDirs(blendFile, splatInfo)
    if not resolvedGroups:
        return

    vegetationGroups = [name for name, _, isVeg in resolvedGroups if isVeg]
    if vegetationGroups:
        print(f"vegetation maps: {', '.join(vegetationGroups)}")

    import tempfile, shutil

    for groupName, groupDir, isVegetation in resolvedGroups:
        outDir = OUTPUT_DIR / stem / groupName
        pngs = sorted(groupDir.glob("*.png"))
        if not pngs:
            continue

        # Classify tiles as active or noop
        activePngs = []
        noopStems = []
        for png in pngs:
            if isNoopSplatPng(png):
                noopStems.append(png.stem)
            else:
                activePngs.append(png)

        groupLabel = "vegetation-map" if isVegetation else "terrain-splat"
        print(f"splat textures: {groupName} -> {outDir} [{groupLabel}]"
              f"  ({len(activePngs)} active, {len(noopStems)} skipped)")

        # Remove stale .ktx2 files for noop tiles
        if outDir.is_dir():
            for noopStem in noopStems:
                stale = outDir / f"{noopStem}.ktx2"
                if stale.exists():
                    stale.unlink()
                    print(f"  removed stale: {stale.name}")

        if not activePngs:
            continue

        # Copy only active PNGs to a temp dir for conversion
        with tempfile.TemporaryDirectory() as tmp:
            tmpPath = Path(tmp)
            for png in activePngs:
                shutil.copy2(png, tmpPath / png.name)

            run(sys.executable, str(convertScript),
                "--input-dir", str(tmpPath),
                "--output-dir", str(outDir))


def pipelineMain():
    if not Path("./data").is_dir():
        print("where is data dir?")
        sys.exit(1)

    scriptsTmp = Path(os.environ["SCRIPTS_TMP"])
    scriptsTmp.mkdir(parents=True, exist_ok=True)

    for blendFile in BLEND_FILES:
        convertBlendFile(blendFile, scriptsTmp)


# ═══════════════════════════════════════════════════════════════════════════
#  ENTRY POINT
# ═══════════════════════════════════════════════════════════════════════════
if _INSIDE_BLENDER:
    blenderExport()
else:
    pipelineMain()



# [enes@enes ~]$ /home/enes/Projects/ales/game-c-lib/build/meshoptimizer/linux/gltfpack -h
# gltfpack 0.21
# Usage: gltfpack [options] -i input -o output

# Basics:
# 	-i file: input file to process, .obj/.gltf/.glb
# 	-o file: output file path, .gltf/.glb
# 	-c: produce compressed gltf/glb files (-cc for higher compression ratio)

# Textures:
# 	-tc: convert all textures to KTX2 with BasisU supercompression
# 	-tu: use UASTC when encoding textures (much higher quality and much larger size)
# 	-tq N: set texture encoding quality (default: 8; N should be between 1 and 10
# 	-ts R: scale texture dimensions by the ratio R (default: 1; R should be between 0 and 1)
# 	-tl N: limit texture dimensions to N pixels (default: 0 = no limit)
# 	-tp: resize textures to nearest power of 2 to conform to WebGL1 restrictions
# 	-tfy: flip textures along Y axis during BasisU supercompression
# 	-tj N: use N threads when compressing textures
# 	-tr: keep referring to original texture paths instead of copying/embedding images
# 	Texture classes:
# 	-tc C: use ETC1S when encoding textures of class C
# 	-tu C: use UASTC when encoding textures of class C
# 	-tq C N: set texture encoding quality for class C
# 	... where C is a comma-separated list (no spaces) with valid values color,normal,attrib

# Simplification:
# 	-si R: simplify meshes targeting triangle/point count ratio R (default: 1; R should be between 0 and 1)
# 	-sa: aggressively simplify to the target ratio disregarding quality
# 	-sv: take vertex attributes into account when simplifying meshes
# 	-slb: lock border vertices during simplification to avoid gaps on connected meshes

# Vertex precision:
# 	-vp N: use N-bit quantization for positions (default: 14; N should be between 1 and 16)
# 	-vt N: use N-bit quantization for texture coordinates (default: 12; N should be between 1 and 16)
# 	-vn N: use N-bit quantization for normals (default: 8; N should be between 1 and 16) and tangents (up to 8-bit)
# 	-vc N: use N-bit quantization for colors (default: 8; N should be between 1 and 16)

# Vertex positions:
# 	-vpi: use integer attributes for positions (default)
# 	-vpn: use normalized attributes for positions
# 	-vpf: use floating point attributes for positions

# Vertex attributes:
# 	-vtf: use floating point attributes for texture coordinates
# 	-vnf: use floating point attributes for normals
# 	-kv: keep source vertex attributes even if they aren't used

# Animations:
# 	-at N: use N-bit quantization for translations (default: 16; N should be between 1 and 24)
# 	-ar N: use N-bit quantization for rotations (default: 12; N should be between 4 and 16)
# 	-as N: use N-bit quantization for scale (default: 16; N should be between 1 and 24)
# 	-af N: resample animations at N Hz (default: 30)
# 	-ac: keep constant animation tracks even if they don't modify the node transform

# Scene:
# 	-kn: keep named nodes and meshes attached to named nodes so that named nodes can be transformed externally
# 	-km: keep named materials and disable named material merging
# 	-ke: keep extras data
# 	-mm: merge instances of the same mesh together when possible
# 	-mi: use EXT_mesh_gpu_instancing when serializing multiple mesh instances

# Miscellaneous:
# 	-cf: produce compressed gltf/glb files with fallback for loaders that don't support compression
# 	-noq: disable quantization; produces much larger glTF files with no extensions
# 	-v: verbose output (print version when used without other options)
# 	-r file: output a JSON report to file
# 	-h: display this help and exit
# [enes@enes ~]$ 


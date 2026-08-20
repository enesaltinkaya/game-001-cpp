#!/bin/bash
if [ ! -d "./data" ]; then
   echo "where is data dir?"
   exit 1
fi
GLTFPACK=/home/enes/Projects/c/cpp-thirdparty/meshoptimizer/git/build-linux/gltfpack

function convertBlendFile() {
  fileName="$(basename $1)"
  filenameWithoutExtension="${fileName%.*}"
  tmpFile="${SCRIPTS_TMP}/$fileName"
  convert=0
  blendFileLastModified=$(date -r $1 "+%Y%m%d%H%M%S")
  
  if [ ! -f "$tmpFile" ]; then
      convert=1
      touch $tmpFile
      echo $blendFileLastModified  > $tmpFile
      echo "$1 does not exist."
  else
      savedModifiedDate=$(cat $tmpFile | xargs)
      if [ "$savedModifiedDate" != "$blendFileLastModified" ]; then
          convert=1
          echo $blendFileLastModified > $tmpFile
      fi
  fi

  if [ $convert == 1 ]; then
    dir="${2:-data/pak_1/models}"
    mkdir -p $dir
    echo "#############################################"
    echo -n "blend to glb ${filenameWithoutExtension}... "

    blender $1 --background --python "${SCRIPTS}/1-blender-scene.py" -- $dir/$filenameWithoutExtension.glb $3
    echo ".glb file... `du -sh "$dir/${filenameWithoutExtension}".glb | cut -f1`"


    echo -n "gltfpack... "

    export KTX_GEN_MIPMAP=1
    # -slb -si 0.5 
    # -tl 512
    $GLTFPACK -vpf -cc -vt 16 -vn 16 -ke -kn -kv -km -af 30 -at 24 -as 24 -ar 16 -i "$dir/${filenameWithoutExtension}.glb" -o "$dir/${filenameWithoutExtension}".pack.glb
    # if [[ $? -ne 0 ]] ; then
    #     echo "$output"
    #     exit 1
    # fi
    echo "`du -sh "$dir/${filenameWithoutExtension}".pack.glb | cut -f1`"
    rm "$dir/${filenameWithoutExtension}.glb"
    mv "$dir/${filenameWithoutExtension}.pack.glb" "$dir/${filenameWithoutExtension}"

    # ── jolt-shape-builder (pre-bake Jolt MeshShape BVH) ──
    TOOLS_DIR="$(dirname "$(realpath "${BASH_SOURCE[0]}")" )/../tools"
    JOLT_SHAPE_BUILDER="$TOOLS_DIR/jolt-shape-builder/jolt-shape-builder"
    joltFile="$dir/${filenameWithoutExtension}.jolt"
    echo -n "jolt shapes... "
    "$JOLT_SHAPE_BUILDER" "$dir/${filenameWithoutExtension}" "$joltFile"

    # ── Combined terrain+obstacles navmesh is built by build-navmesh.sh ──

    echo -n "zstd... "
    output="$(zstd -q -10 --rm -f "$dir/${filenameWithoutExtension}")"
    if [[ $? -ne 0 ]] ; then
        echo "$output"
        exit 1
    fi
    echo "`du -sh "$dir/${filenameWithoutExtension}.zst" | cut -f1`"
    
    mv "$dir/${filenameWithoutExtension}.zst" "$dir/${filenameWithoutExtension}.dat"

    # Compress jolt sidecar if it was produced
    if [ -f "$joltFile" ]; then
        zstd -q -10 --rm -f "$joltFile"
        mv "$joltFile.zst" "$dir/${filenameWithoutExtension}.jolt.dat"
        echo "jolt shapes: `du -sh "$dir/${filenameWithoutExtension}.jolt.dat" | cut -f1`"
    fi
  fi

}

mkdir -p "${SCRIPTS_TMP}"
# convertBlendFile /home/enes/Projects/assets/Scenes/azgaar-tasings.blend
# convertBlendFile /home/enes/Projects/assets/Scenes/terrain-cala.blend
convertBlendFile /home/enes/Projects/assets/Scenes/Characters/eve.blend
convertBlendFile /home/enes/Projects/assets/Scenes/Characters/animations.blend
# convertBlendFile /home/enes/Projects/assets/Scenes/Characters/eve-b.blend
# convertBlendFile /home/enes/Projects/assets/Scenes/test.blend
convertBlendFile /home/enes/Projects/assets/Scenes/test2.blend
convertBlendFile /home/enes/Projects/assets/Scenes/test3.blend
# props models (deciduous etc.) are packed by 2-blender-props.sh (near + far LOD)


# [enes@enes game-001]$ /home/enes/Projects/c/cpp-thirdparty/meshoptimizer/git/build-linux/gltfpack -h
# gltfpack 1.0
# Usage: gltfpack [options] -i input -o output

# Basics:
#         -i file: input file to process, .obj/.gltf/.glb
#         -o file: output file path, .gltf/.glb
#         -c: produce compressed gltf/glb files (-cc/-cz for higher compression ratio)

# Textures:
#         -tc: convert all textures to KTX2 with BasisU supercompression
#         -tu: use UASTC when encoding textures (much higher quality and much larger size)
#         -tw: convert all textures to WebP
#         -tq N: set texture encoding quality (default: 8; N should be between 1 and 10)
#         -ts R: scale texture dimensions by the ratio R (default: 1; R should be between 0 and 1)
#         -tl N: limit texture dimensions to N pixels (default: 0 = no limit)
#         -tp: resize textures to nearest power of 2 to conform to WebGL1 restrictions
#         -tfy: flip textures along Y axis during BasisU supercompression
#         -tj N: use N threads when compressing textures
#         -tr: keep referring to original texture paths instead of copying/embedding images
#         Texture classes:
#         -tc C: use ETC1S when encoding textures of class C
#         -tu C: use UASTC when encoding textures of class C
#         -tw C: use WebP when encoding textures of class C
#         -tq C N: set texture encoding quality for class C
#         -ts C R: scale texture dimensions for class C
#         -tl C N: limit texture dimensions for class C
#         ... where C is a comma-separated list (no spaces) with valid values color,normal,attrib

# Simplification:
#         -si R: simplify meshes targeting triangle/point count ratio R (default: 1; R should be between 0 and 1)
#         -se E: limit simplification error to E (default: 0.01 = 1% deviation; E should be between 0 and 1)
#         -sp: use permissive simplification mode to allow simplification across attribute discontinuities
#         -sa: aggressively simplify to the target ratio disregarding quality
#         -slb: lock border vertices during simplification to avoid gaps on connected meshes

# Vertex precision:
#         -vp N: use N-bit quantization for positions (default: 14; N should be between 1 and 16)
#         -vt N: use N-bit quantization for texture coordinates (default: 12; N should be between 1 and 16)
#         -vn N: use N-bit quantization for normals (default: 8; N should be between 1 and 16) and tangents (up to 8-bit)
#         -vc N: use N-bit quantization for colors (default: 8; N should be between 1 and 16)

# Vertex positions:
#         -vpi: use integer attributes for positions (default)
#         -vpn: use normalized attributes for positions
#         -vpf: use floating point attributes for positions

# Vertex attributes:
#         -vtf: use floating point attributes for texture coordinates
#         -vnf: use floating point attributes for normals
#         -vi: use interleaved vertex attributes (reduces compression efficiency)
#         -kv: keep source vertex attributes even if they aren't used

# Animations:
#         -at N: use N-bit quantization for translations (default: 16; N should be between 1 and 24)
#         -ar N: use N-bit quantization for rotations (default: 12; N should be between 4 and 16)
#         -as N: use N-bit quantization for scale (default: 16; N should be between 1 and 24)
#         -af N: resample animations at N Hz (default: 30; use 0 to disable)
#         -ac: keep constant animation tracks even if they don't modify the node transform

# Scene:
#         -kn: keep named nodes and meshes attached to named nodes so that named nodes can be transformed externally
#         -km: keep named materials and disable named material merging
#         -ke: keep extras data
#         -mm: merge instances of the same mesh together when possible
#         -mi: use EXT_mesh_gpu_instancing when serializing multiple mesh instances

# Miscellaneous:
#         -cf: produce compressed gltf/glb files with fallback for loaders that don't support compression
#         -ce ext|khr: use EXT or KHR version of meshopt compression extension for compression
#         -noq: disable quantization; produces much larger glTF files with no extensions
#         -v: verbose output (when used with other options)
#         -v: print version (when used without other options)
#         -r file: output a JSON report to file
#         -h: display this help and exit

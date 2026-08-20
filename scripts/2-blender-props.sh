#!/bin/bash
# Props model packer: for each .blend entered at the bottom, produces
#   <name>.dat      - full-detail model
#   <name>_far.dat  - simplified far LOD (gltfpack -si $FAR_SIMPLIFY_RATIO)
# Same pipeline as 1-blender-scene.sh minus the jolt shapes (props have no
# physics).  The game loads <name>.dat for the near variant and <name>_far.dat
# for the far LOD variant (e.g. deciduous.dat / deciduous_far.dat).
if [ ! -d "./data" ]; then
   echo "where is data dir?"
   exit 1
fi
GLTFPACK=/home/enes/Projects/c/cpp-thirdparty/meshoptimizer/git/build-linux/gltfpack
FAR_SIMPLIFY_RATIO=0.05

function convertPropsBlend() {
  fileName="$(basename $1)"
  filenameWithoutExtension="${fileName%.*}"
  tmpFile="${SCRIPTS_TMP}/$fileName.props"
  convert=0
  blendFileLastModified=$(date -r $1 "+%Y%m%d%H%M%S")

  if [ ! -f "$tmpFile" ]; then
      convert=1
      touch $tmpFile
      echo $blendFileLastModified  > $tmpFile
      echo "no cache for $1 - converting."
  else
      savedModifiedDate=$(cat $tmpFile | xargs)
      if [ "$savedModifiedDate" != "$blendFileLastModified" ]; then
          convert=1
          echo $blendFileLastModified > $tmpFile
      fi
  fi

  if [ $convert == 1 ]; then
    dir="${2:-data/pak_1/models/props}"
    mkdir -p $dir
    echo "#############################################"
    echo -n "blend to glb ${filenameWithoutExtension}... "

    blender $1 --background --python "${SCRIPTS}/1-blender-scene.py" -- $dir/$filenameWithoutExtension.glb $3
    echo ".glb file... `du -sh "$dir/${filenameWithoutExtension}".glb | cut -f1`"

    export KTX_GEN_MIPMAP=1

    echo -n "gltfpack (full)... "
    $GLTFPACK -vpf -cc -vt 16 -vn 16 -ke -kn -kv -km -i "$dir/${filenameWithoutExtension}.glb" -o "$dir/${filenameWithoutExtension}".full.glb
    echo "`du -sh "$dir/${filenameWithoutExtension}".full.glb | cut -f1`"

    echo -n "gltfpack (far, -si $FAR_SIMPLIFY_RATIO)... "
    $GLTFPACK -vpf -cc -vt 16 -vn 16 -ke -kn -kv -km -si $FAR_SIMPLIFY_RATIO -sp -i "$dir/${filenameWithoutExtension}.glb" -o "$dir/${filenameWithoutExtension}".far.glb
    echo "`du -sh "$dir/${filenameWithoutExtension}".far.glb | cut -f1`"

    rm "$dir/${filenameWithoutExtension}.glb"
    mv "$dir/${filenameWithoutExtension}.full.glb" "$dir/${filenameWithoutExtension}"
    mv "$dir/${filenameWithoutExtension}.far.glb" "$dir/${filenameWithoutExtension}_far"

    echo -n "zstd... "
    output="$(zstd -q -10 --rm -f "$dir/${filenameWithoutExtension}")"
    if [[ $? -ne 0 ]] ; then
        echo "$output"
        exit 1
    fi
    mv "$dir/${filenameWithoutExtension}.zst" "$dir/${filenameWithoutExtension}.dat"
    output="$(zstd -q -10 --rm -f "$dir/${filenameWithoutExtension}_far")"
    if [[ $? -ne 0 ]] ; then
        echo "$output"
        exit 1
    fi
    mv "$dir/${filenameWithoutExtension}_far.zst" "$dir/${filenameWithoutExtension}_far.dat"
    echo "done"
  fi

}

mkdir -p "${SCRIPTS_TMP}"
# convertPropsBlend /path/to/model.blend [dir]   (default dir: data/pak_1/models/props)
convertPropsBlend /var/home/enes/Projects/assets/models/Gledista_Triacanthos_BLEND/deciduous.blend data/pak_1/models/props
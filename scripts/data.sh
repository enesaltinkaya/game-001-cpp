#needs to be run from project root directory

#if BIN_DIR is set then use it, otherwise use "realpath build"
#BIN_DIR is set in release.sh
BIN_DIR="${BIN_DIR:-$(realpath build)}"
DATA_DIR="${BIN_DIR}/data"
RELEASE="${RELEASE:-0}"

checkDataDirectory() {
  if [ -d data ]; then
    mkdir -p ${DATA_DIR}
    copyStatic
    zipPaks
  fi
}

copyStatic() {
  if [ -d data/static ]; then
    cp -r data/static ${DATA_DIR}
  fi
}

zipPak() {
  MD5=($(tar -cf - $1 | md5sum))
  MD5_FILE="${SCRIPTS_TMP}/$1"

  DO_ZIP=0
  ZIP_LEVEL="-0"
  if [ ! -f "$MD5_FILE" ]; then
      DO_ZIP=1
      touch $MD5_FILE
      echo $MD5 > $MD5_FILE
  else
      SAVED_MD5=$(cat $MD5_FILE | xargs)
      if [ "$SAVED_MD5" != "$MD5" ]; then
          DO_ZIP=1
          echo $MD5 > $MD5_FILE
      fi
  fi

  if [ ! -f "${DATA_DIR}/$1.pak" ]; then
    DO_ZIP=1
    ZIP_LEVEL="-0"
  fi

  if [ $RELEASE == 1 ]; then
    DO_ZIP=1
    ZIP_LEVEL="-0"
  fi

  if [ $DO_ZIP == 1 ]; then
    mkdir -p $1
    cd $1
    rm -f "${DATA_DIR}/$1.pak"

    if [ $RELEASE == 1 ]; then
        skip="**/*.debug"
    else
        skip="**/*.release"
    fi

    zip -x $skip \*.frag -x \*.vert -x \*.shader -x \*.tese -x \*.tesc -q -r ${ZIP_LEVEL} "${DATA_DIR}/$1.pak" *
    file_size_kb=`du -sh "${DATA_DIR}/$1.pak" | cut -f1`
    echo "zipping $1 level:${ZIP_LEVEL} size:${file_size_kb}"
    cd ..
  fi

}

zipPaks() {
  cd data
  readarray -t pakDirectories <<< $(find ./ -type d -name "pak_*")
  for i in "${pakDirectories[@]}"; do
      zipPak "$i"
  done
  cd ..
}

mkdir -p "${SCRIPTS_TMP}"
checkDataDirectory


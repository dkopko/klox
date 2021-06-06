#!/bin/bash

set -o errexit
set -o nounset

SCRIPT_ROOT="$(cd "$(dirname "$0")" ; pwd)"

CLOX_REPO="https://github.com/dkopko/craftinginterpreters.git"
CB_REPO="https://github.com/dkopko/cb.git"
KLOX_REPO="https://github.com/dkopko/klox.git"
CB_LOCAL_ROOT="/home/daniel/workspace/cb"  #Adjust to a local CB path to test local changes.
KLOX_LOCAL_ROOT="${SCRIPT_ROOT}"
TESTBED_ROOT="${SCRIPT_ROOT}/testbed"

rm -rf "${TESTBED_ROOT}"
mkdir -p "${TESTBED_ROOT}"
cd "${TESTBED_ROOT}"

git clone "${CLOX_REPO}" || true

git clone "${CB_REPO}" || true
if [[ -d "${CB_LOCAL_ROOT}" ]]
then
    cp -pr "${CB_LOCAL_ROOT}"/src/*.[ch] "${TESTBED_ROOT}"/cb/src
    cp -pr "${CB_LOCAL_ROOT}"/CMakeLists.txt "${TESTBED_ROOT}"/cb
    cp -pr "${CB_LOCAL_ROOT}"/Makefile "${TESTBED_ROOT}"/cb
fi

git clone "${KLOX_REPO}" || true
cp -pr "${KLOX_LOCAL_ROOT}"/c/*.cpp "${TESTBED_ROOT}"/klox/c
cp -pr "${KLOX_LOCAL_ROOT}"/c/*.h "${TESTBED_ROOT}"/klox/c
cp -pr "${KLOX_LOCAL_ROOT}"/c/CMakeLists.txt "${TESTBED_ROOT}"/klox/c
cp -pr "${KLOX_LOCAL_ROOT}"/c/Makefile "${TESTBED_ROOT}"/klox/c


{
  cd "${TESTBED_ROOT}"/craftinginterpreters
  git checkout ilat
  CI_COMMIT="$(git rev-parse --short HEAD)"
  make clean
  make clox
}

{
  cd "${TESTBED_ROOT}"/cb
  CB_COMMIT="$(git rev-parse --short HEAD)"
  make clean
  make -j
}

{
  cd "${TESTBED_ROOT}"/klox/c

  KLOX_COMMIT="$(git rev-parse --short HEAD)"

  # Turn on KLOX_ILAT
  sed -i -e 's#-DKLOX_ILAT=0#-DKLOX_ILAT=1#' CMakeLists.txt

  make clean
  make -j CBROOT="${TESTBED_ROOT}"/cb
}

cd "${KLOX_LOCAL_ROOT}"

rm -f ilat.out
./util/test_testbed_clox.py
./benchmark.sh ./testbed/craftinginterpreters/clox
mv ilat.out ilat.clox.out
KLOX_RING_SIZE=1073741824 ./util/test_testbed_klox.py   # Run with sufficient pre-sizing for all tests, to avoid incorporating resize costs.
./benchmark.sh ./testbed/klox/c/BUILD/RelWithDebInfo/klox
mv ilat.out ilat.klox.out


{
  echo "Crafting Interpreters commit: ${CI_COMMIT}"
  echo "CB Commit:                    ${CB_COMMIT}"
  echo "Klox Commit:                  ${KLOX_COMMIT}"
  echo
  echo "CB local Changes:"
  (cd "${TESTBED_ROOT}"/cb ; git diff )
  echo
  echo "Klox Changes:"
  (cd "${TESTBED_ROOT}"/klox ; git diff )

  ./compareilat ilat.clox.out ilat.klox.out |sort -n -r -k14
} |tee ilat_compare.$(date +'%Y%m%d-%H%M%S').out


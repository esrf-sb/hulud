#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build-prod}"
HDF5_VERSION="${HDF5_VERSION:-1.14.3}"
HDF5_PREFIX="${HDF5_PREFIX:-$ROOT_DIR/.deps/hdf5-${HDF5_VERSION}}"
HDF5_DIR="${HDF5_DIR:-${HDF5_PREFIX}/cmake}"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)}"
CMAKE_BIN="${CMAKE_BIN:-cmake}"

"${ROOT_DIR}/scripts/build-static-hdf5.sh"

"${CMAKE_BIN}" -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DHULUD_PRODUCTION_PLUGIN=ON \
    -DHDF5_DIR="${HDF5_DIR}" \
    "$@"

"${CMAKE_BIN}" --build "${BUILD_DIR}" -j"${JOBS}"

echo "Production-compatible plugin available at ${BUILD_DIR}/hulud-plugin.so"

#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
HDF5_VERSION="${HDF5_VERSION:-1.14.3}"
HDF5_PREFIX="${HDF5_PREFIX:-$ROOT_DIR/.deps/hdf5-${HDF5_VERSION}}"
HDF5_BUILD_DIR="${HDF5_BUILD_DIR:-$ROOT_DIR/build-hdf5-${HDF5_VERSION}}"
HDF5_DOWNLOAD_URL="${HDF5_DOWNLOAD_URL:-https://support.hdfgroup.org/ftp/HDF5/releases/hdf5-1.14/hdf5-${HDF5_VERSION}/src/hdf5-${HDF5_VERSION}.tar.gz}"
HDF5_CONFIG_DIR="${HDF5_PREFIX}/cmake"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)}"
CMAKE_BIN="${CMAKE_BIN:-cmake}"

if [ -f "${HDF5_CONFIG_DIR}/hdf5-config.cmake" ]; then
    echo "Using existing static HDF5 at ${HDF5_PREFIX}"
    exit 0
fi

mkdir -p "${ROOT_DIR}/.deps"

tmpdir=$(mktemp -d)
cleanup() {
    rm -rf "${tmpdir}"
}
trap cleanup EXIT

archive="${tmpdir}/hdf5-${HDF5_VERSION}.tar.gz"
src_dir="${tmpdir}/hdf5-${HDF5_VERSION}"

download_hdf5() {
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL "${HDF5_DOWNLOAD_URL}" -o "${archive}"
        return
    fi
    if command -v wget >/dev/null 2>&1; then
        wget -q "${HDF5_DOWNLOAD_URL}" -O "${archive}"
        return
    fi
    echo "Neither curl nor wget is available to download HDF5." >&2
    exit 1
}

echo "Downloading HDF5 ${HDF5_VERSION} from ${HDF5_DOWNLOAD_URL}"
download_hdf5
tar -xzf "${archive}" -C "${tmpdir}"

"${CMAKE_BIN}" -S "${src_dir}" -B "${HDF5_BUILD_DIR}" \
    -DCMAKE_INSTALL_PREFIX="${HDF5_PREFIX}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DHDF5_ENABLE_Z_LIB_SUPPORT=ON \
    -DBUILD_SHARED_LIBS=OFF \
    -DHDF5_BUILD_CPP_LIB=OFF \
    -DHDF5_BUILD_FORTRAN=OFF \
    -DHDF5_BUILD_JAVA=OFF \
    -DHDF5_BUILD_HL_LIB=ON \
    -DHDF5_ENABLE_THREADSAFE=ON \
    -DALLOW_UNSUPPORTED=ON

"${CMAKE_BIN}" --build "${HDF5_BUILD_DIR}" -j"${JOBS}"
"${CMAKE_BIN}" --install "${HDF5_BUILD_DIR}"

if [ ! -f "${HDF5_CONFIG_DIR}/hdf5-config.cmake" ]; then
    echo "Static HDF5 was built, but ${HDF5_CONFIG_DIR}/hdf5-config.cmake is missing." >&2
    exit 1
fi

echo "Static HDF5 ${HDF5_VERSION} installed in ${HDF5_PREFIX}"

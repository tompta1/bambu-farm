#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd)
BUILD_DIR="${BUILD_DIR:-${SCRIPT_DIR}/target/debug/shared}"
OUT_DIR="${OUT_DIR:-${SCRIPT_DIR}/dist}"
PACKAGE_NAME="${PACKAGE_NAME:-bambu-farm-oss-network-plugin-linux-aarch64}"
ARCHIVE_PATH="${OUT_DIR}/${PACKAGE_NAME}.tar.gz"

require_file() {
    if [ ! -f "$1" ]; then
        echo "Missing required file: $1" >&2
        exit 1
    fi
}

require_file "${BUILD_DIR}/libbambu_networking.so"
require_file "${BUILD_DIR}/libBambuSource.so"
require_file "${SCRIPT_DIR}/install-flatpak-plugin.sh"
require_file "${REPO_ROOT}/bambu-farm-server/bambufarm_example.toml"
require_file "${REPO_ROOT}/README.md"

TMP_DIR=$(mktemp -d)
trap 'rm -rf "${TMP_DIR}"' EXIT

mkdir -p "${TMP_DIR}/${PACKAGE_NAME}" "${OUT_DIR}"
cp -f "${BUILD_DIR}/libbambu_networking.so" "${TMP_DIR}/${PACKAGE_NAME}/"
cp -f "${BUILD_DIR}/libBambuSource.so" "${TMP_DIR}/${PACKAGE_NAME}/"
cp -f "${SCRIPT_DIR}/install-flatpak-plugin.sh" "${TMP_DIR}/${PACKAGE_NAME}/"
cp -f "${REPO_ROOT}/bambu-farm-server/bambufarm_example.toml" "${TMP_DIR}/${PACKAGE_NAME}/"
cp -f "${REPO_ROOT}/README.md" "${TMP_DIR}/${PACKAGE_NAME}/"

tar -C "${TMP_DIR}" -czf "${ARCHIVE_PATH}" "${PACKAGE_NAME}"
printf 'Created %s\n' "${ARCHIVE_PATH}"

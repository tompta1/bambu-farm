#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd)
APP_ID="${APP_ID:-com.bambulab.BambuStudio}"
BUILD_DIR="${BUILD_DIR:-${SCRIPT_DIR}/target/debug/shared}"
if [ -f "${SCRIPT_DIR}/libbambu_networking.so" ] && [ -f "${SCRIPT_DIR}/libBambuSource.so" ]; then
    BUILD_DIR="${SCRIPT_DIR}"
fi
HOST_CONFIG_DIR="${XDG_CONFIG_HOME:-${HOME}/.config}/BambuStudio"
HOST_PLUGIN_DIR="${HOST_CONFIG_DIR}/plugins"
FLATPAK_PLUGIN_DIR="${HOME}/.var/app/${APP_ID}/config/BambuStudio/plugins"
FLATPAK_CONFIG_DIR="${HOME}/.var/app/${APP_ID}/config/BambuStudio"

CONFIG_FILE="${CONFIG_FILE:-}"
if [ -z "${CONFIG_FILE}" ]; then
    if [ -f "${REPO_ROOT}/bambu-farm-server/bambufarm.toml" ]; then
        CONFIG_FILE="${REPO_ROOT}/bambu-farm-server/bambufarm.toml"
    elif [ -f "${SCRIPT_DIR}/bambufarm_example.toml" ]; then
        CONFIG_FILE="${SCRIPT_DIR}/bambufarm_example.toml"
    else
        CONFIG_FILE="${REPO_ROOT}/bambu-farm-server/bambufarm_example.toml"
    fi
fi

usage() {
    cat <<EOF
Usage: $0 install|lock|unlock|status

install  Copy the OSS plugin into the host and Flatpak plugin directories.
lock     Make both plugin directories read-only to stop Bambu Studio overwriting them.
unlock   Re-enable writes to both plugin directories.
status   Show active plugin architecture/version and directory permissions.
EOF
}

require_file() {
    if [ ! -f "$1" ]; then
        echo "Missing required file: $1" >&2
        exit 1
    fi
}

copy_active_bundle() {
    target_dir="$1"
    mkdir -p "${target_dir}"
    if [ "$(readlink -f "${BUILD_DIR}/libbambu_networking.so")" != "$(readlink -f "${target_dir}/libbambu_networking.so" 2>/dev/null || true)" ]; then
        cp -f "${BUILD_DIR}/libbambu_networking.so" "${target_dir}/libbambu_networking.so"
    fi
    if [ "$(readlink -f "${BUILD_DIR}/libBambuSource.so")" != "$(readlink -f "${target_dir}/libBambuSource.so" 2>/dev/null || true)" ]; then
        cp -f "${BUILD_DIR}/libBambuSource.so" "${target_dir}/libBambuSource.so"
    fi
}

show_status() {
    for dir in "${HOST_PLUGIN_DIR}" "${FLATPAK_PLUGIN_DIR}"; do
        echo "== ${dir} =="
        ls -ld "${dir}" 2>/dev/null || true
        file "${dir}/libbambu_networking.so" 2>/dev/null || true
        grep -a -n '01.06.00.00\|02.03.00.62\|02.05.00.66' "${dir}/libbambu_networking.so" 2>/dev/null | head -n 1 || true
        echo
    done
}

cmd="${1:-}"
case "${cmd}" in
    install)
        require_file "${BUILD_DIR}/libbambu_networking.so"
        require_file "${BUILD_DIR}/libBambuSource.so"
        require_file "${CONFIG_FILE}"
        copy_active_bundle "${HOST_PLUGIN_DIR}"
        copy_active_bundle "${FLATPAK_PLUGIN_DIR}"
        mkdir -p "${FLATPAK_CONFIG_DIR}"
        cp -f "${CONFIG_FILE}" "${FLATPAK_CONFIG_DIR}/bambufarm.toml"
        show_status
        ;;
    lock)
        chmod u-w "${HOST_PLUGIN_DIR}" "${FLATPAK_PLUGIN_DIR}"
        show_status
        ;;
    unlock)
        chmod u+w "${HOST_PLUGIN_DIR}" "${FLATPAK_PLUGIN_DIR}"
        show_status
        ;;
    status)
        show_status
        ;;
    *)
        usage >&2
        exit 1
        ;;
esac

#!/usr/bin/env bash

set -Eeuo pipefail

REPO_ROOT="${SCRIPT_DIR:?SCRIPT_DIR is required}"
LKM_TOOLS_DIR="${REPO_ROOT}/prebuilts/LKM_Tools"
BOOT_EDITOR_DIR="${REPO_ROOT}/prebuilts/vendor_boot_unpack"
KBUILD_PATH="${DIST_DIR:-${OUT_DIR:?OUT_DIR is required}/dist}"
PKG_VENDOR_BOOT="${LKM_TOOLS_DIR}/02.prepare_vendor_boot_modules.sh"
MODULES_LIST="${LKM_TOOLS_DIR}/vendor_boot/modules_list.txt"
OEM_LOAD_FILE="${LKM_TOOLS_DIR}/vendor_boot/modules.load"
OUTPUT_DIR="${BOOT_EDITOR_DIR}/build/unzip_boot/root.1/lib/modules"
SYSTEM_MAP="${KBUILD_PATH}/System.map"
STRIP_TOOL="${REPO_ROOT}/kernel_platform/prebuilts/clang/host/linux-x86/clang-r450784e/bin/llvm-strip"

[[ -x "${PKG_VENDOR_BOOT}" ]] || chmod +x "${PKG_VENDOR_BOOT}"
[[ -f "${MODULES_LIST}" ]] || { echo "vendor_boot modules list not found: ${MODULES_LIST}" >&2; exit 1; }
[[ -f "${OEM_LOAD_FILE}" ]] || { echo "vendor_boot modules.load not found: ${OEM_LOAD_FILE}" >&2; exit 1; }

mkdir -p "${OUTPUT_DIR}"
"${PKG_VENDOR_BOOT}" \
    "${MODULES_LIST}" \
    "${KBUILD_PATH}" \
    "${OEM_LOAD_FILE}" \
    "${SYSTEM_MAP}" \
    "${STRIP_TOOL}" \
    "${OUTPUT_DIR}"

(
    cd "${BOOT_EDITOR_DIR}"
    ./gradlew pack
    cp vendor_boot.img "${REPO_ROOT}/vendor_boot.img"
)

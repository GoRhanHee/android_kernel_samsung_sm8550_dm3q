#!/usr/bin/env bash

set -Eeuo pipefail

REPO_ROOT="${SCRIPT_DIR:?SCRIPT_DIR is required}"
LKM_TOOLS_DIR="${REPO_ROOT}/prebuilts/LKM_Tools"
AIT_DIR="${REPO_ROOT}/prebuilts/vendor_dlkm_unpack"
KBUILD_PATH="${DIST_DIR:-${OUT_DIR:?OUT_DIR is required}/dist}"
PKG_VENDOR_DLKM="${LKM_TOOLS_DIR}/03.prepare_vendor_dlkm.sh"
VENDOR_DLKM_MODULES_LIST="${LKM_TOOLS_DIR}/vendor_dlkm/modules_list.txt"
VENDOR_BOOT_MODULES_LIST="${LKM_TOOLS_DIR}/vendor_boot/modules_list.txt"
VENDOR_DLKM_MODULES_LOAD_FILE="${LKM_TOOLS_DIR}/vendor_dlkm/modules.load"
OUTPUT_DIR="${AIT_DIR}/EXTRACTED_IMAGES/extracted_vendor_dlkm"
MODULES_OUTPUT_DIR="${OUTPUT_DIR}/lib/modules"
SYSTEM_MAP="${KBUILD_PATH}/System.map"
STRIP_TOOL="${REPO_ROOT}/kernel_platform/prebuilts/clang/host/linux-x86/clang-r450784e/bin/llvm-strip"
REPACK_CONFIG="${AIT_DIR}/CONFIGS/vendor_dlkm_repack.conf"
REPACKED_IMAGE="${AIT_DIR}/REPACKED_IMAGES/vendor_dlkm_repacked.img"

[[ -x "${PKG_VENDOR_DLKM}" ]] || chmod +x "${PKG_VENDOR_DLKM}"
[[ -f "${VENDOR_DLKM_MODULES_LIST}" ]] || { echo "vendor_dlkm modules list not found: ${VENDOR_DLKM_MODULES_LIST}" >&2; exit 1; }
[[ -f "${VENDOR_DLKM_MODULES_LOAD_FILE}" ]] || { echo "vendor_dlkm modules.load not found: ${VENDOR_DLKM_MODULES_LOAD_FILE}" >&2; exit 1; }

mkdir -p "${MODULES_OUTPUT_DIR}"
"${PKG_VENDOR_DLKM}" \
    "${VENDOR_DLKM_MODULES_LIST}" \
    "${KBUILD_PATH}" \
    "${VENDOR_DLKM_MODULES_LOAD_FILE}" \
    "${SYSTEM_MAP}" \
    "${STRIP_TOOL}" \
    "${MODULES_OUTPUT_DIR}" \
    "${VENDOR_BOOT_MODULES_LIST}" \
    "" \
    ""

cat > "${REPACK_CONFIG}" <<EOF
ACTION=repack
SOURCE_DIR=${OUTPUT_DIR}
OUTPUT_IMAGE=${REPACKED_IMAGE}
FILESYSTEM=erofs
CREATE_SPARSE_IMAGE=false
COMPRESSION_MODE=lz4
EOF

(
    cd "${AIT_DIR}"
    if (( EUID == 0 )); then
        ./android_image_tools.sh --conf="${REPACK_CONFIG}" --quiet
    else
        sudo ./android_image_tools.sh --conf="${REPACK_CONFIG}" --quiet
    fi
)

cp "${REPACKED_IMAGE}" "${REPO_ROOT}/vendor_dlkm.img"

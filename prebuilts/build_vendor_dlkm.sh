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
OBJCOPY_TOOL="${REPO_ROOT}/kernel_platform/prebuilts/clang/host/linux-x86/clang-r450784e/bin/llvm-objcopy"
REPACK_CONFIG="${AIT_DIR}/CONFIGS/vendor_dlkm_repack.conf"
REPACKED_IMAGE="${AIT_DIR}/REPACKED_IMAGES/vendor_dlkm_repacked.img"

[[ -x "${PKG_VENDOR_DLKM}" ]] || chmod +x "${PKG_VENDOR_DLKM}"
[[ -x "${OBJCOPY_TOOL}" ]] || { echo "llvm-objcopy not found: ${OBJCOPY_TOOL}" >&2; exit 1; }
[[ -f "${VENDOR_DLKM_MODULES_LIST}" ]] || { echo "vendor_dlkm modules list not found: ${VENDOR_DLKM_MODULES_LIST}" >&2; exit 1; }
[[ -f "${VENDOR_DLKM_MODULES_LOAD_FILE}" ]] || { echo "vendor_dlkm modules.load not found: ${VENDOR_DLKM_MODULES_LOAD_FILE}" >&2; exit 1; }

STOCK_MODULES_DIR="$(mktemp -d)"
trap 'rm -rf "${STOCK_MODULES_DIR}"' EXIT
cp -a "${MODULES_OUTPUT_DIR}/." "${STOCK_MODULES_DIR}/"

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

while IFS= read -r module; do
    "${OBJCOPY_TOOL}" \
        --remove-section=.BTF \
        --remove-section=.BTF.ext \
        "${module}"
done < <(find "${MODULES_OUTPUT_DIR}" -maxdepth 1 -type f -name "*.ko" -print)

UNRESOLVED=0
if [[ -s "${MODULES_OUTPUT_DIR}/missing_modules.txt" ]]; then
    while IFS= read -r module; do
        [[ -n "${module}" ]] || continue
        if [[ -f "${STOCK_MODULES_DIR}/${module}" ]]; then
            cp "${STOCK_MODULES_DIR}/${module}" "${MODULES_OUTPUT_DIR}/${module}"
        else
            UNRESOLVED=1
        fi
    done < "${MODULES_OUTPUT_DIR}/missing_modules.txt"
fi
if (( UNRESOLVED == 0 )); then
    rm -f "${MODULES_OUTPUT_DIR}/missing_modules.txt"
fi
while IFS= read -r stock_file; do
    target="${MODULES_OUTPUT_DIR}/$(basename "${stock_file}")"
    [[ -e "${target}" ]] || cp "${stock_file}" "${target}"
done < <(find "${STOCK_MODULES_DIR}" -maxdepth 1 -type f ! -name '*.ko')

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

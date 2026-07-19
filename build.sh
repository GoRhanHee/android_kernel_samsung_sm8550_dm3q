#!/usr/bin/env bash

set -Eeuo pipefail

readonly SCRIPT_NAME="$(basename "$0")"
readonly SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly SOURCE_DIR="${SOURCE_DIR:-${SCRIPT_DIR}}"
readonly KERNEL_PLATFORM="${SOURCE_DIR}/kernel_platform"
readonly TOOLCHAIN_URL="${TOOLCHAIN_URL:-https://github.com/GoRhanHee/samsung_sm8550_toolchain/releases/download/toolchain/toolchain.tar.xz}"
readonly CLANG_BIN="${KERNEL_PLATFORM}/prebuilts/clang/host/linux-x86/clang-r450784e/bin/clang"
readonly JOBS="${JOBS:-$(nproc)}"
export LTO="${LTO:-thin}"

usage() {
    cat <<EOF
Usage:
  ${SCRIPT_NAME} common
  ${SCRIPT_NAME} full

Modes:
  common  Build only the Android common GKI kernel and generic GKI boot images.
  full    Build the common kernel, msm-kernel, DTBs and external/vendor modules.

Examples:
  ${SCRIPT_NAME} common
  ${SCRIPT_NAME} full

Environment overrides:
  SOURCE_DIR       Kernel source directory (default: ${SOURCE_DIR})
  JOBS             Parallel jobs for the common build (default: ${JOBS})
  COMMON_OUT_DIR   Common kernel object directory
  COMMON_DIST_DIR  Common kernel artifact directory
  FULL_OUT_DIR     Full-build output directory
  TOOLCHAIN_URL    Samsung toolchain archive URL
  LTO              LTO mode: none, thin or full (default: ${LTO})
EOF
}

die() {
    echo "error: $*" >&2
    exit 1
}

require_command() {
    command -v "$1" >/dev/null 2>&1 || die "required command not found: $1"
}

update_submodules() {
    [[ -f "${SOURCE_DIR}/.gitmodules" ]] || return

    require_command git
    echo "[submodule] Syncing and updating submodules"
    git -C "${SOURCE_DIR}" submodule sync --recursive
    git -C "${SOURCE_DIR}" submodule update --init --recursive
}

import_kernelsu_next() {
    require_command curl
    require_command bash

    echo "[KernelSU-Next] Importing dev branch"
    (
        cd "${KERNEL_PLATFORM}/common"
        curl -LSs \
            "https://raw.githubusercontent.com/KernelSU-Next/KernelSU-Next/next/kernel/setup.sh" | \
            bash -s dev
    )
}

prepare_toolchain() {
    if [[ -x "${CLANG_BIN}" ]]; then
        echo "[toolchain] Using ${CLANG_BIN}"
        return
    fi

    require_command wget
    require_command tar

    local archive
    archive="$(mktemp "${TMPDIR:-/tmp}/sm8550-toolchain.XXXXXX.tar.xz")"

    echo "[toolchain] Downloading ${TOOLCHAIN_URL}"
    wget -q --show-progress --progress=dot:giga \
        -O "${archive}" "${TOOLCHAIN_URL}"

    echo "[toolchain] Extracting prebuilts into ${KERNEL_PLATFORM}"
    tar -xf "${archive}" -C "${KERNEL_PLATFORM}" \
        --strip-components=1 toolchain/prebuilts
    rm -f "${archive}"

    [[ -x "${CLANG_BIN}" ]] || \
        die "clang-r450784e was not found after extracting the toolchain"
}

build_common() {
    local out_dir="${COMMON_OUT_DIR:-${SOURCE_DIR}/out/common}"
    local dist_dir="${COMMON_DIST_DIR:-${SOURCE_DIR}/out/common-dist}"

    echo "[common] OUT_DIR=${out_dir}"
    echo "[common] DIST_DIR=${dist_dir}"

    (
        cd "${KERNEL_PLATFORM}"
        BUILD_CONFIG=common/build.config.gki.aarch64 \
        OUT_DIR="${out_dir}" \
        DIST_DIR="${dist_dir}" \
        ./build/build.sh "-j${JOBS}"
    )

    echo "[common] Artifacts: ${dist_dir}"
}

build_full() {
    export BUILD_TARGET="dm3q_kor_singlex"
    export MODEL="dm3q"
    export PROJECT_NAME="dm3q"
    export REGION="kor"
    export CARRIER="singlex"
    export TARGET_BUILD_VARIANT="${TARGET_BUILD_VARIANT:-user}"

    export ANDROID_BUILD_TOP="${SOURCE_DIR}"
    export CHIPSET_NAME="kalama"
    export TARGET_PRODUCT="gki"
    export TARGET_BOARD_PLATFORM="gki"
    export ANDROID_PRODUCT_OUT="${ANDROID_BUILD_TOP}/out/target/product/${MODEL}"
    export OUT_DIR="${FULL_OUT_DIR:-${ANDROID_BUILD_TOP}/out/msm-${CHIPSET_NAME}-${CHIPSET_NAME}-${TARGET_PRODUCT}}"
    export DIST_DIR="${OUT_DIR}/dist"
    export MERGE_CONFIG="${ANDROID_BUILD_TOP}/kernel_platform/common/scripts/kconfig/merge_config.sh"

    export KBUILD_EXTRA_SYMBOLS="${ANDROID_BUILD_TOP}/out/vendor/qcom/opensource/mmrm-driver/Module.symvers \
        ${ANDROID_BUILD_TOP}/out/vendor/qcom/opensource/mm-drivers/hw_fence/Module.symvers \
        ${ANDROID_BUILD_TOP}/out/vendor/qcom/opensource/mm-drivers/sync_fence/Module.symvers \
        ${ANDROID_BUILD_TOP}/out/vendor/qcom/opensource/mm-drivers/msm_ext_display/Module.symvers \
        ${ANDROID_BUILD_TOP}/out/vendor/qcom/opensource/securemsm-kernel/Module.symvers \
		${ANDROID_BUILD_TOP}/out/vendor/qcom/opensource/graphics-kernel/Module.symvers \
		${ANDROID_BUILD_TOP}/out/vendor/qcom/opensource/datarmnet/core/Module.symvers \
		${ANDROID_BUILD_TOP}/out/vendor/qcom/opensource/wlan/qcacld-3.0/.kiwi_v2/Module.symvers \
		${ANDROID_BUILD_TOP}/out/vendor/qcom/opensource/wlan/platform/Module.symvers \
		${ANDROID_BUILD_TOP}/out/vendor/qcom/opensource/camera-kernel/Module.symvers \
		${ANDROID_BUILD_TOP}/out/vendor/qcom/opensource/eva-kernel/Module.symvers \
		${ANDROID_BUILD_TOP}/out/vendor/qcom/opensource/video-driver/Module.symvers \
		${ANDROID_BUILD_TOP}/out/vendor/qcom/opensource/display-drivers/msm/Module.symvers \
		${ANDROID_BUILD_TOP}/out/vendor/qcom/opensource/datarmnet-ext/aps/Module.symvers \
		${ANDROID_BUILD_TOP}/out/vendor/qcom/opensource/datarmnet-ext/wlan/Module.symvers \
		${ANDROID_BUILD_TOP}/out/vendor/qcom/opensource/datarmnet-ext/shs/Module.symvers \
		${ANDROID_BUILD_TOP}/out/vendor/qcom/opensource/datarmnet-ext/perf_tether/Module.symvers \
		${ANDROID_BUILD_TOP}/out/vendor/qcom/opensource/datarmnet-ext/perf/Module.symvers \
		${ANDROID_BUILD_TOP}/out/vendor/qcom/opensource/datarmnet-ext/sch/Module.symvers \
		${ANDROID_BUILD_TOP}/out/vendor/qcom/opensource/datarmnet-ext/offload/Module.symvers \
		${ANDROID_BUILD_TOP}/out/vendor/qcom/opensource/bt-kernel/Module.symvers \
		${ANDROID_BUILD_TOP}/out/vendor/qcom/opensource/dataipa/drivers/platform/msm/Module.symvers \
		${ANDROID_BUILD_TOP}/out/vendor/qcom/opensource/audio-kernel/Module.symvers \
        "

    export MODNAME="audio_dlkm"
    export KBUILD_EXT_MODULES="../vendor/qcom/opensource/mm-drivers/msm_ext_display \
        ../vendor/qcom/opensource/mm-drivers/sync_fence \
        ../vendor/qcom/opensource/mm-drivers/hw_fence \
        ../vendor/qcom/opensource/mmrm-driver \
        ../vendor/qcom/opensource/securemsm-kernel \
        ../vendor/qcom/opensource/display-drivers/msm \
        ../vendor/qcom/opensource/audio-kernel \
        ../vendor/qcom/opensource/camera-kernel \
        ../vendor/qcom/opensource/video-driver \
        ../vendor/qcom/opensource/graphics-kernel \
        ../vendor/qcom/opensource/dataipa/drivers/platform/msm \
        ../vendor/qcom/opensource/datarmnet/core \
        ../vendor/qcom/opensource/datarmnet-ext/aps \
        ../vendor/qcom/opensource/datarmnet-ext/offload \
        ../vendor/qcom/opensource/datarmnet-ext/shs \
        ../vendor/qcom/opensource/datarmnet-ext/sch \
        ../vendor/qcom/opensource/datarmnet-ext/perf \
        ../vendor/qcom/opensource/datarmnet-ext/perf_tether \
        ../vendor/qcom/opensource/datarmnet-ext/wlan \
        ../vendor/qcom/opensource/eva-kernel \
        ../vendor/qcom/opensource/wlan/platform \
        ../vendor/qcom/opensource/bt-kernel \
        ../vendor/qcom/opensource/wlan/qcacld-3.0/.kiwi_v2 \
    "  

    echo "[full] BUILD_TARGET=${BUILD_TARGET}"
    echo "[full] OUT_DIR=${OUT_DIR}"

    (
        cd "${SOURCE_DIR}"
        RECOMPILE_KERNEL=1 \
            ./kernel_platform/build/android/prepare_vendor.sh sec "${TARGET_PRODUCT}"
    )

    if [[ -f "${DIST_DIR}/kiwi_v2.ko" ]]; then
        cp "${DIST_DIR}/kiwi_v2.ko" \
            "${DIST_DIR}/qca_cld3_kiwi_v2.ko"
    fi

    echo "[full] Artifacts: ${OUT_DIR}/dist"
}

require_packaging_command() {
    require_command "$1"
}

run_privileged() {
    if (( EUID == 0 )); then
        "$@"
    else
        sudo "$@"
    fi
}

prepare_packaging_tools() {
    local prebuilts_dir="${SOURCE_DIR}/prebuilts"

    require_packaging_command git
    require_packaging_command wget
    require_packaging_command tar
    require_packaging_command lz4
    require_packaging_command unzip

    rm -rf \
        "${prebuilts_dir}/LKM_Tools" \
        "${prebuilts_dir}/vendor_boot_unpack" \
        "${prebuilts_dir}/vendor_dlkm_unpack"
    mkdir -p "${prebuilts_dir}"

    git clone --depth=1 \
        https://github.com/ravindu644/LKM_Tools.git \
        "${prebuilts_dir}/LKM_Tools"
    git clone --depth=1 \
        https://github.com/cfig/Android_boot_image_editor.git \
        "${prebuilts_dir}/vendor_boot_unpack"
    git clone --depth=1 \
        https://github.com/ravindu644/Android_Image_Tools.git \
        "${prebuilts_dir}/vendor_dlkm_unpack"
}

write_module_metadata() {
    local modules_dir="$1"
    local metadata_dir="$2"
    local modules_dep="${modules_dir}/modules.dep"
    local modules_load="${modules_dir}/modules.load"

    [[ -f "${modules_dep}" ]] || die "modules.dep not found: ${modules_dep}"
    [[ -f "${modules_load}" ]] || die "modules.load not found: ${modules_load}"

    mkdir -p "${metadata_dir}"
    bash "${SOURCE_DIR}/prebuilts/LKM_Tools/01.module_dep.sh" \
        "${modules_dep}" "${metadata_dir}"
    cp "${modules_load}" "${metadata_dir}/modules.load"
}

unpack_vendor_boot() {
    local vboot_url="https://github.com/GoRhanHee/Firmware_Samsung/releases/download/S918NKSS8FZF1_KOO_OKR/S918NKSS8FZF1_kernel.tar"
    local vboot_tar="${SOURCE_DIR}/.stock_vendor_boot.tar"
    local extract_dir="${SOURCE_DIR}/.stock_vendor_boot"
    local editor_dir="${SOURCE_DIR}/prebuilts/vendor_boot_unpack"
    local vendor_boot_lz4
    local modules_dir

    wget -q --show-progress \
        -O "${vboot_tar}" "${vboot_url}"
    rm -rf "${extract_dir}"
    mkdir -p "${extract_dir}"
    tar -xf "${vboot_tar}" -C "${extract_dir}"

    vendor_boot_lz4="$({ find "${extract_dir}" -type f -name 'vendor_boot.img.lz4' -print -quit; })"
    [[ -n "${vendor_boot_lz4}" ]] || die "vendor_boot.img.lz4 not found"

    lz4 -d -f \
        "${vendor_boot_lz4}" \
        "${SOURCE_DIR}/vendor_boot.stock.img"

    (
        cd "${editor_dir}"
        cp "${SOURCE_DIR}/vendor_boot.stock.img" vendor_boot.img
        ./gradlew unpack
    )

    modules_dir="${editor_dir}/build/unzip_boot/root.1/lib/modules"
    write_module_metadata \
        "${modules_dir}" \
        "${SOURCE_DIR}/prebuilts/LKM_Tools/vendor_boot"
}

unpack_vendor_dlkm() {
    local vdlkm_url="https://github.com/GoRhanHee/Firmware_Samsung/releases/download/S918NKSS8FZF1_KOO_OKR/S918NKSS8FZF1_vendor_dlkm.zip"
    local vdlkm_zip="${SOURCE_DIR}/.stock_vendor_dlkm.zip"
    local extract_dir="${SOURCE_DIR}/.stock_vendor_dlkm"
    local image_tools_dir="${SOURCE_DIR}/prebuilts/vendor_dlkm_unpack"
    local vdlkm_img
    local modules_dir
    local config_file="${image_tools_dir}/CONFIGS/vendor_dlkm_unpack.conf"

    wget -q --show-progress \
        -O "${vdlkm_zip}" "${vdlkm_url}"
    rm -rf "${extract_dir}"
    mkdir -p "${extract_dir}"
    unzip -q -o "${vdlkm_zip}" -d "${extract_dir}"

    vdlkm_img="$({ find "${extract_dir}" -type f -name 'vendor_dlkm.img' -print -quit; })"
    [[ -n "${vdlkm_img}" ]] || die "vendor_dlkm.img not found in vendor_dlkm archive"

    mkdir -p "${image_tools_dir}/INPUT_IMAGES" "${image_tools_dir}/CONFIGS"
    cp "${vdlkm_img}" \
        "${image_tools_dir}/INPUT_IMAGES/vendor_dlkm.img"
    printf '%s\n' \
        'ACTION=unpack' \
        'INPUT_IMAGE=vendor_dlkm.img' \
        'EXTRACT_DIR=extracted_vendor_dlkm' \
        > "${config_file}"

    (
        cd "${image_tools_dir}"
        run_privileged ./android_image_tools.sh \
            --conf="${config_file}" --quiet
    )
    if (( EUID != 0 )); then
        sudo chown -R "$(id -u):$(id -g)" "${image_tools_dir}"
    fi

    modules_dir="${image_tools_dir}/EXTRACTED_IMAGES/extracted_vendor_dlkm/lib/modules"
    write_module_metadata \
        "${modules_dir}" \
        "${SOURCE_DIR}/prebuilts/LKM_Tools/vendor_dlkm"
}

build_vendor_boot() {
    SCRIPT_DIR="${SCRIPT_DIR}" \
        DIST_DIR="${DIST_DIR}" \
        OUT_DIR="${OUT_DIR}" \
        "${SCRIPT_DIR}/prebuilts/build_vendor_boot.sh"
}

build_vendor_dlkm() {
    SCRIPT_DIR="${SCRIPT_DIR}" \
        DIST_DIR="${DIST_DIR}" \
        OUT_DIR="${OUT_DIR}" \
        "${SCRIPT_DIR}/prebuilts/build_vendor_dlkm.sh"
}

collect_packaged_images() {
    local package_dir="${OUT_DIR}/packaged"
    local boot_image="${DIST_DIR}/boot.img"

    [[ -f "${boot_image}" ]] || die "built boot.img not found: ${boot_image}"
    [[ -f "${SOURCE_DIR}/vendor_boot.img" ]] || die "rebuilt vendor_boot.img not found"
    [[ -f "${SOURCE_DIR}/vendor_dlkm.img" ]] || die "rebuilt vendor_dlkm.img not found"

    mkdir -p "${package_dir}"
    cp "${boot_image}" "${package_dir}/boot.img"
    cp "${SOURCE_DIR}/vendor_boot.img" "${package_dir}/vendor_boot.img"
    cp "${SOURCE_DIR}/vendor_dlkm.img" "${package_dir}/vendor_dlkm.img"
    cp "${SOURCE_DIR}/vendor_boot.img" "${DIST_DIR}/vendor_boot.img"
    cp "${SOURCE_DIR}/vendor_dlkm.img" "${DIST_DIR}/vendor_dlkm.img"
}

cleanup_packaging_workspace() {
    rm -rf \
        "${SOURCE_DIR}/prebuilts/LKM_Tools" \
        "${SOURCE_DIR}/prebuilts/vendor_boot_unpack" \
        "${SOURCE_DIR}/prebuilts/vendor_dlkm_unpack" \
        "${SOURCE_DIR}/.stock_vendor_boot" \
        "${SOURCE_DIR}/.stock_vendor_dlkm"
    rm -f \
        "${SOURCE_DIR}/.stock_vendor_boot.tar" \
        "${SOURCE_DIR}/.stock_vendor_dlkm.zip" \
        "${SOURCE_DIR}/vendor_boot.stock.img" \
        "${SOURCE_DIR}/vendor_dlkm.stock.img" \
        "${SOURCE_DIR}/vendor_boot.img" \
        "${SOURCE_DIR}/vendor_dlkm.img"
}

main() {
    local mode="${1:-}"
    case "${mode}" in
        common)
            [[ $# -eq 1 ]] || die "common mode does not accept a BUILD_TARGET"
            update_submodules
            [[ -d "${KERNEL_PLATFORM}/common" ]] || \
                die "kernel source not found: ${KERNEL_PLATFORM}"
            import_kernelsu_next
            prepare_toolchain
            build_common
            ;;
        full)
            [[ $# -eq 1 ]] || die "full mode is fixed to dm3q_kor_singlex"
            update_submodules
            [[ -d "${KERNEL_PLATFORM}/common" ]] || \
                die "kernel source not found: ${KERNEL_PLATFORM}"
            import_kernelsu_next
            prepare_toolchain
            build_full
            prepare_packaging_tools
            unpack_vendor_boot
            unpack_vendor_dlkm
            build_vendor_boot
            build_vendor_dlkm
            collect_packaged_images
            cleanup_packaging_workspace
            ;;
        -h|--help|help)
            usage
            ;;
        *)
            usage >&2
            exit 2
            ;;
    esac
}

main "$@"

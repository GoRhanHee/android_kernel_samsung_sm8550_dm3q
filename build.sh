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
${ANDROID_BUILD_TOP}/out/vendor/qcom/opensource/securemsm-kernel/Module.symvers"

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
        ../vendor/nxp/opensource/driver \
        ../vendor/st/opensource/driver \
        ../vendor/st/opensource/eSE-driver \
        ../vendor/qcom/opensource/wlan/qcacld-3.0 \
    "  

    echo "[full] BUILD_TARGET=${BUILD_TARGET}"
    echo "[full] OUT_DIR=${OUT_DIR}"

    (
        cd "${SOURCE_DIR}"
        RECOMPILE_KERNEL=1 \
            ./kernel_platform/build/android/prepare_vendor.sh sec "${TARGET_PRODUCT}"
    )

    echo "[full] Artifacts: ${OUT_DIR}/dist"
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

#!/usr/bin/env bash

set -Eeuo pipefail

readonly SCRIPT_NAME="$(basename "$0")"
readonly SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly SOURCE_DIR="${SOURCE_DIR:-${SCRIPT_DIR}}"
readonly KERNEL_PLATFORM="${SOURCE_DIR}/kernel_platform"
readonly TOOLCHAIN_URL="${TOOLCHAIN_URL:-https://github.com/GoRhanHee/samsung_sm8550_toolchain/releases/download/toolchain/toolchain.tar.xz}"
readonly CLANG_BIN="${KERNEL_PLATFORM}/prebuilts/clang/host/linux-x86/clang-r450784e/bin/clang"
readonly JOBS="${JOBS:-$(nproc)}"

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
EOF
}

die() {
    echo "error: $*" >&2
    exit 1
}

require_command() {
    command -v "$1" >/dev/null 2>&1 || die "required command not found: $1"
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
    export TARGET_PRODUCT="gki"
    export TARGET_BOARD_PLATFORM="gki"
    export ANDROID_PRODUCT_OUT="${ANDROID_BUILD_TOP}/out/target/product/${MODEL}"
    export OUT_DIR="${FULL_OUT_DIR:-${ANDROID_BUILD_TOP}/out/msm-kalama-kalama-${TARGET_PRODUCT}}"

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
../vendor/qcom/opensource/camera-kernel"

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
            [[ -d "${KERNEL_PLATFORM}/common" ]] || \
                die "kernel source not found: ${KERNEL_PLATFORM}"
            prepare_toolchain
            build_common
            ;;
        full)
            [[ $# -eq 1 ]] || die "full mode is fixed to dm3q_kor_singlex"
            [[ -d "${KERNEL_PLATFORM}/common" ]] || \
                die "kernel source not found: ${KERNEL_PLATFORM}"
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

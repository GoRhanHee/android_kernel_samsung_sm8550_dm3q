#!/usr/bin/env bash

set -Eeuo pipefail

readonly SCRIPT_NAME="$(basename "$0")"
readonly SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly TEMPLATE_DIR="${SCRIPT_DIR}/flashable"
STAGE_DIR=""

usage() {
    cat <<EOF
Usage:
  ${SCRIPT_NAME} OUTPUT_ZIP IMAGE_DIR

IMAGE_DIR must contain:
  boot.img
  vendor_boot.img
  vendor_dlkm.img

Example:
  ${SCRIPT_NAME} out/dm3q-kernel-flashable.zip out/msm-kalama-kalama-gki/packaged
EOF
}

die() {
    echo "error: $*" >&2
    exit 1
}

require_command() {
    command -v "$1" >/dev/null 2>&1 ||
        die "required command not found: $1"
}

cleanup() {
    if [[ -n "${STAGE_DIR}" && -d "${STAGE_DIR}" ]]; then
        rm -rf "${STAGE_DIR}"
    fi
}

main() {
    [[ $# -eq 2 ]] || {
        usage >&2
        exit 2
    }

    local output_zip="$1"
    local image_dir="$2"
    local output_dir
    local output_name
    local stage_dir
    local archive
    local image

    require_command sha256sum
    require_command unzip
    require_command zip

    [[ -d "${TEMPLATE_DIR}/META-INF" ]] ||
        die "flashable template not found: ${TEMPLATE_DIR}"
    [[ -d "${image_dir}" ]] || die "image directory not found: ${image_dir}"

    if [[ "${output_zip}" != /* ]]; then
        output_zip="${PWD}/${output_zip}"
    fi
    if [[ "${image_dir}" != /* ]]; then
        image_dir="${PWD}/${image_dir}"
    fi

    output_dir="$(dirname "${output_zip}")"
    output_name="$(basename "${output_zip}")"
    mkdir -p "${output_dir}"

    STAGE_DIR="$(mktemp -d "${TMPDIR:-/tmp}/dm3q-flashable.XXXXXX")"
    stage_dir="${STAGE_DIR}"
    trap cleanup EXIT

    mkdir -p "${stage_dir}/files"
    cp -a "${TEMPLATE_DIR}/META-INF" "${stage_dir}/"

    for image in boot.img vendor_boot.img vendor_dlkm.img; do
        [[ -s "${image_dir}/${image}" ]] ||
            die "required image is missing or empty: ${image_dir}/${image}"
        cp "${image_dir}/${image}" "${stage_dir}/files/${image}"
        chmod 0644 "${stage_dir}/files/${image}"
        printf '%s %s\n' \
            "${image}" \
            "$(wc -c < "${image_dir}/${image}")" \
            >> "${stage_dir}/META-INF/com/google/android/image-sizes"
    done

    (
        cd "${stage_dir}"
        sha256sum \
            files/boot.img \
            files/vendor_boot.img \
            files/vendor_dlkm.img \
            > SHA256SUMS
        zip -0 -q -r "${output_name}" META-INF files SHA256SUMS
    )

    archive="${stage_dir}/${output_name}"
    unzip -tq "${archive}" >/dev/null
    mv -f "${archive}" "${output_zip}"

    echo "Created ${output_zip}"
    (
        cd "${output_dir}"
        sha256sum "$(basename "${output_zip}")"
    )
}

main "$@"

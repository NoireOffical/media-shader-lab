#!/usr/bin/env bash

set -u

script_directory="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_directory="$(cd "${script_directory}/.." && pwd)"
local_bin="${project_directory}/.tools/env/bin"
local_env="${project_directory}/.tools/env"
vmaf_prefix="${project_directory}/.tools/vmaf"
ffmpeg_vmaf_prefix="${project_directory}/.tools/ffmpeg-vmaf"

if [[ -x "${local_bin}/cmake" ]]; then
    export PATH="${local_bin}:${PATH}"
    echo "Using project-local tools from ${local_bin}"
fi
if [[ -x "${ffmpeg_vmaf_prefix}/bin/ffmpeg" ]]; then
    export PATH="${ffmpeg_vmaf_prefix}/bin:${PATH}"
    echo "Using libvmaf-enabled FFmpeg from ${ffmpeg_vmaf_prefix}"
fi
export PKG_CONFIG_PATH="${ffmpeg_vmaf_prefix}/lib/pkgconfig:${vmaf_prefix}/lib/pkgconfig:${local_env}/lib/pkgconfig:${PKG_CONFIG_PATH:-}"

missing=0
for tool in cmake pkg-config ffmpeg; do
    if command -v "$tool" >/dev/null 2>&1; then
        echo "[ok] $tool"
    else
        echo "[missing] $tool"
        missing=1
    fi
done

if command -v pkg-config >/dev/null 2>&1; then
    for package in libavformat libavcodec libavutil libavfilter libswscale libvmaf glfw3; do
        if pkg-config --exists "$package"; then
            echo "[ok] $package $(pkg-config --modversion "$package")"
        else
            echo "[missing] $package"
            missing=1
        fi
    done
fi

if command -v ffmpeg >/dev/null 2>&1; then
    if ffmpeg -hide_banner -encoders 2>/dev/null | grep -q 'libx265'; then
        echo "[ok] libx265 H.265 software encoder"
    else
        echo "[missing] libx265 H.265 software encoder"
        missing=1
    fi
    if ffmpeg -hide_banner -encoders 2>/dev/null | grep -q 'hevc_videotoolbox'; then
        echo "[ok] hevc_videotoolbox hardware encoder"
    else
        echo "[optional] hevc_videotoolbox is unavailable on this platform"
    fi
    if ffmpeg -hide_banner -filters 2>/dev/null | grep -q 'libvmaf'; then
        echo "[ok] libvmaf quality filter"
    else
        echo "[missing] libvmaf quality filter"
        missing=1
    fi
    if ffmpeg -hide_banner -hwaccels 2>/dev/null | grep -q 'videotoolbox'; then
        echo "[ok] VideoToolbox hardware decode API"
    else
        echo "[optional] VideoToolbox hardware decoding is unavailable"
    fi
fi

if [[ $missing -ne 0 ]]; then
    echo "Run scripts/build_ffmpeg_with_vmaf.sh, then install the remaining macOS build dependencies."
    exit 1
fi

echo "All build dependencies are available."

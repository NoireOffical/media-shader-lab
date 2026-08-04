#!/usr/bin/env bash

set -euo pipefail

script_directory="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_directory="$(cd "${script_directory}/.." && pwd)"
tools_directory="${project_directory}/.tools"
environment_prefix="${tools_directory}/env"
source_directory="${tools_directory}/src"
build_directory="${tools_directory}/build"
vmaf_source="${source_directory}/vmaf-3.2.0"
vmaf_build="${build_directory}/vmaf"
vmaf_prefix="${tools_directory}/vmaf"
ffmpeg_source="${source_directory}/ffmpeg-8.1.2"
ffmpeg_build="${build_directory}/ffmpeg-vmaf"
ffmpeg_prefix="${tools_directory}/ffmpeg-vmaf"
ffmpeg_archive="${source_directory}/ffmpeg-8.1.2.tar.gz"
ffmpeg_url="https://ffmpeg.org/releases/ffmpeg-8.1.2.tar.gz"
ffmpeg_sha256="32faba5ef67340d54724941eae1425580791195312a4fd13bf6f820a2818bf22"
micromamba="${tools_directory}/bin/micromamba"
jobs="${MEDIA_BUILD_JOBS:-8}"

mkdir -p "${source_directory}" "${build_directory}"

if [[ ! -x "${environment_prefix}/bin/meson" ||
      ! -x "${environment_prefix}/bin/nasm" ]]; then
    if [[ ! -x "${micromamba}" ]]; then
        echo "Missing ${micromamba}; create the project environment first."
        exit 1
    fi
    "${micromamba}" install --yes --prefix "${environment_prefix}" \
        --channel conda-forge meson nasm ninja pkg-config
fi

export PATH="${environment_prefix}/bin:${PATH}"

if [[ ! -d "${vmaf_source}/.git" ]]; then
    git clone --depth 1 --branch v3.2.0 \
        https://github.com/Netflix/vmaf.git "${vmaf_source}"
fi

if [[ -f "${vmaf_build}/build.ninja" ]]; then
    meson setup --reconfigure "${vmaf_build}" "${vmaf_source}/libvmaf" \
        --prefix "${vmaf_prefix}" --buildtype release \
        -Denable_docs=false -Denable_tests=true -Dbuilt_in_models=true
else
    meson setup "${vmaf_build}" "${vmaf_source}/libvmaf" \
        --prefix "${vmaf_prefix}" --buildtype release \
        -Denable_docs=false -Denable_tests=true -Dbuilt_in_models=true
fi
meson compile -C "${vmaf_build}" -j "${jobs}"
meson test -C "${vmaf_build}" --print-errorlogs
meson install -C "${vmaf_build}"

if [[ ! -f "${ffmpeg_archive}" ]]; then
    curl --fail --location "${ffmpeg_url}" --output "${ffmpeg_archive}"
fi
actual_sha256="$(shasum -a 256 "${ffmpeg_archive}" | awk '{print $1}')"
if [[ "${actual_sha256}" != "${ffmpeg_sha256}" ]]; then
    echo "FFmpeg archive checksum mismatch: ${actual_sha256}"
    exit 1
fi
if [[ ! -x "${ffmpeg_source}/configure" ]]; then
    tar -xzf "${ffmpeg_archive}" -C "${source_directory}"
fi

mkdir -p "${ffmpeg_build}" "${ffmpeg_prefix}"
export PKG_CONFIG_PATH="${vmaf_prefix}/lib/pkgconfig:${environment_prefix}/lib/pkgconfig"

cd "${ffmpeg_build}"
"${ffmpeg_source}/configure" \
    --prefix="${ffmpeg_prefix}" \
    --pkg-config="${environment_prefix}/bin/pkg-config" \
    --disable-doc \
    --disable-debug \
    --disable-static \
    --enable-shared \
    --enable-pic \
    --enable-pthreads \
    --enable-openssl \
    --enable-gpl \
    --enable-version3 \
    --enable-libx264 \
    --enable-libx265 \
    --enable-libvmaf \
    --disable-ffplay \
    --enable-rpath \
    --extra-cflags="-I${vmaf_prefix}/include -I${environment_prefix}/include" \
    --extra-ldflags="-L${vmaf_prefix}/lib -L${environment_prefix}/lib -Wl,-rpath,${vmaf_prefix}/lib -Wl,-rpath,${environment_prefix}/lib"
make -j "${jobs}"
make install

"${ffmpeg_prefix}/bin/ffmpeg" -hide_banner -filters | grep libvmaf
echo "Installed libvmaf-enabled FFmpeg at ${ffmpeg_prefix}/bin/ffmpeg"

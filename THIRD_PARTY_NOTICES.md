# Third-party notices

## Dear ImGui

- Project: [Dear ImGui](https://github.com/ocornut/imgui)
- Version: 1.92.8
- Copyright: Omar Cornut and Dear ImGui contributors
- License: MIT
- Vendored source: `third_party/imgui/`
- Full license text: `third_party/imgui/LICENSE.txt`

The GLFW and OpenGL3 platform/renderer backends used by Media Shader Lab are distributed with the Dear ImGui source tree under the same license.

Other build and runtime dependencies are resolved externally through Homebrew or conda-forge and are not redistributed as part of the repository source.

## libvmaf

- Project: [Netflix VMAF](https://github.com/Netflix/vmaf)
- Version used by the reproducible build script: 3.2.0
- Copyright: Netflix, Inc.
- License: BSD+Patent

## FFmpeg

- Project: [FFmpeg](https://ffmpeg.org/)
- Version used by the reproducible build script: 8.1.2
- License: LGPL/GPL depending on build configuration

The local build produced by `scripts/build_ffmpeg_with_vmaf.sh` enables GPL and
version-3 components together with libx264/libx265, so that binary is GPLv3-or-later.
The generated sources and binaries live under the Git-ignored `.tools/` directory
and are not distributed as part of this repository's MIT-licensed source.

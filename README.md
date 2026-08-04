# Media Shader Lab

[![CI](https://github.com/NoireOffical/media-shader-lab/actions/workflows/ci.yml/badge.svg)](https://github.com/NoireOffical/media-shader-lab/actions/workflows/ci.yml)

[简体中文](README.zh-CN.md) | English

A small end-to-end video pipeline built with C++17, FFmpeg, OpenGL, GLSL, and Dear ImGui. It decodes compressed video, applies interactive GPU effects, exports the processed framebuffer as H.265/MP4, preserves source audio, and measures playback, encoding, and compression quality.

This repository is designed as a portfolio project for multimedia, graphics, and media-infrastructure roles. The MVP deliberately keeps the data path visible instead of hiding it behind a media framework.

If the multimedia dependencies are not installed yet, the standalone metrics module can still be compiled with the Apple/LLVM toolchain:

```bash
make test-core
```

## What it demonstrates

- FFmpeg software decoding and selectable VideoToolbox hardware decoding with correct packet/frame back-pressure, seeking, and flushing
- Zero-copy VideoToolbox rendering: retained NV12 `CVPixelBuffer`/IOSurface planes are bound directly to OpenGL rectangle textures and converted in GLSL
- Pixel-format conversion through `libswscale` as the portable software and unsupported-surface fallback
- OpenGL texture upload and a GLSL full-screen rendering pipeline
- Interactive grayscale, sepia, edge-detection, and vignette filters
- Dear ImGui controls for file loading, drag and drop, play/pause, seeking, and shader parameters
- Source/processed split comparison and a live pipeline-latency graph
- Exact-resolution offscreen framebuffer readback for processed-frame export
- Double-buffered PBO upload/readback with one-frame pipelining and tail-frame flushing
- OpenGL Timer Query profiling for exact Shader GPU time plus transfer submission/wait telemetry
- `libx265` software and VideoToolbox hardware H.265 encoding
- MP4 muxing with optional source-audio stream copy and deterministic timestamps
- Post-encode PSNR, 8x8 luma SSIM, and VMAF evaluation against the pre-encode Shader output
- Headless decode benchmarking and JSON metrics export
- A reproducible benchmark harness that records device/build/input metadata and compares software, hardware-copy, zero-copy, synchronous, and PBO paths in JSON/CSV
- Unit and integration tests for metrics, seeking, H.265 encoding, flushing, and decode round trips
- GitHub Actions builds/tests on Linux and macOS, plus ASan/UBSan and CodeQL scanning

## Visual result

Interactive playback and performance dashboard:

![Dear ImGui video control panel](docs/assets/control-panel.jpg)

The following comparison was captured from the application's OpenGL framebuffer rather than recreated in an image editor:

![OpenGL shader filter comparison](docs/assets/filter-comparison.png)

## Architecture

```text
Compressed video
      │
      ▼
FFmpeg demuxer → software / VideoToolbox decoder
                 ├── libswscale → RGB24 → PBO / synchronous upload ─┐
                 ├── retained CVPixelBuffer / IOSurface (NV12) ─────┤
                 └── headless benchmark → metrics.json              │
                                                                    ▼
                                                           OpenGL textures
                                  │
                                  ▼
                    GLSL filter → window display
                                  │
                                  ├── exact-size offscreen framebuffer
                                  │              │ RGB24
                                  │              ▼
                                  │       libswscale → YUV420P
                                  │              ▼
                                  │       libx265 / VideoToolbox
                                  │              ▼
                                  │     HEVC + source audio → MP4
                                  │              ▼
                                  │       decode round trip → PSNR/SSIM/VMAF
                                  ▼
                       decode/render latency report
```

## Build on macOS

Install the project dependencies:

```bash
brew install cmake pkg-config ffmpeg glfw
```

Alternatively, create a project-local environment without administrator access:

```bash
micromamba create --yes --prefix .tools/env --file environment.yml
bash scripts/build_ffmpeg_with_vmaf.sh
```

The script builds pinned libvmaf 3.2.0 and FFmpeg 8.1.2 with `--enable-libvmaf`, `libx264`, and `libx265` under `.tools/`. Then build and test:

```bash
make build
make test
```

You can check dependencies before building:

```bash
bash scripts/check_dependencies.sh
```

## Run

The repository includes a 20-second, 720p/30 FPS H.264/AAC clip derived from Blender Foundation's open movie Big Buck Bunny. See the [source, CC BY 3.0 attribution, checksums, and reproduction command](assets/videos/README.md).

![Big Buck Bunny test clip preview](docs/assets/test-video-preview.jpg)

Generate a short H.264 test clip if you do not have an input file:

```bash
ffmpeg -f lavfi -i testsrc2=size=1280x720:rate=30 -t 8 -c:v libx264 sample.mp4
```

Open the interactive renderer:

```bash
./build/media_shader_lab \
  --input assets/videos/big_buck_bunny_720p_20s.mp4 \
  --filter edge
```

Runtime keys:

- `0`: original
- `1`: grayscale
- `2`: sepia
- `3`: edge detection
- `4`: animated vignette
- `Space`: play/pause
- `Esc`: exit

Run a headless decode benchmark and save machine-readable results:

```bash
./build/media_shader_lab \
  --input sample.mp4 \
  --headless \
  --no-sync \
  --max-frames 240 \
  --metrics-output metrics.json
```

Render one filtered frame to a PPM image without showing a window:

```bash
./build/media_shader_lab \
  --input sample.mp4 \
  --filter edge \
  --snapshot edge.ppm \
  --no-sync
```

Use `--ui-snapshot dashboard.ppm --snapshot-frame 45` to capture the control panel after its live metrics history has been populated.

Export the processed frames as H.265, copy the source audio, and write a quality report:

```bash
./build/media_shader_lab \
  --input assets/videos/big_buck_bunny_720p_20s.mp4 \
  --filter edge \
  --output edge-hevc.mp4 \
  --encoder libx265 \
  --preset medium \
  --crf 24 \
  --quality-output edge-hevc.quality.json \
  --no-sync
```

Use `--encoder hevc_videotoolbox --bitrate-kbps 6000` for macOS hardware encoding. Use `--no-audio` for a video-only output. The same controls are available in the interactive dashboard.

Use `--decoder videotoolbox` to select macOS hardware decoding. Eligible NV12 frames use the IOSurface zero-copy path automatically; `--no-zero-copy` forces the CPU-transfer baseline. Use `--no-pbo` to run the synchronous upload/readback baseline. The dashboard reports upload submission and exact GPU Shader time during preview. Readback submission, readback GPU Timer Query (when the driver exposes a non-zero result), and PBO map-wait are sampled only by export/quality paths; the dashboard shows `N/A` for unavailable samples instead of presenting them as zero.

## Reproducible benchmark

After a release build, run the standard 300-frame, three-repeat benchmark:

```bash
python3 scripts/benchmark_pipeline.py
```

It compares software decoding, VideoToolbox CPU transfer, VideoToolbox IOSurface zero-copy, synchronous OpenGL transfer, and PBO transfer. Results are written under `build/benchmarks/<timestamp>/benchmark.json` and `benchmark.csv`, together with per-run logs and outputs. The report records the Git commit, dirty-tree state, machine/OS, input SHA-256, frame count, filter, encoder, and individual commands. Use `--frames 60 --repeats 1` for a quick smoke run, `--decode-only` on a machine without a display, and `--quality` to add PSNR/SSIM/VMAF reports.

## Continuous integration

Every push and pull request builds and tests the project on Linux and macOS. A separate Linux job runs AddressSanitizer and UndefinedBehaviorSanitizer, while CodeQL scans the compiled C++ path. Dependabot checks GitHub Actions versions weekly.

## Metrics

The live dashboard shows playback FPS over the most recent one-second window, average decode latency, average render latency, and p95 pipeline latency. The rolling FPS window resets after pause, seek, source reload, and export so offline H.265/VMAF work does not distort playback performance. Export output adds encoding FPS, encoded bytes, and optional post-encode PSNR/SSIM/VMAF. The quality reference is the exact Shader framebuffer before RGB-to-YUV conversion and HEVC compression; VMAF uses libvmaf's built-in `vmaf_v0.6.1` model.

## Roadmap

1. Add CPU/GPU utilization, power, and memory telemetry around the existing Timer Query measurements.
2. Add an optional ONNX Runtime stage for segmentation or super-resolution.
3. Port the renderer to Vulkan and compare the backends with the existing benchmark harness.

## Resume-ready description

> Built an end-to-end C++17 video pipeline with FFmpeg and OpenGL/GLSL, including direct VideoToolbox CVPixelBuffer/IOSurface-to-GPU rendering with an automatic CPU fallback, double-buffered PBO transfer, Timer Query profiling, H.265 software/hardware encoding, audio remuxing, and reproducible latency, throughput, PSNR, SSIM, and VMAF evaluation; added Linux/macOS CI, sanitizers, and CodeQL.

The interactive dashboard uses vendored Dear ImGui 1.92.8 under its MIT License; see [third-party notices](THIRD_PARTY_NOTICES.md).

Do not add performance numbers to a resume until they have been reproduced on a named device, resolution, codec, and test clip.

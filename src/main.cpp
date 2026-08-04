#include "medialab/GLRenderer.hpp"
#include "medialab/Metrics.hpp"
#include "medialab/QualityMetrics.hpp"
#include "medialab/VideoDecoder.hpp"
#include "medialab/VideoEncoder.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

struct Options {
    std::string input;
    std::string shader_directory = MEDIA_SHADER_DIR;
    std::string metrics_output;
    std::string snapshot_output;
    std::string output;
    std::string encoder = "libx265";
    std::string preset = "medium";
    std::string quality_output;
    int crf = 24;
    int bitrate_kbps = 6000;
    int gop_size = 0;
    bool copy_audio = true;
    bool include_controls_in_snapshot = false;
    std::size_t snapshot_frame = 1;
    medialab::FilterMode filter = medialab::FilterMode::Original;
    medialab::DecoderBackend decoder_backend =
        medialab::DecoderBackend::Software;
    std::size_t max_frames = 0;
    bool headless = false;
    bool sync_to_pts = true;
    bool asynchronous_pbo = true;
    bool zero_copy = true;
};

void print_help() {
    std::cout
        << "Media Shader Lab\n\n"
        << "Usage: media_shader_lab --input VIDEO [options]\n\n"
        << "Options:\n"
        << "  --filter NAME        original, grayscale, sepia, edge, vignette\n"
        << "  --decoder NAME       software or videotoolbox (default software)\n"
        << "  --headless           decode and benchmark without opening a window\n"
        << "  --max-frames N       stop after N decoded frames\n"
        << "  --no-sync            process as fast as possible instead of following PTS\n"
        << "  --no-pbo             use synchronous texture upload/readback\n"
        << "  --no-zero-copy       transfer VideoToolbox frames through CPU memory\n"
        << "  --metrics-output P   write the final metrics report as JSON\n"
        << "  --snapshot P         render one frame to a binary PPM image\n"
        << "  --ui-snapshot P      render one frame including the control panel\n"
        << "  --snapshot-frame N   capture frame N instead of the first frame\n"
        << "  --output P           export Shader output as H.265 in MP4\n"
        << "  --encoder NAME       libx265 or hevc_videotoolbox\n"
        << "  --crf N              libx265 constant-quality value, 0-51 (default 24)\n"
        << "  --preset NAME        libx265 preset (default medium)\n"
        << "  --bitrate-kbps N     hardware encoder bitrate (default 6000)\n"
        << "  --gop N              keyframe interval; 0 selects two seconds\n"
        << "  --no-audio           do not copy the source audio stream\n"
        << "  --quality-output P   evaluate output and write PSNR/SSIM/VMAF JSON\n"
        << "  --shader-dir P       directory containing video.vert and video.frag\n"
        << "  --help               show this message\n\n"
        << "Runtime keys: 0 original, 1 grayscale, 2 sepia, 3 edge, 4 vignette, Esc exit\n";
}

std::size_t parse_count(const std::string& value) {
    if (value.empty() || value.front() == '-') {
        throw std::invalid_argument("invalid frame count: " + value);
    }
    std::size_t consumed = 0;
    const auto result = std::stoull(value, &consumed);
    if (consumed != value.size()) {
        throw std::invalid_argument("invalid frame count: " + value);
    }
    return static_cast<std::size_t>(result);
}

int parse_integer(const std::string& value, const std::string& option) {
    std::size_t consumed = 0;
    const long result = std::stol(value, &consumed);
    if (consumed != value.size() || result < std::numeric_limits<int>::min() ||
        result > std::numeric_limits<int>::max()) {
        throw std::invalid_argument("invalid value for " + option + ": " + value);
    }
    return static_cast<int>(result);
}

Options parse_options(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        auto next_value = [&]() -> std::string {
            if (i + 1 >= argc) {
                throw std::invalid_argument("missing value after " + argument);
            }
            return argv[++i];
        };

        if (argument == "--input") {
            options.input = next_value();
        } else if (argument == "--filter") {
            options.filter = medialab::parse_filter(next_value());
        } else if (argument == "--decoder") {
            options.decoder_backend =
                medialab::parse_decoder_backend(next_value());
        } else if (argument == "--headless") {
            options.headless = true;
        } else if (argument == "--max-frames") {
            options.max_frames = parse_count(next_value());
        } else if (argument == "--no-sync") {
            options.sync_to_pts = false;
        } else if (argument == "--no-pbo") {
            options.asynchronous_pbo = false;
        } else if (argument == "--no-zero-copy") {
            options.zero_copy = false;
        } else if (argument == "--metrics-output") {
            options.metrics_output = next_value();
        } else if (argument == "--snapshot") {
            options.snapshot_output = next_value();
        } else if (argument == "--ui-snapshot") {
            options.snapshot_output = next_value();
            options.include_controls_in_snapshot = true;
        } else if (argument == "--snapshot-frame") {
            options.snapshot_frame = parse_count(next_value());
            if (options.snapshot_frame == 0) {
                throw std::invalid_argument("--snapshot-frame must be greater than zero");
            }
        } else if (argument == "--output") {
            options.output = next_value();
        } else if (argument == "--encoder") {
            options.encoder = next_value();
            if (options.encoder != "libx265" &&
                options.encoder != "hevc_videotoolbox") {
                throw std::invalid_argument(
                    "--encoder must be libx265 or hevc_videotoolbox");
            }
        } else if (argument == "--crf") {
            options.crf = parse_integer(next_value(), argument);
            if (options.crf < 0 || options.crf > 51) {
                throw std::invalid_argument("--crf must be between 0 and 51");
            }
        } else if (argument == "--preset") {
            options.preset = next_value();
        } else if (argument == "--bitrate-kbps") {
            options.bitrate_kbps = parse_integer(next_value(), argument);
            if (options.bitrate_kbps <= 0) {
                throw std::invalid_argument("--bitrate-kbps must be positive");
            }
        } else if (argument == "--gop") {
            options.gop_size = parse_integer(next_value(), argument);
            if (options.gop_size < 0) {
                throw std::invalid_argument("--gop cannot be negative");
            }
        } else if (argument == "--no-audio") {
            options.copy_audio = false;
        } else if (argument == "--quality-output") {
            options.quality_output = next_value();
        } else if (argument == "--shader-dir") {
            options.shader_directory = next_value();
        } else if (argument == "--help" || argument == "-h") {
            print_help();
            std::exit(0);
        } else {
            throw std::invalid_argument("unknown argument: " + argument);
        }
    }
    if (options.input.empty()) {
        throw std::invalid_argument("--input is required");
    }
    if (!options.quality_output.empty() && options.output.empty()) {
        throw std::invalid_argument("--quality-output requires --output");
    }
    return options;
}

double milliseconds(std::chrono::steady_clock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

void print_input_info(const std::string& path, const medialab::VideoDecoder& decoder) {
    std::cout << "input=" << path
              << " codec=" << decoder.codec_name()
              << " decoder=" << decoder.backend_description()
              << " size=" << decoder.width() << 'x' << decoder.height()
              << " fps=" << decoder.fps()
              << " duration_s=" << decoder.duration_seconds() << '\n';
}

void save_metrics(const Options& options,
                  const medialab::MetricsCollector& metrics,
                  double elapsed_seconds) {
    std::cout << medialab::format_summary(metrics.summarize(elapsed_seconds)) << '\n';
    if (options.metrics_output.empty()) {
        return;
    }
    std::ofstream output(options.metrics_output);
    if (!output) {
        throw std::runtime_error("could not open metrics output: " +
                                 options.metrics_output);
    }
    output << metrics.to_json(elapsed_seconds) << '\n';
}

struct ExportParameters {
    std::string output;
    std::string encoder = "libx265";
    std::string preset = "medium";
    std::string quality_output;
    int crf = 24;
    int bitrate_kbps = 6000;
    int gop_size = 0;
    bool copy_audio = true;
    std::size_t max_frames = 0;
    medialab::DecoderBackend decoder_backend =
        medialab::DecoderBackend::Software;
    bool zero_copy = true;
};

struct ExportResult {
    medialab::EncoderStats encoder;
    medialab::QualitySummary quality;
    medialab::GpuPerformanceSummary gpu;
    bool quality_evaluated = false;
    bool cancelled = false;
    bool decoder_zero_copy = false;
};

medialab::QualitySummary evaluate_quality(
    const std::string& input_path,
    const std::string& encoded_path,
    const std::string& report_path,
    std::size_t max_frames,
    medialab::GLRenderer& renderer,
    const medialab::InteractiveState& filter_state) {
    medialab::VideoDecoder source_decoder(input_path);
    medialab::VideoDecoder encoded_decoder(encoded_path);
    medialab::VideoFrame source;
    medialab::VideoFrame reference;
    medialab::VideoFrame compared;
    medialab::QualityMetricsCollector quality;
    medialab::VmafCalculator vmaf;
    std::deque<medialab::VideoFrame> compared_frames;
    std::size_t frames = 0;
    const auto record_pair = [&](const medialab::VideoFrame& reference_frame,
                                 const medialab::VideoFrame& compared_frame) {
        if (reference_frame.width != compared_frame.width ||
            reference_frame.height != compared_frame.height) {
            throw std::runtime_error(
                "encoded output dimensions changed during quality evaluation");
        }
        quality.record(reference_frame.rgb,
                       compared_frame.rgb,
                       reference_frame.width,
                       reference_frame.height);
        if (vmaf.available()) {
            vmaf.record(reference_frame.rgb,
                        compared_frame.rgb,
                        reference_frame.width,
                        reference_frame.height);
        }
    };
    while (source_decoder.read(source) && encoded_decoder.read(compared)) {
        compared_frames.push_back(std::move(compared));
        compared = medialab::VideoFrame{};
        if (renderer.process(source,
                             filter_state,
                             source.pts_seconds,
                             reference)) {
            if (compared_frames.empty()) {
                throw std::runtime_error(
                    "quality comparison frame queue underflow");
            }
            record_pair(reference, compared_frames.front());
            compared_frames.pop_front();
            ++frames;
        }
        if (max_frames > 0 && frames >= max_frames) {
            break;
        }
    }
    while (renderer.flush_process(reference)) {
        if ((max_frames == 0 || frames < max_frames) &&
            !compared_frames.empty()) {
            record_pair(reference, compared_frames.front());
            compared_frames.pop_front();
            ++frames;
        }
    }
    if (frames == 0) {
        throw std::runtime_error("quality evaluation decoded no frame pairs");
    }
    if (vmaf.available()) {
        quality.set_vmaf(vmaf.finish(), vmaf.model_name());
    }
    if (!report_path.empty()) {
        std::ofstream output(report_path);
        if (!output) {
            throw std::runtime_error("could not open quality report: " +
                                     report_path);
        }
        output << quality.to_json() << '\n';
    }
    return quality.summarize();
}

ExportResult export_processed_video(
    const std::string& input_path,
    const ExportParameters& parameters,
    medialab::GLRenderer& renderer,
    medialab::InteractiveState& ui_state,
    bool show_progress_ui) {
    medialab::VideoDecoder decoder(input_path,
                                    parameters.decoder_backend,
                                    parameters.zero_copy);
    medialab::EncoderConfig config;
    config.output_path = parameters.output;
    config.source_path = input_path;
    config.encoder_name = parameters.encoder;
    config.preset = parameters.preset;
    config.width = decoder.width();
    config.height = decoder.height();
    config.fps = decoder.fps() > 0.0 ? decoder.fps() : 30.0;
    config.crf = parameters.crf;
    config.bitrate_kbps = parameters.bitrate_kbps;
    config.gop_size = parameters.gop_size;
    config.copy_audio = parameters.copy_audio;

    medialab::InteractiveState filter_state = ui_state;
    filter_state.split_view = false;
    filter_state.export_requested = false;
    filter_state.export_in_progress = false;
    ui_state.export_requested = false;
    ui_state.export_cancel_requested = false;
    ui_state.export_in_progress = true;
    ui_state.export_progress = 0.0F;
    ui_state.status_message = "Exporting H.265 with " + parameters.encoder + "...";
    ui_state.readback_metrics_available = false;
    ui_state.readback_gpu_available = false;
    ui_state.pbo_wait_available = false;
    renderer.reset_profiling();

    medialab::VideoEncoder encoder(config);
    medialab::VideoFrame source;
    medialab::VideoFrame processed;
    std::size_t frame_count = 0;
    bool cancelled = false;
    while (decoder.read(source)) {
        if (renderer.process(source,
                             filter_state,
                             source.pts_seconds,
                             processed)) {
            encoder.write_rgb(processed);
        }
        ++frame_count;
        if (decoder.duration_seconds() > 0.0) {
            ui_state.export_progress = std::clamp(
                static_cast<float>(source.pts_seconds /
                                   decoder.duration_seconds()),
                0.0F,
                1.0F);
        }

        if (show_progress_ui && frame_count % 3 == 0) {
            renderer.poll_events();
            renderer.render(source,
                            ui_state,
                            source.pts_seconds);
            if (renderer.should_close() || ui_state.export_cancel_requested) {
                cancelled = true;
                break;
            }
        } else if (!show_progress_ui && frame_count % 60 == 0) {
            renderer.poll_events();
            if (decoder.duration_seconds() > 0.0) {
                const double percent = std::clamp(
                    source.pts_seconds / decoder.duration_seconds() * 100.0,
                    0.0,
                    100.0);
                std::cerr << "\rH.265 export " << frame_count
                          << " frames (" << static_cast<int>(percent)
                          << "%)" << std::flush;
            } else {
                std::cerr << "\rH.265 export " << frame_count
                          << " frames" << std::flush;
            }
        }
        if (parameters.max_frames > 0 &&
            frame_count >= parameters.max_frames) {
            break;
        }
    }

    while (renderer.flush_process(processed)) {
        encoder.write_rgb(processed);
    }

    ExportResult result;
    result.encoder = encoder.finish();
    result.gpu = renderer.profiling_summary();
    result.decoder_zero_copy = decoder.zero_copy_active();
    if (!show_progress_ui) {
        std::cerr << '\n';
    }
    result.cancelled = cancelled;
    ui_state.export_in_progress = false;
    ui_state.export_cancel_requested = false;
    ui_state.export_progress = 1.0F;

    if (!cancelled && !parameters.quality_output.empty()) {
        ui_state.status_message = "Evaluating H.265 PSNR/SSIM/VMAF...";
        if (show_progress_ui && !renderer.should_close()) {
            renderer.poll_events();
            if (source.width > 0) {
                renderer.render(source,
                                ui_state,
                                source.pts_seconds);
            }
        }
        result.quality = evaluate_quality(input_path,
                                          parameters.output,
                                          parameters.quality_output,
                                          parameters.max_frames,
                                          renderer,
                                          filter_state);
        result.quality_evaluated = true;
    }
    renderer.reset_profiling();
    ui_state.upload_submit_ms = result.gpu.upload_submit_ms;
    ui_state.gpu_shader_ms = result.gpu.shader_ms;
    ui_state.readback_submit_ms = result.gpu.readback_submit_ms;
    ui_state.readback_gpu_ms = result.gpu.readback_gpu_ms;
    ui_state.pbo_map_wait_ms = result.gpu.pbo_map_wait_ms;
    ui_state.readback_metrics_available = result.gpu.readback_sampled;
    ui_state.readback_gpu_available = result.gpu.readback_gpu_sampled;
    ui_state.readback_used_pbo = result.gpu.readback_used_pbo;
    ui_state.pbo_wait_available =
        result.gpu.pbo_wait_sampled && result.gpu.readback_used_pbo;
    return result;
}

void print_export_result(const ExportParameters& parameters,
                         const ExportResult& result) {
    std::cout << "output=" << parameters.output
              << " encoder=" << parameters.encoder
              << " frames=" << result.encoder.frames
              << " decoder_zero_copy="
              << (result.decoder_zero_copy ? "true" : "false")
              << " encoding_fps=" << result.encoder.encoding_fps()
              << " video_bytes=" << result.encoder.video_bytes
              << " upload_submit_ms=" << result.gpu.upload_submit_ms
              << " gpu_shader_ms=" << result.gpu.shader_ms
              << " readback_submit_ms=" << result.gpu.readback_submit_ms
              << " readback_gpu_ms=" << result.gpu.readback_gpu_ms
              << " readback_gpu_available="
              << (result.gpu.readback_gpu_sampled ? "true" : "false")
              << " pbo_map_wait_ms=" << result.gpu.pbo_map_wait_ms
              << " pbo_wait_available="
              << (result.gpu.pbo_wait_sampled ? "true" : "false");
    if (result.quality_evaluated) {
        std::cout << " psnr_db=" << result.quality.average_psnr_db
                  << " ssim=" << result.quality.average_ssim;
        if (result.quality.vmaf_available) {
            std::cout << " vmaf=" << result.quality.average_vmaf;
        }
        std::cout << " quality_report=" << parameters.quality_output;
    }
    if (result.cancelled) {
        std::cout << " cancelled=true";
    }
    std::cout << '\n';
}

int run_export(const Options& options) {
    medialab::VideoDecoder decoder(options.input,
                                    options.decoder_backend,
                                    options.zero_copy);
    print_input_info(options.input, decoder);
    medialab::GLRenderer renderer(decoder.width(),
                                  decoder.height(),
                                  options.shader_directory,
                                  false);
    medialab::InteractiveState state;
    state.filter = options.filter;
    state.input_path = options.input;
    state.duration_seconds = decoder.duration_seconds();
    state.asynchronous_pbo = options.asynchronous_pbo;

    ExportParameters parameters;
    parameters.output = options.output;
    parameters.encoder = options.encoder;
    parameters.preset = options.preset;
    parameters.quality_output = options.quality_output;
    parameters.crf = options.crf;
    parameters.bitrate_kbps = options.bitrate_kbps;
    parameters.gop_size = options.gop_size;
    parameters.copy_audio = options.copy_audio;
    parameters.max_frames = options.max_frames;
    parameters.decoder_backend = options.decoder_backend;
    parameters.zero_copy = options.zero_copy;
    const ExportResult result = export_processed_video(options.input,
                                                       parameters,
                                                       renderer,
                                                       state,
                                                       false);
    print_export_result(parameters, result);
    return 0;
}

int run_headless(const Options& options) {
    medialab::VideoDecoder decoder(options.input,
                                    options.decoder_backend,
                                    options.zero_copy);
    print_input_info(options.input, decoder);

    medialab::MetricsCollector metrics;
    medialab::VideoFrame frame;
    std::size_t frame_count = 0;
    const auto session_start = std::chrono::steady_clock::now();
    while (true) {
        const auto decode_start = std::chrono::steady_clock::now();
        if (!decoder.read(frame)) {
            break;
        }
        const auto decode_end = std::chrono::steady_clock::now();
        metrics.record(milliseconds(decode_end - decode_start), 0.0);
        ++frame_count;
        if (options.max_frames > 0 && frame_count >= options.max_frames) {
            break;
        }
    }

    const double elapsed_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - session_start).count();
    save_metrics(options, metrics, elapsed_seconds);
    std::cout << "decoder_zero_copy="
              << (decoder.zero_copy_active() ? "true" : "false") << '\n';
    return 0;
}

int run_interactive(const Options& options) {
    auto decoder = std::make_unique<medialab::VideoDecoder>(
        options.input, options.decoder_backend, options.zero_copy);
    print_input_info(options.input, *decoder);

    medialab::InteractiveState state;
    state.filter = options.filter;
    state.input_path = options.input;
    state.decoder_backend = options.decoder_backend ==
            medialab::DecoderBackend::VideoToolbox
        ? 1
        : 0;
    state.active_decoder = decoder->backend_description();
    state.duration_seconds = decoder->duration_seconds();
    state.export_encoder = options.encoder == "hevc_videotoolbox" ? 1 : 0;
    state.export_crf = options.crf;
    state.export_bitrate_kbps = options.bitrate_kbps;
    state.export_gop_size = options.gop_size;
    state.export_preset = options.preset;
    state.export_copy_audio = options.copy_audio;
    state.asynchronous_pbo = options.asynchronous_pbo;
    state.status_message = "Ready. Drop a video file anywhere to load it.";

    medialab::GLRenderer renderer(decoder->width(),
                                  decoder->height(),
                                  options.shader_directory,
                                  options.snapshot_output.empty());
    medialab::MetricsCollector metrics;
    medialab::RecentFrameRate playback_fps;
    medialab::VideoFrame frame;
    bool has_frame = false;
    bool reset_frame_clock = true;
    std::size_t frame_count = 0;
    auto next_frame_deadline = std::chrono::steady_clock::now();
    auto metrics_start = std::chrono::steady_clock::now();
    const auto application_start = metrics_start;

    while (!renderer.should_close()) {
        renderer.poll_events();

        const std::string dropped_path = renderer.take_dropped_path();
        if (!dropped_path.empty()) {
            state.input_path = dropped_path;
            state.load_requested = true;
        }

        if (state.export_requested) {
            playback_fps.reset();
            state.wall_fps = 0.0;
            ExportParameters parameters;
            parameters.output = state.export_path;
            parameters.encoder = state.export_encoder == 0
                ? "libx265"
                : "hevc_videotoolbox";
            parameters.preset = state.export_preset;
            parameters.crf = state.export_crf;
            parameters.bitrate_kbps = state.export_bitrate_kbps;
            parameters.gop_size = state.export_gop_size;
            parameters.copy_audio = state.export_copy_audio;
            parameters.max_frames = options.max_frames;
            parameters.decoder_backend = decoder->backend();
            parameters.zero_copy = options.zero_copy;
            if (state.export_quality_report) {
                parameters.quality_output = state.export_path + ".quality.json";
            }
            try {
                const ExportResult result = export_processed_video(
                    state.input_path,
                    parameters,
                    renderer,
                    state,
                    true);
                print_export_result(parameters, result);
                if (result.cancelled) {
                    state.status_message =
                        "Export cancelled; partial video saved: " +
                        parameters.output;
                } else if (result.quality_evaluated) {
                    state.status_message =
                        "Export complete: " + parameters.output +
                        " | PSNR " +
                        std::to_string(result.quality.average_psnr_db) +
                        " dB | SSIM " +
                        std::to_string(result.quality.average_ssim);
                    if (result.quality.vmaf_available) {
                        state.status_message +=
                            " | VMAF " +
                            std::to_string(result.quality.average_vmaf);
                    }
                } else {
                    state.status_message =
                        "Export complete: " + parameters.output;
                }
            } catch (const std::exception& error) {
                renderer.reset_profiling();
                state.export_in_progress = false;
                state.export_cancel_requested = false;
                state.export_requested = false;
                state.readback_metrics_available = false;
                state.readback_gpu_available = false;
                state.pbo_wait_available = false;
                state.status_message = std::string("Export failed: ") +
                                       error.what();
            }
            reset_frame_clock = true;
        }

        if (state.load_requested) {
            state.load_requested = false;
            try {
                const auto requested_backend = state.decoder_backend == 1
                    ? medialab::DecoderBackend::VideoToolbox
                    : medialab::DecoderBackend::Software;
                auto replacement = std::make_unique<medialab::VideoDecoder>(
                    state.input_path, requested_backend, options.zero_copy);
                print_input_info(state.input_path, *replacement);
                decoder = std::move(replacement);
                state.active_decoder = decoder->backend_description();
                state.duration_seconds = decoder->duration_seconds();
                state.progress_seconds = 0.0;
                state.paused = false;
                state.status_message = "Loaded: " + state.input_path;
                state.pipeline_history_ms.clear();
                metrics = medialab::MetricsCollector{};
                playback_fps.reset();
                state.wall_fps = 0.0;
                metrics_start = std::chrono::steady_clock::now();
                frame_count = 0;
                has_frame = false;
                reset_frame_clock = true;
            } catch (const std::exception& error) {
                state.decoder_backend = decoder->backend() ==
                        medialab::DecoderBackend::VideoToolbox
                    ? 1
                    : 0;
                state.status_message = std::string("Load failed: ") + error.what();
            }
        }

        if (state.seek_requested) {
            state.seek_requested = false;
            try {
                decoder->seek(state.seek_target_seconds);
                has_frame = false;
                reset_frame_clock = true;
                playback_fps.reset();
                state.wall_fps = 0.0;
                state.status_message = "Seeked to " +
                    std::to_string(state.seek_target_seconds) + " seconds";
            } catch (const std::exception& error) {
                state.status_message = std::string("Seek failed: ") + error.what();
            }
        }

        bool decoded_this_frame = false;
        double decode_ms = 0.0;
        if (!state.paused || !has_frame) {
            const auto decode_start = std::chrono::steady_clock::now();
            if (decoder->read(frame)) {
                const auto decode_end = std::chrono::steady_clock::now();
                decode_ms = milliseconds(decode_end - decode_start);
                decoded_this_frame = true;
                has_frame = true;
                state.active_decoder = decoder->backend_description();
                state.progress_seconds = std::max(0.0, frame.pts_seconds);
            } else if (has_frame) {
                state.paused = true;
                state.status_message = "End of video. Drag the progress slider to replay.";
            } else {
                throw std::runtime_error("the selected video contains no decodable frames");
            }
        }

        if (!has_frame) {
            continue;
        }

        if (decoded_this_frame && options.sync_to_pts && options.snapshot_output.empty()) {
            const double source_fps = decoder->fps() > 0.0 ? decoder->fps() : 30.0;
            const auto frame_interval =
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(1.0 / source_fps));
            if (reset_frame_clock) {
                next_frame_deadline = std::chrono::steady_clock::now();
                reset_frame_clock = false;
            } else {
                next_frame_deadline += frame_interval;
                const auto now = std::chrono::steady_clock::now();
                if (next_frame_deadline + std::chrono::milliseconds(250) < now) {
                    next_frame_deadline = now;
                }
                std::this_thread::sleep_until(next_frame_deadline);
            }
        }
        if (state.paused) {
            reset_frame_clock = true;
            playback_fps.reset();
            state.wall_fps = 0.0;
        }

        const auto render_start = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(
            render_start - application_start).count();
        const bool snapshot_due = !options.snapshot_output.empty() &&
                                  frame_count + 1 >= options.snapshot_frame;
        const std::string snapshot_path =
            snapshot_due ? options.snapshot_output : std::string{};
        renderer.render(frame,
                        state,
                        elapsed,
                        snapshot_path,
                        options.include_controls_in_snapshot);
        const auto render_end = std::chrono::steady_clock::now();

        if (decoded_this_frame) {
            const double render_ms = milliseconds(render_end - render_start);
            metrics.record(decode_ms, render_ms);
            state.pipeline_history_ms.push_back(
                static_cast<float>(decode_ms + render_ms));
            if (state.pipeline_history_ms.size() > 240) {
                state.pipeline_history_ms.erase(state.pipeline_history_ms.begin());
            }
            const double metric_seconds = std::chrono::duration<double>(
                render_end - metrics_start).count();
            const auto summary = metrics.summarize(metric_seconds);
            const double frame_timestamp_seconds =
                std::chrono::duration<double>(
                    render_end.time_since_epoch()).count();
            playback_fps.record(frame_timestamp_seconds);
            state.wall_fps = playback_fps.fps();
            state.average_decode_ms = summary.average_decode_ms;
            state.average_render_ms = summary.average_render_ms;
            state.p95_pipeline_ms = summary.p95_pipeline_ms;
            ++frame_count;
        }

        if (options.max_frames > 0 && frame_count >= options.max_frames) {
            break;
        }
        if (snapshot_due) {
            std::cout << "snapshot=" << options.snapshot_output << '\n';
            break;
        }
    }

    const double elapsed_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - metrics_start).count();
    save_metrics(options, metrics, elapsed_seconds);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);
        if (!options.output.empty()) {
            return run_export(options);
        }
        return options.headless ? run_headless(options) : run_interactive(options);
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}

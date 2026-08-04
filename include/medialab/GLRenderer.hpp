#pragma once

#include <memory>
#include <string>
#include <vector>

#include "medialab/VideoDecoder.hpp"

namespace medialab {

enum class FilterMode {
    Original = 0,
    Grayscale = 1,
    Sepia = 2,
    Edge = 3,
    Vignette = 4,
};

const char* filter_name(FilterMode mode) noexcept;
FilterMode parse_filter(const std::string& value);

struct GpuPerformanceSummary {
    double upload_submit_ms = 0.0;
    double shader_ms = 0.0;
    double readback_submit_ms = 0.0;
    double readback_gpu_ms = 0.0;
    double pbo_map_wait_ms = 0.0;
    bool readback_sampled = false;
    bool readback_gpu_sampled = false;
    bool pbo_wait_sampled = false;
    bool readback_used_pbo = false;
};

struct InteractiveState {
    FilterMode filter = FilterMode::Original;
    bool paused = false;
    bool split_view = false;
    float effect_intensity = 1.0F;
    float edge_strength = 1.8F;
    float vignette_strength = 1.0F;
    double progress_seconds = 0.0;
    double duration_seconds = 0.0;
    double wall_fps = 0.0;
    double average_decode_ms = 0.0;
    double average_render_ms = 0.0;
    double p95_pipeline_ms = 0.0;
    double upload_submit_ms = 0.0;
    double gpu_shader_ms = 0.0;
    double readback_submit_ms = 0.0;
    double readback_gpu_ms = 0.0;
    double pbo_map_wait_ms = 0.0;
    bool readback_metrics_available = false;
    bool readback_gpu_available = false;
    bool pbo_wait_available = false;
    bool readback_used_pbo = false;
    std::vector<float> pipeline_history_ms;
    std::string input_path;
    int decoder_backend = 0;
    std::string active_decoder = "software";
    bool asynchronous_pbo = true;
    std::string status_message;
    std::string export_path = "processed-hevc.mp4";
    int export_encoder = 0;
    int export_crf = 24;
    int export_bitrate_kbps = 6000;
    int export_gop_size = 0;
    std::string export_preset = "medium";
    bool export_copy_audio = true;
    bool export_quality_report = true;
    bool export_requested = false;
    bool export_cancel_requested = false;
    bool export_in_progress = false;
    float export_progress = 0.0F;
    bool load_requested = false;
    bool seek_requested = false;
    double seek_target_seconds = 0.0;
};

class GLRenderer {
public:
    GLRenderer(int video_width,
               int video_height,
               const std::string& shader_directory,
               bool visible = true);
    ~GLRenderer();

    GLRenderer(const GLRenderer&) = delete;
    GLRenderer& operator=(const GLRenderer&) = delete;

    bool should_close() const;
    void poll_events();
    void render(const VideoFrame& frame,
                InteractiveState& state,
                double elapsed_seconds,
                const std::string& snapshot_output = {},
                bool include_controls_in_snapshot = false);
    bool process(const VideoFrame& frame,
                 const InteractiveState& state,
                 double elapsed_seconds,
                 VideoFrame& output);
    bool flush_process(VideoFrame& output);
    void reset_profiling();
    GpuPerformanceSummary profiling_summary();
    std::string take_dropped_path();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace medialab

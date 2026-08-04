#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "medialab/VideoDecoder.hpp"

namespace medialab {

struct EncoderConfig {
    std::string output_path;
    std::string source_path;
    std::string encoder_name = "libx265";
    std::string preset = "medium";
    int width = 0;
    int height = 0;
    double fps = 30.0;
    int crf = 24;
    int bitrate_kbps = 6000;
    int gop_size = 0;
    bool copy_audio = true;
};

struct EncoderStats {
    std::size_t frames = 0;
    std::uint64_t video_bytes = 0;
    double elapsed_seconds = 0.0;

    double encoding_fps() const noexcept;
};

class VideoEncoder {
public:
    explicit VideoEncoder(const EncoderConfig& config);
    ~VideoEncoder();

    VideoEncoder(const VideoEncoder&) = delete;
    VideoEncoder& operator=(const VideoEncoder&) = delete;
    VideoEncoder(VideoEncoder&&) noexcept;
    VideoEncoder& operator=(VideoEncoder&&) noexcept;

    void write_rgb(const VideoFrame& frame);
    EncoderStats finish();
    const EncoderConfig& config() const noexcept;
    bool finished() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace medialab

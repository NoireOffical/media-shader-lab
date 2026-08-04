#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace medialab {

enum class DecoderBackend {
    Software = 0,
    VideoToolbox = 1,
};

const char* decoder_backend_name(DecoderBackend backend) noexcept;
DecoderBackend parse_decoder_backend(const std::string& value);

struct VideoFrame {
    int width = 0;
    int height = 0;
    double pts_seconds = 0.0;
    std::vector<std::uint8_t> rgb;
};

class VideoDecoder {
public:
    explicit VideoDecoder(
        const std::string& input_path,
        DecoderBackend backend = DecoderBackend::Software);
    ~VideoDecoder();

    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;
    VideoDecoder(VideoDecoder&&) noexcept;
    VideoDecoder& operator=(VideoDecoder&&) noexcept;

    bool read(VideoFrame& output);
    void seek(double seconds);
    double fps() const noexcept;
    double duration_seconds() const noexcept;
    int width() const noexcept;
    int height() const noexcept;
    std::string codec_name() const;
    DecoderBackend backend() const noexcept;
    std::string backend_description() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace medialab

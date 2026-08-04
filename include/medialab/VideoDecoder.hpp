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

enum class HardwareSurfaceFormat {
    None = 0,
    Nv12VideoRange = 1,
    Nv12FullRange = 2,
};

enum class VideoColorMatrix {
    Bt601 = 0,
    Bt709 = 1,
    Bt2020 = 2,
};

const char* decoder_backend_name(DecoderBackend backend) noexcept;
DecoderBackend parse_decoder_backend(const std::string& value);

struct VideoFrame {
    int width = 0;
    int height = 0;
    double pts_seconds = 0.0;
    std::vector<std::uint8_t> rgb;
    // On macOS this owns a retained CVPixelBufferRef. It stays opaque here so
    // the public decoder API remains portable and does not expose Apple types.
    std::shared_ptr<void> hardware_surface;
    HardwareSurfaceFormat hardware_format = HardwareSurfaceFormat::None;
    VideoColorMatrix color_matrix = VideoColorMatrix::Bt601;

    bool has_hardware_surface() const noexcept {
        return hardware_surface != nullptr &&
               hardware_format != HardwareSurfaceFormat::None;
    }
};

class VideoDecoder {
public:
    explicit VideoDecoder(
        const std::string& input_path,
        DecoderBackend backend = DecoderBackend::Software,
        bool prefer_zero_copy = true);
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
    bool zero_copy_active() const noexcept;
    std::string backend_description() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace medialab

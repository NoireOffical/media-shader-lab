#include "medialab/VideoDecoder.hpp"
#include "medialab/VideoEncoder.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: encoder_test OUTPUT.mp4\n";
        return 2;
    }

    medialab::EncoderConfig config;
    config.output_path = argv[1];
    config.width = 160;
    config.height = 90;
    config.fps = 30.0;
    config.crf = 28;
    config.preset = "ultrafast";
    config.copy_audio = false;

    medialab::VideoEncoder encoder(config);
    medialab::VideoFrame frame;
    frame.width = config.width;
    frame.height = config.height;
    frame.rgb.resize(static_cast<std::size_t>(frame.width) *
                     static_cast<std::size_t>(frame.height) * 3U);
    for (int frame_index = 0; frame_index < 30; ++frame_index) {
        frame.pts_seconds = static_cast<double>(frame_index) / config.fps;
        for (int y = 0; y < frame.height; ++y) {
            for (int x = 0; x < frame.width; ++x) {
                const std::size_t offset =
                    (static_cast<std::size_t>(y) * frame.width + x) * 3U;
                frame.rgb[offset] = static_cast<std::uint8_t>((x + frame_index * 2) % 256);
                frame.rgb[offset + 1] = static_cast<std::uint8_t>((y * 2) % 256);
                frame.rgb[offset + 2] = static_cast<std::uint8_t>((x + y) % 256);
            }
        }
        encoder.write_rgb(frame);
    }
    const auto stats = encoder.finish();
    assert(stats.frames == 30);

    medialab::VideoDecoder decoder(argv[1]);
    assert(decoder.codec_name().find("hevc") != std::string::npos);
    assert(std::abs(decoder.duration_seconds() - 1.0) < 0.05);
    std::size_t decoded_frames = 0;
    while (decoder.read(frame)) {
        ++decoded_frames;
    }
    assert(decoded_frames == 30);
    std::cout << "H.265 encoder integration test passed\n";
    return 0;
}

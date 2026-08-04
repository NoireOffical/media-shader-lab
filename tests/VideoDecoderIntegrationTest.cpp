#include "medialab/VideoDecoder.hpp"

#include <iostream>

int main(int argc, char** argv) {
    if (argc != 2 && argc != 3) {
        std::cerr << "usage: decoder_seek_test VIDEO [software|videotoolbox]\n";
        return 2;
    }

    const auto backend = argc == 3
        ? medialab::parse_decoder_backend(argv[2])
        : medialab::DecoderBackend::Software;
    try {
        medialab::VideoDecoder decoder(argv[1], backend);
        medialab::VideoFrame first;
        if (!decoder.read(first) || first.pts_seconds >= 0.1) {
            std::cerr << "unexpected first frame PTS: " << first.pts_seconds << '\n';
            return 1;
        }

        decoder.seek(1.5);
        medialab::VideoFrame after_seek;
        if (!decoder.read(after_seek) ||
            after_seek.pts_seconds < 1.499 ||
            after_seek.pts_seconds >= 2.0) {
            std::cerr << "unexpected PTS after seek: "
                      << after_seek.pts_seconds << '\n';
            return 1;
        }

        std::cout << "decoder_seek_test passed: backend="
                  << decoder.backend_description()
                  << " pts=" << after_seek.pts_seconds << '\n';
        return 0;
    } catch (const std::exception& error) {
        if (backend == medialab::DecoderBackend::VideoToolbox) {
            std::cerr << "VideoToolbox test skipped: " << error.what() << '\n';
            return 77;
        }
        std::cerr << "decoder test failed: " << error.what() << '\n';
        return 1;
    }
}

#include "medialab/VideoDecoder.hpp"

#include <cmath>
#include <stdexcept>
#include <utility>

#if defined(__APPLE__)
#include <CoreVideo/CoreVideo.h>
#include <CoreVideo/CVPixelBufferIOSurface.h>
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace medialab {
namespace {

std::string ffmpeg_error(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

void require_ok(int result, const std::string& operation) {
    if (result < 0) {
        throw std::runtime_error(operation + ": " + ffmpeg_error(result));
    }
}

VideoColorMatrix color_matrix_for(AVColorSpace color_space) noexcept {
    switch (color_space) {
        case AVCOL_SPC_BT470BG:
        case AVCOL_SPC_SMPTE170M:
        case AVCOL_SPC_SMPTE240M:
            return VideoColorMatrix::Bt601;
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL:
            return VideoColorMatrix::Bt2020;
        case AVCOL_SPC_BT709:
            return VideoColorMatrix::Bt709;
        default:
            // Match libswscale's default coefficients for streams that do not
            // carry color-space metadata, so software and zero-copy paths agree.
            return VideoColorMatrix::Bt601;
    }
}

}  // namespace

const char* decoder_backend_name(DecoderBackend backend) noexcept {
    switch (backend) {
        case DecoderBackend::Software: return "software";
        case DecoderBackend::VideoToolbox: return "videotoolbox";
    }
    return "unknown";
}

DecoderBackend parse_decoder_backend(const std::string& value) {
    if (value == "software") {
        return DecoderBackend::Software;
    }
    if (value == "videotoolbox") {
        return DecoderBackend::VideoToolbox;
    }
    throw std::invalid_argument(
        "decoder must be software or videotoolbox: " + value);
}

class VideoDecoder::Impl {
public:
    explicit Impl(const std::string& input_path,
                  DecoderBackend backend_value,
                  bool prefer_zero_copy_value)
        : selected_backend(backend_value),
          prefer_zero_copy(prefer_zero_copy_value) {
        require_ok(avformat_open_input(&format, input_path.c_str(), nullptr, nullptr),
                   "could not open input");
        try {
            require_ok(avformat_find_stream_info(format, nullptr),
                       "could not read stream information");

            const int selected = av_find_best_stream(
                format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
            require_ok(selected, "could not find a video stream");
            stream_index = selected;
            stream = format->streams[stream_index];

            const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
            if (codec == nullptr) {
                throw std::runtime_error("no decoder is available for the video codec");
            }
            codec_label = codec->name != nullptr ? codec->name : "unknown";

            codec_context = avcodec_alloc_context3(codec);
            if (codec_context == nullptr) {
                throw std::runtime_error("could not allocate decoder context");
            }
            require_ok(avcodec_parameters_to_context(codec_context, stream->codecpar),
                       "could not copy codec parameters");

            if (selected_backend == DecoderBackend::VideoToolbox) {
#if defined(__APPLE__)
                const AVCodecHWConfig* hardware_config = nullptr;
                for (int index = 0; ; ++index) {
                    const AVCodecHWConfig* candidate =
                        avcodec_get_hw_config(codec, index);
                    if (candidate == nullptr) {
                        break;
                    }
                    if ((candidate->methods &
                         AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0 &&
                        candidate->device_type ==
                            AV_HWDEVICE_TYPE_VIDEOTOOLBOX) {
                        hardware_config = candidate;
                        break;
                    }
                }
                if (hardware_config == nullptr) {
                    throw std::runtime_error(
                        "the selected codec does not support VideoToolbox decoding");
                }
                hardware_pixel_format = hardware_config->pix_fmt;
                codec_context->opaque = this;
                codec_context->get_format = select_hardware_format;
                require_ok(av_hwdevice_ctx_create(&hardware_device,
                                                  AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
                                                  nullptr,
                                                  nullptr,
                                                  0),
                           "could not create VideoToolbox device");
                codec_context->hw_device_ctx = av_buffer_ref(hardware_device);
                if (codec_context->hw_device_ctx == nullptr) {
                    throw std::runtime_error(
                        "could not reference VideoToolbox device");
                }
#else
                throw std::runtime_error(
                    "VideoToolbox decoding is only available on macOS");
#endif
            }
            require_ok(avcodec_open2(codec_context, codec, nullptr),
                       "could not open decoder");

            decoded = av_frame_alloc();
            software_frame = av_frame_alloc();
            packet = av_packet_alloc();
            if (decoded == nullptr || software_frame == nullptr ||
                packet == nullptr) {
                throw std::runtime_error("could not allocate FFmpeg frame or packet");
            }

            const AVRational guessed = av_guess_frame_rate(format, stream, nullptr);
            frame_rate = guessed.num > 0 && guessed.den > 0 ? av_q2d(guessed) : 0.0;
        } catch (...) {
            cleanup();
            throw;
        }
    }

    ~Impl() { cleanup(); }

    static AVPixelFormat select_hardware_format(
        AVCodecContext* context,
        const AVPixelFormat* formats) {
        auto* self = static_cast<Impl*>(context->opaque);
        if (self == nullptr) {
            return AV_PIX_FMT_NONE;
        }
        for (const AVPixelFormat* format = formats;
             *format != AV_PIX_FMT_NONE;
             ++format) {
            if (*format == self->hardware_pixel_format) {
                return *format;
            }
        }
        return AV_PIX_FMT_NONE;
    }

    bool read(VideoFrame& output) {
        while (true) {
            const int receive_result = avcodec_receive_frame(codec_context, decoded);
            if (receive_result == 0) {
                convert(decoded, output);
                if (discard_before_seconds >= 0.0 &&
                    output.pts_seconds + 0.001 < discard_before_seconds) {
                    continue;
                }
                discard_before_seconds = -1.0;
                return true;
            }
            if (receive_result == AVERROR_EOF) {
                return false;
            }
            if (receive_result != AVERROR(EAGAIN)) {
                require_ok(receive_result, "decoder failed to receive a frame");
            }

            if (demux_eof) {
                if (!flush_sent) {
                    const int flush_result = avcodec_send_packet(codec_context, nullptr);
                    if (flush_result != AVERROR_EOF) {
                        require_ok(flush_result, "decoder flush failed");
                    }
                    flush_sent = true;
                    continue;
                }
                return false;
            }

            if (!packet_pending) {
                int read_result = 0;
                do {
                    read_result = av_read_frame(format, packet);
                    if (read_result < 0) {
                        if (read_result != AVERROR_EOF) {
                            require_ok(read_result, "demuxer failed while reading a packet");
                        }
                        demux_eof = true;
                        break;
                    }
                    if (packet->stream_index == stream_index) {
                        packet_pending = true;
                    } else {
                        av_packet_unref(packet);
                    }
                } while (!packet_pending);
                if (demux_eof) {
                    continue;
                }
            }

            const int send_result = avcodec_send_packet(codec_context, packet);
            if (send_result == 0) {
                av_packet_unref(packet);
                packet_pending = false;
            } else if (send_result != AVERROR(EAGAIN)) {
                av_packet_unref(packet);
                packet_pending = false;
                require_ok(send_result, "decoder failed to accept a packet");
            }
        }
    }

    double duration_seconds() const noexcept {
        if (stream != nullptr && stream->duration != AV_NOPTS_VALUE) {
            return static_cast<double>(stream->duration) * av_q2d(stream->time_base);
        }
        if (format != nullptr && format->duration != AV_NOPTS_VALUE) {
            return static_cast<double>(format->duration) / AV_TIME_BASE;
        }
        return 0.0;
    }

    void seek(double seconds) {
        const double clamped = std::max(0.0, std::min(seconds, duration_seconds()));
        const auto microseconds = static_cast<std::int64_t>(
            std::llround(clamped * static_cast<double>(AV_TIME_BASE)));
        const std::int64_t target = av_rescale_q(
            microseconds, AV_TIME_BASE_Q, stream->time_base);
        require_ok(av_seek_frame(format, stream_index, target, AVSEEK_FLAG_BACKWARD),
                   "could not seek video");
        avformat_flush(format);
        avcodec_flush_buffers(codec_context);
        av_packet_unref(packet);
        packet_pending = false;
        demux_eof = false;
        flush_sent = false;
        frames_without_pts = 0;
        discard_before_seconds = clamped;
    }

    void convert(const AVFrame* source, VideoFrame& output) {
        output.hardware_surface.reset();
        output.hardware_format = HardwareSurfaceFormat::None;
        output.color_matrix = color_matrix_for(source->colorspace);

#if defined(__APPLE__)
        if (prefer_zero_copy && source->format == hardware_pixel_format &&
            source->data[3] != nullptr) {
            auto pixel_buffer = reinterpret_cast<CVPixelBufferRef>(source->data[3]);
            const OSType pixel_format = CVPixelBufferGetPixelFormatType(pixel_buffer);
            HardwareSurfaceFormat surface_format = HardwareSurfaceFormat::None;
            if (pixel_format == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange) {
                surface_format = HardwareSurfaceFormat::Nv12VideoRange;
            } else if (pixel_format ==
                       kCVPixelFormatType_420YpCbCr8BiPlanarFullRange) {
                surface_format = HardwareSurfaceFormat::Nv12FullRange;
            }
            if (surface_format != HardwareSurfaceFormat::None &&
                CVPixelBufferGetPlaneCount(pixel_buffer) == 2 &&
                CVPixelBufferGetIOSurface(pixel_buffer) != nullptr) {
                CVPixelBufferRetain(pixel_buffer);
                output.hardware_surface = std::shared_ptr<void>(
                    pixel_buffer,
                    [](void* surface) {
                        CVPixelBufferRelease(
                            static_cast<CVPixelBufferRef>(surface));
                    });
                output.hardware_format = surface_format;
                output.width = source->width;
                output.height = source->height;
                output.rgb.clear();
                hardware_frame_received = true;
                zero_copy_frame_received = true;
                set_timestamp(source, output);
                return;
            }
        }
#endif

        const AVFrame* readable = source;
        if (source->format == hardware_pixel_format) {
            av_frame_unref(software_frame);
            require_ok(av_hwframe_transfer_data(software_frame, source, 0),
                       "could not transfer VideoToolbox frame to system memory");
            readable = software_frame;
            hardware_frame_received = true;
        }
        output.width = readable->width;
        output.height = readable->height;
        output.rgb.resize(static_cast<std::size_t>(output.width) *
                          static_cast<std::size_t>(output.height) * 3U);
        std::uint8_t* destination[] = {output.rgb.data(), nullptr, nullptr, nullptr};
        int destination_stride[] = {output.width * 3, 0, 0, 0};
        scaler = sws_getCachedContext(scaler,
                                      readable->width,
                                      readable->height,
                                      static_cast<AVPixelFormat>(readable->format),
                                      readable->width,
                                      readable->height,
                                      AV_PIX_FMT_RGB24,
                                      SWS_BILINEAR,
                                      nullptr,
                                      nullptr,
                                      nullptr);
        if (scaler == nullptr) {
            throw std::runtime_error("could not initialize RGB conversion");
        }
        sws_scale(scaler,
                  readable->data,
                  readable->linesize,
                  0,
                  readable->height,
                  destination,
                  destination_stride);

        set_timestamp(source, output);
    }

    void set_timestamp(const AVFrame* source, VideoFrame& output) {
        const std::int64_t timestamp = source->best_effort_timestamp;
        if (timestamp != AV_NOPTS_VALUE) {
            output.pts_seconds = static_cast<double>(timestamp) * av_q2d(stream->time_base);
        } else {
            output.pts_seconds = frame_rate > 0.0
                                     ? static_cast<double>(frames_without_pts++) / frame_rate
                                     : 0.0;
        }
    }

    void cleanup() noexcept {
        if (packet != nullptr) {
            av_packet_free(&packet);
        }
        if (decoded != nullptr) {
            av_frame_free(&decoded);
        }
        if (software_frame != nullptr) {
            av_frame_free(&software_frame);
        }
        if (scaler != nullptr) {
            sws_freeContext(scaler);
            scaler = nullptr;
        }
        if (codec_context != nullptr) {
            avcodec_free_context(&codec_context);
        }
        if (hardware_device != nullptr) {
            av_buffer_unref(&hardware_device);
        }
        if (format != nullptr) {
            avformat_close_input(&format);
        }
    }

    AVFormatContext* format = nullptr;
    AVCodecContext* codec_context = nullptr;
    AVStream* stream = nullptr;
    SwsContext* scaler = nullptr;
    AVFrame* decoded = nullptr;
    AVFrame* software_frame = nullptr;
    AVPacket* packet = nullptr;
    AVBufferRef* hardware_device = nullptr;
    AVPixelFormat hardware_pixel_format = AV_PIX_FMT_NONE;
    int stream_index = -1;
    double frame_rate = 0.0;
    std::size_t frames_without_pts = 0;
    bool packet_pending = false;
    bool demux_eof = false;
    bool flush_sent = false;
    double discard_before_seconds = -1.0;
    std::string codec_label;
    DecoderBackend selected_backend = DecoderBackend::Software;
    bool hardware_frame_received = false;
    bool zero_copy_frame_received = false;
    bool prefer_zero_copy = true;
};

VideoDecoder::VideoDecoder(const std::string& input_path,
                           DecoderBackend backend,
                           bool prefer_zero_copy)
    : impl_(std::make_unique<Impl>(input_path, backend, prefer_zero_copy)) {}

VideoDecoder::~VideoDecoder() = default;
VideoDecoder::VideoDecoder(VideoDecoder&&) noexcept = default;
VideoDecoder& VideoDecoder::operator=(VideoDecoder&&) noexcept = default;

bool VideoDecoder::read(VideoFrame& output) { return impl_->read(output); }
void VideoDecoder::seek(double seconds) { impl_->seek(seconds); }
double VideoDecoder::fps() const noexcept { return impl_->frame_rate; }
double VideoDecoder::duration_seconds() const noexcept { return impl_->duration_seconds(); }
int VideoDecoder::width() const noexcept { return impl_->codec_context->width; }
int VideoDecoder::height() const noexcept { return impl_->codec_context->height; }
std::string VideoDecoder::codec_name() const { return impl_->codec_label; }
DecoderBackend VideoDecoder::backend() const noexcept {
    return impl_->selected_backend;
}
bool VideoDecoder::zero_copy_active() const noexcept {
    return impl_->zero_copy_frame_received;
}
std::string VideoDecoder::backend_description() const {
    if (impl_->selected_backend == DecoderBackend::VideoToolbox) {
        if (impl_->zero_copy_frame_received) {
            return "VideoToolbox hardware (IOSurface zero-copy)";
        }
        if (impl_->hardware_frame_received) {
            return impl_->prefer_zero_copy
                ? "VideoToolbox hardware (CPU fallback)"
                : "VideoToolbox hardware (CPU transfer)";
        }
        return impl_->prefer_zero_copy
            ? "VideoToolbox hardware (zero-copy requested)"
            : "VideoToolbox hardware";
    }
    return "FFmpeg software";
}

}  // namespace medialab

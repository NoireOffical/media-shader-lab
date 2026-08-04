#include "medialab/VideoEncoder.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
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

void open_input(AVFormatContext** context, const std::string& path) {
    require_ok(avformat_open_input(context, path.c_str(), nullptr, nullptr),
               "could not open input " + path);
    require_ok(avformat_find_stream_info(*context, nullptr),
               "could not inspect input " + path);
}

bool read_stream_packet(AVFormatContext* context,
                        int stream_index,
                        AVPacket* packet) {
    av_packet_unref(packet);
    while (true) {
        const int result = av_read_frame(context, packet);
        if (result == AVERROR_EOF) {
            return false;
        }
        require_ok(result, "failed while remuxing packets");
        if (packet->stream_index == stream_index) {
            return true;
        }
        av_packet_unref(packet);
    }
}

double packet_seconds(const AVPacket* packet, AVRational time_base) {
    const std::int64_t timestamp =
        packet->dts != AV_NOPTS_VALUE ? packet->dts : packet->pts;
    if (timestamp == AV_NOPTS_VALUE) {
        return std::numeric_limits<double>::infinity();
    }
    return static_cast<double>(timestamp) * av_q2d(time_base);
}

void write_remux_packet(AVFormatContext* output,
                        AVPacket* packet,
                        AVRational source_time_base,
                        AVStream* destination,
                        std::int64_t timestamp_offset) {
    if (packet->pts != AV_NOPTS_VALUE) {
        packet->pts -= timestamp_offset;
    }
    if (packet->dts != AV_NOPTS_VALUE) {
        packet->dts -= timestamp_offset;
    }
    av_packet_rescale_ts(packet, source_time_base, destination->time_base);
    packet->stream_index = destination->index;
    packet->pos = -1;
    require_ok(av_interleaved_write_frame(output, packet),
               "could not write remuxed packet");
}

void mux_video_and_source_audio(const std::string& video_path,
                                const std::string& source_path,
                                const std::string& output_path) {
    AVFormatContext* video_input = nullptr;
    AVFormatContext* source_input = nullptr;
    AVFormatContext* output = nullptr;
    AVPacket* video_packet = nullptr;
    AVPacket* audio_packet = nullptr;

    try {
        open_input(&video_input, video_path);
        open_input(&source_input, source_path);
        const int video_index = av_find_best_stream(
            video_input, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        require_ok(video_index, "encoded output contains no video stream");
        const int audio_index = av_find_best_stream(
            source_input, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

        require_ok(avformat_alloc_output_context2(
                       &output, nullptr, nullptr, output_path.c_str()),
                   "could not create final output container");
        if (output == nullptr) {
            throw std::runtime_error("could not create final output container");
        }

        AVStream* output_video = avformat_new_stream(output, nullptr);
        if (output_video == nullptr) {
            throw std::runtime_error("could not allocate final video stream");
        }
        AVStream* input_video = video_input->streams[video_index];
        require_ok(avcodec_parameters_copy(output_video->codecpar,
                                           input_video->codecpar),
                   "could not copy encoded video parameters");
        output_video->codecpar->codec_tag = 0;
        output_video->time_base = input_video->time_base;

        AVStream* output_audio = nullptr;
        AVStream* input_audio = nullptr;
        if (audio_index >= 0) {
            input_audio = source_input->streams[audio_index];
            output_audio = avformat_new_stream(output, nullptr);
            if (output_audio == nullptr) {
                throw std::runtime_error("could not allocate final audio stream");
            }
            require_ok(avcodec_parameters_copy(output_audio->codecpar,
                                               input_audio->codecpar),
                       "could not copy source audio parameters");
            output_audio->codecpar->codec_tag = 0;
            output_audio->time_base = input_audio->time_base;
        }

        if ((output->oformat->flags & AVFMT_NOFILE) == 0) {
            require_ok(avio_open(&output->pb, output_path.c_str(), AVIO_FLAG_WRITE),
                       "could not open final output");
        }
        require_ok(avformat_write_header(output, nullptr),
                   "could not write final container header");

        video_packet = av_packet_alloc();
        audio_packet = av_packet_alloc();
        if (video_packet == nullptr || audio_packet == nullptr) {
            throw std::runtime_error("could not allocate remux packets");
        }

        bool has_video = read_stream_packet(video_input, video_index, video_packet);
        bool has_audio = output_audio != nullptr &&
                         read_stream_packet(source_input, audio_index, audio_packet);
        const std::int64_t video_offset = has_video
            ? (video_packet->dts != AV_NOPTS_VALUE ? video_packet->dts
                                                   : video_packet->pts)
            : 0;
        const std::int64_t audio_offset = has_audio
            ? (audio_packet->dts != AV_NOPTS_VALUE ? audio_packet->dts
                                                   : audio_packet->pts)
            : 0;
        const double video_duration = video_input->duration > 0
            ? static_cast<double>(video_input->duration) / AV_TIME_BASE
            : std::numeric_limits<double>::infinity();

        while (has_video || has_audio) {
            const double video_time = has_video
                ? packet_seconds(video_packet, input_video->time_base)
                : std::numeric_limits<double>::infinity();
            const double audio_time = has_audio
                ? packet_seconds(audio_packet, input_audio->time_base)
                : std::numeric_limits<double>::infinity();
            if (has_video && video_time <= audio_time) {
                write_remux_packet(output,
                                   video_packet,
                                   input_video->time_base,
                                   output_video,
                                   video_offset);
                has_video = read_stream_packet(video_input,
                                               video_index,
                                               video_packet);
            } else if (has_audio) {
                const double normalized_audio_time =
                    audio_time - static_cast<double>(audio_offset) *
                                     av_q2d(input_audio->time_base);
                if (normalized_audio_time <= video_duration) {
                    write_remux_packet(output,
                                       audio_packet,
                                       input_audio->time_base,
                                       output_audio,
                                       audio_offset);
                    has_audio = read_stream_packet(source_input,
                                                   audio_index,
                                                   audio_packet);
                } else {
                    has_audio = false;
                }
            }
        }

        require_ok(av_write_trailer(output),
                   "could not finalize final container");
    } catch (...) {
        if (video_packet != nullptr) av_packet_free(&video_packet);
        if (audio_packet != nullptr) av_packet_free(&audio_packet);
        if (output != nullptr) {
            if (output->pb != nullptr &&
                (output->oformat->flags & AVFMT_NOFILE) == 0) {
                avio_closep(&output->pb);
            }
            avformat_free_context(output);
        }
        if (source_input != nullptr) avformat_close_input(&source_input);
        if (video_input != nullptr) avformat_close_input(&video_input);
        throw;
    }

    av_packet_free(&video_packet);
    av_packet_free(&audio_packet);
    if (output->pb != nullptr && (output->oformat->flags & AVFMT_NOFILE) == 0) {
        avio_closep(&output->pb);
    }
    avformat_free_context(output);
    avformat_close_input(&source_input);
    avformat_close_input(&video_input);
}

AVPixelFormat select_pixel_format(const AVCodec* codec) {
#if LIBAVCODEC_VERSION_MAJOR >= 61
    const void* formats = nullptr;
    int format_count = 0;
    const int result = avcodec_get_supported_config(
        nullptr,
        codec,
        AV_CODEC_CONFIG_PIX_FORMAT,
        0,
        &formats,
        &format_count);
    if (result < 0 || formats == nullptr || format_count <= 0) {
        return AV_PIX_FMT_YUV420P;
    }
    const auto* pixel_formats = static_cast<const AVPixelFormat*>(formats);
#else
    const auto* pixel_formats = codec->pix_fmts;
    if (pixel_formats == nullptr) {
        return AV_PIX_FMT_YUV420P;
    }
    int format_count = 0;
    while (pixel_formats[format_count] != AV_PIX_FMT_NONE) {
        ++format_count;
    }
#endif
    for (int i = 0; i < format_count; ++i) {
        if (pixel_formats[i] == AV_PIX_FMT_YUV420P) {
            return AV_PIX_FMT_YUV420P;
        }
    }
    for (int i = 0; i < format_count; ++i) {
        if (pixel_formats[i] == AV_PIX_FMT_NV12) {
            return AV_PIX_FMT_NV12;
        }
    }
    throw std::runtime_error("encoder does not accept YUV420P or NV12 input");
}

}  // namespace

double EncoderStats::encoding_fps() const noexcept {
    return elapsed_seconds > 0.0
        ? static_cast<double>(frames) / elapsed_seconds
        : 0.0;
}

class VideoEncoder::Impl {
public:
    explicit Impl(EncoderConfig encoder_config)
        : config_value(std::move(encoder_config)),
          started_at(std::chrono::steady_clock::now()) {
        validate_config();
        temporary_path = config_value.copy_audio && !config_value.source_path.empty()
            ? config_value.output_path + ".video-only.tmp.mp4"
            : config_value.output_path;
        initialize();
    }

    ~Impl() {
        cleanup();
        if (!temporary_path.empty() && temporary_path != config_value.output_path) {
            std::remove(temporary_path.c_str());
        }
    }

    void validate_config() const {
        if (config_value.output_path.empty()) {
            throw std::invalid_argument("encoder output path is empty");
        }
        if (!config_value.source_path.empty() &&
            std::filesystem::absolute(config_value.output_path).lexically_normal() ==
                std::filesystem::absolute(config_value.source_path).lexically_normal()) {
            throw std::invalid_argument(
                "encoder output path must differ from the source video");
        }
        if (config_value.width <= 0 || config_value.height <= 0 ||
            config_value.width % 2 != 0 || config_value.height % 2 != 0) {
            throw std::invalid_argument(
                "H.265 YUV420 output requires positive even dimensions");
        }
        if (!(config_value.fps > 0.0)) {
            throw std::invalid_argument("encoder FPS must be positive");
        }
        if (config_value.crf < 0 || config_value.crf > 51) {
            throw std::invalid_argument("CRF must be between 0 and 51");
        }
        if (config_value.bitrate_kbps <= 0) {
            throw std::invalid_argument("bitrate must be positive");
        }
    }

    void initialize() {
        const AVCodec* codec = avcodec_find_encoder_by_name(
            config_value.encoder_name.c_str());
        if (codec == nullptr) {
            throw std::runtime_error("H.265 encoder is unavailable: " +
                                     config_value.encoder_name);
        }

        require_ok(avformat_alloc_output_context2(
                       &format, nullptr, "mp4", temporary_path.c_str()),
                   "could not create MP4 output context");
        if (format == nullptr) {
            throw std::runtime_error("could not create MP4 output context");
        }

        stream = avformat_new_stream(format, nullptr);
        if (stream == nullptr) {
            throw std::runtime_error("could not create output video stream");
        }
        codec_context = avcodec_alloc_context3(codec);
        if (codec_context == nullptr) {
            throw std::runtime_error("could not allocate H.265 encoder context");
        }

        const AVRational frame_rate = av_d2q(config_value.fps, 1000000);
        codec_context->codec_id = codec->id;
        codec_context->codec_type = AVMEDIA_TYPE_VIDEO;
        codec_context->width = config_value.width;
        codec_context->height = config_value.height;
        codec_context->pix_fmt = select_pixel_format(codec);
        codec_context->time_base = av_inv_q(frame_rate);
        codec_context->framerate = frame_rate;
        codec_context->gop_size = config_value.gop_size > 0
            ? config_value.gop_size
            : std::max(1, static_cast<int>(config_value.fps * 2.0));
        codec_context->max_b_frames =
            config_value.encoder_name == "libx265" ? 3 : 0;
        codec_context->bit_rate =
            static_cast<std::int64_t>(config_value.bitrate_kbps) * 1000;
        codec_context->color_range = AVCOL_RANGE_MPEG;
        codec_context->colorspace = AVCOL_SPC_BT709;
        codec_context->color_primaries = AVCOL_PRI_BT709;
        codec_context->color_trc = AVCOL_TRC_BT709;
        if ((format->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
            codec_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }

        AVDictionary* encoder_options = nullptr;
        if (config_value.encoder_name == "libx265") {
            av_dict_set(&encoder_options, "preset", config_value.preset.c_str(), 0);
            av_dict_set_int(&encoder_options, "crf", config_value.crf, 0);
            av_dict_set(&encoder_options, "x265-params", "log-level=error", 0);
        } else if (config_value.encoder_name == "hevc_videotoolbox") {
            av_dict_set(&encoder_options, "realtime", "true", 0);
            av_dict_set(&encoder_options, "allow_sw", "true", 0);
        }
        const int open_result = avcodec_open2(codec_context,
                                              codec,
                                              &encoder_options);
        av_dict_free(&encoder_options);
        require_ok(open_result, "could not open H.265 encoder");

        stream->time_base = codec_context->time_base;
        require_ok(avcodec_parameters_from_context(stream->codecpar,
                                                   codec_context),
                   "could not copy H.265 stream parameters");
        stream->codecpar->codec_tag = MKTAG('h', 'v', 'c', '1');

        if ((format->oformat->flags & AVFMT_NOFILE) == 0) {
            require_ok(avio_open(&format->pb,
                                 temporary_path.c_str(),
                                 AVIO_FLAG_WRITE),
                       "could not open encoded output");
        }
        require_ok(avformat_write_header(format, nullptr),
                   "could not write MP4 header");
        header_written = true;

        frame = av_frame_alloc();
        packet = av_packet_alloc();
        if (frame == nullptr || packet == nullptr) {
            throw std::runtime_error("could not allocate encoder frame or packet");
        }
        frame->format = codec_context->pix_fmt;
        frame->width = codec_context->width;
        frame->height = codec_context->height;
        frame->color_range = codec_context->color_range;
        frame->colorspace = codec_context->colorspace;
        frame->color_primaries = codec_context->color_primaries;
        frame->color_trc = codec_context->color_trc;
        require_ok(av_frame_get_buffer(frame, 32),
                   "could not allocate encoder frame buffer");

        scaler = sws_getContext(config_value.width,
                                config_value.height,
                                AV_PIX_FMT_RGB24,
                                config_value.width,
                                config_value.height,
                                codec_context->pix_fmt,
                                SWS_BICUBIC,
                                nullptr,
                                nullptr,
                                nullptr);
        if (scaler == nullptr) {
            throw std::runtime_error("could not create RGB-to-YUV converter");
        }
    }

    void write_rgb(const VideoFrame& source) {
        if (is_finished) {
            throw std::logic_error("cannot write after encoder finish");
        }
        if (source.width != config_value.width ||
            source.height != config_value.height ||
            source.rgb.size() != static_cast<std::size_t>(source.width) *
                                     static_cast<std::size_t>(source.height) * 3U) {
            throw std::invalid_argument("RGB frame does not match encoder dimensions");
        }

        require_ok(av_frame_make_writable(frame),
                   "encoder frame is not writable");
        const std::uint8_t* source_data[] = {
            source.rgb.data(), nullptr, nullptr, nullptr};
        const int source_stride[] = {source.width * 3, 0, 0, 0};
        const int rows = sws_scale(scaler,
                                   source_data,
                                   source_stride,
                                   0,
                                   source.height,
                                   frame->data,
                                   frame->linesize);
        if (rows != source.height) {
            throw std::runtime_error("RGB-to-YUV conversion returned incomplete frame");
        }
        frame->pts = static_cast<std::int64_t>(frame_count);
        send_frame(frame);
        ++frame_count;
    }

    void send_frame(AVFrame* input) {
        require_ok(avcodec_send_frame(codec_context, input),
                   "H.265 encoder rejected a frame");
        while (true) {
            const int result = avcodec_receive_packet(codec_context, packet);
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
                return;
            }
            require_ok(result, "H.265 encoder failed to return a packet");
            av_packet_rescale_ts(packet,
                                 codec_context->time_base,
                                 stream->time_base);
            if (packet->duration <= 0) {
                packet->duration = av_rescale_q(1,
                                                codec_context->time_base,
                                                stream->time_base);
            }
            packet->stream_index = stream->index;
            video_bytes += static_cast<std::uint64_t>(packet->size);
            require_ok(av_interleaved_write_frame(format, packet),
                       "could not mux H.265 packet");
            av_packet_unref(packet);
        }
    }

    EncoderStats finish() {
        if (is_finished) {
            return stats_value;
        }
        if (frame_count == 0) {
            throw std::runtime_error("cannot finalize an empty H.265 video");
        }

        send_frame(nullptr);
        require_ok(av_write_trailer(format), "could not finalize MP4 output");
        trailer_written = true;
        close_output_file();

        if (temporary_path != config_value.output_path) {
            mux_video_and_source_audio(temporary_path,
                                       config_value.source_path,
                                       config_value.output_path);
            std::remove(temporary_path.c_str());
            temporary_path.clear();
        }

        is_finished = true;
        stats_value.frames = frame_count;
        stats_value.video_bytes = video_bytes;
        stats_value.elapsed_seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started_at).count();
        return stats_value;
    }

    void close_output_file() noexcept {
        if (format != nullptr && format->pb != nullptr &&
            (format->oformat->flags & AVFMT_NOFILE) == 0) {
            avio_closep(&format->pb);
        }
    }

    void cleanup() noexcept {
        if (packet != nullptr) av_packet_free(&packet);
        if (frame != nullptr) av_frame_free(&frame);
        if (scaler != nullptr) {
            sws_freeContext(scaler);
            scaler = nullptr;
        }
        if (codec_context != nullptr) avcodec_free_context(&codec_context);
        close_output_file();
        if (format != nullptr) {
            avformat_free_context(format);
            format = nullptr;
        }
    }

    EncoderConfig config_value;
    AVFormatContext* format = nullptr;
    AVCodecContext* codec_context = nullptr;
    AVStream* stream = nullptr;
    SwsContext* scaler = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    std::string temporary_path;
    std::size_t frame_count = 0;
    std::uint64_t video_bytes = 0;
    bool header_written = false;
    bool trailer_written = false;
    bool is_finished = false;
    EncoderStats stats_value;
    std::chrono::steady_clock::time_point started_at;
};

VideoEncoder::VideoEncoder(const EncoderConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}

VideoEncoder::~VideoEncoder() = default;
VideoEncoder::VideoEncoder(VideoEncoder&&) noexcept = default;
VideoEncoder& VideoEncoder::operator=(VideoEncoder&&) noexcept = default;

void VideoEncoder::write_rgb(const VideoFrame& frame) {
    impl_->write_rgb(frame);
}

EncoderStats VideoEncoder::finish() { return impl_->finish(); }

const EncoderConfig& VideoEncoder::config() const noexcept {
    return impl_->config_value;
}

bool VideoEncoder::finished() const noexcept { return impl_->is_finished; }

}  // namespace medialab

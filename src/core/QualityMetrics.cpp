#include "medialab/QualityMetrics.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <thread>

#if defined(MEDIA_HAS_VMAF) && MEDIA_HAS_VMAF
extern "C" {
#include <libvmaf/libvmaf.h>
}
#endif

namespace medialab {
namespace {

double luma(const std::uint8_t* pixel) {
    return 0.2126 * static_cast<double>(pixel[0]) +
           0.7152 * static_cast<double>(pixel[1]) +
           0.0722 * static_cast<double>(pixel[2]);
}

void validate_frames(const std::vector<std::uint8_t>& reference_rgb,
                     const std::vector<std::uint8_t>& compared_rgb,
                     int width,
                     int height) {
    const std::size_t expected = static_cast<std::size_t>(width) *
                                 static_cast<std::size_t>(height) * 3U;
    if (width <= 0 || height <= 0 || reference_rgb.size() != expected ||
        compared_rgb.size() != expected) {
        throw std::invalid_argument("quality metric frames have incompatible sizes");
    }
}

}  // namespace

double calculate_psnr_rgb(const std::vector<std::uint8_t>& reference_rgb,
                          const std::vector<std::uint8_t>& compared_rgb) {
    if (reference_rgb.empty() || reference_rgb.size() != compared_rgb.size()) {
        throw std::invalid_argument("PSNR inputs have incompatible sizes");
    }
    long double squared_error = 0.0;
    for (std::size_t index = 0; index < reference_rgb.size(); ++index) {
        const long double difference =
            static_cast<long double>(reference_rgb[index]) -
            static_cast<long double>(compared_rgb[index]);
        squared_error += difference * difference;
    }
    const long double mse = squared_error /
                            static_cast<long double>(reference_rgb.size());
    if (mse <= std::numeric_limits<long double>::epsilon()) {
        return 100.0;
    }
    return 10.0 * std::log10(255.0 * 255.0 / static_cast<double>(mse));
}

double calculate_ssim_luma(const std::vector<std::uint8_t>& reference_rgb,
                           const std::vector<std::uint8_t>& compared_rgb,
                           int width,
                           int height) {
    validate_frames(reference_rgb, compared_rgb, width, height);
    constexpr int block_size = 8;
    constexpr double c1 = 6.5025;
    constexpr double c2 = 58.5225;
    double score_sum = 0.0;
    std::size_t blocks = 0;

    for (int block_y = 0; block_y < height; block_y += block_size) {
        for (int block_x = 0; block_x < width; block_x += block_size) {
            const int block_width = std::min(block_size, width - block_x);
            const int block_height = std::min(block_size, height - block_y);
            const int count = block_width * block_height;
            if (count <= 1) {
                continue;
            }

            double reference_mean = 0.0;
            double compared_mean = 0.0;
            for (int y = 0; y < block_height; ++y) {
                for (int x = 0; x < block_width; ++x) {
                    const std::size_t offset =
                        (static_cast<std::size_t>(block_y + y) *
                             static_cast<std::size_t>(width) +
                         static_cast<std::size_t>(block_x + x)) * 3U;
                    reference_mean += luma(reference_rgb.data() + offset);
                    compared_mean += luma(compared_rgb.data() + offset);
                }
            }
            reference_mean /= static_cast<double>(count);
            compared_mean /= static_cast<double>(count);

            double reference_variance = 0.0;
            double compared_variance = 0.0;
            double covariance = 0.0;
            for (int y = 0; y < block_height; ++y) {
                for (int x = 0; x < block_width; ++x) {
                    const std::size_t offset =
                        (static_cast<std::size_t>(block_y + y) *
                             static_cast<std::size_t>(width) +
                         static_cast<std::size_t>(block_x + x)) * 3U;
                    const double reference_delta =
                        luma(reference_rgb.data() + offset) - reference_mean;
                    const double compared_delta =
                        luma(compared_rgb.data() + offset) - compared_mean;
                    reference_variance += reference_delta * reference_delta;
                    compared_variance += compared_delta * compared_delta;
                    covariance += reference_delta * compared_delta;
                }
            }
            const double divisor = static_cast<double>(count - 1);
            reference_variance /= divisor;
            compared_variance /= divisor;
            covariance /= divisor;

            const double numerator =
                (2.0 * reference_mean * compared_mean + c1) *
                (2.0 * covariance + c2);
            const double denominator =
                (reference_mean * reference_mean +
                 compared_mean * compared_mean + c1) *
                (reference_variance + compared_variance + c2);
            score_sum += denominator > 0.0 ? numerator / denominator : 1.0;
            ++blocks;
        }
    }
    return blocks > 0 ? score_sum / static_cast<double>(blocks) : 1.0;
}

void QualityMetricsCollector::record(
    const std::vector<std::uint8_t>& reference_rgb,
    const std::vector<std::uint8_t>& compared_rgb,
    int width,
    int height) {
    validate_frames(reference_rgb, compared_rgb, width, height);
    psnr_sum_ += calculate_psnr_rgb(reference_rgb, compared_rgb);
    ssim_sum_ += calculate_ssim_luma(reference_rgb,
                                     compared_rgb,
                                     width,
                                     height);
    ++frames_;
}

QualitySummary QualityMetricsCollector::summarize() const noexcept {
    QualitySummary result;
    result.frames = frames_;
    if (frames_ > 0) {
        result.average_psnr_db = psnr_sum_ / static_cast<double>(frames_);
        result.average_ssim = ssim_sum_ / static_cast<double>(frames_);
    }
    result.vmaf_available = has_vmaf_;
    result.average_vmaf = vmaf_score_;
    result.vmaf_model = vmaf_model_;
    return result;
}

void QualityMetricsCollector::set_vmaf(double score,
                                       const std::string& model) {
    if (!std::isfinite(score)) {
        throw std::invalid_argument("VMAF score must be finite");
    }
    has_vmaf_ = true;
    vmaf_score_ = score;
    vmaf_model_ = model;
}

std::string QualityMetricsCollector::to_json() const {
    const QualitySummary summary = summarize();
    std::ostringstream output;
    output << std::fixed << std::setprecision(6)
           << "{\n"
           << "  \"reference\": \"shader_output_before_encode\",\n"
           << "  \"psnr_colorspace\": \"rgb24\",\n"
           << "  \"ssim_method\": \"8x8_luma_blocks\",\n"
           << "  \"frames\": " << summary.frames << ",\n"
           << "  \"average_psnr_db\": " << summary.average_psnr_db << ",\n"
           << "  \"average_ssim\": " << summary.average_ssim << ",\n"
           << "  \"vmaf_available\": "
           << (summary.vmaf_available ? "true" : "false");
    if (summary.vmaf_available) {
        output << ",\n"
               << "  \"vmaf_model\": \"" << summary.vmaf_model << "\",\n"
               << "  \"average_vmaf\": " << summary.average_vmaf << "\n";
    } else {
        output << "\n";
    }
    output
           << "}";
    return output.str();
}

class VmafCalculator::Impl {
public:
#if defined(MEDIA_HAS_VMAF) && MEDIA_HAS_VMAF
    Impl() {
        VmafConfiguration configuration = {};
        configuration.log_level = VMAF_LOG_LEVEL_WARNING;
        const unsigned hardware_threads = std::thread::hardware_concurrency();
        configuration.n_threads = std::max(
            1U, std::min(4U, hardware_threads > 0 ? hardware_threads : 1U));
        configuration.n_subsample = 1;
        require_vmaf(vmaf_init(&context, configuration),
                     "could not initialize libvmaf");
        try {
            VmafModelConfig model_configuration = {};
            require_vmaf(vmaf_model_load(&model,
                                         &model_configuration,
                                         model_label),
                         "could not load built-in VMAF model");
            require_vmaf(vmaf_use_features_from_model(context, model),
                         "could not register VMAF model features");
        } catch (...) {
            if (model != nullptr) {
                vmaf_model_destroy(model);
                model = nullptr;
            }
            vmaf_close(context);
            context = nullptr;
            throw;
        }
    }

    ~Impl() {
        if (context != nullptr && frame_count > 0 && !flushed) {
            vmaf_read_pictures(context, nullptr, nullptr, 0);
        }
        if (model != nullptr) {
            vmaf_model_destroy(model);
            model = nullptr;
        }
        if (context != nullptr) {
            vmaf_close(context);
            context = nullptr;
        }
    }

    static void require_vmaf(int result, const std::string& operation) {
        if (result != 0) {
            throw std::runtime_error(operation +
                                     " (libvmaf error " +
                                     std::to_string(result) + ")");
        }
    }

    static void fill_picture(VmafPicture& picture,
                             const std::vector<std::uint8_t>& rgb,
                             int width,
                             int height) {
        require_vmaf(vmaf_picture_alloc(&picture,
                                        VMAF_PIX_FMT_YUV420P,
                                        8,
                                        static_cast<unsigned>(width),
                                        static_cast<unsigned>(height)),
                     "could not allocate a VMAF picture");
        for (int y = 0; y < height; ++y) {
            auto* destination = static_cast<std::uint8_t*>(picture.data[0]) +
                                static_cast<std::ptrdiff_t>(y) *
                                    picture.stride[0];
            for (int x = 0; x < width; ++x) {
                const std::size_t offset =
                    (static_cast<std::size_t>(y) *
                         static_cast<std::size_t>(width) +
                     static_cast<std::size_t>(x)) * 3U;
                const double normalized_luma =
                    (0.2126 * static_cast<double>(rgb[offset]) +
                     0.7152 * static_cast<double>(rgb[offset + 1]) +
                     0.0722 * static_cast<double>(rgb[offset + 2])) / 255.0;
                destination[x] = static_cast<std::uint8_t>(std::clamp(
                    std::lround(16.0 + 219.0 * normalized_luma),
                    16L,
                    235L));
            }
        }
        for (int plane = 1; plane < 3; ++plane) {
            for (unsigned y = 0; y < picture.h[plane]; ++y) {
                auto* destination =
                    static_cast<std::uint8_t*>(picture.data[plane]) +
                    static_cast<std::ptrdiff_t>(y) * picture.stride[plane];
                std::memset(destination, 128, picture.w[plane]);
            }
        }
    }

    void record(const std::vector<std::uint8_t>& reference_rgb,
                const std::vector<std::uint8_t>& compared_rgb,
                int width,
                int height) {
        validate_frames(reference_rgb, compared_rgb, width, height);
        if (flushed) {
            throw std::logic_error("cannot add frames after VMAF finish");
        }
        VmafPicture reference = {};
        VmafPicture compared = {};
        fill_picture(reference, reference_rgb, width, height);
        try {
            fill_picture(compared, compared_rgb, width, height);
        } catch (...) {
            vmaf_picture_unref(&reference);
            throw;
        }
        const int result = vmaf_read_pictures(context,
                                              &reference,
                                              &compared,
                                              static_cast<unsigned>(frame_count));
        if (result != 0) {
            vmaf_picture_unref(&reference);
            vmaf_picture_unref(&compared);
            require_vmaf(result, "could not submit pictures to libvmaf");
        }
        ++frame_count;
    }

    double finish() {
        if (frame_count == 0) {
            throw std::runtime_error("cannot calculate VMAF without frames");
        }
        if (!flushed) {
            require_vmaf(vmaf_read_pictures(context, nullptr, nullptr, 0),
                         "could not flush libvmaf");
            flushed = true;
            require_vmaf(vmaf_score_pooled(context,
                                           model,
                                           VMAF_POOL_METHOD_MEAN,
                                           &score,
                                           0,
                                           static_cast<unsigned>(frame_count - 1)),
                         "could not calculate pooled VMAF score");
        }
        return score;
    }

    VmafContext* context = nullptr;
    VmafModel* model = nullptr;
    std::size_t frame_count = 0;
    bool flushed = false;
    double score = 0.0;
    static constexpr const char* model_label = "vmaf_v0.6.1";
#else
    void record(const std::vector<std::uint8_t>&,
                const std::vector<std::uint8_t>&,
                int,
                int) {
        throw std::runtime_error("libvmaf support is not compiled in");
    }

    double finish() {
        throw std::runtime_error("libvmaf support is not compiled in");
    }
#endif
};

VmafCalculator::VmafCalculator() : impl_(std::make_unique<Impl>()) {}
VmafCalculator::~VmafCalculator() = default;
VmafCalculator::VmafCalculator(VmafCalculator&&) noexcept = default;
VmafCalculator& VmafCalculator::operator=(VmafCalculator&&) noexcept = default;

bool VmafCalculator::available() const noexcept {
#if defined(MEDIA_HAS_VMAF) && MEDIA_HAS_VMAF
    return true;
#else
    return false;
#endif
}

const char* VmafCalculator::model_name() const noexcept {
#if defined(MEDIA_HAS_VMAF) && MEDIA_HAS_VMAF
    return Impl::model_label;
#else
    return "unavailable";
#endif
}

void VmafCalculator::record(
    const std::vector<std::uint8_t>& reference_rgb,
    const std::vector<std::uint8_t>& compared_rgb,
    int width,
    int height) {
    impl_->record(reference_rgb, compared_rgb, width, height);
}

double VmafCalculator::finish() { return impl_->finish(); }

}  // namespace medialab

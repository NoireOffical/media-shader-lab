#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace medialab {

struct QualitySummary {
    std::size_t frames = 0;
    double average_psnr_db = 0.0;
    double average_ssim = 0.0;
    bool vmaf_available = false;
    double average_vmaf = 0.0;
    std::string vmaf_model;
};

class QualityMetricsCollector {
public:
    void record(const std::vector<std::uint8_t>& reference_rgb,
                const std::vector<std::uint8_t>& compared_rgb,
                int width,
                int height);
    QualitySummary summarize() const noexcept;
    void set_vmaf(double score, const std::string& model);
    std::string to_json() const;

private:
    std::size_t frames_ = 0;
    double psnr_sum_ = 0.0;
    double ssim_sum_ = 0.0;
    bool has_vmaf_ = false;
    double vmaf_score_ = 0.0;
    std::string vmaf_model_;
};

class VmafCalculator {
public:
    VmafCalculator();
    ~VmafCalculator();

    VmafCalculator(const VmafCalculator&) = delete;
    VmafCalculator& operator=(const VmafCalculator&) = delete;
    VmafCalculator(VmafCalculator&&) noexcept;
    VmafCalculator& operator=(VmafCalculator&&) noexcept;

    bool available() const noexcept;
    const char* model_name() const noexcept;
    void record(const std::vector<std::uint8_t>& reference_rgb,
                const std::vector<std::uint8_t>& compared_rgb,
                int width,
                int height);
    double finish();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

double calculate_psnr_rgb(const std::vector<std::uint8_t>& reference_rgb,
                          const std::vector<std::uint8_t>& compared_rgb);
double calculate_ssim_luma(const std::vector<std::uint8_t>& reference_rgb,
                           const std::vector<std::uint8_t>& compared_rgb,
                           int width,
                           int height);

}  // namespace medialab

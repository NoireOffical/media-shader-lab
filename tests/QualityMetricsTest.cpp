#include "medialab/QualityMetrics.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

int main() {
    constexpr int width = 16;
    constexpr int height = 16;
    std::vector<std::uint8_t> reference(width * height * 3, 128);
    std::vector<std::uint8_t> identical = reference;
    assert(std::abs(medialab::calculate_psnr_rgb(reference, identical) - 100.0) < 1e-9);
    assert(std::abs(medialab::calculate_ssim_luma(
                        reference, identical, width, height) - 1.0) < 1e-9);

    std::vector<std::uint8_t> distorted = reference;
    for (std::size_t index = 0; index < distorted.size(); index += 3) {
        distorted[index] = static_cast<std::uint8_t>(index % 255);
    }
    const double psnr = medialab::calculate_psnr_rgb(reference, distorted);
    const double ssim = medialab::calculate_ssim_luma(
        reference, distorted, width, height);
    assert(psnr > 0.0 && psnr < 100.0);
    assert(ssim >= -1.0 && ssim < 1.0);

    medialab::QualityMetricsCollector collector;
    collector.record(reference, distorted, width, height);
    const auto summary = collector.summarize();
    assert(summary.frames == 1);
    assert(std::abs(summary.average_psnr_db - psnr) < 1e-9);

    medialab::VmafCalculator vmaf;
    if (vmaf.available()) {
        constexpr int vmaf_width = 192;
        constexpr int vmaf_height = 108;
        std::vector<std::uint8_t> gradient(vmaf_width * vmaf_height * 3);
        for (int y = 0; y < vmaf_height; ++y) {
            for (int x = 0; x < vmaf_width; ++x) {
                const std::size_t offset =
                    (static_cast<std::size_t>(y) * vmaf_width + x) * 3U;
                gradient[offset] = static_cast<std::uint8_t>(x * 255 /
                                                              (vmaf_width - 1));
                gradient[offset + 1] = static_cast<std::uint8_t>(y * 255 /
                                                                  (vmaf_height - 1));
                gradient[offset + 2] = 128;
            }
        }
        for (int frame = 0; frame < 4; ++frame) {
            vmaf.record(gradient,
                        gradient,
                        vmaf_width,
                        vmaf_height);
        }
        const double vmaf_score = vmaf.finish();
        assert(vmaf_score > 90.0 && vmaf_score <= 100.5);

        medialab::VmafCalculator degraded_vmaf;
        const std::vector<std::uint8_t> black(gradient.size(), 0);
        for (int frame = 0; frame < 4; ++frame) {
            degraded_vmaf.record(gradient,
                                 black,
                                 vmaf_width,
                                 vmaf_height);
        }
        assert(degraded_vmaf.finish() < vmaf_score);
        collector.set_vmaf(vmaf_score, vmaf.model_name());
        const auto vmaf_summary = collector.summarize();
        assert(vmaf_summary.vmaf_available);
        assert(vmaf_summary.average_vmaf > 90.0);
        assert(collector.to_json().find("\"average_vmaf\"") !=
               std::string::npos);
    }
    std::cout << "quality metrics tests passed\n";
    return 0;
}

#include "medialab/Metrics.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <numeric>
#include <sstream>
#include <stdexcept>

namespace medialab {
namespace {

double average(const std::vector<double>& values) {
    if (values.empty()) {
        return 0.0;
    }
    return std::accumulate(values.begin(), values.end(), 0.0) /
           static_cast<double>(values.size());
}

double percentile(std::vector<double> values, double fraction) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const auto rank = static_cast<std::size_t>(
        std::ceil(fraction * static_cast<double>(values.size())));
    const auto index = rank == 0 ? 0 : std::min(rank - 1, values.size() - 1);
    return values[index];
}

}  // namespace

void MetricsCollector::record(double decode_ms, double render_ms) {
    decode_ms_.push_back(decode_ms);
    render_ms_.push_back(render_ms);
}

MetricsSummary MetricsCollector::summarize(double elapsed_seconds) const {
    MetricsSummary result;
    result.frames = decode_ms_.size();
    result.elapsed_seconds = elapsed_seconds;
    result.wall_fps = elapsed_seconds > 0.0
                          ? static_cast<double>(result.frames) / elapsed_seconds
                          : 0.0;
    result.average_decode_ms = average(decode_ms_);
    result.average_render_ms = average(render_ms_);

    std::vector<double> pipeline_ms;
    pipeline_ms.reserve(result.frames);
    for (std::size_t i = 0; i < result.frames; ++i) {
        pipeline_ms.push_back(decode_ms_[i] + render_ms_[i]);
    }
    result.p95_pipeline_ms = percentile(pipeline_ms, 0.95);
    result.max_pipeline_ms = pipeline_ms.empty()
                                 ? 0.0
                                 : *std::max_element(pipeline_ms.begin(), pipeline_ms.end());
    return result;
}

std::string MetricsCollector::to_json(double elapsed_seconds) const {
    const auto s = summarize(elapsed_seconds);
    std::ostringstream out;
    out << std::fixed << std::setprecision(3)
        << "{\n"
        << "  \"frames\": " << s.frames << ",\n"
        << "  \"elapsed_seconds\": " << s.elapsed_seconds << ",\n"
        << "  \"wall_fps\": " << s.wall_fps << ",\n"
        << "  \"average_decode_ms\": " << s.average_decode_ms << ",\n"
        << "  \"average_render_ms\": " << s.average_render_ms << ",\n"
        << "  \"p95_pipeline_ms\": " << s.p95_pipeline_ms << ",\n"
        << "  \"max_pipeline_ms\": " << s.max_pipeline_ms << "\n"
        << "}";
    return out.str();
}

RecentFrameRate::RecentFrameRate(double window_seconds)
    : window_seconds_(window_seconds) {
    if (!std::isfinite(window_seconds_) || window_seconds_ <= 0.0) {
        throw std::invalid_argument(
            "frame-rate window must be a positive finite duration");
    }
}

void RecentFrameRate::record(double timestamp_seconds) {
    if (!std::isfinite(timestamp_seconds)) {
        throw std::invalid_argument("frame timestamp must be finite");
    }
    if (!timestamps_.empty() && timestamp_seconds < timestamps_.back()) {
        timestamps_.clear();
    }
    timestamps_.push_back(timestamp_seconds);
    while (timestamps_.size() > 1 &&
           timestamp_seconds - timestamps_.front() > window_seconds_) {
        timestamps_.pop_front();
    }
}

void RecentFrameRate::reset() noexcept { timestamps_.clear(); }

double RecentFrameRate::fps() const noexcept {
    if (timestamps_.size() < 2) {
        return 0.0;
    }
    const double elapsed = timestamps_.back() - timestamps_.front();
    return elapsed > 0.0
        ? static_cast<double>(timestamps_.size() - 1) / elapsed
        : 0.0;
}

std::string format_summary(const MetricsSummary& s) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(2)
        << "frames=" << s.frames
        << " wall_fps=" << s.wall_fps
        << " decode_avg_ms=" << s.average_decode_ms
        << " render_avg_ms=" << s.average_render_ms
        << " pipeline_p95_ms=" << s.p95_pipeline_ms
        << " pipeline_max_ms=" << s.max_pipeline_ms;
    return out.str();
}

}  // namespace medialab

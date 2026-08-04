#pragma once

#include <cstddef>
#include <deque>
#include <string>
#include <vector>

namespace medialab {

struct MetricsSummary {
    std::size_t frames = 0;
    double elapsed_seconds = 0.0;
    double wall_fps = 0.0;
    double average_decode_ms = 0.0;
    double average_render_ms = 0.0;
    double p95_pipeline_ms = 0.0;
    double max_pipeline_ms = 0.0;
};

class MetricsCollector {
public:
    void record(double decode_ms, double render_ms);
    MetricsSummary summarize(double elapsed_seconds) const;
    std::string to_json(double elapsed_seconds) const;

private:
    std::vector<double> decode_ms_;
    std::vector<double> render_ms_;
};

class RecentFrameRate {
public:
    explicit RecentFrameRate(double window_seconds = 1.0);

    void record(double timestamp_seconds);
    void reset() noexcept;
    double fps() const noexcept;

private:
    double window_seconds_ = 1.0;
    std::deque<double> timestamps_;
};

std::string format_summary(const MetricsSummary& summary);

}  // namespace medialab

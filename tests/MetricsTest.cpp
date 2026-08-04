#include "medialab/Metrics.hpp"

#include <cassert>
#include <cmath>
#include <iostream>

namespace {

bool close_to(double left, double right, double tolerance = 1e-9) {
    return std::abs(left - right) <= tolerance;
}

}  // namespace

int main() {
    medialab::MetricsCollector metrics;
    metrics.record(2.0, 1.0);
    metrics.record(4.0, 2.0);
    metrics.record(6.0, 3.0);
    metrics.record(8.0, 4.0);

    const auto summary = metrics.summarize(2.0);
    assert(summary.frames == 4);
    assert(close_to(summary.wall_fps, 2.0));
    assert(close_to(summary.average_decode_ms, 5.0));
    assert(close_to(summary.average_render_ms, 2.5));
    assert(close_to(summary.p95_pipeline_ms, 12.0));
    assert(close_to(summary.max_pipeline_ms, 12.0));

    const std::string json = metrics.to_json(2.0);
    assert(json.find("\"frames\": 4") != std::string::npos);

    medialab::RecentFrameRate recent_fps;
    for (int frame = 0; frame <= 30; ++frame) {
        recent_fps.record(static_cast<double>(frame) / 30.0);
    }
    assert(close_to(recent_fps.fps(), 30.0, 1e-6));

    recent_fps.record(10.0);
    assert(close_to(recent_fps.fps(), 0.0));
    recent_fps.record(10.0 + 1.0 / 30.0);
    assert(close_to(recent_fps.fps(), 30.0, 1e-6));
    recent_fps.reset();
    assert(close_to(recent_fps.fps(), 0.0));
    std::cout << "metrics_test passed\n";
    return 0;
}

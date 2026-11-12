// SurfaceRT-WiDi - closed-loop rate control driven by sink-side feedback.
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace widi {

// What the sink tells the sender about its decode pipeline.
struct RateFeedback {
    uint16_t decodeMs = 0;   // average JPEG decode time for the last window
    uint8_t clientFps = 0;   // frames actually presented per second
    uint32_t decoded = 0;    // cumulative counters from StatsInfo
    uint32_t dropped = 0;
};

// What the sender should use for the next window.
struct RateSettings {
    uint8_t quality = 60;      // JPEG quality, 20..85
    uint8_t fps = 20;          // 5..30
    uint8_t scalePercent = 100;  // 50..100 of the captured desktop
};

enum class RateAction { None, StepDown, StepUp };

// Surface RT (Tegra 3, Cortex-A9) cannot decode 1080p JPEG in real time, so the
// sender adapts instead of the sink dropping frames silently. The policy is
// deliberately simple and fully deterministic so it can be unit tested.
class RateController {
public:
    explicit RateController(const RateSettings& start = RateSettings());

    void feedback(const RateFeedback& fb);
    void reset();
    RateSettings current() const { return current_; }
    RateAction last_action() const { return last_action_; }

    // Exposed for diagnostics/tests.
    int consecutive_good_windows() const { return good_windows_; }
    uint16_t decode_budget_ms() const;

private:
    void step_down();
    void step_up();

    RateSettings current_;
    RateAction last_action_ = RateAction::None;
    uint32_t last_decoded_ = 0;
    uint32_t last_dropped_ = 0;
    int good_windows_ = 0;
    bool primed_ = false;
};

}  // namespace widi

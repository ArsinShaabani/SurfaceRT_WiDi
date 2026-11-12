// SurfaceRT-WiDi - rate control policy.
// SPDX-License-Identifier: MIT
#include "widi/rate_control.h"

namespace widi {
namespace {
constexpr uint8_t kMinQuality = 20;
constexpr uint8_t kMaxQuality = 85;
constexpr uint8_t kMinFps = 5;
constexpr uint8_t kMaxFps = 30;
constexpr uint8_t kMinScale = 50;
// Windows that must look healthy before we spend bandwidth again.
constexpr int kGoodWindowsBeforeUp = 8;
}  // namespace

RateController::RateController(const RateSettings& start) : current_(start) {}

uint16_t RateController::decode_budget_ms() const {
    // Leave ~30% head room in the frame interval for network + presentation.
    const int interval = 1000 / (current_.fps ? current_.fps : 1);
    return static_cast<uint16_t>(interval * 7 / 10);
}

void RateController::reset() {
    current_ = RateSettings();
    last_action_ = RateAction::None;
    last_decoded_ = 0;
    last_dropped_ = 0;
    good_windows_ = 0;
    primed_ = false;
}

void RateController::step_down() {
    if (current_.fps > 12) {
        current_.fps = static_cast<uint8_t>(current_.fps - 4);
    } else if (current_.scalePercent > kMinScale + 10) {
        current_.scalePercent = static_cast<uint8_t>(current_.scalePercent - 10);
    } else if (current_.quality > kMinQuality) {
        current_.quality = static_cast<uint8_t>(current_.quality - 10);
    } else if (current_.scalePercent > kMinScale) {
        current_.scalePercent = kMinScale;
    } else {
        current_.quality = kMinQuality;
    }
    if (current_.fps < kMinFps) current_.fps = kMinFps;
    last_action_ = RateAction::StepDown;
    good_windows_ = 0;
}

void RateController::step_up() {
    if (current_.quality < 75) {
        current_.quality = static_cast<uint8_t>(current_.quality + 5);
    } else if (current_.scalePercent < 100) {
        current_.scalePercent = static_cast<uint8_t>(current_.scalePercent + 10);
    } else if (current_.fps < kMaxFps) {
        current_.fps = static_cast<uint8_t>(current_.fps + 2);
    }
    if (current_.quality > kMaxQuality) current_.quality = kMaxQuality;
    if (current_.scalePercent > 100) current_.scalePercent = 100;
    if (current_.fps > kMaxFps) current_.fps = kMaxFps;
    last_action_ = RateAction::StepUp;
    good_windows_ = 0;
}

void RateController::feedback(const RateFeedback& fb) {
    const uint16_t budget = decode_budget_ms();
    const uint32_t dropped_delta =
        fb.dropped >= last_dropped_ ? fb.dropped - last_dropped_ : 0;
    last_decoded_ = fb.decoded;
    last_dropped_ = fb.dropped;

    if (!primed_) {
        // First report: react immediately but do not "reward" yet.
        primed_ = true;
        if (dropped_delta > 0 || fb.decodeMs > budget) {
            step_down();
        } else {
            last_action_ = RateAction::None;
        }
        return;
    }

    const bool overloaded = dropped_delta > 0 || fb.decodeMs > budget;
    if (overloaded) {
        step_down();
        return;
    }

    const bool healthy = fb.decodeMs * 2 < budget &&
                         (fb.clientFps == 0 || fb.clientFps + 2 >= current_.fps);
    if (healthy) {
        if (++good_windows_ >= kGoodWindowsBeforeUp) {
            step_up();
        } else {
            last_action_ = RateAction::None;
        }
    } else {
        good_windows_ = 0;
        last_action_ = RateAction::None;
    }
}

}  // namespace widi

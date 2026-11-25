#include "native/dynamic_resolution.h"

#include "host/log.h"

#include <algorithm>
#include <cmath>

namespace ct2::perf {

DynamicResolution::DynamicResolution(DynamicResConfig cfg) : cfg_(cfg) {
    Reset(cfg_.max_percent);
}

void DynamicResolution::Configure(const DynamicResConfig& cfg) {
    cfg_ = cfg;
    current_percent_ = std::clamp(current_percent_, cfg_.min_percent, cfg_.max_percent);
}

void DynamicResolution::Reset(uint32_t initial_percent) {
    const auto now = Clock::now();
    current_percent_ = std::clamp(initial_percent, cfg_.min_percent, cfg_.max_percent);
    last_frame_ = now;
    window_start_ = now;
    changed_at_ = now;
    ceiling_until_ = now;
    has_last_frame_ = false;
    raised_last_ = false;
    ceiling_percent_ = cfg_.max_percent + 1;
    window_frames_ = 0;
}

uint32_t DynamicResolution::FloorStep(uint32_t v) {
    return (v / kStep) * kStep;
}

void DynamicResolution::Change(uint32_t to, bool up, Clock::time_point now) {
    log::Info("CT2DynRes", "Scale %u%% -> %u%% (%s)",
              current_percent_, to, up ? "step-up" : "drop");
    current_percent_ = to;
    raised_last_ = up;
    changed_at_ = now;
    window_start_ = now;
    window_frames_ = 0;
}

bool DynamicResolution::OnPresent(bool in_menu_or_pause, uint32_t* out_new_percent) {
    if (!cfg_.enabled) return false;

    const auto now = Clock::now();
    if (!has_last_frame_) {
        last_frame_ = now;
        window_start_ = now;
        has_last_frame_ = true;
        return false;
    }

    const auto delta = now - last_frame_;
    last_frame_ = now;

    // Discard window on loading/background hitches (>250ms), menus, or post-rebuild settle time.
    if (in_menu_or_pause || delta > kHitch || (now - changed_at_) < kSettle) {
        window_start_ = now;
        window_frames_ = 0;
        return false;
    }

    ++window_frames_;
    const auto elapsed = now - window_start_;
    if (elapsed < kWindow) return false;

    const float secs = std::chrono::duration<float>(elapsed).count();
    const float fps = static_cast<float>(window_frames_) / secs;
    window_start_ = now;
    window_frames_ = 0;

    const uint32_t p = current_percent_;
    const uint32_t lo = cfg_.min_percent;
    const uint32_t hi = cfg_.max_percent;

    // Fast drop via sqrt(fps / target) since pixel count scales with percent^2.
    if (fps < cfg_.target_fps * kLow && p > lo) {
        uint32_t to = FloorStep(
            static_cast<uint32_t>(static_cast<float>(p) * std::sqrt(fps / cfg_.target_fps)));
        to = std::clamp(std::min(to, p - kStep), lo, hi);

        // Anti-oscillation ceiling: if we stepped up < 3s ago, blacklist this level for 30s.
        if (raised_last_ && (now - changed_at_) < (kWindow * 3)) {
            ceiling_percent_ = p;
            ceiling_until_ = now + kCeilingFor;
        }

        if (to != p) {
            Change(to, /*up=*/false, now);
            if (out_new_percent) *out_new_percent = current_percent_;
            return true;
        }
        return false;
    }

    // Slow raise after hold time and ceiling cooldown.
    if (fps >= cfg_.target_fps * kHeld && p < hi &&
        (now - changed_at_) >= std::chrono::seconds(cfg_.hold_seconds)) {
        const uint32_t to = std::min(p + kStep, hi);
        if (now >= ceiling_until_ || to < ceiling_percent_) {
            Change(to, /*up=*/true, now);
            if (out_new_percent) *out_new_percent = current_percent_;
            return true;
        }
    }

    return false;
}

} // namespace ct2::perf

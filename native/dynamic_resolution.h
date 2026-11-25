#pragma once

#include <chrono>
#include <cstdint>

namespace ct2::perf {

struct DynamicResConfig {
    bool enabled = true;
    float target_fps = 60.0f;
    uint32_t min_percent = 50;
    uint32_t max_percent = 100;
    uint32_t hold_seconds = 5;
};

// Dynamic resolution controller with anti-oscillation ceiling:
//  - Measures 1s windows, discarding loading hitches (>250ms), menus, and 500ms settle periods
//  - Drops immediately via sqrt(fps / target) when FPS < 92% target
//  - Steps up +5% after hold_seconds at >= 98% target
//  - Enforces a 30s blacklist ceiling if a step-up drops back within 3s
class DynamicResolution {
public:
    static constexpr auto kWindow = std::chrono::seconds(1);
    static constexpr auto kHitch = std::chrono::milliseconds(250);
    static constexpr auto kSettle = std::chrono::milliseconds(500);
    static constexpr auto kCeilingFor = std::chrono::seconds(30);
    static constexpr uint32_t kStep = 5;
    static constexpr float kLow = 0.92f;
    static constexpr float kHeld = 0.98f;

    explicit DynamicResolution(DynamicResConfig cfg = {});

    void Configure(const DynamicResConfig& cfg);
    void Reset(uint32_t initial_percent = 100);

    // Call once per presented frame. Returns true if scale percent changed.
    bool OnPresent(bool in_menu_or_pause, uint32_t* out_new_percent = nullptr);

    uint32_t CurrentPercent() const { return current_percent_; }

private:
    using Clock = std::chrono::steady_clock;

    static uint32_t FloorStep(uint32_t v);
    void Change(uint32_t to, bool up, Clock::time_point now);

    DynamicResConfig cfg_{};
    uint32_t current_percent_ = 100;

    Clock::time_point last_frame_{};
    Clock::time_point window_start_{};
    Clock::time_point changed_at_{};
    Clock::time_point ceiling_until_{};
    bool has_last_frame_ = false;
    bool raised_last_ = false;
    uint32_t ceiling_percent_ = 101;
    uint32_t window_frames_ = 0;
};

} // namespace ct2::perf

#include "game/game_loop.h"

#include "host/memory.h"

#include <cmath>

namespace ct2::game {

void GameLoop::ResetPacing() {
    pacing_initialized_ = false;
    consecutive_skips_ = 0;
}

bool GameLoop::TickFrameSkip(uint32_t guest_skip_flag_va) {
    if (!switches_.frame_skip_60hz) {
        if (guest_skip_flag_va) kernel::W32(guest_skip_flag_va, 0);
        consecutive_skips_ = 0;
        return false;
    }

    const auto now = Clock::now();
    if (!pacing_initialized_) {
        due_ = now;
        pacing_initialized_ = true;
    }

    // Never bank credit ahead of wall clock and never spiral after pause/load hitches.
    if (due_ > now || (now - due_) > kMaxLag) {
        due_ = now;
    }

    bool skip_draw = false;
    if ((now - due_) >= kStep && consecutive_skips_ < kMaxConsecutiveSkips) {
        skip_draw = true;
    }

    if (guest_skip_flag_va) {
        kernel::W32(guest_skip_flag_va, skip_draw ? 1u : 0u);
        skip_draw = kernel::R32(guest_skip_flag_va) != 0;
    }

    consecutive_skips_ = skip_draw ? (consecutive_skips_ + 1) : 0;
    due_ += kStep;
    return skip_draw;
}

bool GameLoop::ShouldRenderEnvCubemap(uint64_t scene_frame_index) const {
    if (scene_frame_index < kSceneWarmupFrames) return true;

    switch (switches_.env_cubemap) {
        case ReflectionCadence::Full:    return true;
        case ReflectionCadence::Half:    return (scene_frame_index % 2) == 0;
        case ReflectionCadence::Quarter: return (scene_frame_index % 4) == 0;
        case ReflectionCadence::Off:     return (scene_frame_index % kOffRefreshInterval) == 0;
    }
    return true;
}

float GameLoop::ComputeHorPlusProjXScale(uint32_t surface_w, uint32_t surface_h, bool enabled) {
    if (!enabled || surface_w == 0 || surface_h == 0) return 1.0f;
    constexpr float kNativeAspect = 4.0f / 3.0f;
    const float current_aspect = static_cast<float>(surface_w) / static_cast<float>(surface_h);
    return kNativeAspect / current_aspect;
}

RectI GameLoop::ScaleGuestRectSeamless(const RectF& guest_rect,
                                       float scale_x, float scale_y,
                                       float offset_x, float offset_y) {
    RectI out{};
    out.x0 = static_cast<int32_t>(std::floor(guest_rect.x0 * scale_x + offset_x));
    out.y0 = static_cast<int32_t>(std::floor(guest_rect.y0 * scale_y + offset_y));
    out.x1 = static_cast<int32_t>(std::ceil(guest_rect.x1 * scale_x + offset_x));
    out.y1 = static_cast<int32_t>(std::ceil(guest_rect.y1 * scale_y + offset_y));
    return out;
}

} // namespace ct2::game

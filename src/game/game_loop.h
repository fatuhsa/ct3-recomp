#pragma once

#include <chrono>
#include <cstdint>

namespace ct2::game {

enum class ReflectionCadence : uint8_t {
    Full = 1,
    Half = 2,
    Quarter = 4,
    Off = 0,
};

// Engine-level graphics switches patched directly into guest game flags
// rather than brittle renderer heuristics.
struct GraphicsSwitches {
    ReflectionCadence env_cubemap = ReflectionCadence::Half;
    bool road_reflections = true;
    bool particle_draw = true;      // Never disable master particle spawn (corrupts object pools)
    bool bloom_glow = true;
    bool screen_post_fx = true;     // Motion blur / screen raindrop passes
    bool ultrawide_hor_plus = true; // Hor+ 3D projection, 4:3/16:9 centered 2D HUD
    bool frame_skip_60hz = true;
};

struct RectF {
    float x0, y0, x1, y1;
};

struct RectI {
    int32_t x0, y0, x1, y1;
};

// Fixed-timestep 60 Hz frame skip accumulator + engine switch helpers.
class GameLoop {
public:
    static constexpr auto kStep = std::chrono::microseconds(16667);
    static constexpr auto kMaxLag = std::chrono::milliseconds(250);
    static constexpr int kMaxConsecutiveSkips = 3; // Guarantee >= 15 rendered FPS
    static constexpr uint32_t kSceneWarmupFrames = 120;
    static constexpr uint32_t kOffRefreshInterval = 512;

    void ResetPacing();

    // Evaluates whether the current 60 Hz logic tick should skip rendering
    // (returns true if draw should be skipped; updates guest skip-draw flag when wired).
    bool TickFrameSkip(uint32_t guest_skip_flag_va = 0);

    // Environment cubemap render decision:
    // Always renders during the first 120 frames of a scene and every 512th frame
    // even when set to Off so vehicles/materials never render uninitialized pure white.
    bool ShouldRenderEnvCubemap(uint64_t scene_frame_index) const;

    // Hor+ ultrawide 3D projection X-scale multiplier relative to native 4:3 Dreamcast output.
    static float ComputeHorPlusProjXScale(uint32_t surface_w, uint32_t surface_h,
                                          bool enabled);

    // Guest-to-host subpixel & seam alignment:
    // Always floor low edges and ceil high edges so adjacent quads/scissors have zero 1px gaps.
    static RectI ScaleGuestRectSeamless(const RectF& guest_rect,
                                        float scale_x, float scale_y,
                                        float offset_x = 0.0f, float offset_y = 0.0f);

    GraphicsSwitches& Switches() { return switches_; }
    const GraphicsSwitches& Switches() const { return switches_; }

private:
    using Clock = std::chrono::steady_clock;

    GraphicsSwitches switches_{};
    Clock::time_point due_{};
    bool pacing_initialized_ = false;
    int consecutive_skips_ = 0;
};

} // namespace ct2::game
